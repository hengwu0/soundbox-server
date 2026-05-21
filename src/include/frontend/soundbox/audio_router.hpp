#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "common/rate_limited_logger.hpp"
#include "frontend/soundbox/audio_pipe.hpp"
#include "frontend/soundbox/mode_controller.hpp"

namespace xiaoai_server::soundbox {

// AudioRouter 从 AudioPipe 取音频，并按 AudioMode 分发到正确链路。
//
// 路由策略：
// - Kws: 转发给 wakeup/KWS 模块。
// - LlmWorking: 转发给 audio/VAD/AEC 模块。
// - LlmStarting/LlmStopping/Stopped/Fault: 直接丢弃。
//
// WebSocket 层不得直接调用 wakeup、VAD、AEC；它只 Push 到 AudioPipe。
class AudioRouter {
 public:
  // Callbacks 是不同音频链路的业务入口。
  struct Callbacks {
    // on_wakeup_audio 接收 KWS 模式单声道音频。
    std::function<void(const std::vector<uint8_t>&)> on_wakeup_audio;
    // on_audio 接收 LLM raw 双通道音频。
    std::function<void(const std::vector<uint8_t>&)> on_audio;
  };

  // 构造音频路由器。
  //
  // 参数说明：
  // - pipe: record 音频队列引用。
  // - mode: 音频模式控制器引用。
  // - callbacks: wakeup/audio 两条链路入口。
  AudioRouter(AudioPipe& pipe, const ModeController& mode, Callbacks callbacks);

  // 析构并停止路由线程。
  ~AudioRouter();

  // 启动路由线程；重复调用不重复启动。
  void Start();

  // 停止路由线程。
  void Stop();

  // 返回路由层累计丢弃包数。
  uint64_t dropped_packets() const;

 private:
  // 路由线程主循环。
  void WorkerLoop();

  // pipe_ 是音频来源队列。
  AudioPipe& pipe_;
  // mode_ 是当前 AudioMode 读取入口。
  const ModeController& mode_;
  // callbacks_ 保存业务音频链路。
  Callbacks callbacks_;
  // running_ 控制 worker 生命周期。
  std::atomic<bool> running_{false};
  // dropped_packets_ 记录因模式不可路由而丢弃的包数。
  std::atomic<uint64_t> dropped_packets_{0};
  // drop_logger_ 聚合过渡态和 Fault 下的高频丢弃日志，避免音频包刷屏。
  xiaoai_server::RateLimitedLogger drop_logger_;
  // worker_ 是异步路由线程。
  std::thread worker_;
};

}  // namespace xiaoai_server::soundbox
