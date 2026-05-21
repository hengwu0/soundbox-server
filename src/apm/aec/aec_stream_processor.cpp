#include "apm/aec/aec_stream_processor.hpp"

#include "common/audio_frame.hpp"
#include "common/unix_socket.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::apm::aec {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);

}  // namespace

AecStreamProcessor::AecStreamProcessor(
    std::string frontend_listen_socket_path,
    std::string llm_socket_path,
    audio_processing_module::WebRtcProcessorOptions options)
    : frontend_listen_socket_path_(std::move(frontend_listen_socket_path)),
      llm_socket_path_(std::move(llm_socket_path)),
      options_(options) {}

void AecStreamProcessor::Run() {
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
  std::vector<uint8_t> input_frame(kInputBytesPerFrame, 0);
  uint64_t frames_processed = 0;

  while (true) {
    std::fill(input_frame.begin(), input_frame.end(), 0);
    const size_t bytes_read =
        ReadFrameOrEof(frontend_socket.get(), input_frame.data(), input_frame.size());
    if (bytes_read == 0) {
      break;
    }

    const std::vector<int16_t> interleaved =
        DecodeS16LittleEndian(input_frame.data(), input_frame.size());
    const StereoSplit split = SplitInterleavedStereoS16(interleaved);
    const std::vector<int16_t> processed =
        processor.Process10Ms(split.mic, split.reference);
    const std::vector<uint8_t> processed_bytes = EncodeS16LittleEndian(processed);
    WriteAll(llm_socket.get(), processed_bytes.data(), processed_bytes.size());
    ++frames_processed;

    if (bytes_read < input_frame.size()) {
      break;
    }
  }

  std::cout << "[apm/aec] finished: frames=" << frames_processed << '\n';
}

}  // namespace audio_processing_module::apm::aec
