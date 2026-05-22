#pragma once

#include "common/wav_writer.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace audio_processing_module::tests::aec {

class FileRecorder {
 public:
  FileRecorder(std::string listen_socket_path, std::string output_file);

  const std::string& listen_socket_path() const { return listen_socket_path_; }
  const std::string& output_file() const { return output_file_; }
  uint64_t frames_written() const { return frames_written_.load(); }

  void Run();
  void RunOneClient();
  void Stop();

 private:
  void HandleClient(int input_fd,
                     WavWriter* writer,
                     uint64_t* frames_written);

  std::string listen_socket_path_;
  std::string output_file_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<uint64_t> frames_written_{0};
};

}  // namespace audio_processing_module::tests::aec
