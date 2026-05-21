#include "frontend/soundbox/client.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include "common/log.hpp"
#include "frontend/soundbox/client_internal.hpp"

namespace xiaoai_server::soundbox {

namespace {

// kLog 是 soundbox 连接层日志器。
const auto kLog = xiaoai_server::GetLogger("soundbox");

// 生成远端播放链路需要的默认音频参数。
nlohmann::json PlaybackConfigJson(const config::Config& cfg) {
  const int buffer_frames =
      std::max(240, cfg.xiaozhi.downlink_sample_rate * cfg.xiaozhi.opus_frame_duration_ms / 1000);
  const int period_frames = std::max(120, buffer_frames / 4);
  return nlohmann::json{
      {"pcm", "noop"},
      {"channels", 1},
      {"bits_per_sample", 16},
      {"sample_rate", cfg.xiaozhi.downlink_sample_rate},
      {"period_size", period_frames},
      {"buffer_size", buffer_frames},
  };
}

// 从 WebSocket URL 中提取 token 查询参数，并生成真正的连接地址。
std::pair<std::string, std::string> ExtractUrlAndToken(const std::string& raw_url,
                                                       const std::string& configured_token) {
  const auto query_pos = raw_url.find('?');
  if (query_pos == std::string::npos) {
    return {raw_url, configured_token};
  }

  std::string base = raw_url.substr(0, query_pos);
  std::string query = raw_url.substr(query_pos + 1);
  std::string token = configured_token;
  std::string rebuilt_query;

  size_t pos = 0;
  while (pos <= query.size()) {
    size_t next = query.find('&', pos);
    if (next == std::string::npos) {
      next = query.size();
    }
    const std::string item = query.substr(pos, next - pos);
    if (!item.empty()) {
      const auto eq = item.find('=');
      const std::string key = item.substr(0, eq);
      const std::string value = (eq == std::string::npos) ? std::string() : item.substr(eq + 1);
      if (key == "token") {
        if (token.empty()) {
          token = value;
        }
      } else {
        if (!rebuilt_query.empty()) {
          rebuilt_query.push_back('&');
        }
        rebuilt_query += item;
      }
    }
    if (next == query.size()) {
      break;
    }
    pos = next + 1;
  }

  if (!rebuilt_query.empty()) {
    base += "?" + rebuilt_query;
  }
  return {base, token};
}

// 将 CommandGuardAction 转成日志文本。
const char* GuardActionName(CommandGuardAction action) {
  switch (action) {
    case CommandGuardAction::Send:
      return "send";
    case CommandGuardAction::IdempotentSuccess:
      return "idempotent_success";
    case CommandGuardAction::Ignore:
      return "ignore";
    case CommandGuardAction::Reject:
      return "reject";
  }
  return "unknown";
}

}  // namespace

// 构造 soundbox 客户端。
//
// 参数说明：
// - cfg: 全量配置。
// - callbacks: App 注册的 wakeup/audio/断线回调。
// 返回值：
// - 无。
SoundBoxClient::SoundBoxClient(config::Config cfg, Callbacks callbacks)
    : cfg_(std::move(cfg)),
      callbacks_(std::move(callbacks)),
      audio_pipe_(kAudioPipeMaxPackets, kAudioPipeMaxBytes),
      unknown_logger_(kLog, std::chrono::milliseconds(3000), true) {
  AudioRouter::Callbacks router_callbacks;
  router_callbacks.on_wakeup_audio = [this](const std::vector<uint8_t>& chunk) {
    if (callbacks_.on_wakeup_audio) {
      callbacks_.on_wakeup_audio(chunk);
    }
  };
  router_callbacks.on_audio = [this](const std::vector<uint8_t>& chunk) {
    if (callbacks_.on_audio) {
      callbacks_.on_audio(chunk);
    }
  };
  audio_router_ =
      std::make_unique<AudioRouter>(audio_pipe_, mode_controller_, std::move(router_callbacks));
}

// 析构客户端并停止连接。
SoundBoxClient::~SoundBoxClient() { Stop(); }

// 启动连接和远端音频链路。
bool SoundBoxClient::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return true;
  }

  response_hub_.Reset();
  command_guard_.ResetAudioState();
  mode_controller_.ForceEnter(AudioMode::Stopped, "start");
  audio_pipe_.Start();
  if (audio_router_) {
    audio_router_->Start();
  }

  if (!EnsureConnection(std::chrono::milliseconds(cfg_.soundbox.connect_timeout_ms))) {
    Stop();
    return false;
  }
  if (!StartRemoteAudio(false)) {
    Stop();
    return false;
  }
  return true;
}

