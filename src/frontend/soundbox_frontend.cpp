#include "frontend/soundbox_frontend.hpp"

#include "common/log.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

namespace soundbox_server::frontend {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);
constexpr auto kAcceptPollTimeout = std::chrono::milliseconds(200);

const auto kLog = xiaoai_server::GetLogger("frontend");

const char* ControlMessageTypeName(ControlMessageType type) {
  switch (type) {
    case ControlMessageType::kSessionStart:
      return "session_start";
    case ControlMessageType::kSessionEnd:
      return "session_end";
  }
  return "unknown";
}

int64_t GetIntegerField(const nlohmann::json& message, const char* field) {
  const auto it = message.find(field);
  if (it == message.end()) {
    throw std::runtime_error(std::string("control message missing field: ") + field);
  }
  if (!it->is_number_integer() && !it->is_number_unsigned()) {
    throw std::runtime_error(std::string("control message field must be integer: ") +
                             field);
  }
  return it->get<int64_t>();
}

std::optional<int64_t> GetOptionalIntegerField(const nlohmann::json& message,
                                               const char* field) {
  if (!message.contains(field)) {
    return std::nullopt;
  }
  return GetIntegerField(message, field);
}

std::string GetStringField(const nlohmann::json& message, const char* field) {
  const auto it = message.find(field);
  if (it == message.end()) {
    throw std::runtime_error(std::string("control message missing field: ") + field);
  }
  if (!it->is_string()) {
    throw std::runtime_error(std::string("control message field must be string: ") +
                             field);
  }
  return it->get<std::string>();
}

std::optional<double> GetOptionalNumberField(const nlohmann::json& message,
                                             const char* field) {
  const auto it = message.find(field);
  if (it == message.end()) {
    return std::nullopt;
  }
  if (!it->is_number()) {
    throw std::runtime_error(std::string("control message field must be numeric: ") +
                             field);
  }
  return it->get<double>();
}

std::string ReadJsonLine(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    const ssize_t result = ::read(fd, &ch, 1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("read control line: ") +
                               std::strerror(errno));
    }
    if (result == 0) {
      return "";
    }
    if (ch == '\n') {
      return line;
    }
    line.push_back(ch);
  }
}

void ShutdownSocket(int fd) {
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
  }
}

}  // namespace

const char* StateName(Frontend::State state) {
  switch (state) {
    case Frontend::State::kKws:
      return "kws";
    case Frontend::State::kLlmStarting:
      return "LLMStarting";
    case Frontend::State::kAec:
      return "Aec";
    case Frontend::State::kLlmStopping:
      return "LLMStopping";
    case Frontend::State::kFault:
      return "Fault";
  }
  return "Unknown";
}

ControlMessage ParseControlMessageLine(const std::string& line,
                                       ControlMessageType expected_type) {
  const auto message = nlohmann::json::parse(line, nullptr, false);
  if (message.is_discarded() || !message.is_object()) {
    throw std::runtime_error("control message line is not a JSON object");
  }

  const std::string type = GetStringField(message, "type");
  const std::string expected_type_name = ControlMessageTypeName(expected_type);
  if (type != expected_type_name) {
    throw std::runtime_error("unexpected control message type: " + type +
                             ", expected: " + expected_type_name);
  }

  ControlMessage parsed;
  parsed.type = expected_type;
  parsed.timestamp_ms = GetOptionalIntegerField(message, "timestamp_ms");

  if (expected_type == ControlMessageType::kSessionStart) {
    parsed.reason = GetStringField(message, "reason");
    if (parsed.reason != "kws_hit") {
      throw std::runtime_error("session_start reason must be kws_hit");
    }
    parsed.score = GetOptionalNumberField(message, "score");
    return parsed;
  }

  if (message.contains("reason")) {
    parsed.reason = GetStringField(message, "reason");
  }
  if (message.contains("score")) {
    parsed.score = GetOptionalNumberField(message, "score");
  }
  return parsed;
}

Frontend::Frontend(Options options) : options_(std::move(options)) {
  if (options_.kws_socket_path.empty() || options_.aec_socket_path.empty() ||
      options_.playback_socket_path.empty()) {
    throw std::runtime_error("frontend socket paths must not be empty");
  }
}

