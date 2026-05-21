#include "llm/file_recorder.hpp"

#include "common/audio_frame.hpp"
#include "common/log.hpp"
#include "common/unix_socket.hpp"
#include "common/wav_writer.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::llm {
namespace {

constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);
constexpr auto kSocketAcceptPollTimeout = std::chrono::milliseconds(200);

const auto kLog = xiaoai_server::GetLogger("llm");

}  // namespace

FileRecorder::FileRecorder(std::string listen_socket_path, std::string output_file)
    : listen_socket_path_(std::move(listen_socket_path)),
      output_file_(std::move(output_file)) {}

void FileRecorder::Run() {
  // FileRecorder is a single-client sink: only the current AEC stream should
  // connect to this socket at a time; after disconnect we accept the next AEC
  // stream instead of keeping a client list or broadcast path.
  stop_requested_.store(false);
  frames_written_.store(0);
  FileDescriptor server = CreateUnixServerSocket(listen_socket_path_, 1);
  SocketPathGuard guard(listen_socket_path_);
  kLog->info("processed audio listen ready socket={}", listen_socket_path_);

  WavWriter writer(output_file_, AudioFormat::Mono16kS16());
  uint64_t frames_written = 0;

  while (!stop_requested_.load()) {
    try {
      FileDescriptor input =
          AcceptUnixClientWithTimeout(server.get(), kSocketAcceptPollTimeout);
      kLog->info("AEC connected; writing recording file to disk");
      HandleClient(input.get(), &writer, &frames_written);
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

  writer.Close();
  kLog->info("finished frames={} wav={} data_bytes={}",
             frames_written, output_file_, writer.data_bytes());
}

void FileRecorder::RunOneClient() {
  stop_requested_.store(false);
  frames_written_.store(0);

  FileDescriptor server = CreateUnixServerSocket(listen_socket_path_, 1);
  SocketPathGuard guard(listen_socket_path_);
  kLog->info("processed audio listen ready socket={}", listen_socket_path_);

  FileDescriptor input =
      AcceptUnixClientWithTimeout(server.get(), kSocketAcceptTimeout);
  kLog->info("AEC connected; writing recording file to disk");

  WavWriter writer(output_file_, AudioFormat::Mono16kS16());
  uint64_t frames_written = 0;
  HandleClient(input.get(), &writer, &frames_written);

  writer.Close();
  kLog->info("finished frames={} wav={} data_bytes={}",
             frames_written, output_file_, writer.data_bytes());
}

void FileRecorder::Stop() {
  stop_requested_.store(true);
}

void FileRecorder::HandleClient(int input_fd,
                                WavWriter* writer,
                                uint64_t* frames_written) {
  std::vector<uint8_t> output_frame(kOutputBytesPerFrame, 0);
  while (true) {
    const size_t bytes_read =
        ReadFrameOrEof(input_fd, output_frame.data(), output_frame.size());
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read % kBytesPerSample != 0) {
      throw std::runtime_error("processed pcm stream ended on an odd byte");
    }

    const std::vector<int16_t> samples =
        DecodeS16LittleEndian(output_frame.data(), bytes_read);
    writer->WriteSamples(samples);
    ++(*frames_written);
    frames_written_.store(*frames_written);

    if (bytes_read < output_frame.size()) {
      break;
    }
  }
}

}  // namespace audio_processing_module::llm
