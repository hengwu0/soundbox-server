#pragma once

#include <string>

namespace audio_processing_module::frontend {

class AudioStreamFrontend {
 public:
  AudioStreamFrontend(std::string input_file, std::string aec_socket_path);

  const std::string& input_file() const { return input_file_; }
  const std::string& aec_socket_path() const { return aec_socket_path_; }

  void Run();

 private:
  std::string input_file_;
  std::string aec_socket_path_;
};

}  // namespace audio_processing_module::frontend
