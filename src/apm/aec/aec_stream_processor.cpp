#include "apm/aec/aec_stream_processor.hpp"

#include "common/audio_frame.hpp"
#include "common/unix_socket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::apm::aec {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);
constexpr size_t kSocketReadChunkBytes = 4096;

std::runtime_error ErrnoError(const std::string& action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}

int64_t MonotonicMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void TryWriteSessionEnd(int fd, const std::string& reason) {
  const std::string line =
      "{\"type\":\"session_end\",\"reason\":\"" + reason +
      "\",\"timestamp_ms\":" + std::to_string(MonotonicMilliseconds()) + "}\n";
  size_t written = 0;
  while (written < line.size()) {
    const ssize_t result =
        ::send(fd, line.data() + written, line.size() - written, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
        return;
      }
      throw ErrnoError("send AEC session_end");
    }
    if (result == 0) {
      return;
    }
    written += static_cast<size_t>(result);
  }
}

}  // namespace

AecStreamProcessor::AecStreamProcessor(
    std::string frontend_listen_socket_path,
    std::string llm_socket_path,
    audio_processing_module::WebRtcProcessorOptions options)
    : frontend_listen_socket_path_(std::move(frontend_listen_socket_path)),
      llm_socket_path_(std::move(llm_socket_path)),
      options_(options) {}

void AecStreamProcessor::Run() {
  // AEC frontend socket is intentionally single-client: one frontend owns the
  // active LLM turn; disconnecting that client ends this processing stream.
  FileDescriptor frontend_server =
      CreateUnixServerSocket(frontend_listen_socket_path_, 1);
  SocketPathGuard frontend_guard(frontend_listen_socket_path_);
  std::cout << "[apm/aec] frontend listen ready: "
            << frontend_listen_socket_path_ << '\n';

  FileDescriptor llm_socket = ConnectUnixSocketWithRetry(
      llm_socket_path_, kSocketConnectTimeout, kSocketRetryInterval);
  std::cout << "[apm/aec] connected to LLM listen socket: "
            << llm_socket_path_ << '\n';

  FileDescriptor frontend_socket =
      AcceptUnixClientWithTimeout(frontend_server.get(), kSocketAcceptTimeout);
  std::cout << "[apm/aec] frontend connected; WebRTC AEC/NS/AGC enabled\n";

  audio_processing_module::WebRtcProcessor processor(options_);
  std::vector<uint8_t> read_buffer(kSocketReadChunkBytes, 0);
  std::vector<uint8_t> pending;
  pending.reserve(kInputBytesPerFrame * 2);
  uint64_t frames_processed = 0;

  while (true) {
    const ssize_t bytes_read =
        ::read(frontend_socket.get(), read_buffer.data(), read_buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ErrnoError("read AEC pcm");
    }
    if (bytes_read == 0) {
      break;
    }

    pending.insert(pending.end(), read_buffer.begin(),
                   read_buffer.begin() + bytes_read);
    while (pending.size() >= kInputBytesPerFrame) {
      const std::vector<int16_t> interleaved =
          DecodeS16LittleEndian(pending.data(), kInputBytesPerFrame);
      const StereoSplit split = SplitInterleavedStereoS16(interleaved);
      const std::vector<int16_t> processed =
          processor.Process10Ms(split.mic, split.reference);
      const std::vector<uint8_t> processed_bytes = EncodeS16LittleEndian(processed);
      WriteAll(llm_socket.get(), processed_bytes.data(), processed_bytes.size());
      pending.erase(pending.begin(), pending.begin() + kInputBytesPerFrame);
      ++frames_processed;
    }
  }

  if (!pending.empty()) {
    throw std::runtime_error("AEC PCM stream ended before a complete 10ms frame");
  }

  if (frames_processed > 0) {
    TryWriteSessionEnd(frontend_socket.get(), "input_eof");
  }

  std::cout << "[apm/aec] finished: frames=" << frames_processed << '\n';
}

}  // namespace audio_processing_module::apm::aec
