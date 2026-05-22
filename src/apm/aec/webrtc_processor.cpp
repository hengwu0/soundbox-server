#include "apm/aec/webrtc_processor.hpp"

#include "modules/audio_processing/include/audio_processing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace audio_processing_module {
namespace {

/// 将本模块的 NoiseSuppressionLevel 枚举转换为 WebRTC 底层枚举。
/// @param level 本模块定义的噪声抑制级别。
/// @return WebRTC 底层对应的 NoiseSuppression::Level。
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
  return WebRtcLevel::kHigh;  ///< 默认值保护。
}

/// 将本模块的 AgcMode 枚举转换为 WebRTC 底层枚举。
/// @param mode 本模块定义的 AGC 模式。
/// @return WebRTC 底层对应的 GainController1::Mode。
webrtc::AudioProcessing::Config::GainController1::Mode ToWebRtcAgcMode(
    WebRtcProcessorOptions::AgcMode mode) {
  using WebRtcMode = webrtc::AudioProcessing::Config::GainController1::Mode;
  switch (mode) {
    case WebRtcProcessorOptions::AgcMode::kAdaptiveDigital:
      return WebRtcMode::kAdaptiveDigital;
    case WebRtcProcessorOptions::AgcMode::kFixedDigital:
      return WebRtcMode::kFixedDigital;
  }
  return WebRtcMode::kAdaptiveDigital;  ///< 默认值保护。
}

/// 将浮点数限幅到 S16 范围 [-32768, 32767]。
/// @param value 原始浮点采样值。
/// @return 限幅并四舍五入后的 int16_t 值。
int16_t ClampToS16(float value) {
  const auto rounded = static_cast<int32_t>(std::lround(value));
  return static_cast<int16_t>(std::max(-32768, std::min(32767, rounded)));
}

}  // namespace

/// 计算 S16 采样的 RMS 值，用于自动增益控制的输入信号强度评估。
/// @param samples S16 采样点向量。
/// @return RMS 值（float），空向量返回 0。
float ComputeRms(const std::vector<int16_t>& samples) {
  if (samples.empty()) {
    return 0.0f;
  }

  // 计算总能量（浮点精度，避免 S16 平方溢出）
  double energy = 0.0;
  for (const int16_t sample : samples) {
    const double value = static_cast<double>(sample);
    energy += value * value;
  }
  return static_cast<float>(std::sqrt(energy / static_cast<double>(samples.size())));
}

/// 对麦克风采样进行 AEC 前置自动增益处理。
/// 当 RMS 低于目标时放大，高于目标时不做放大处理。
/// 使用带 attack/release 平滑的增益变化，避免音量突变引入失真。
/// @param samples 输入输出参数，S16 采样。空或增益无效时不做处理。
/// @param config  AEC 前置自动增益配置（启用开关、目标 RMS、最大增益等）。
/// @param state   跨帧保存的当前增益状态。
void ProcessPreAecAutoGain(std::vector<int16_t>* samples,
                            const PreAecAutoGainConfig& config,
                            AutomaticGainState* state) {
  if (!samples || !state || samples->empty() || !config.enabled) {
    return;  ///< 配置禁用或参数无效时退出。
  }

  const float rms = ComputeRms(*samples);
  const float safe_target = std::max(1.0f, config.target_rms);   ///< 目标 RMS 至少为 1，防除零。
  const float safe_max_gain = std::max(1.0f, config.max_gain);   ///< 最大增益至少为 1。
  float desired_gain = safe_max_gain;                             ///< 期望增益，默认最大。
  if (rms > 1.0f) {
    desired_gain = safe_target / rms;  ///< 以目标 RMS 反算增益。
  }
  desired_gain = std::max(1.0f, std::min(safe_max_gain, desired_gain));  ///< 限幅到 [1, max]。

  // 根据增益的需要升/降选择 attack 或 release 平滑系数
  const float smoothing =
      (desired_gain > state->current_gain) ? config.attack : config.release;
  const float bounded_smoothing = std::max(0.0f, std::min(1.0f, smoothing));
  state->current_gain =
      state->current_gain +
      (desired_gain - state->current_gain) * bounded_smoothing;
  state->current_gain = std::max(1.0f, std::min(safe_max_gain, state->current_gain));

  // 如果增益非常接近 1.0，直接跳过乘算以省计算
  if (state->current_gain >= 0.999f && state->current_gain <= 1.001f) {
    return;
  }

  // 对每个采样点乘以当前增益并限幅
  for (int16_t& sample : *samples) {
    sample = ClampToS16(static_cast<float>(sample) * state->current_gain);
  }
}