// 停止连接和远端音频链路。
void SoundBoxClient::Stop() {
  if (!running_.load()) {
    return;
  }

  (void)StopRemoteAudio(true);
  running_.store(false);
  response_hub_.Close();
  conn_cv_.notify_all();

  if (audio_router_) {
    audio_router_->Stop();
  }
  audio_pipe_.Stop();
  CloseConnection();
  command_guard_.ResetAudioState();
  mode_controller_.ForceEnter(AudioMode::Stopped, "client_stop");
}

// 推送播放 PCM。
bool SoundBoxClient::PlayPcm(const std::vector<uint8_t>& chunk) {
  if (chunk.empty()) {
    return true;
  }
  nlohmann::json stream = {
      {"id", NextId()},
      {"tag", "play"},
      {"bytes", chunk},
  };
  const auto dumped = stream.dump();
  return SendBinary(std::vector<uint8_t>(dumped.begin(), dumped.end()));
}

// 重置播放链路。
bool SoundBoxClient::ResetPlayback() {
  nlohmann::json ignored;
  const auto stop_decision = PrepareCommand("stop_play", false);
  if (stop_decision.action == CommandGuardAction::Send) {
    if (Call("stop_play", nullptr, std::chrono::milliseconds(kAudioCommandTimeoutMs), &ignored)) {
      MarkCommandSucceeded("stop_play");
    }
  }

  const auto start_decision = PrepareCommand("start_play", false);
  if (start_decision.action == CommandGuardAction::IdempotentSuccess) {
    return true;
  }
  if (start_decision.action != CommandGuardAction::Send) {
    kLog->warn("soundbox start_play rejected by command guard: action={} reason={}",
               GuardActionName(start_decision.action), start_decision.reason);
    return false;
  }
  if (!Call("start_play", PlaybackConfigJson(cfg_),
            std::chrono::milliseconds(kAudioCommandTimeoutMs), &ignored)) {
    return false;
  }
  MarkCommandSucceeded("start_play");
  return true;
}

// 执行本地 TTS 播报。
bool SoundBoxClient::SpeakText(const std::string& text) {
  if (text.empty()) {
    return true;
  }
  const std::string script = "(/usr/sbin/tts_play.sh '" + ShellEscapeSingleQuoted(text) +
                             "' >/dev/null 2>&1 &)";
  nlohmann::json ignored;
  return Call("run_shell", script, std::chrono::milliseconds(1500), &ignored);
}

// 请求进入 LLM raw 音频模式。
bool SoundBoxClient::NotifyLlmStart() {
  const auto decision = command_guard_.PrepareLlmStart(mode_controller_);
  if (decision.action == CommandGuardAction::IdempotentSuccess ||
      decision.action == CommandGuardAction::Ignore) {
    kLog->debug("llm_start skipped by guard: action={} reason={}",
                GuardActionName(decision.action), decision.reason);
    return true;
  }
  if (decision.action != CommandGuardAction::Send) {
    kLog->warn("llm_start rejected by guard: action={} reason={}", GuardActionName(decision.action),
               decision.reason);
    return false;
  }

  nlohmann::json response;
  const bool ok = Call("llm_start", nullptr, std::chrono::milliseconds(kLlmStartTimeoutMs),
                       &response);
  if (ok && response.value("msg", std::string()) == "llm_start_ok") {
    mode_controller_.ForceEnter(AudioMode::LlmWorking, "llm_start_ok");
    return true;
  }

  mode_controller_.ForceEnter(AudioMode::Fault, ok ? "llm_start_failed" : "llm_start_timeout");
  (void)RestartSoundboxAudioLink();
  return false;
}

// 请求退出 LLM raw 音频模式。
bool SoundBoxClient::NotifyLlmStop() {
  const auto decision = command_guard_.PrepareLlmStop(mode_controller_);
  if (decision.action == CommandGuardAction::IdempotentSuccess ||
      decision.action == CommandGuardAction::Ignore) {
    kLog->debug("llm_stop skipped by guard: action={} reason={}", GuardActionName(decision.action),
                decision.reason);
    return true;
  }
  if (decision.action != CommandGuardAction::Send) {
    kLog->warn("llm_stop rejected by guard: action={} reason={}", GuardActionName(decision.action),
               decision.reason);
    return false;
  }

  nlohmann::json response;
  const bool ok =
      Call("llm_stop", nullptr, std::chrono::milliseconds(kLlmStopTimeoutMs), &response);
  if (ok && response.value("msg", std::string()) == "llm_stop_ok") {
    mode_controller_.ForceEnter(AudioMode::Kws, "llm_stop_ok");
    return true;
  }

  mode_controller_.ForceEnter(AudioMode::Fault, ok ? "llm_stop_failed" : "llm_stop_timeout");
  (void)RestartSoundboxAudioLink();
  return false;
}

