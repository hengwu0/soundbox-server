#include "apm/kws/kws_socket_server.hpp"

#include "common/log.hpp"
#include "common/unix_socket.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::apm::kws {
namespace {

constexpr auto kAcceptPollTimeout = std::chrono::milliseconds(200);

const auto kLog = xiaoai_server::GetLogger("apm/kws");

std::runtime_error ErrnoError(const std::string& action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}

int64_t MonotonicMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void WriteSessionStart(int client_fd, const xiaoai_server::wakeup::KwsHit& hit) {
  const std::string line =
      "{\"type\":\"session_start\",\"reason\":\"kws_hit\",\"keyword\":\"" +
      hit.keyword + "\",\"score\":0,\"timestamp_ms\":" +
      std::to_string(MonotonicMilliseconds()) + "}\n";
  WriteAll(client_fd, reinterpret_cast<const uint8_t*>(line.data()), line.size());
}

}  // namespace

KwsSocketServer::KwsSocketServer(
    Options options,
    std::shared_ptr<xiaoai_server::wakeup::IKwsEngine> engine)
    : options_(std::move(options)), engine_(std::move(engine)) {
  if (options_.listen_socket_path.empty()) {
    throw std::runtime_error("KWS listen socket path is empty");
  }
  if (!engine_) {
    throw std::runtime_error("KWS engine is null");
  }
  if (options_.bits_per_sample != 16 || options_.channels <= 0 ||
      options_.sample_rate <= 0 || options_.read_chunk_bytes == 0) {
    throw std::runtime_error("invalid KWS socket server audio options");
  }
}

void KwsSocketServer::Run() {
  FileDescriptor server = CreateUnixServerSocket(options_.listen_socket_path, 1);
  SocketPathGuard guard(options_.listen_socket_path);
  kLog->info("frontend listen ready socket={}", options_.listen_socket_path);

  while (!stop_requested_.load()) {
    try {
      // 单客户端 listen：一次只服务一个 frontend；断开后回到 accept 等下一次连接。
      FileDescriptor client =
          AcceptUnixClientWithTimeout(server.get(), kAcceptPollTimeout);
      HandleClient(client.get());
    } catch (const std::runtime_error& error) {
      if (stop_requested_.load()) {
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

void KwsSocketServer::RunOneClient() {
  FileDescriptor server = CreateUnixServerSocket(options_.listen_socket_path, 1);
  SocketPathGuard guard(options_.listen_socket_path);
  FileDescriptor client =
      AcceptUnixClientWithTimeout(server.get(), std::chrono::seconds(10));
  HandleClient(client.get());
}

void KwsSocketServer::Stop() {
  stop_requested_.store(true);
}

void KwsSocketServer::HandleClient(int client_fd) {
  kLog->info("frontend connected; KWS PCM stream enabled");
  engine_->Reset();

  std::vector<uint8_t> buffer(options_.read_chunk_bytes);
  while (!stop_requested_.load()) {
    const ssize_t bytes_read = ::read(client_fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ErrnoError("read KWS pcm");
    }
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read % 2 != 0) {
      throw std::runtime_error("KWS PCM stream ended on an odd byte");
    }

    const auto hit = engine_->AcceptPcm16(
        buffer.data(), static_cast<size_t>(bytes_read), options_.sample_rate,
        options_.channels, options_.bits_per_sample);
    if (hit.has_value()) {
      WriteSessionStart(client_fd, *hit);
      engine_->Reset();
    }
  }

  kLog->info("frontend disconnected");
}

}  // namespace audio_processing_module::apm::kws
