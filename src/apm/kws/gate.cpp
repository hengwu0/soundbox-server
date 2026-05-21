#include "apm/kws/gate.hpp"

#include "common/log.hpp"

namespace xiaoai_server::wakeup {

namespace {
// kLog 是唤醒门控模块专用日志器。
const auto kLog = xiaoai_server::GetLogger("gate");
}  // namespace

// 构造唤醒门控，并启动负责超时回收的后台线程。
// 参数说明：
// - timeout: 激活态最大空闲时长；非法值会回退到 60 秒。
// - hooks: 状态切换回调集合。
// 返回值：
// - 无。
Gate::Gate(std::chrono::milliseconds timeout, Hooks hooks)
    : timeout_(timeout.count() > 0 ? timeout : std::chrono::seconds(60)), hooks_(std::move(hooks)) {
  timeout_thread_ = std::thread([this]() { TimeoutLoop(); });
}

// 析构唤醒门控并确保后台线程退出。
// 参数说明：
// - 无。
// 返回值：
// - 无。
Gate::~Gate() { Close(); }

// 查询当前门控状态。
// 参数说明：
// - 无。
// 返回值：
// - 返回当前 Step 状态。
Step Gate::step() const {
  std::lock_guard<std::mutex> lock(mu_);
  return step_;
}

// 尝试通过本地关键词命中激活门控。
// 参数说明：
// - keyword: 命中的唤醒词文本。
// 返回值：
// - 从空闲态成功进入激活态时返回 true，否则返回 false。
bool Gate::TryWakeup(const std::string& keyword) {
  // reason 记录本次激活来源，便于日志和上层状态处理定位。
  const std::string reason = "local_kws: " + keyword;
  // on_arm 是锁外执行的激活回调，避免回调里重入 Gate 时死锁。
  std::function<void(const std::string&)> on_arm;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (step_ != Step::kIdle) {
      kLog->debug("gate reject wakeup: busy");
      return false;
    }
    step_ = Step::kActive;
    RefreshTimeoutLocked();
    on_arm = hooks_.on_arm;
  }
  kLog->info("gate to active: {}", reason);
  if (on_arm) {
    on_arm(reason);
  }
  return true;
}

// 强制进入激活态，并在原先为空闲态时触发 on_arm 回调。
// 参数说明：
// - reason: 激活原因。
// 返回值：
// - 无。
void Gate::Activate(const std::string& reason) {
  // was_idle 表示本次 Activate 是否真的从空闲态进入激活态。
  bool was_idle = false;
  // on_arm 是锁外执行的激活回调。
  std::function<void(const std::string&)> on_arm;
  {
    std::lock_guard<std::mutex> lock(mu_);
    was_idle = (step_ == Step::kIdle);
    step_ = Step::kActive;
    RefreshTimeoutLocked();
    on_arm = hooks_.on_arm;
  }
  if (was_idle && on_arm) {
    on_arm(reason);
  }
}

// 退回空闲态，并在状态确实变化时触发 after_disarm 回调。
// 参数说明：
// - reason: 退回空闲原因。
// 返回值：
// - 无。
void Gate::Disarm(const std::string& reason) {
  // after 是锁外执行的退回空闲回调。
  std::function<void(const std::string&)> after;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (step_ == Step::kIdle) {
      return;
    }
    step_ = Step::kIdle;
    ++timeout_epoch_;
    after = hooks_.after_disarm;
  }
  kLog->info("gate to idle: {}", reason);
  timeout_cv_.notify_all();
  if (after) {
    after(reason);
  }
}

// 刷新激活态超时截止时间。
// 参数说明：
// - 无。
// 返回值：
// - 无。
void Gate::RefreshTimeout() {
  std::lock_guard<std::mutex> lock(mu_);
  if (step_ == Step::kActive) {
    RefreshTimeoutLocked();
  }
}

// 在调用方已经持有 mu_ 时刷新超时截止时间。
// 参数说明：
// - 无。
// 返回值：
// - 无。
void Gate::RefreshTimeoutLocked() {
  // timeout_deadline_ 总是按当前时间重新计算，避免旧 deadline 继续生效。
  timeout_deadline_ = std::chrono::steady_clock::now() + timeout_;
  ++timeout_epoch_;
  timeout_cv_.notify_all();
}

// 更新 AI 播报状态，用于暂停或恢复激活态超时计时。
// 参数说明：
// - is_speaking: true 表示 AI 正在播报。
// 返回值：
// - 无。
void Gate::SetAiSpeaking(bool is_speaking) {
  std::lock_guard<std::mutex> lock(mu_);
  if (is_ai_speaking_ == is_speaking) {
    return;
  }
  is_ai_speaking_ = is_speaking;
  if (step_ != Step::kActive) {
    return;
  }

  if (is_ai_speaking_) {
    // AI 正在说话时暂停超时，避免回复过长导致会话被误关闭。
    ++timeout_epoch_;
    timeout_cv_.notify_all();
  } else {
    // AI 播报结束后从当前时间重新开始倒计时。
    RefreshTimeoutLocked();
  }
}

// 关闭门控并等待超时线程退出。
// 参数说明：
// - 无。
// 返回值：
// - 无。
void Gate::Close() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      return;
    }
    closed_ = true;
    ++timeout_epoch_;
  }
  timeout_cv_.notify_all();
  if (timeout_thread_.joinable()) {
    timeout_thread_.join();
  }
}

// 超时线程主循环，负责在激活态空闲过久时自动退回空闲。
// 参数说明：
// - 无。
// 返回值：
// - 无。
void Gate::TimeoutLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!closed_) {
    if (step_ != Step::kActive) {
      timeout_cv_.wait(lock, [this]() { return closed_ || step_ == Step::kActive; });
      continue;
    }

    if (is_ai_speaking_) {
      // epoch 是当前等待周期的版本号，用于识别状态被刷新。
      const auto epoch = timeout_epoch_;
      timeout_cv_.wait(lock, [this, epoch]() {
        return closed_ || step_ != Step::kActive || timeout_epoch_ != epoch || !is_ai_speaking_;
      });
      continue;
    }

    // epoch/deadline 是本轮超时等待的状态快照。
    const auto epoch = timeout_epoch_;
    const auto deadline = timeout_deadline_;
    if (timeout_cv_.wait_until(lock, deadline, [this, epoch]() {
          return closed_ || step_ != Step::kActive || timeout_epoch_ != epoch || is_ai_speaking_;
        })) {
      continue;
    }

    step_ = Step::kIdle;
    ++timeout_epoch_;
    // after 需要在锁外调用，避免上层 Stop/Disarm 回调造成死锁。
    auto after = hooks_.after_disarm;
    lock.unlock();
    kLog->info("gate timeout -> idle");
    if (after) {
      after("timeout");
    }
    lock.lock();
  }
}

}  // namespace xiaoai_server::wakeup
