#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xiaoai_server::wakeup {

// 描述唤醒门控当前所处的状态。
enum class Step {
  kIdle,
  kActive,
};

// 描述门控状态切换时需要通知上层的回调。
struct Hooks {
  // after_disarm 在门控退回空闲后调用，用于通知 App 停止上行等清理动作。
  std::function<void(const std::string&)> after_disarm;
  // on_arm 在门控进入激活态时调用，用于通知 App 建立 xiaozhi 会话。
  std::function<void(const std::string&)> on_arm;
};

// 唤醒门控，用于管理一次语音会话的激活/超时/打断状态。
class Gate {
 public:
  // 构造门控对象并启动超时线程。
  // 参数说明：
  // - timeout: 激活态下的超时时长。
  // - hooks: 状态切换回调集合。
  // 返回值：
  // - 无。
  Gate(std::chrono::milliseconds timeout, Hooks hooks);

  // 析构门控对象并关闭超时线程。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  ~Gate();

  // 查询当前门控状态。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 返回当前 Step 状态。
  Step step() const;

  // 尝试由唤醒词触发一次激活。
  // 参数说明：
  // - keyword: 本次命中的唤醒词文本。
  // 返回值：
  // - 成功激活返回 true，否则返回 false。
  bool TryWakeup(const std::string& keyword);

  // 强制把门控置为激活态。
  // 参数说明：
  // - reason: 激活原因描述。
  // 返回值：
  // - 无。
  void Activate(const std::string& reason);

  // 把门控切回空闲态。
  // 参数说明：
  // - reason: 退回空闲的原因描述。
  // 返回值：
  // - 无。
  void Disarm(const std::string& reason);

  // 刷新当前激活态的超时截止时间。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void RefreshTimeout();

  // 更新 AI 是否正在说话的状态，以便暂停或恢复超时。
  // 参数说明：
  // - is_speaking: 最新的 AI 播报状态。
  // 返回值：
  // - 无。
  void SetAiSpeaking(bool is_speaking);

  // 关闭门控并停止后台线程。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Close();

 private:
  // 在持锁状态下刷新超时截止时间。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void RefreshTimeoutLocked();

  // 后台线程函数，负责监控激活态超时。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void TimeoutLoop();

  // timeout_ 是激活态无新语音时的会话保活时长。
  std::chrono::milliseconds timeout_;
  // hooks_ 保存状态切换时回调给 App 的函数集合。
  Hooks hooks_;

  // mu_ 保护 step_、deadline、AI 播报状态和关闭标志。
  mutable std::mutex mu_;
  // timeout_cv_ 用于唤醒后台超时线程重新计算等待时间。
  std::condition_variable timeout_cv_;
  // step_ 表示门控当前处于空闲还是激活态。
  Step step_{Step::kIdle};
  // is_ai_speaking_ 为 true 时暂停超时倒计时，避免 AI 播报中自动断会话。
  bool is_ai_speaking_{false};
  // timeout_deadline_ 是下一次激活态超时的绝对时间点。
  std::chrono::steady_clock::time_point timeout_deadline_{};
  // closed_ 表示门控已关闭，后台线程应退出。
  bool closed_{false};
  // timeout_epoch_ 用于区分旧 deadline 和刷新后的新 deadline。
  uint64_t timeout_epoch_{0};
  // timeout_thread_ 是负责执行超时判断的后台线程。
  std::thread timeout_thread_;
};

}  // namespace xiaoai_server::wakeup
