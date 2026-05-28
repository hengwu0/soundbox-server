#include "llm/llm_client.hpp"

#include "common/log.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace soundbox_server::llm {
namespace {

/** @brief 连接重试间隔，100 毫秒 */
constexpr auto kConnectRetryInterval = std::chrono::milliseconds(100);

/** @brief 连接总体超时时间，10 秒 */
constexpr auto kConnectTotalTimeout = std::chrono::seconds(10);

/** @brief LLM/xiaozhi 模块的日志记录器 */
const auto kLog = xiaoai_server::GetLogger("llm");

bool SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool SetBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

bool IsFatalConnectError(int error_code) {
  return error_code != EINPROGRESS && error_code != EAGAIN &&
         error_code != EWOULDBLOCK && error_code != ECONNREFUSED &&
         error_code != ENOENT;
}

void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::shutdown(*fd, SHUT_RDWR);
    ::close(*fd);
    *fd = -1;
  }
}

std::string ReadJsonLineFromTcp(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    const ssize_t result = ::recv(fd, &ch, 1, 0);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("recv xiaozhi command line: ") +
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

int ConnectTcpEndpoint(const std::string& host,
                       int port,
                       const char* channel_name,
                       const std::atomic<bool>& stop_requested) {
  const auto deadline = std::chrono::steady_clock::now() + kConnectTotalTimeout;

  while (std::chrono::steady_clock::now() < deadline && !stop_requested.load()) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      kLog->warn("{} tcp socket create failed: {}", channel_name, std::strerror(errno));
      std::this_thread::sleep_for(kConnectRetryInterval);
      continue;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      ::close(fd);
      throw std::runtime_error(std::string("invalid xiaozhi ") + channel_name +
                               " host address: " + host);
    }

    SetNonBlocking(fd);
    const int connect_result =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (connect_result < 0 && errno != EINPROGRESS) {
      const int connect_errno = errno;
      ::close(fd);
      if (IsFatalConnectError(connect_errno)) {
        throw std::runtime_error(std::string("fatal xiaozhi ") + channel_name +
                                 " tcp connect error to " + host + ":" +
                                 std::to_string(port) + ": " + std::strerror(connect_errno));
      }
      std::this_thread::sleep_for(kConnectRetryInterval);
      continue;
    }

    if (connect_result == 0) {
      SetBlocking(fd);
      kLog->info("connected to xiaozhi {} channel {}:{}", channel_name, host, port);
      return fd;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              std::chrono::steady_clock::now())
            .count());
    if (remaining_ms <= 0) {
      ::close(fd);
      break;
    }

    const int poll_result = ::poll(&pfd, 1, remaining_ms);
    if (poll_result < 0) {
      ::close(fd);
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("poll xiaozhi ") + channel_name +
                               " tcp connect failed: " + std::strerror(errno));
    }
    if (poll_result == 0) {
      ::close(fd);
      continue;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0) {
      ::close(fd);
      continue;
    }
    if (socket_error != 0) {
      ::close(fd);
      if (IsFatalConnectError(socket_error)) {
        throw std::runtime_error(std::string("fatal xiaozhi ") + channel_name +
                                 " tcp connect error: " + std::strerror(socket_error));
      }
      std::this_thread::sleep_for(kConnectRetryInterval);
      continue;
    }

    SetBlocking(fd);
    kLog->info("connected to xiaozhi {} channel {}:{}", channel_name, host, port);
    return fd;
  }

  throw std::runtime_error(std::string("timed out connecting to xiaozhi ") +
                           channel_name + " channel " + host + ":" + std::to_string(port));
}

}  // namespace

LlmClient::LlmClient(LlmClientOptions options,
                     OnSessionEndCallback on_session_end,
                     OnPlaybackAudioCallback on_playback_audio)
    : options_(std::move(options)),
      on_session_end_(std::move(on_session_end)),
      on_playback_audio_(std::move(on_playback_audio)) {}

LlmClient::~LlmClient() { Stop(); }

bool LlmClient::connected() const { return connected_.load(); }

bool LlmClient::Connect() {
  stop_requested_.store(false);

  int audio_fd = -1;
  int command_fd = -1;
  try {
    audio_fd = ConnectTcpEndpoint(options_.host, options_.port, "audio", stop_requested_);
    command_fd = ConnectTcpEndpoint(options_.command_host, options_.command_port, "command",
                                    stop_requested_);
  } catch (...) {
    CloseFd(&audio_fd);
    CloseFd(&command_fd);
    throw;
  }

  {
    std::lock_guard<std::mutex> audio_lock(audio_mu_);
    CloseFd(&audio_fd_);
    audio_fd_ = audio_fd;
    audio_fd = -1;
  }
  {
    std::lock_guard<std::mutex> command_lock(command_mu_);
    CloseFd(&command_fd_);
    command_fd_ = command_fd;
    command_fd = -1;
  }

  connected_.store(true);
  audio_thread_ = std::thread([this] { AudioReaderLoop(); });
  command_thread_ = std::thread([this] { CommandReaderLoop(); });
  return true;
}

void LlmClient::Disconnect() {
  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    CloseFd(&audio_fd_);
  }
  {
    std::lock_guard<std::mutex> lock(command_mu_);
    CloseFd(&command_fd_);
  }
  connected_.store(false);
}

