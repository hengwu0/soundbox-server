#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "common/rate_limited_logger.hpp"
#include "config/config.hpp"
#include "frontend/soundbox/audio_pipe.hpp"
#include "frontend/soundbox/audio_router.hpp"
#include "frontend/soundbox/command_guard.hpp"
#include "frontend/soundbox/event_dispatcher.hpp"
#include "frontend/soundbox/mode_controller.hpp"
#include "frontend/soundbox/packet_parser.hpp"
#include "frontend/soundbox/response_hub.hpp"

namespace xiaoai_server::soundbox {

// SoundBoxClient 是与 open-xiaoai-client 通信的连接层。
//
// 职责边界：
// - 只负责 WebSocket 建连、发送包、关闭连接和连接状态回调。
// - 收包后统一进入 PacketParser，再分发到 ResponseHub/EventDispatcher/AudioPipe。
// - 不在 WebSocket 回调里直接执行 KWS、VAD、AEC 或小爱 instruction 业务逻辑。
class SoundBoxClient {
 public:
  // 定义 soundbox 层向 App 暴露的音频和连接事件。
  struct Callbacks {
    // on_wakeup_audio 接收 Kws 模式的单声道 KWS 音频。
    std::function<void(const std::vector<uint8_t>&)> on_wakeup_audio;
    // on_audio 接收 LlmWorking 模式的 raw 双通道音频。
    std::function<void(const std::vector<uint8_t>&)> on_audio;
    // on_connection_closed 在 WebSocket 异常断开时调用。
    std::function<void(const std::string&)> on_connection_closed;
  };

  // 构造 soundbox 客户端。
  explicit SoundBoxClient(config::Config cfg, Callbacks callbacks);

  // 析构并停止连接。
  ~SoundBoxClient();

  // 启动连接和远端播放/录音链路。
  bool Start();

  // 停止远端链路和 WebSocket 连接。
  void Stop();

  // 推送一块待播放 PCM 到 open-xiaoai-client。
  bool PlayPcm(const std::vector<uint8_t>& chunk);

  // 中断当前播放并重新创建播放链路。
  bool ResetPlayback();

  // 让客户端执行本地文本播报命令。
  bool SpeakText(const std::string& text);

  // KWS 命中后请求客户端切到 LLM raw 双通道模式。
  bool NotifyLlmStart();

  // LLM 会话结束后请求客户端切回 KWS 单声道模式。
  bool NotifyLlmStop();

  // 返回当前音频模式，供 App 判断处理路径和日志定位。
  AudioMode CurrentMode() const;

 private:
  // 固定远端播放/录音命令超时；llm_start/llm_stop 使用配置里的独立超时。
  static constexpr int kAudioCommandTimeoutMs = 3000;
  static constexpr size_t kAudioPipeMaxPackets = 100;
  static constexpr size_t kAudioPipeMaxBytes = 10 * 1024 * 1024;

  // 确保底层 WebSocket 已连接。
  bool EnsureConnection(std::chrono::milliseconds timeout);

  // 创建并打开 WebSocket。
  bool OpenConnection(std::chrono::milliseconds timeout);

  // 关闭 WebSocket。
  void CloseConnection();

  // 启动远端播放和录音链路。
  bool StartRemoteAudio(bool recovery = false);

  // 停止远端播放和录音链路。
  bool StopRemoteAudio(bool recovery = false);

  // Fault 后仅重启 soundbox audio 链路，不重启 open-xiaoai-server 进程。
  bool RestartSoundboxAudioLink();

  // 发送 RPC 并等待响应。
  bool Call(const std::string& command, const nlohmann::json& payload,
            std::chrono::milliseconds timeout, nlohmann::json* out_response,
            bool recovery = false);

  // 根据命令名执行幂等检查。
  CommandGuardDecision PrepareCommand(const std::string& command, bool recovery);

  // 标记命令成功，更新幂等层记录。
  void MarkCommandSucceeded(const std::string& command);

  // 发送文本帧。
  bool SendText(const std::string& text);

  // 发送二进制帧。
  bool SendBinary(const std::vector<uint8_t>& payload);

  // 处理一帧 WebSocket message。
  void HandleMessageFrame(bool binary, const std::string& payload);

  // 生成请求 ID。
  static std::string NextId();

  // 转义单引号 shell 字符串。
  static std::string ShellEscapeSingleQuoted(const std::string& text);

  // cfg_ 保存运行配置。
  config::Config cfg_;
  // callbacks_ 保存 App 传入的回调。
  Callbacks callbacks_;
  // packet_parser_ 是收包统一解析入口。
  PacketParser packet_parser_;
  // response_hub_ 管理 request_id 等待和唤醒。
  ResponseHub response_hub_;
  // event_dispatcher_ 处理 playing/instruction 事件日志。
  EventDispatcher event_dispatcher_;
  // mode_controller_ 管理 Kws/LlmStarting/LlmWorking/LlmStopping/Fault/Stopped。
  ModeController mode_controller_;
  // command_guard_ 对 start/stop/llm_start/llm_stop 做幂等保护。
  CommandGuard command_guard_;
  // audio_pipe_ 承接 record 音频队列和背压统计。
  AudioPipe audio_pipe_;
  // audio_router_ 按 AudioMode 将音频路由到 wakeup 或 audio。
  std::unique_ptr<AudioRouter> audio_router_;
  // unknown_logger_ 聚合 Unknown packet 和连接未就绪等高频日志。
  xiaoai_server::RateLimitedLogger unknown_logger_;

  // conn_mu_ 保护连接状态。
  mutable std::mutex conn_mu_;
  // conn_cv_ 等待 open/error 事件。
  std::condition_variable conn_cv_;
  // ws_ 是 ixwebsocket 连接对象。
  std::unique_ptr<ix::WebSocket> ws_;
  // ws_connected_ 表示 WebSocket open。
  bool ws_connected_{false};
  // connect_failed_ 表示最近连接尝试失败。
  bool connect_failed_{false};

  // write_mu_ 串行化 WebSocket 写。
  mutable std::mutex write_mu_;
  // running_ 表示客户端已启动。
  std::atomic<bool> running_{false};
};

}  // namespace xiaoai_server::soundbox
