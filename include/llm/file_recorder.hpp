#pragma once

#include <string>

namespace audio_processing_module::llm {

class FileRecorder {
 public:
  FileRecorder(std::string listen_socket_path, std::string output_file);

  const std::string& listen_socket_path() const { return listen_socket_path_; }
  const std::string& output_file() const { return output_file_; }

  void Run();

 private:
  std::string listen_socket_path_;
  std::string output_file_;
};

}  // namespace audio_processing_module::llm
