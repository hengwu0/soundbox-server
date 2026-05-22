#pragma once

#include "apm/aec/webrtc_processor.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace audio_processing_module::apm::aec {

/// AEC 流处理器：通过 Unix Socket 接收前端的立体声 PCM，经 WebRTC 处理后将单声道结果送给下游。
class AecStreamProcessor {
 public:
  /// 音频下沉回调：处理后的 PCM 通过此回调发送给下游（如 LLM）。
  using AudioSink = std::function<void(const std::vector<uint8_t>& processed_pcm)>;

  /// 构造 AEC 流处理器。
  /// @param frontend_listen_socket_path 监听前端连接的 Unix Socket 路径。
  /// @param audio_sink                  处理后的音频下沉回调。
  /// @param options                     WebRTC 处理器参数（延迟、NS 级别、AGC 模式等）。
  AecStreamProcessor(std::string frontend_listen_socket_path,
                      AudioSink audio_sink,
                      audio_processing_module::WebRtcProcessorOptions options);

  /// 获取前端监听 socket 路径。
  const std::string& frontend_listen_socket_path() const {
    return frontend_listen_socket_path_;
  }

  /// 进入主循环：持续 accept 前端连接并处理。
  void Run();
  /// 接受单个客户端连接并处理（调试/单测用）。
  void RunOneClient();
  /// 请求停止主循环。
  void Stop();

 private:
  /// 从已连接的客户端 fd 读取 PCM 数据并送入 WebRTC 处理。
  void HandleClient(int frontend_fd);

  std::string frontend_listen_socket_path_;    ///< 前端监听 socket 路径。
  AudioSink audio_sink_;                       ///< 处理后的音频输出回调。
  audio_processing_module::WebRtcProcessorOptions options_;  ///< WebRTC 处理器参数。
  std::atomic<bool> stop_requested_{false};     ///< 原子停止标记，Run() 循环中轮询。
};

}  // namespace audio_processing_module::apm::aec