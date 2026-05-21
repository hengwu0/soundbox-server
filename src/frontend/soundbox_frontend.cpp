#include "frontend/soundbox_frontend.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace soundbox_server::frontend {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);
constexpr auto kAcceptPollTimeout = std::chrono::milliseconds(200);

bool ContainsType(const std::string& line, const char* type) {
  return line.find("\"type\"") != std::string::npos &&
         line.find(type) != std::string::npos;
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
    std::cout << "[frontend] soundbox connection closed: " << reason << '\n';
    SetState(State::kFault, "soundbox_connection_closed");
    Stop();
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
  kws_socket_.reset();
  aec_socket_.reset();

  if (!was_stopped) {
    std::cout << "[frontend] stop requested\n";
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
    std::cout << "[frontend] drop KWS audio while state=" << StateName(state())
              << '\n';
    return;
  }
  audio_processing_module::WriteAll(kws_socket_.get(), chunk.data(), chunk.size());
}

void Frontend::OnAecAudio(const std::vector<uint8_t>& chunk) {
  if (state() != State::kAec) {
    std::cout << "[frontend] drop AEC audio while state=" << StateName(state())
              << '\n';
    return;
  }
  audio_processing_module::WriteAll(aec_socket_.get(), chunk.data(), chunk.size());
}

void Frontend::KwsControlReaderLoop() {
  try {
    while (!IsStopping()) {
      const std::string line = ReadJsonLine(kws_socket_.get());
      if (line.empty()) {
        break;
      }
      if (ContainsType(line, "session_start")) {
        HandleSessionStart();
      }
    }
  } catch (const std::exception& error) {
    if (!IsStopping()) {
      std::cout << "[frontend] KWS control reader failed: " << error.what()
                << '\n';
      SetState(State::kFault, "kws_control_reader_failed");
      Stop();
    }
  }
}

void Frontend::AecControlReaderLoop() {
  try {
    while (!IsStopping()) {
      const std::string line = ReadJsonLine(aec_socket_.get());
      if (line.empty()) {
        break;
      }
      if (ContainsType(line, "session_end")) {
        HandleSessionEnd();
      }
    }
  } catch (const std::exception& error) {
    if (!IsStopping()) {
      std::cout << "[frontend] AEC control reader failed: " << error.what()
                << '\n';
      SetState(State::kFault, "aec_control_reader_failed");
      Stop();
    }
  }
}

void Frontend::PlaybackAcceptLoop() {
  audio_processing_module::FileDescriptor server =
      audio_processing_module::CreateUnixServerSocket(options_.playback_socket_path, 1);
  audio_processing_module::SocketPathGuard guard(options_.playback_socket_path);
  std::cout << "[frontend] playback listen ready: "
            << options_.playback_socket_path << '\n';

  while (!IsStopping()) {
    try {
      // 单客户端 listen：每次只接一个 playback writer，断开后重新 accept。
      audio_processing_module::FileDescriptor client =
          audio_processing_module::AcceptUnixClientWithTimeout(server.get(),
                                                               kAcceptPollTimeout);
      PlaybackClientLoop(client.get());
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
    const ssize_t bytes_read = ::read(client_fd, buffer.data(), buffer.size());
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
      std::cout << "[frontend] PlayPcm failed for playback chunk bytes="
                << bytes_read << '\n';
    }
  }
}

void Frontend::HandleSessionStart() {
  if (state() != State::kKws) {
    std::cout << "[frontend] ignore duplicate session_start while state="
              << StateName(state()) << '\n';
    return;
  }
  SetState(State::kLlmStarting, "session_start");
  if (client_ && client_->NotifyLlmStart()) {
    SetState(State::kAec, "llm_start_ok");
    return;
  }
  SetState(State::kKws, "llm_start_failed");
}

void Frontend::HandleSessionEnd() {
  if (state() != State::kAec) {
    std::cout << "[frontend] ignore duplicate session_end while state="
              << StateName(state()) << '\n';
    return;
  }
  SetState(State::kLlmStopping, "session_end");
  if (client_ && client_->NotifyLlmStop()) {
    SetState(State::kKws, "llm_stop_ok");
    return;
  }
  SetState(State::kFault, "llm_stop_failed");
  Stop();
}

void Frontend::SetState(State next, const char* reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == next) {
    return;
  }
  std::cout << "[frontend] state " << StateName(state_) << " -> "
            << StateName(next) << " reason=" << reason << '\n';
  state_ = next;
}

bool Frontend::IsStopping() const {
  return stop_requested_.load();
}

}  // namespace soundbox_server::frontend
