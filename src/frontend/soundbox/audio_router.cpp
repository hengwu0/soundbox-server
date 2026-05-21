#include "frontend/soundbox/audio_router.hpp"

#include <utility>

#include "common/log.hpp"

namespace xiaoai_server::soundbox {

namespace {

// kLog 是 soundbox 音频路由日志器。
const auto kLog = xiaoai_server::GetLogger("soundbox");

}  // namespace

// 构造 AudioRouter。
//
// 参数说明：
// - pipe: record 音频队列。
// - mode: 音频状态机。
// - callbacks: 业务链路入口。
// 返回值：
// - 无。
AudioRouter::AudioRouter(AudioPipe& pipe, const ModeController& mode, Callbacks callbacks)
    : pipe_(pipe),
      mode_(mode),
      callbacks_(std::move(callbacks)),
      drop_logger_(kLog, std::chrono::milliseconds(3000), true) {}

// 析构 AudioRouter。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
AudioRouter::~AudioRouter() { Stop(); }

// 启动路由线程。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void AudioRouter::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  worker_ = std::thread([this]() { WorkerLoop(); });
}

// 停止路由线程。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void AudioRouter::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  pipe_.Stop();
  if (worker_.joinable()) {
    worker_.join();
  }
}

// 返回路由丢弃包数。
//
// 参数说明：
// - 无。
// 返回值：
// - 返回 dropped_packets_。
uint64_t AudioRouter::dropped_packets() const { return dropped_packets_.load(); }

// 路由线程主循环。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void AudioRouter::WorkerLoop() {
  while (running_.load()) {
    auto chunk = pipe_.Pop();
    if (!chunk) {
      return;
    }

    const auto mode = mode_.Current();
    switch (mode) {
      case AudioMode::Kws:
        if (callbacks_.on_wakeup_audio) {
          callbacks_.on_wakeup_audio(*chunk);
        }
        break;
      case AudioMode::LlmWorking:
        if (callbacks_.on_audio) {
          callbacks_.on_audio(*chunk);
        }
        break;
      case AudioMode::Stopped:
      case AudioMode::LlmStarting:
      case AudioMode::LlmStopping:
      case AudioMode::Fault:
        ++dropped_packets_;
        drop_logger_.Log(
            spdlog::level::debug,
            std::string("drop-record-") + AudioModeName(mode),
            std::string("[soundbox] drop record audio while ") + AudioModeName(mode),
            chunk->size());
        break;
    }
  }
}

}  // namespace xiaoai_server::soundbox
