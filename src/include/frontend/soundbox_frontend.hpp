#pragma once

#include "config/config.hpp"
#include "common/unix_socket.hpp"
#include "frontend/soundbox/client.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace soundbox_server::frontend {

class Frontend {
 public:
  enum class State {
    kKws,
    kLlmStarting,
    kAec,
    kLlmStopping,
    kFault,
  };

  struct Options {
    std::string kws_socket_path;
    std::string aec_socket_path;
    std::string playback_socket_path;
    xiaoai_server::config::Config soundbox_config;
    size_t playback_read_chunk_bytes{4096};
  };

  explicit Frontend(Options options);
  ~Frontend();

  State state() const;

  void Run();
  void Stop();

 private:
  void OnWakeupAudio(const std::vector<uint8_t>& chunk);
  void OnAecAudio(const std::vector<uint8_t>& chunk);
  void KwsControlReaderLoop();
  void AecControlReaderLoop();
  void PlaybackAcceptLoop();
  void PlaybackClientLoop(int client_fd);
  void HandleSessionStart();
  void HandleSessionEnd();
  void SetState(State state, const char* reason);
  bool IsStopping() const;

  Options options_;
  std::unique_ptr<xiaoai_server::soundbox::SoundBoxClient> client_;
  audio_processing_module::FileDescriptor kws_socket_;
  audio_processing_module::FileDescriptor aec_socket_;
  std::thread kws_control_thread_;
  std::thread aec_control_thread_;
  std::thread playback_thread_;
  mutable std::mutex mu_;
  std::condition_variable stop_cv_;
  State state_{State::kKws};
  std::atomic<bool> stop_requested_{false};
};

const char* StateName(Frontend::State state);

}  // namespace soundbox_server::frontend