bool LlmClient::SendAllLocked(int fd,
                              const uint8_t* data,
                              size_t byte_count,
                              const char* channel_name) {
  size_t written = 0;
  while (written < byte_count) {
    const ssize_t result =
        ::send(fd, data + written, byte_count - written, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      kLog->warn("xiaozhi {} channel send failed: {}", channel_name, std::strerror(errno));
      return false;
    }
    if (result == 0) {
      kLog->warn("xiaozhi {} channel send returned 0 bytes", channel_name);
      return false;
    }
    written += static_cast<size_t>(result);
  }
  return true;
}

void LlmClient::SendAudio(const std::vector<uint8_t>& chunk) {
  if (chunk.empty()) {
    return;
  }

  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    if (!connected_.load() || audio_fd_ < 0) {
      return;
    }
    ok = SendAllLocked(audio_fd_, chunk.data(), chunk.size(), "audio");
  }
  if (!ok) {
    Disconnect();
  }
}

bool LlmClient::SendSessionStart(const std::string& reason,
                                 std::optional<double> score,
                                 std::optional<int64_t> timestamp_ms) {
  nlohmann::json message = {
      {"type", "session_start"},
      {"reason", reason.empty() ? "kws_hit" : reason},
  };
  if (score) {
    message["score"] = *score;
  }
  if (timestamp_ms) {
    message["timestamp_ms"] = *timestamp_ms;
  }
  std::string line = message.dump();
  line.push_back('\n');

  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(command_mu_);
    if (!connected_.load() || command_fd_ < 0) {
      kLog->warn("drop session_start because xiaozhi command channel is not connected");
      return false;
    }
    ok = SendAllLocked(command_fd_, reinterpret_cast<const uint8_t*>(line.data()), line.size(),
                       "command");
  }
  if (!ok) {
    Disconnect();
    return false;
  }
  kLog->info("sent session_start to xiaozhi reason={}", message.value("reason", ""));
  return true;
}

void LlmClient::set_session_end_callback(OnSessionEndCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mu_);
  on_session_end_ = std::move(callback);
}

void LlmClient::set_playback_audio_callback(OnPlaybackAudioCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mu_);
  on_playback_audio_ = std::move(callback);
}

void LlmClient::Stop() {
  stop_requested_.store(true);
  Disconnect();

  const std::thread::id current_thread = std::this_thread::get_id();
  if (audio_thread_.joinable() && audio_thread_.get_id() != current_thread) {
    audio_thread_.join();
  }
  if (command_thread_.joinable() && command_thread_.get_id() != current_thread) {
    command_thread_.join();
  }
}

void LlmClient::AudioReaderLoop() {
  try {
    std::vector<uint8_t> buffer(std::max<size_t>(1, options_.read_chunk_bytes));
    while (!stop_requested_.load() && connected_.load()) {
      int fd = -1;
      {
        std::lock_guard<std::mutex> lock(audio_mu_);
        fd = audio_fd_;
      }
      if (fd < 0) {
        break;
      }

      const ssize_t bytes_read = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (bytes_read < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error(std::string("recv xiaozhi downlink audio: ") +
                                 std::strerror(errno));
      }
      if (bytes_read == 0) {
        kLog->warn("xiaozhi audio channel disconnected");
        Disconnect();
        break;
      }

      std::vector<uint8_t> chunk(buffer.begin(), buffer.begin() + bytes_read);
      OnPlaybackAudioCallback callback;
      {
        std::lock_guard<std::mutex> lock(callback_mu_);
        callback = on_playback_audio_;
      }
      if (callback) {
        callback(chunk);
      }
    }
  } catch (const std::exception& error) {
    if (!stop_requested_.load()) {
      kLog->error("xiaozhi audio reader failed: {}", error.what());
      Disconnect();
    }
  }
}

void LlmClient::CommandReaderLoop() {
  try {
    while (!stop_requested_.load() && connected_.load()) {
      int fd = -1;
      {
        std::lock_guard<std::mutex> lock(command_mu_);
        fd = command_fd_;
      }
      if (fd < 0) {
        break;
      }

      const std::string line = ReadJsonLineFromTcp(fd);
      if (line.empty()) {
        kLog->warn("xiaozhi command channel disconnected");
        Disconnect();
        break;
      }

      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_discarded() || !message.is_object()) {
        kLog->warn("invalid JSON line from xiaozhi command channel: {}", line);
        continue;
      }

      const std::string type = message.value("type", "");
      if (type == "session_end") {
        const std::string reason = message.value("reason", "xiaozhi");
        kLog->info("received session_end from xiaozhi reason={}", reason);
        OnSessionEndCallback callback;
        {
          std::lock_guard<std::mutex> lock(callback_mu_);
          callback = on_session_end_;
        }
        if (callback) {
          callback(reason);
        }
      } else {
        kLog->debug("ignored xiaozhi command type={}", type);
      }
    }
  } catch (const std::exception& error) {
    if (!stop_requested_.load()) {
      kLog->error("xiaozhi command reader failed: {}", error.what());
      Disconnect();
    }
  }
}

}  // namespace soundbox_server::llm
