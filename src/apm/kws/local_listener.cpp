#include "apm/kws/local_listener.hpp"

#include "common/log.hpp"

namespace xiaoai_server::wakeup {

namespace {
// kLog 是本地 KWS 监听器专用日志器。
const auto kLog = xiaoai_server::GetLogger("kws");
}  // namespace

// 构造本地 KWS 监听器。
// 参数说明：
// - cfg: 门控、触发器、KWS 引擎和音频格式配置。
// 返回值：
// - 无。
LocalListener::LocalListener(Config cfg) : cfg_(std::move(cfg)) {}

// 接收一块 PCM 并尝试检测唤醒词。
// 参数说明：
// - chunk: 单声道 S16 PCM 字节流。
// 返回值：
// - 无。
void LocalListener::AcceptPcm(const std::vector<uint8_t>& chunk) {
  if (chunk.empty() || !cfg_.gate || !cfg_.trigger || !cfg_.kws_engine) {
    return;
  }

  if (cfg_.gate->step() != Step::kIdle) {
    return;
  }

  // hit 保存当前 PCM 块的关键词检测结果。
  auto hit = cfg_.kws_engine->AcceptPcm16(chunk.data(), chunk.size(), cfg_.sample_rate,
                                          cfg_.channels, cfg_.bit_depth);
  if (!hit.has_value()) {
    return;
  }
  kLog->info("kws hit: keyword='{}'", hit->keyword);

  // now 是本次命中的时间点，用于做最小触发间隔判断。
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      return;
    }
    if (last_trigger_.time_since_epoch().count() > 0) {
      // delta 是距离上一次成功触发的毫秒数。
      auto delta =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_trigger_).count();
      if (delta < cfg_.min_trigger_interval_ms) {
        kLog->debug("kws skip by interval: delta={}ms min={}ms", delta,
                    cfg_.min_trigger_interval_ms);
        return;
      }
    }
  }

  if (cfg_.trigger->FireFromText(hit->keyword)) {
    kLog->info("kws trigger accepted: keyword='{}'", hit->keyword);
    std::lock_guard<std::mutex> lock(mu_);
    last_trigger_ = now;
  } else {
    kLog->debug("kws trigger rejected: keyword='{}'", hit->keyword);
  }
}

// 关闭监听器并重置底层 KWS 流状态。
// 参数说明：
// - 无。
// 返回值：
// - 无。
void LocalListener::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  closed_ = true;
  if (cfg_.kws_engine) {
    cfg_.kws_engine->Reset();
  }
}

}  // namespace xiaoai_server::wakeup
