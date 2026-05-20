#pragma once

#include <chrono>
#include <string>

namespace audio_processing_module {

struct PipelineOptions {
  std::string input_file;
  std::string output_file;
  std::string socket_dir;
};

class ModuleAFileSource {
 public:
  ModuleAFileSource(std::string input_file, std::string capture_socket);
  void Run();

 private:
  std::string input_file_;
  std::string capture_socket_;
};

class ModuleBWebRtcProcessor {
 public:
  ModuleBWebRtcProcessor(std::string capture_socket, std::string processed_socket);
  void Run();

 private:
  std::string capture_socket_;
  std::string processed_socket_;
};

class ModuleCWavSink {
 public:
  ModuleCWavSink(std::string processed_socket, std::string output_file);
  void Run();

 private:
  std::string processed_socket_;
  std::string output_file_;
};

}  // namespace audio_processing_module