Frontend::~Frontend() {
  Stop();
}

Frontend::State Frontend::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

void Frontend::Run() {
  stop_requested_.store(false);
  SetState(State::kKws, "startup");

  kws_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
      options_.kws_socket_path, kSocketConnectTimeout, kSocketRetryInterval);
  aec_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
      options_.aec_socket_path, kSocketConnectTimeout, kSocketRetryInterval);

  xiaoai_server::soundbox::SoundBoxClient::Callbacks callbacks;
  callbacks.on_wakeup_audio = [this](const std::vector<uint8_t>& chunk) {
    OnWakeupAudio(chunk);
  };
  callbacks.on_audio = [this](const std::vector<uint8_t>& chunk) {
    OnAecAudio(chunk);
  };
  callbacks.on_connection_closed = [this](const std::string& reason) {
    kLog->warn("soundbox connection closed reason={}", reason);
    EnterFaultAndStop("soundbox_connection_closed");
  };
  client_ = std::make_unique<xiaoai_server::soundbox::SoundBoxClient>(
      options_.soundbox_config, std::move(callbacks));

  kws_control_thread_ = std::thread([this] { KwsControlReaderLoop(); });
  aec_control_thread_ = std::thread([this] { AecControlReaderLoop(); });
  playback_thread_ = std::thread([this] { PlaybackAcceptLoop(); });

  if (!client_->Start()) {
    SetState(State::kFault, "soundbox_start_failed");
    Stop();
    throw std::runtime_error("failed to start soundbox frontend");
  }

  std::unique_lock<std::mutex> lock(mu_);
  stop_cv_.wait(lock, [this] { return stop_requested_.load(); });
}

void Frontend::Stop() {
  const bool was_stopped = stop_requested_.exchange(true);
  stop_cv_.notify_all();
  if (client_) {
    client_->Stop();
  }
  ShutdownSocket(kws_socket_.get());
  ShutdownSocket(aec_socket_.get());
  kws_socket_.reset();
  aec_socket_.reset();

  if (!was_stopped) {
    kLog->info("stop requested");
  }
  const std::thread::id current_thread = std::this_thread::get_id();
  if (kws_control_thread_.joinable() &&
      kws_control_thread_.get_id() != current_thread) {
    kws_control_thread_.join();
  }
  if (aec_control_thread_.joinable() &&
      aec_control_thread_.get_id() != current_thread) {
    aec_control_thread_.join();
  }
  if (playback_thread_.joinable() && playback_thread_.get_id() != current_thread) {
    playback_thread_.join();
  }
}

void Frontend::OnWakeupAudio(const std::vector<uint8_t>& chunk) {
  if (state() != State::kKws) {
    kLog->debug("drop KWS audio while state={}", StateName(state()));
    return;
  }
  audio_processing_module::WriteAll(kws_socket_.get(), chunk.data(), chunk.size());
}

void Frontend::OnAecAudio(const std::vector<uint8_t>& chunk) {
  if (state() != State::kAec) {
    kLog->debug("drop AEC audio while state={}", StateName(state()));
    return;
  }
  audio_processing_module::WriteAll(aec_socket_.get(), chunk.data(), chunk.size());
}

void Frontend::KwsControlReaderLoop() {
  try {
    while (!IsStopping()) {
      const std::string line = ReadJsonLine(kws_socket_.get());
      if (line.empty()) {
        throw std::runtime_error("KWS control socket disconnected");
      }
      const ControlMessage message =
          ParseControlMessageLine(line, ControlMessageType::kSessionStart);
      HandleSessionStart(message);
    }
  } catch (const std::exception& error) {
    if (!IsStopping()) {
      kLog->error("KWS control reader failed: {}", error.what());
      SetState(State::kFault, "kws_control_reader_failed");
      EnterFaultAndStop("kws_control_reader_failed");
    }
  }
}