/// 构造 WebRtcProcessor 并初始化底层 AudioProcessing 模块。
/// @param options 处理器参数。
WebRtcProcessor::WebRtcProcessor(WebRtcProcessorOptions options)
    : options_(options) {
  webrtc::AudioProcessingBuilder builder;
  apm_.reset(builder.Create());
  if (!apm_) {
    throw std::runtime_error("failed to create WebRTC AudioProcessing module");
  }

  // 配置 WebRTC 处理管线
  webrtc::AudioProcessing::Config config;
  config.echo_canceller.enabled = true;                           ///< 启用回声消除 (AEC)。
  config.echo_canceller.mobile_mode = false;                      ///< 非手机模式。
  config.echo_canceller.enforce_high_pass_filtering = true;       ///< 强制高通滤波。
  config.high_pass_filter.enabled = true;                         ///< 启用高通滤波器。
  config.noise_suppression.enabled = true;                        ///< 启用噪声抑制 (NS)。
  config.noise_suppression.level = ToWebRtcNsLevel(options_.ns_level);  ///< NS 级别。
  config.gain_controller1.enabled = true;                         ///< 启用自动增益控制 (AGC)。
  config.gain_controller1.mode = ToWebRtcAgcMode(options_.agc_mode);   ///< AGC 模式。
  config.gain_controller1.target_level_dbfs = options_.agc_target_level_dbfs;
  config.gain_controller1.compression_gain_db = options_.agc_compression_gain_db;
  config.gain_controller1.enable_limiter = options_.agc_limiter_enabled;
  config.gain_controller1.analog_gain_controller.enabled = false;  ///< 禁用模拟 AGC。
  config.gain_controller2.enabled = false;                         ///< 不启用 AGC2 新模块。
  apm_->ApplyConfig(config);

  // 初始化流配置：单声道 16kHz 的正向和反向流
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

/// 析构 WebRtcProcessor。
WebRtcProcessor::~WebRtcProcessor() = default;

/// 以 10ms 帧送入麦克风和参考信号进行 AEC 联合处理。
/// 处理顺序：反向流（参考信号）-> 前置自动增益 -> 设置延迟 -> 正向流（麦克风）。
/// @param mic       近端麦克风 10ms 单声道采样。
/// @param reference 远端扬声器回采 10ms 单声道采样（反向流）。
/// @return AEC + NS + AGC 处理后的 10ms 单声道采样。
std::vector<int16_t> WebRtcProcessor::Process10Ms(
    const std::vector<int16_t>& mic,
    const std::vector<int16_t>& reference) {
  if (mic.size() != kSamplesPerChannelPerFrame) {
    throw std::invalid_argument("mic frame must contain exactly 10 ms");
  }
  if (reference.size() != kSamplesPerChannelPerFrame) {
    throw std::invalid_argument("reference frame must contain exactly 10 ms");
  }

  // 1. 送入反向流（远端参考信号），WebRTC 内部记录扬声器内容做回声消除
  std::vector<int16_t> reverse_out(reference.size(), 0);
  const webrtc::StreamConfig stream_config(kSampleRateHz, 1, false);
  CheckWebRtcResult(
      apm_->ProcessReverseStream(reference.data(), stream_config, stream_config,
                                 reverse_out.data()),
      "AudioProcessing::ProcessReverseStream");

  // 2. AEC 前置自动增益：调整麦克风音量以优化 AEC 输入信号质量
  std::vector<int16_t> adjusted_mic = mic;
  ProcessPreAecAutoGain(&adjusted_mic, options_.pre_aec_auto_gain,
                        &pre_aec_gain_state_);

  // 3. 设置远近端延迟（ms），告知 AEC 对齐窗口
  CheckWebRtcResult(apm_->set_stream_delay_ms(options_.delay_ms),
                    "AudioProcessing::set_stream_delay_ms");
  // 4. 送入正向流（麦克风信号），经过 AEC/NS/AGC 管线处理
  std::vector<int16_t> processed(adjusted_mic.size(), 0);
  CheckWebRtcResult(apm_->ProcessStream(adjusted_mic.data(), stream_config,
                                        stream_config, processed.data()),
                    "AudioProcessing::ProcessStream");

  // 更新时间戳和帧序号
  timestamp_ += static_cast<uint32_t>(kSamplesPerChannelPerFrame);
  ++frame_id_;

  return processed;
}

/// 检查 WebRTC API 调用结果，非预期错误码时抛出异常。
/// @param result    底层 API 返回码。
/// @param operation 当前操作名，用于异常消息。
void WebRtcProcessor::CheckWebRtcResult(int result, const char* operation) const {
  if (result != webrtc::AudioProcessing::kNoError &&
      result != webrtc::AudioProcessing::kBadStreamParameterWarning) {
    throw std::runtime_error(std::string(operation) +
                             " failed with WebRTC code " +
                             std::to_string(result));
  }
}

}  // namespace audio_processing_module