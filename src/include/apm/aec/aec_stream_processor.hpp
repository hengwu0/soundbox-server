#pragma once

#include "apm/aec/webrtc_processor.hpp"

#include <atomic>
#include <string>

namespace audio_processing_module::apm::aec {

class AecStreamProcessor {
 public:
  AecStreamProcessor(std::string frontend_listen_socket_path,
                     std::string llm_socket_path,
                     audio_processing_module::WebRtcProcessorOptions options);

  const std::string& frontend_listen_socket_path() const {
    return frontend_listen_socket_path_;
  }
  const std::string& llm_socket_path() const { return llm_socket_path_; }

  void Run();
  void RunOneClient();
  void Stop();

 private:
  void HandleClient(int frontend_fd, int llm_fd);

  std::string frontend_listen_socket_path_;
  std::string llm_socket_path_;
  audio_processing_module::WebRtcProcessorOptions options_;
  std::atomic<bool> stop_requested_{false};
};

}  // namespace audio_processing_module::apm::aec
