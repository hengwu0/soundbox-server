#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audio_processing_module {

/// 系统默认采样率：16kHz。
inline constexpr int kSampleRateHz = 16000;
/// 每采样点位深：16 bit。
inline constexpr int kBitsPerSample = 16;
/// 输入端通道数：2（立体声，左声道 mic + 右声道 reference 参考信号）。
inline constexpr int kInputChannels = 2;
/// 输出端通道数：1（单声道，AEC 处理后）。
inline constexpr int kOutputChannels = 1;
/// 每帧时长：10 毫秒。
inline constexpr int kFrameDurationMs = 10;
/// 每帧每通道采样数 = 16000 * 10 / 1000 = 160。
inline constexpr size_t kSamplesPerChannelPerFrame =
    static_cast<size_t>(kSampleRateHz * kFrameDurationMs / 1000);
/// 输入端每帧采样数（立体声）= 160 * 2 = 320。
inline constexpr size_t kInputSamplesPerFrame =
    kSamplesPerChannelPerFrame * static_cast<size_t>(kInputChannels);
/// 输出端每帧采样数（单声道）= 160 * 1 = 160。
inline constexpr size_t kOutputSamplesPerFrame =
    kSamplesPerChannelPerFrame * static_cast<size_t>(kOutputChannels);
/// 每采样点字节数 = 16 / 8 = 2。
inline constexpr size_t kBytesPerSample = kBitsPerSample / 8;
/// 输入端每帧字节数 = 320 * 2 = 640。
inline constexpr size_t kInputBytesPerFrame =
    kInputSamplesPerFrame * kBytesPerSample;
/// 输出端每帧字节数 = 160 * 2 = 320。
inline constexpr size_t kOutputBytesPerFrame =
    kOutputSamplesPerFrame * kBytesPerSample;

/// 描述一种音频格式：采样率、通道数、位深。
struct AudioFormat {
  int sample_rate_hz;   ///< 采样率 (Hz)。
  int channels;         ///< 通道数。
  int bits_per_sample;  ///< 每采样点位深 (bits)。

  /// 工厂方法：立体声 16kHz 16-bit PCM。
  static AudioFormat Stereo16kS16();
  /// 工厂方法：单声道 16kHz 16-bit PCM。
  static AudioFormat Mono16kS16();

  /// 计算每采样点字节数（bits / 8）。
  size_t BytesPerSample() const;
  /// 计算每秒字节率（采样率 x 通道数 x 每采样字节数）。
  size_t ByteRate() const;
  /// 计算块对齐（所有通道单帧的字节数 = channels * BytesPerSample）。
  size_t BlockAlign() const;
};

/// 双声道分离结果：mic（左声道，待处理信号）和 reference（右声道，参考信号）。
struct StereoSplit {
  std::vector<int16_t> mic;        ///< 左声道：麦克风采集信号（给 AEC 的近端信号）。
  std::vector<int16_t> reference;  ///< 右声道：扬声器回采信号（给 AEC 的远端参考信号）。
};

/// 将交织的双声道 S16 PCM 分离为独立左右声道。
/// @param interleaved              交织采样数据指针。
/// @param interleaved_sample_count 交织采样点总数（偶数）。
/// @return 分离后的 StereoSplit 结构。
StereoSplit SplitInterleavedStereoS16(const int16_t* interleaved,
                                      size_t interleaved_sample_count);

/// 将交织双声道分离（vector 重载）。
StereoSplit SplitInterleavedStereoS16(const std::vector<int16_t>& interleaved);

/// 将小端 S16 字节流解码为 int16_t 向量。
/// @param bytes      原始字节数据。
/// @param byte_count 字节数（偶数）。
/// @return S16 采样点向量。
std::vector<int16_t> DecodeS16LittleEndian(const uint8_t* bytes,
                                           size_t byte_count);

/// 将 int16_t 采样点编码为小端字节流。
/// @param samples     采样点指针。
/// @param sample_count 采样点数量。
/// @return 小端字节流。
std::vector<uint8_t> EncodeS16LittleEndian(const int16_t* samples,
                                           size_t sample_count);

/// 编码 S16 向量为小端字节流（vector 重载）。
std::vector<uint8_t> EncodeS16LittleEndian(const std::vector<int16_t>& samples);

/// 根据 PCM 字节数和音频格式计算时长字符串（格式如 "1.234s"）。
/// @param byte_count PCM 字节数。
/// @param format     音频格式。
/// @return 可读的时长字符串。
std::string FormatDurationFromPcmBytes(uint64_t byte_count,
                                       const AudioFormat& format);

}  // namespace audio_processing_module