// 读取当前音频模式。
AudioMode SoundBoxClient::CurrentMode() const { return mode_controller_.Current(); }

// 确保连接可用。
bool SoundBoxClient::EnsureConnection(std::chrono::milliseconds timeout) {
  if (!running_.load()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    if (ws_ && ws_connected_) {
      return true;
    }
  }
  return OpenConnection(timeout);
}

// 打开 WebSocket 连接。
bool SoundBoxClient::OpenConnection(std::chrono::milliseconds timeout) {
  auto [url, token] = ExtractUrlAndToken(cfg_.soundbox.ws_url, cfg_.soundbox.ws_token);
  if (url.empty()) {
    return false;
  }
  response_hub_.Reset();
  kLog->debug("soundbox connecting: url='{}' token_present={}", url, !token.empty());

  auto ws = std::make_unique<ix::WebSocket>();
  ws->setUrl(url);
  if (!token.empty()) {
    ws->setExtraHeaders({{"Authorization", "Bearer " + token}});
  }

  ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      std::lock_guard<std::mutex> lock(conn_mu_);
      ws_connected_ = true;
      connect_failed_ = false;
      conn_cv_.notify_all();
      kLog->info("soundbox connected");
      return;
    }

    if (msg->type == ix::WebSocketMessageType::Close ||
        msg->type == ix::WebSocketMessageType::Error) {
      bool should_notify = false;
      {
        std::lock_guard<std::mutex> lock(conn_mu_);
        should_notify = ws_connected_;
        ws_connected_ = false;
        connect_failed_ = true;
      }
      conn_cv_.notify_all();
      response_hub_.Close();
      mode_controller_.ForceEnter(AudioMode::Stopped, "connection_closed");
      if (msg->type == ix::WebSocketMessageType::Error) {
        kLog->warn("soundbox ws error: {}", msg->errorInfo.reason);
      } else {
        kLog->warn("soundbox disconnected");
      }
      if (should_notify && running_.load() && callbacks_.on_connection_closed) {
        callbacks_.on_connection_closed("soundbox_connection_lost");
      }
      return;
    }

    if (msg->type == ix::WebSocketMessageType::Message) {
      HandleMessageFrame(msg->binary, msg->str);
    }
  });

  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    ws_connected_ = false;
    connect_failed_ = false;
    ws_ = std::move(ws);
  }
  ws_->start();

  std::unique_lock<std::mutex> lock(conn_mu_);
  if (!conn_cv_.wait_for(lock, timeout, [this]() {
        return ws_connected_ || connect_failed_ || !running_.load();
      })) {
    kLog->error("soundbox connect timeout");
    lock.unlock();
    CloseConnection();
    return false;
  }
  if (!ws_connected_) {
    lock.unlock();
    CloseConnection();
    return false;
  }
  return true;
}

// 关闭 WebSocket。
void SoundBoxClient::CloseConnection() {
  std::unique_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lock(conn_mu_);
    ws_connected_ = false;
    connect_failed_ = false;
    ws = std::move(ws_);
  }
  if (!ws) {
    return;
  }
  try {
    ws->stop();
  } catch (...) {
  }
}

// 启动远端音频链路。
bool SoundBoxClient::StartRemoteAudio(bool recovery) {
  nlohmann::json ignored;
  auto start_play = PrepareCommand("start_play", recovery);
  if (start_play.action == CommandGuardAction::Send) {
    if (!Call("start_play", PlaybackConfigJson(cfg_),
              std::chrono::milliseconds(kAudioCommandTimeoutMs), &ignored, recovery)) {
      kLog->error("soundbox start_play failed");
      return false;
    }
    MarkCommandSucceeded("start_play");
  } else if (start_play.action == CommandGuardAction::Reject) {
    kLog->warn("start_play rejected by guard: reason={}", start_play.reason);
    return false;
  }

  auto fast_recording = PrepareCommand("fast_recording", recovery);
  if (fast_recording.action == CommandGuardAction::Send) {
    if (!Call("fast_recording", nullptr, std::chrono::milliseconds(kAudioCommandTimeoutMs),
              &ignored, recovery)) {
      kLog->error("soundbox fast_recording failed");
      return false;
    }
    MarkCommandSucceeded("fast_recording");
  } else if (fast_recording.action == CommandGuardAction::Reject) {
    kLog->warn("fast_recording rejected by guard: reason={}", fast_recording.reason);
    return false;
  }

  mode_controller_.ForceEnter(AudioMode::Kws,
                              recovery ? "restart_soundbox_audio_success" : "start_recording_ok");
  kLog->info("soundbox remote audio ready");
  return true;
}

