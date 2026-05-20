#include "audio_processing_module/modules.hpp"

#include "audio_processing_module/audio_frame.hpp"
#include "audio_processing_module/unix_socket.hpp"
#include "audio_processing_module/wav_writer.hpp"
#include "audio_processing_module/webrtc_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace audio_processing_module {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);

uint64_t FileSizeOrThrow(const std::string& path) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::runtime_error("failed to stat input pcm file: " + path);
  }
  return size;
}

}  // namespace

ModuleAFileSource::ModuleAFileSource(std::string input_file,
                                     std::string capture_socket)
    : input_file_(std::move(input_file)),
      capture_socket_(std::move(capture_socket)) {}

void ModuleAFileSource::Run() {
  const uint64_t input_bytes = FileSizeOrThrow(input_file_);
  std::ifstream input(input_file_, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input pcm file: " + input_file_);
  }

  FileDescriptor server = CreateUnixServerSocket(capture_socket_, 1);
  SocketPathGuard guard(capture_socket_);
  std::cout << "[A] capture socket ready: " << capture_socket_ << '\n';

  FileDescriptor client =
      AcceptUnixClientWithTimeout(server.get(), kSocketAcceptTimeout);
  std::cout << "[A] downstream connected; streaming "
            << FormatDurationFromPcmBytes(input_bytes, AudioFormat::Stereo16kS16())
            << " of stereo PCM\n";

  std::vector<uint8_t> frame(kInputBytesPerFrame, 0);
  uint64_t source_bytes_sent = 0;
  uint64_t frames_sent = 0;
  auto next_frame_time = std::chrono::steady_clock::now();

  while (true) {
    std::fill(frame.begin(), frame.end(), 0);
    input.read(reinterpret_cast<char*>(frame.data()),
               static_cast<std::streamsize>(frame.size()));
    const std::streamsize bytes_read = input.gcount();
    if (bytes_read < 0) {
      throw std::runtime_error("failed to read input pcm file: " + input_file_);
    }
    if (bytes_read == 0) {
      break;
    }

    source_bytes_sent += static_cast<uint64_t>(bytes_read);
    WriteAll(client.get(), frame.data(), frame.size());
    ++frames_sent;

    next_frame_time += std::chrono::milliseconds(kFrameDurationMs);
    std::this_thread::sleep_until(next_frame_time);

    if (static_cast<size_t>(bytes_read) < frame.size()) {
      break;
    }
  }

  if (input.bad()) {
    throw std::runtime_error("input pcm stream failed: " + input_file_);
  }

  std::cout << "[A] finished: frames=" << frames_sent
            << ", source_bytes=" << source_bytes_sent << '\n';
}

ModuleBWebRtcProcessor::ModuleBWebRtcProcessor(std::string capture_socket,
                                               std::string processed_socket,
                                               WebRtcProcessorOptions options)
    : capture_socket_(std::move(capture_socket)),
      processed_socket_(std::move(processed_socket)),
      options_(options) {}

void ModuleBWebRtcProcessor::Run() {
  FileDescriptor output_server = CreateUnixServerSocket(processed_socket_, 1);
  SocketPathGuard guard(processed_socket_);
  std::cout << "[B] processed socket ready: " << processed_socket_ << '\n';

  FileDescriptor input = ConnectUnixSocketWithRetry(
      capture_socket_, kSocketConnectTimeout, kSocketRetryInterval);
  std::cout << "[B] connected to capture socket\n";

  FileDescriptor output =
      AcceptUnixClientWithTimeout(output_server.get(), kSocketAcceptTimeout);
  std::cout << "[B] downstream connected; WebRTC AEC/NS/AGC enabled\n";

  WebRtcProcessor processor(options_);
  std::vector<uint8_t> input_frame(kInputBytesPerFrame, 0);
  uint64_t frames_processed = 0;

  while (true) {
    std::fill(input_frame.begin(), input_frame.end(), 0);
    const size_t bytes_read =
        ReadFrameOrEof(input.get(), input_frame.data(), input_frame.size());
    if (bytes_read == 0) {
      break;
    }

    const std::vector<int16_t> interleaved =
        DecodeS16LittleEndian(input_frame.data(), input_frame.size());
    const StereoSplit split = SplitInterleavedStereoS16(interleaved);
    const std::vector<int16_t> processed =
        processor.Process10Ms(split.mic, split.reference);
    const std::vector<uint8_t> processed_bytes = EncodeS16LittleEndian(processed);
    WriteAll(output.get(), processed_bytes.data(), processed_bytes.size());
    ++frames_processed;

    if (bytes_read < input_frame.size()) {
      break;
    }
  }

  std::cout << "[B] finished: frames=" << frames_processed << '\n';
}

ModuleCWavSink::ModuleCWavSink(std::string processed_socket,
                               std::string output_file)
    : processed_socket_(std::move(processed_socket)),
      output_file_(std::move(output_file)) {}

void ModuleCWavSink::Run() {
  FileDescriptor input = ConnectUnixSocketWithRetry(
      processed_socket_, kSocketConnectTimeout, kSocketRetryInterval);
  std::cout << "[C] connected to processed socket\n";

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
  std::cout << "[C] finished: frames=" << frames_written
            << ", wav=" << output_file_ << ", data_bytes="
            << writer.data_bytes() << '\n';
}

}  // namespace audio_processing_module
