#pragma once

#include "common/audio_frame.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace webrtc {
class AudioProcessing;  ///< 前向声明 WebRTC AudioProcessing 内部类，隐藏头文件依赖。
}  // namespace webrtc

namespace audio_processing_module {

/// AEC 前置自动增益控制（Auto Gain）参数配置。
struct PreAecAutoGainConfig {
  bool enabled = true;        ///< 是否启用前置自动增益。
  float target_rms = 2400.0f; ///< S16 域目标 RMS 值，超过此值不放大。
  float max_gain = 6.0f;      ///< 最大线性增益倍数。
  float attack = 0.35f;       ///< 增益上升（attack）平滑系数，值越小越慢。
  float release = 0.08f;      ///< 增益下降（release）平滑系数，值越小越慢。
};

/// 自动增益控制器运行时状态。
struct AutomaticGainState {
  float current_gain = 1.0f;  ///< 当前应用的线性增益系数。
};

/// WebRTC 音频处理器参数选项。
struct WebRtcProcessorOptions {
  /// 噪声抑制级别枚举。
  enum class NoiseSuppressionLevel {
    kLow,       ///< 低。
    kModerate,  ///< 中等。
    kHigh,      ///< 高。
    kVeryHigh,  ///< 极高。
  };

  /// AGC 模式枚举。
  enum class AgcMode {
    kAdaptiveDigital, ///< 自适应数字 AGC。
    kFixedDigital,    ///< 固定数字 AGC。
  };

  int delay_ms = 2;    ///< 扬声器到麦克风的估计延迟（ms），供 AEC 对齐远近端音频。
  NoiseSuppressionLevel ns_level = NoiseSuppressionLevel::kHigh;  ///< 噪声抑制等级。
  AgcMode agc_mode = AgcMode::kAdaptiveDigital;                   ///< 自动增益控制模式。
  int agc_target_level_dbfs = 3;      ///< AGC 目标输出电平 (dBFS)。
  int agc_compression_gain_db = 9;    ///< AGC 压缩增益上限 (dB)。
  bool agc_limiter_enabled = true;    ///< 是否启用 AGC 限幅器。
  PreAecAutoGainConfig pre_aec_auto_gain; ///< AEC 前置自动增益（调整近端麦克风音量）。
};

/// 计算 S16 采样的 RMS（均方根）值。
/// @param samples S16 采样点向量。
/// @return RMS 值。
float ComputeRms(const std::vector<int16_t>& samples);

/// 对麦克风采样做 AEC 前置自动增益处理，平滑调整音量到目标 RMS。
/// @param samples 输入输出参数，原位修改。
/// @param config  增益控制器配置。
/// @param state   增益控制器运行时状态。
void ProcessPreAecAutoGain(std::vector<int16_t>* samples,
                           const PreAecAutoGainConfig& config,
                           AutomaticGainState* state);

/// WebRTC AudioProcessing 模块的 C++ 封装。
/// 对每 10ms 的单声道麦克风/参考帧做 AEC + NS + AGC 处理。
class WebRtcProcessor {
 public:
  /// 构造并初始化 WebRTC AudioProcessing 实例。
  /// @param options WebRTC 处理参数（NS 等级、AGC 模式等）。
  explicit WebRtcProcessor(WebRtcProcessorOptions options = {});
  ~WebRtcProcessor();

  /// 禁止拷贝。
  WebRtcProcessor(const WebRtcProcessor&) = delete;
  WebRtcProcessor& operator=(const WebRtcProcessor&) = delete;

  /// 处理 10ms 的单声道音频帧（mic + reference），返回处理后的单声道 PCM。
  /// @param mic       近端麦克风采样（160 samples, S16）。
  /// @param reference 远端参考信号（160 samples, S16），如扬声器回采。
  /// @return AEC/NS/AGC 处理后的单声道 S16 采样。
  /// @throws std::invalid_argument 帧大小不为 160 时抛出。
  std::vector<int16_t> Process10Ms(const std::vector<int16_t>& mic,
                                   const std::vector<int16_t>& reference);

 private:
  /// 检查 WebRTC API 返回值，非预期错误时抛出异常。
  /// @param result    WebRTC API 返回码。
  /// @param operation 操作描述（用于异常信息）。
  void CheckWebRtcResult(int result, const char* operation) const;

  std::unique_ptr<webrtc::AudioProcessing> apm_;  ///< WebRTC AudioProcessing 核心实例。
  WebRtcProcessorOptions options_;                  ///< 处理器参数配置。
  AutomaticGainState pre_aec_gain_state_;           ///< 前置自动增益的运行时状态。
  uint32_t timestamp_ = 0;                          ///< 当前音频时间戳（采样点编号）。
  int frame_id_ = 0;                                ///< 当前帧序号，自增。
};

}  // namespace audio_processing_module