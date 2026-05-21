#include "llm/file_recorder.hpp"

#include "common/audio_frame.hpp"
#include "common/unix_socket.hpp"
#include "common/wav_writer.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::llm {
namespace {

constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);

}  // namespace

FileRecorder::FileRecorder(std::string listen_socket_path, std::string output_file)
    : listen_socket_path_(std::move(listen_socket_path)),
      output_file_(std::move(output_file)) {}

void FileRecorder::Run() {
  // FileRecorder is a single-client sink: only the current AEC stream should
  // connect to this socket, so no client list or broadcast path is needed.
  FileDescriptor server = CreateUnixServerSocket(listen_socket_path_, 1);
  SocketPathGuard guard(listen_socket_path_);
  std::cout << "[llm] processed audio listen ready: " << listen_socket_path_
            << '\n';

  FileDescriptor input =
      AcceptUnixClientWithTimeout(server.get(), kSocketAcceptTimeout);
  std::cout << "[llm] AEC connected; writing recording file to disk\n";

  WavWriter writer(output_file_, AudioFormat::Mono16kS16());
  std::vector<uint8_t> output_frame(kOutputBytesPerFrame, 0);
  uint64_t frames_written = 0;

  while (true) {
    const size_t bytes_read =
        ReadFrameOrEof(input.get(), output_frame.data(), output_frame.size());
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read % kBytesPerSample != 0) {
      throw std::runtime_error("processed pcm stream ended on an odd byte");
    }

    const std::vector<int16_t> samples =
        DecodeS16LittleEndian(output_frame.data(), bytes_read);
    writer.WriteSamples(samples);
    ++frames_written;

    if (bytes_read < output_frame.size()) {
      break;
    }
  }

  writer.Close();
  std::cout << "[llm] finished: frames=" << frames_written
            << ", wav=" << output_file_ << ", data_bytes="
            << writer.data_bytes() << '\n';
}

}  // namespace audio_processing_module::llm
