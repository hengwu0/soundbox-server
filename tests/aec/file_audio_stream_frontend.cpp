#include "file_audio_stream_frontend.hpp"

#include "common/audio_frame.hpp"
#include "common/unix_socket.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace audio_processing_module::tests::aec {
namespace {

constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
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

FileAudioStreamFrontend::FileAudioStreamFrontend(std::string input_file,
                                                 std::string aec_socket_path)
    : input_file_(std::move(input_file)),
      aec_socket_path_(std::move(aec_socket_path)) {}

void FileAudioStreamFrontend::Run() {
  const uint64_t input_bytes = FileSizeOrThrow(input_file_);
  std::ifstream input(input_file_, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input pcm file: " + input_file_);
  }

  std::cout << "[tests/aec/frontend] establishing audio stream socket connection: "
            << aec_socket_path_ << '\n';
  FileDescriptor aec_socket = ConnectUnixSocketWithRetry(
      aec_socket_path_, kSocketConnectTimeout, kSocketRetryInterval);
  std::cout << "[tests/aec/frontend] connected; streaming "
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
    WriteAll(aec_socket.get(), frame.data(), frame.size());
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

  std::cout << "[tests/aec/frontend] finished: frames=" << frames_sent
            << ", source_bytes=" << source_bytes_sent << '\n';
}

}  // namespace audio_processing_module::tests::aec
