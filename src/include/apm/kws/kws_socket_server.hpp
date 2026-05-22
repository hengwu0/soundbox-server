#pragma once

#include "apm/kws/kws_engine.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace audio_processing_module::apm::kws {

/// KWS Socket 服务端：通过 Unix Domain Socket 接收前端 PCM 流，交给 KWS 引擎做关键词检测。
/// 命中后通过 socket 回写 session_start 事件给前端。
class KwsSocketServer {
 public:
  /// KWS Socket 服务端配置选项。
  struct Options {
    std::string listen_socket_path;  ///< 监听的前端 socket 路径。
    int sample_rate{16000};          ///< 期望的 PCM 采样率。
    int channels{1};                 ///< 期望的 PCM 通道数。
    int bits_per_sample{16};         ///< 期望的 PCM 位深。
    size_t read_chunk_bytes{320};    ///< 每帧从 socket 读取的字节数（10ms @ 16kHz mono）。
  };

  /// 构造 KWS Socket 服务端。
  /// @param options 监听和音频格式配置。
  /// @param engine  KWS 引擎实例，用于对目标 PCM 做关键词检测。
  KwsSocketServer(Options options,
                  std::shared_ptr<xiaoai_server::wakeup::IKwsEngine> engine);

  /// 获取监听 socket 路径。
  const std::string& listen_socket_path() const {
    return options_.listen_socket_path;
  }

  /// 进入主循环：持续 accept 前端连接并处理 KWS 检测。
  void Run();
  /// 接受单个客户端连接并处理（调试/单测用）。
  void RunOneClient();
  /// 请求停止主循环。
  void Stop();

 private:
  /// 从已连接的客户端 fd 读取 PCM 并送入 KWS 引擎检测。
  void HandleClient(int client_fd);

  Options options_;                                      ///< Socket 服务端配置。
  std::shared_ptr<xiaoai_server::wakeup::IKwsEngine> engine_;  ///< KWS 检测引擎。
  std::atomic<bool> stop_requested_{false};              ///< 原子停止标记。
};

}  // namespace audio_processing_module::apm::kws