void Frontend::AecControlReaderLoop() {
  try {
    while (!IsStopping()) {
      const std::string line = ReadJsonLine(aec_socket_.get());
      if (line.empty()) {
        throw std::runtime_error("AEC control socket disconnected");
      }
      const ControlMessage message =
          ParseControlMessageLine(line, ControlMessageType::kSessionEnd);
      HandleSessionEnd(message);
    }
  } catch (const std::exception& error) {
    if (!IsStopping()) {
      kLog->error("AEC control reader failed: {}", error.what());
      SetState(State::kFault, "aec_control_reader_failed");
      EnterFaultAndStop("aec_control_reader_failed");
    }
  }
}

void Frontend::PlaybackAcceptLoop() {
  audio_processing_module::FileDescriptor server =
      audio_processing_module::CreateUnixServerSocket(options_.playback_socket_path, 1);
  audio_processing_module::SocketPathGuard guard(options_.playback_socket_path);
  kLog->info("playback listen ready socket={}", options_.playback_socket_path);

  while (!IsStopping()) {
    try {
      // 单客户端 listen：每次只接一个 playback writer，断开后重新 accept。
      audio_processing_module::FileDescriptor client =
          audio_processing_module::AcceptUnixClientWithTimeout(server.get(),
                                                               kAcceptPollTimeout);
      try {
        PlaybackClientLoop(client.get());
      } catch (const std::runtime_error& error) {
        if (IsStopping()) {
          break;
        }
        kLog->warn("playback client failed: {}; waiting for next client",
                   error.what());
      }
    } catch (const std::runtime_error& error) {
      if (IsStopping()) {
        break;
      }
      if (std::string(error.what()).find("timed out waiting") !=
          std::string::npos) {
        continue;
      }
      throw;
    }
  }
}

void Frontend::PlaybackClientLoop(int client_fd) {
  std::vector<uint8_t> buffer(options_.playback_read_chunk_bytes);
  while (!IsStopping()) {
    const ssize_t bytes_read = options_.playback_read
                                   ? options_.playback_read(client_fd, buffer.data(),
                                                            buffer.size())
                                   : ::read(client_fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("read playback pcm: ") +
                               std::strerror(errno));
    }
    if (bytes_read == 0) {
      break;
    }
    std::vector<uint8_t> chunk(buffer.begin(), buffer.begin() + bytes_read);
    if (client_ && !client_->PlayPcm(chunk)) {
      kLog->warn("PlayPcm failed playback_chunk_bytes={}", bytes_read);
    }
  }
}

void Frontend::HandleSessionStart(const ControlMessage& message) {
  if (state() != State::kKws) {
    kLog->warn("ignore duplicate session_start while state={}", StateName(state()));
    return;
  }
  SetState(State::kLlmStarting,
           message.reason.empty() ? "session_start" : message.reason.c_str());
  if (client_ && client_->NotifyLlmStart()) {
    SetState(State::kAec, "llm_start_ok");
    return;
  }
  SetState(State::kKws, "llm_start_failed");
}

void Frontend::HandleSessionEnd(const ControlMessage& message) {
  if (state() != State::kAec) {
    kLog->warn("ignore duplicate session_end while state={}", StateName(state()));
    return;
  }
  SetState(State::kLlmStopping,
           message.reason.empty() ? "session_end" : message.reason.c_str());
  if (client_ && client_->NotifyLlmStop()) {
    SetState(State::kKws, "llm_stop_ok");
    return;
  }
  SetState(State::kFault, "llm_stop_failed");
  EnterFaultAndStop("llm_stop_failed");
}

void Frontend::EnterFaultAndStop(const char* reason) {
  SetState(State::kFault, reason);
  const bool was_stopped = stop_requested_.exchange(true);
  stop_cv_.notify_all();
  ShutdownSocket(kws_socket_.get());
  ShutdownSocket(aec_socket_.get());
  kws_socket_.reset();
  aec_socket_.reset();
  if (client_) {
    client_->Stop();
  }
  if (!was_stopped) {
    kLog->info("stop requested");
  }
}

void Frontend::SetState(State next, const char* reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == next) {
    return;
  }
  kLog->info("state change from={} to={} reason={}",
             StateName(state_), StateName(next), reason);
  state_ = next;
}

bool Frontend::IsStopping() const {
  return stop_requested_.load();
}

}  // namespace soundbox_server::frontend