// 停止远端音频链路。
bool SoundBoxClient::StopRemoteAudio(bool recovery) {
  nlohmann::json ignored;
  bool ok = true;

  auto stop_recording = PrepareCommand("stop_recording", recovery);
  if (stop_recording.action == CommandGuardAction::Send) {
    if (Call("stop_recording", nullptr, std::chrono::milliseconds(kAudioCommandTimeoutMs),
             &ignored, recovery)) {
      MarkCommandSucceeded("stop_recording");
    } else {
      ok = false;
    }
  } else if (stop_recording.action == CommandGuardAction::Reject) {
    ok = false;
  }

  auto stop_play = PrepareCommand("stop_play", recovery);
  if (stop_play.action == CommandGuardAction::Send) {
    if (Call("stop_play", nullptr, std::chrono::milliseconds(kAudioCommandTimeoutMs), &ignored,
             recovery)) {
      MarkCommandSucceeded("stop_play");
    } else {
      ok = false;
    }
  } else if (stop_play.action == CommandGuardAction::Reject) {
    ok = false;
  }
  return ok;
}

// Fault 恢复：仅重启 soundbox audio 链路，不重启进程。
bool SoundBoxClient::RestartSoundboxAudioLink() {
  audio_pipe_.Clear();
  const bool stopped = StopRemoteAudio(true);
  const bool started = StartRemoteAudio(true);
  if (stopped && started) {
    mode_controller_.ForceEnter(AudioMode::Kws, "restart_soundbox_audio_success");
    return true;
  }
  mode_controller_.ForceEnter(AudioMode::Stopped, "restart_soundbox_audio_failed");
  internal::NotifyRestartSoundboxAudioFailed(
      [this]() { CloseConnection(); },
      [this](const std::string& reason) {
        if (callbacks_.on_connection_closed) {
          callbacks_.on_connection_closed(reason);
        }
      });
  return false;
}

// 发送 RPC 并等待响应。
bool SoundBoxClient::Call(const std::string& command, const nlohmann::json& payload,
                          std::chrono::milliseconds timeout, nlohmann::json* out_response,
                          bool /*recovery*/) {
  if (!EnsureConnection(std::chrono::milliseconds(cfg_.soundbox.connect_timeout_ms))) {
    unknown_logger_.Log(spdlog::level::warn, "send-connection-not-ready",
                        "[soundbox] send request skipped because connection is not ready");
    return false;
  }

  const std::string request_id = NextId();
  nlohmann::json request = {
      {"Request",
       {
           {"id", request_id},
           {"command", command},
       }},
  };
  if (!payload.is_null()) {
    request["Request"]["payload"] = payload;
  }

  const auto mode_before_send = mode_controller_.Current();
  const auto started_at = std::chrono::steady_clock::now();
  kLog->debug("tx soundbox request id={} command={} timeout_ms={} mode={}", request_id, command,
              timeout.count(), AudioModeName(mode_before_send));
  if (!SendText(request.dump())) {
    return false;
  }

  auto response = response_hub_.Wait(request_id, timeout);
  if (!response) {
    kLog->warn("soundbox response timeout request_id={} command={} timeout_ms={} current_mode={}",
               request_id, command, timeout.count(), AudioModeName(mode_controller_.Current()));
    return false;
  }

  const auto cost_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            started_at)
          .count();
  if (out_response) {
    *out_response = *response;
  }
  kLog->debug("rx soundbox response id={} code={} msg={} cost_ms={} mode={}", request_id,
              response->value("code", 0), response->value("msg", std::string()), cost_ms,
              AudioModeName(mode_controller_.Current()));

  const auto code = response->find("code");
  if (code != response->end() && code->is_number_integer() && code->get<int>() != 0) {
    kLog->warn("soundbox rpc '{}' failed: {}", command,
               response->value("msg", std::string("unknown error")));
    return false;
  }
  return true;
}

