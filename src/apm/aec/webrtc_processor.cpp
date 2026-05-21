#include "apm/aec/webrtc_processor.hpp"

#include "modules/audio_processing/include/audio_processing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace audio_processing_module {
namespace {

webrtc::AudioProcessing::Config::NoiseSuppression::Level ToWebRtcNsLevel(
    WebRtcProcessorOptions::NoiseSuppressionLevel level) {
  using WebRtcLevel = webrtc::AudioProcessing::Config::NoiseSuppression::Level;
  switch (level) {
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kLow:
      return WebRtcLevel::kLow;
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kModerate:
      return WebRtcLevel::kModerate;
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kHigh:
      return WebRtcLevel::kHigh;
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kVeryHigh:
      return WebRtcLevel::kVeryHigh;
  }
  return WebRtcLevel::kHigh;
}

webrtc::AudioProcessing::Config::GainController1::Mode ToWebRtcAgcMode(
    WebRtcProcessorOptions::AgcMode mode) {
  using WebRtcMode = webrtc::AudioProcessing::Config::GainController1::Mode;
  switch (mode) {
    case WebRtcProcessorOptions::AgcMode::kAdaptiveDigital:
      return WebRtcMode::kAdaptiveDigital;
    case WebRtcProcessorOptions::AgcMode::kFixedDigital:
      return WebRtcMode::kFixedDigital;
  }
  return WebRtcMode::kAdaptiveDigital;
}

int16_t ClampToS16(float value) {
  const auto rounded = static_cast<int32_t>(std::lround(value));
  return static_cast<int16_t>(std::max(-32768, std::min(32767, rounded)));
}

}  // namespace

float ComputeRms(const std::vector<int16_t>& samples) {
  if (samples.empty()) {
    return 0.0f;
  }

  double energy = 0.0;
  for (const int16_t sample : samples) {
    const double value = static_cast<double>(sample);
    energy += value * value;
  }
  return static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
}

void ProcessPreAecAutoGain(std::vector<int16_t>* samples,
                           const PreAecAutoGainConfig& config,
                           AutomaticGainState* state) {
  if (!samples || !state || samples->empty() || !config.enabled) {
    return;
  }

  const float rms = ComputeRms(*samples);
  const float safe_target = std::max(1.0f, config.target_rms);
  const float safe_max_gain = std::max(1.0f, config.max_gain);
  float desired_gain = safe_max_gain;
  if (rms > 1.0f) {
    desired_gain = safe_target / rms;
  }
  desired_gain = std::max(1.0f, std::min(safe_max_gain, desired_gain));

  const float smoothing =
      (desired_gain > state->current_gain) ? config.attack : config.release;
  const float bounded_smoothing = std::max(0.0f, std::min(1.0f, smoothing));
  state->current_gain =
      state->current_gain +
      (desired_gain - state->current_gain) * bounded_smoothing;
  state->current_gain = std::max(1.0f, std::min(safe_max_gain, state->current_gain));

  if (state->current_gain >= 0.999f && state->current_gain <= 1.001f) {
    return;
  }

  for (int16_t& sample : *samples) {
    sample = ClampToS16(static_cast<float>(sample) * state->current_gain);
  }
}

WebRtcProcessor::WebRtcProcessor(WebRtcProcessorOptions options)
    : options_(options) {
  webrtc::AudioProcessingBuilder builder;
  apm_.reset(builder.Create());
  if (!apm_) {
    throw std::runtime_error("failed to create WebRTC AudioProcessing module");
  }

  webrtc::AudioProcessing::Config config;
  config.echo_canceller.enabled = true;
  config.echo_canceller.mobile_mode = false;
  config.echo_canceller.enforce_high_pass_filtering = true;
  config.high_pass_filter.enabled = true;
  config.noise_suppression.enabled = true;
  config.noise_suppression.level = ToWebRtcNsLevel(options_.ns_level);
  config.gain_controller1.enabled = true;
  config.gain_controller1.mode = ToWebRtcAgcMode(options_.agc_mode);
  config.gain_controller1.target_level_dbfs = options_.agc_target_level_dbfs;
  config.gain_controller1.compression_gain_db = options_.agc_compression_gain_db;
  config.gain_controller1.enable_limiter = options_.agc_limiter_enabled;
  config.gain_controller1.analog_gain_controller.enabled = false;
  config.gain_controller2.enabled = false;
  apm_->ApplyConfig(config);

  webrtc::ProcessingConfig processing_config;
  processing_config.input_stream() = webrtc::StreamConfig(kSampleRateHz, 1, false);
  processing_config.output_stream() = webrtc::StreamConfig(kSampleRateHz, 1, false);
  processing_config.reverse_input_stream() =
      webrtc::StreamConfig(kSampleRateHz, 1, false);
  processing_config.reverse_output_stream() =
      webrtc::StreamConfig(kSampleRateHz, 1, false);
  CheckWebRtcResult(apm_->Initialize(processing_config),
                    "AudioProcessing::Initialize");
}

WebRtcProcessor::~WebRtcProcessor() = default;

std::vector<int16_t> WebRtcProcessor::Process10Ms(
    const std::vector<int16_t>& mic,
    const std::vector<int16_t>& reference) {
  if (mic.size() != kSamplesPerChannelPerFrame) {
    throw std::invalid_argument("mic frame must contain exactly 10 ms");
  }
  if (reference.size() != kSamplesPerChannelPerFrame) {
    throw std::invalid_argument("reference frame must contain exactly 10 ms");
  }

  std::vector<int16_t> reverse_out(reference.size(), 0);
  const webrtc::StreamConfig stream_config(kSampleRateHz, 1, false);
  CheckWebRtcResult(
      apm_->ProcessReverseStream(reference.data(), stream_config, stream_config,
                                 reverse_out.data()),
      "AudioProcessing::ProcessReverseStream");

  std::vector<int16_t> adjusted_mic = mic;
  ProcessPreAecAutoGain(&adjusted_mic, options_.pre_aec_auto_gain,
                        &pre_aec_gain_state_);

  CheckWebRtcResult(apm_->set_stream_delay_ms(options_.delay_ms),
                    "AudioProcessing::set_stream_delay_ms");
  std::vector<int16_t> processed(adjusted_mic.size(), 0);
  CheckWebRtcResult(apm_->ProcessStream(adjusted_mic.data(), stream_config,
                                        stream_config, processed.data()),
                    "AudioProcessing::ProcessStream");

  timestamp_ += static_cast<uint32_t>(kSamplesPerChannelPerFrame);
  ++frame_id_;

  return processed;
}

void WebRtcProcessor::CheckWebRtcResult(int result, const char* operation) const {
  if (result != webrtc::AudioProcessing::kNoError &&
      result != webrtc::AudioProcessing::kBadStreamParameterWarning) {
    throw std::runtime_error(std::string(operation) +
                             " failed with WebRTC code " +
                             std::to_string(result));
  }
}

}  // namespace audio_processing_module
