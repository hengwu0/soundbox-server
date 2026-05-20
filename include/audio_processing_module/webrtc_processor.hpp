#pragma once

#include "audio_processing_module/audio_frame.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace webrtc {
class AudioProcessing;
}  // namespace webrtc

namespace audio_processing_module {

struct PreAecAutoGainConfig {
  bool enabled = true;
  float target_rms = 2400.0f;
  float max_gain = 6.0f;
  float attack = 0.35f;
  float release = 0.08f;
};

struct AutomaticGainState {
  float current_gain = 1.0f;
};

struct WebRtcProcessorOptions {
  enum class NoiseSuppressionLevel {
    kLow,
    kModerate,
    kHigh,
    kVeryHigh,
  };

  enum class AgcMode {
    kAdaptiveDigital,
    kFixedDigital,
  };

  int delay_ms = 2;
  NoiseSuppressionLevel ns_level = NoiseSuppressionLevel::kHigh;
  AgcMode agc_mode = AgcMode::kAdaptiveDigital;
  int agc_target_level_dbfs = 3;
  int agc_compression_gain_db = 9;
  bool agc_limiter_enabled = true;
  PreAecAutoGainConfig pre_aec_auto_gain;
};

float ComputeRms(const std::vector<int16_t>& samples);

void ProcessPreAecAutoGain(std::vector<int16_t>* samples,
                           const PreAecAutoGainConfig& config,
                           AutomaticGainState* state);

class WebRtcProcessor {
 public:
  explicit WebRtcProcessor(WebRtcProcessorOptions options = {});
  ~WebRtcProcessor();

  WebRtcProcessor(const WebRtcProcessor&) = delete;
  WebRtcProcessor& operator=(const WebRtcProcessor&) = delete;

  std::vector<int16_t> Process10Ms(const std::vector<int16_t>& mic,
                                   const std::vector<int16_t>& reference);

 private:
  void CheckWebRtcResult(int result, const char* operation) const;

  std::unique_ptr<webrtc::AudioProcessing> apm_;
  WebRtcProcessorOptions options_;
  AutomaticGainState pre_aec_gain_state_;
  uint32_t timestamp_ = 0;
  int frame_id_ = 0;
};

}  // namespace audio_processing_module
