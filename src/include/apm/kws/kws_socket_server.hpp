#pragma once

#include "apm/kws/kws_engine.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace audio_processing_module::apm::kws {

class KwsSocketServer {
 public:
  struct Options {
    std::string listen_socket_path;
    int sample_rate{16000};
    int channels{1};
    int bits_per_sample{16};
    size_t read_chunk_bytes{320};
  };

  KwsSocketServer(Options options,
                  std::shared_ptr<xiaoai_server::wakeup::IKwsEngine> engine);

  const std::string& listen_socket_path() const {
    return options_.listen_socket_path;
  }

  void Run();
  void RunOneClient();
  void Stop();

 private:
  void HandleClient(int client_fd);

  Options options_;
  std::shared_ptr<xiaoai_server::wakeup::IKwsEngine> engine_;
  std::atomic<bool> stop_requested_{false};
};

}  // namespace audio_processing_module::apm::kws
