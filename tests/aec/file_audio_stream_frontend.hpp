#pragma once

#include <string>

namespace audio_processing_module::tests::aec {

class FileAudioStreamFrontend {
 public:
  FileAudioStreamFrontend(std::string input_file,
                          std::string aec_socket_path,
                          bool realtime = true);

  const std::string& input_file() const { return input_file_; }
  const std::string& aec_socket_path() const { return aec_socket_path_; }

  void Run();

 private:
  std::string input_file_;
  std::string aec_socket_path_;
  bool realtime_{true};
};

}  // namespace audio_processing_module::tests::aec
