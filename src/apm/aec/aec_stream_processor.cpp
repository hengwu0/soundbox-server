#include "apm/aec/aec_stream_processor.hpp"

#include "common/audio_frame.hpp"
#include "common/log.hpp"
#include "common/unix_socket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::apm::aec {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);
constexpr auto kSocketAcceptPollTimeout = std::chrono::milliseconds(200);
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);
constexpr size_t kSocketReadChunkBytes = 4096;
constexpr float kVadSpeechRmsThreshold = 600.0f;
constexpr int kVadMinSpeechFrames = 18;
constexpr int kVadMinSilenceFrames = 65;

const auto kLog = xiaoai_server::GetLogger("apm/aec");

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

class SessionEndDetector {
 public:
  bool ObserveMicFrame(const std::vector<int16_t>& mic) {
    if (ended_) {
      return false;
    }

    const bool speech = ComputeRms(mic) >= kVadSpeechRmsThreshold;
    if (speech) {
      ++consecutive_speech_frames_;
      silence_after_speech_frames_ = 0;
      if (consecutive_speech_frames_ >= kVadMinSpeechFrames) {
        speech_seen_ = true;
      }
      return false;
    }

    consecutive_speech_frames_ = 0;
    if (speech_seen_) {
      ++silence_after_speech_frames_;
    }
    if (speech_seen_ && silence_after_speech_frames_ >= kVadMinSilenceFrames) {
      ended_ = true;
      return true;
    }
    return false;
  }

 private:
  bool speech_seen_{false};
  bool ended_{false};
  int consecutive_speech_frames_{0};
  int silence_after_speech_frames_{0};
};

}  // namespace

AecStreamProcessor::AecStreamProcessor(
    std::string frontend_listen_socket_path,
    std::string llm_socket_path,
    audio_processing_module::WebRtcProcessorOptions options)
    : frontend_listen_socket_path_(std::move(frontend_listen_socket_path)),
      llm_socket_path_(std::move(llm_socket_path)),
      options_(options) {}

void AecStreamProcessor::Run() {
  stop_requested_.store(false);

  // AEC frontend socket is intentionally single-client: one frontend owns the
  // active LLM turn; after disconnect we go back to accept instead of keeping a
  // client list or broadcasting processed audio.
  FileDescriptor frontend_server =
      CreateUnixServerSocket(frontend_listen_socket_path_, 1);
  SocketPathGuard frontend_guard(frontend_listen_socket_path_);
  kLog->info("frontend listen ready socket={}", frontend_listen_socket_path_);

  FileDescriptor llm_socket = ConnectUnixSocketWithRetry(
      llm_socket_path_, kSocketConnectTimeout, kSocketRetryInterval);
  kLog->info("connected to LLM listen socket={}", llm_socket_path_);

  while (!stop_requested_.load()) {
    try {
      FileDescriptor frontend_socket =
          AcceptUnixClientWithTimeout(frontend_server.get(), kSocketAcceptPollTimeout);
      HandleClient(frontend_socket.get(), llm_socket.get());
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

void AecStreamProcessor::RunOneClient() {
  stop_requested_.store(false);

  FileDescriptor frontend_server =
      CreateUnixServerSocket(frontend_listen_socket_path_, 1);
  SocketPathGuard frontend_guard(frontend_listen_socket_path_);
  kLog->info("frontend listen ready socket={}", frontend_listen_socket_path_);

  FileDescriptor llm_socket = ConnectUnixSocketWithRetry(
      llm_socket_path_, kSocketConnectTimeout, kSocketRetryInterval);
  kLog->info("connected to LLM listen socket={}", llm_socket_path_);

  FileDescriptor frontend_socket =
      AcceptUnixClientWithTimeout(frontend_server.get(), kSocketAcceptTimeout);
  HandleClient(frontend_socket.get(), llm_socket.get());
}

void AecStreamProcessor::Stop() {
  stop_requested_.store(true);
}

void AecStreamProcessor::HandleClient(int frontend_fd, int llm_fd) {
  kLog->info("frontend connected; WebRTC AEC/NS/AGC enabled");

  audio_processing_module::WebRtcProcessor processor(options_);
  std::vector<uint8_t> read_buffer(kSocketReadChunkBytes, 0);
  std::vector<uint8_t> pending;
  pending.reserve(kInputBytesPerFrame * 2);
  uint64_t frames_processed = 0;
  SessionEndDetector session_end_detector;

  while (true) {
    const ssize_t bytes_read =
        ::read(frontend_fd, read_buffer.data(), read_buffer.size());
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
      if (session_end_detector.ObserveMicFrame(split.mic)) {
        TryWriteSessionEnd(frontend_fd, "vad_end");
      }
      const std::vector<int16_t> processed =
          processor.Process10Ms(split.mic, split.reference);
      const std::vector<uint8_t> processed_bytes = EncodeS16LittleEndian(processed);
      WriteAll(llm_fd, processed_bytes.data(), processed_bytes.size());
      pending.erase(pending.begin(), pending.begin() + kInputBytesPerFrame);
      ++frames_processed;
    }
  }

  if (!pending.empty()) {
    throw std::runtime_error("AEC PCM stream ended before a complete 10ms frame");
  }

  kLog->info("finished frames={}", frames_processed);
}

}  // namespace audio_processing_module::apm::aec
