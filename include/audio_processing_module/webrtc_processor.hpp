#pragma once

#include "audio_processing_module/audio_frame.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace webrtc {
class AudioProcessing;
}  // namespace webrtc

namespace audio_processing_module {

class WebRtcProcessor {
 public:
  WebRtcProcessor();
  ~WebRtcProcessor();

  WebRtcProcessor(const WebRtcProcessor&) = delete;
  WebRtcProcessor& operator=(const WebRtcProcessor&) = delete;

  std::vector<int16_t> Process10Ms(const std::vector<int16_t>& mic,
                                   const std::vector<int16_t>& reference);

 private:
  void CheckWebRtcResult(int result, const char* operation) const;

  std::unique_ptr<webrtc::AudioProcessing> apm_;
  uint32_t timestamp_ = 0;
  int frame_id_ = 0;
};

}  // namespace audio_processing_module