// 生成命令幂等决策。
CommandGuardDecision SoundBoxClient::PrepareCommand(const std::string& command, bool recovery) {
  if (command == "start_play") {
    return command_guard_.PrepareStartPlay(mode_controller_, recovery);
  }
  if (command == "fast_recording") {
    return command_guard_.PrepareFastRecording(mode_controller_, recovery);
  }
  if (command == "stop_recording") {
    return command_guard_.PrepareStopRecording(mode_controller_, recovery);
  }
  if (command == "stop_play") {
    return command_guard_.PrepareStopPlay(mode_controller_, recovery);
  }
  return CommandGuardDecision{CommandGuardAction::Send, "unguarded_command"};
}

// 标记命令成功。
void SoundBoxClient::MarkCommandSucceeded(const std::string& command) {
  if (command == "start_play") {
    command_guard_.MarkStartPlaySucceeded();
  } else if (command == "fast_recording") {
    command_guard_.MarkFastRecordingSucceeded();
  } else if (command == "stop_recording") {
    command_guard_.MarkStopRecordingSucceeded();
  } else if (command == "stop_play") {
    command_guard_.MarkStopPlaySucceeded();
  }
}

// 发送文本帧。
bool SoundBoxClient::SendText(const std::string& text) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::lock_guard<std::mutex> conn_lock(conn_mu_);
  if (!ws_ || !ws_connected_) {
    unknown_logger_.Log(spdlog::level::warn, "send-text-not-ready",
                        "[soundbox] send text skipped because connection is not ready");
    return false;
  }
  const auto res = ws_->sendText(text);
  kLog->debug("{}", internal::SoundboxJsonDebugLog("[SOUNDBOX][SEND]", text));
  return res.success;
}

// 发送二进制帧。
bool SoundBoxClient::SendBinary(const std::vector<uint8_t>& payload) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::lock_guard<std::mutex> conn_lock(conn_mu_);
  if (!ws_ || !ws_connected_) {
    unknown_logger_.Log(spdlog::level::warn, "send-binary-not-ready",
                        "[soundbox] send binary skipped because connection is not ready",
                        payload.size());
    return false;
  }
  const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
  const auto res = ws_->sendBinary(text);
  kLog->debug("{}", internal::SoundboxAudioDebugLog("[SOUNDBOX][SEND_AUDIO]", text));
  return res.success;
}

// 统一处理 WebSocket message 帧。
void SoundBoxClient::HandleMessageFrame(bool binary, const std::string& payload) {
  const auto packet = packet_parser_.Parse(binary, payload);
  switch (packet.type) {
    case PacketType::Response:
      kLog->debug("{}", internal::SoundboxJsonDebugLog("[SOUNDBOX][RAW]", payload));
      response_hub_.Notify(packet.json);
      break;
    case PacketType::Event:
      kLog->debug("{}", internal::SoundboxJsonDebugLog("[SOUNDBOX][RAW]", payload));
      event_dispatcher_.Handle(packet);
      break;
    case PacketType::RecordAudio: {
      const auto stats = audio_pipe_.Stats();
      const auto mode = mode_controller_.Current();
      kLog->debug("{} mode={} queue={}",
                  internal::SoundboxAudioDebugLog("[SOUNDBOX][AUDIO]",
                                                  packet.audio_bytes.size(), payload.size()),
                  AudioModeName(mode), stats.queued_packets);
      unknown_logger_.Log(spdlog::level::debug, internal::RecordAudioLogKey(mode),
                          packet.raw_summary + " mode=" + AudioModeName(mode) + " queue=" +
                              std::to_string(stats.queued_packets),
                          packet.audio_bytes.size());
      (void)audio_pipe_.Push(std::vector<uint8_t>(packet.audio_bytes.begin(),
                                                  packet.audio_bytes.end()));
      break;
    }
    case PacketType::Unknown:
      if (internal::ExtractSoundboxAudioBytesLength(payload)) {
        kLog->debug("{}", internal::SoundboxAudioDebugLog("[SOUNDBOX][AUDIO]", payload));
      } else {
        kLog->debug("{}", internal::SoundboxJsonDebugLog("[SOUNDBOX][RAW]", payload));
      }
      unknown_logger_.Log(spdlog::level::debug, "unknown-packet", packet.raw_summary);
      break;
  }
}

// 生成请求 ID。
std::string SoundBoxClient::NextId() {
  static std::atomic<uint64_t> seq{1};
  return "soundbox-" + std::to_string(seq.fetch_add(1));
}

// 转义单引号 shell 文本。
std::string SoundBoxClient::ShellEscapeSingleQuoted(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace xiaoai_server::soundbox
