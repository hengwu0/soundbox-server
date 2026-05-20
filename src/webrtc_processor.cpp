#include "audio_processing_module/webrtc_processor.hpp"

#include "webrtc/common.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/interface/module_common_types.h"

#include <stdexcept>
#include <string>

namespace audio_processing_module {

WebRtcProcessor::WebRtcProcessor() {
  webrtc::Config config;
  config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(true));
  config.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(true));

  apm_.reset(webrtc::AudioProcessing::Create(config));
  if (!apm_) {
    throw std::runtime_error("failed to create WebRTC AudioProcessing module");
  }

  CheckWebRtcResult(
      apm_->Initialize(kSampleRateHz, kSampleRateHz, kSampleRateHz,
                       webrtc::AudioProcessing::kMono,
                       webrtc::AudioProcessing::kMono,
                       webrtc::AudioProcessing::kMono),
      "AudioProcessing::Initialize");

  CheckWebRtcResult(apm_->high_pass_filter()->Enable(true),
                    "HighPassFilter::Enable");
  CheckWebRtcResult(apm_->echo_cancellation()->enable_drift_compensation(false),
                    "EchoCancellation::enable_drift_compensation");
  CheckWebRtcResult(
      apm_->echo_cancellation()->set_suppression_level(
          webrtc::EchoCancellation::kHighSuppression),
      "EchoCancellation::set_suppression_level");
  CheckWebRtcResult(apm_->echo_cancellation()->Enable(true),
                    "EchoCancellation::Enable");
  CheckWebRtcResult(
      apm_->noise_suppression()->set_level(webrtc::NoiseSuppression::kHigh),
      "NoiseSuppression::set_level");
  CheckWebRtcResult(apm_->noise_suppression()->Enable(true),
                    "NoiseSuppression::Enable");
  CheckWebRtcResult(apm_->gain_control()->set_mode(webrtc::GainControl::kFixedDigital),
                    "GainControl::set_mode");
  CheckWebRtcResult(apm_->gain_control()->set_target_level_dbfs(3),
                    "GainControl::set_target_level_dbfs");
  CheckWebRtcResult(apm_->gain_control()->set_compression_gain_db(9),
                    "GainControl::set_compression_gain_db");
  CheckWebRtcResult(apm_->gain_control()->enable_limiter(true),
                    "GainControl::enable_limiter");
  CheckWebRtcResult(apm_->gain_control()->Enable(true), "GainControl::Enable");
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

  webrtc::AudioFrame reverse_frame;
  reverse_frame.UpdateFrame(frame_id_, timestamp_, reference.data(),
                            kSamplesPerChannelPerFrame, kSampleRateHz,
                            webrtc::AudioFrame::kNormalSpeech,
                            webrtc::AudioFrame::kVadUnknown, kOutputChannels);
  CheckWebRtcResult(apm_->AnalyzeReverseStream(&reverse_frame),
                    "AudioProcessing::AnalyzeReverseStream");

  webrtc::AudioFrame capture_frame;
  capture_frame.UpdateFrame(frame_id_, timestamp_, mic.data(),
                            kSamplesPerChannelPerFrame, kSampleRateHz,
                            webrtc::AudioFrame::kNormalSpeech,
                            webrtc::AudioFrame::kVadUnknown, kOutputChannels);

  CheckWebRtcResult(apm_->set_stream_delay_ms(0),
                    "AudioProcessing::set_stream_delay_ms");
  CheckWebRtcResult(apm_->ProcessStream(&capture_frame),
                    "AudioProcessing::ProcessStream");

  timestamp_ += static_cast<uint32_t>(kSamplesPerChannelPerFrame);
  ++frame_id_;

  return std::vector<int16_t>(
      capture_frame.data_,
      capture_frame.data_ + capture_frame.samples_per_channel_);
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
