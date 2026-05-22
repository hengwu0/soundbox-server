#include "common/audio_frame.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace audio_processing_module {

/// 返回立体声 16kHz S16 的音频格式描述。
/// @return AudioFormat 对象，channel=2, rate=16000, bits=16。
AudioFormat AudioFormat::Stereo16kS16() {
  return AudioFormat{kSampleRateHz, kInputChannels, kBitsPerSample};
}

/// 返回单声道 16kHz S16 的音频格式描述。
/// @return AudioFormat 对象，channel=1, rate=16000, bits=16。
AudioFormat AudioFormat::Mono16kS16() {
  return AudioFormat{kSampleRateHz, kOutputChannels, kBitsPerSample};
}

/// 计算每采样点字节数（bits / 8）。
/// @return 每采样点字节数。
size_t AudioFormat::BytesPerSample() const {
  return static_cast<size_t>(bits_per_sample / 8);
}

/// 计算每秒字节率（采样率 x 通道数 x 每采样点字节数）。
/// @return 每秒数据字节数。
size_t AudioFormat::ByteRate() const {
  return static_cast<size_t>(sample_rate_hz) * static_cast<size_t>(channels) *
         BytesPerSample();
}

/// 计算块对齐（单帧所有通道的字节总数 = channels * bytes_per_sample）。
/// @return 块对齐字节数。
size_t AudioFormat::BlockAlign() const {
  return static_cast<size_t>(channels) * BytesPerSample();
}

/// 将交织的双声道 S16 PCM 分离为独立的 mic（左）和 reference（右）两个单声道。
/// @param interleaved              交织采样数据指针。
/// @param interleaved_sample_count 交织采样点总数（必须为 2 的倍数）。
/// @return 分离后的 StereoSplit 结构。
/// @throws std::invalid_argument 指针为空或采样数非法时抛出。
StereoSplit SplitInterleavedStereoS16(const int16_t* interleaved,
                                       size_t interleaved_sample_count) {
  if (interleaved == nullptr && interleaved_sample_count != 0) {
    throw std::invalid_argument("interleaved sample pointer is null");
  }
  if (interleaved_sample_count % kInputChannels != 0) {
    throw std::invalid_argument("stereo sample count must be divisible by 2");
  }

  const size_t frames = interleaved_sample_count / kInputChannels;
  StereoSplit split;
  split.mic.reserve(frames);       ///< 左声道 = 麦克风（待处理信号）。
  split.reference.reserve(frames); ///< 右声道 = 参考信号（扬声器回采）。

  // 按帧分离：偶数索引为 mic，奇数索引为 reference
  for (size_t frame = 0; frame < frames; ++frame) {
    split.mic.push_back(interleaved[frame * kInputChannels]);
    split.reference.push_back(interleaved[frame * kInputChannels + 1]);
  }

  return split;
}

/// 分离交织立体声（vector 重载）。
/// @param interleaved 交织采样数据。
/// @return 分离后的 StereoSplit 结构。
StereoSplit SplitInterleavedStereoS16(const std::vector<int16_t>& interleaved) {
  return SplitInterleavedStereoS16(interleaved.data(), interleaved.size());
}

/// 将小端字节流解码为 S16 采样点数组。
/// @param bytes      原始字节数据。
/// @param byte_count 字节数（必须为偶数）。
/// @return S16 采样点向量。
/// @throws std::invalid_argument 指针为空或字节数非法时抛出。
std::vector<int16_t> DecodeS16LittleEndian(const uint8_t* bytes,
                                            size_t byte_count) {
  if (bytes == nullptr && byte_count != 0) {
    throw std::invalid_argument("pcm byte pointer is null");
  }
  if (byte_count % kBytesPerSample != 0) {
    throw std::invalid_argument("s16 pcm byte count must be even");
  }

  std::vector<int16_t> samples(byte_count / kBytesPerSample);
  // 每 2 字节组装一个 int16_t（小端序：低字节在前）
  for (size_t index = 0; index < samples.size(); ++index) {
    const uint16_t lo = bytes[index * 2];
    const uint16_t hi = bytes[index * 2 + 1];
    samples[index] = static_cast<int16_t>((hi << 8) | lo);
  }

  return samples;
}

/// 将 S16 采样点编码为小端字节流。
/// @param samples      S16 采样点指针。
/// @param sample_count 采样点数量。
/// @return 编码后的小端字节流。
/// @throws std::invalid_argument 指针为空时抛出。
std::vector<uint8_t> EncodeS16LittleEndian(const int16_t* samples,
                                            size_t sample_count) {
  if (samples == nullptr && sample_count != 0) {
    throw std::invalid_argument("pcm sample pointer is null");
  }

  std::vector<uint8_t> bytes(sample_count * kBytesPerSample);
  // 每个 int16_t 拆为低字节和高字节（小端序）
  for (size_t index = 0; index < sample_count; ++index) {
    const auto value = static_cast<uint16_t>(samples[index]);
    bytes[index * 2] = static_cast<uint8_t>(value & 0xff);
    bytes[index * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
  }

  return bytes;
}

/// 编码 S16 向量为小端字节流（vector 重载）。
/// @param samples S16 采样点向量。
/// @return 编码后的小端字节流。
std::vector<uint8_t> EncodeS16LittleEndian(const std::vector<int16_t>& samples) {
  return EncodeS16LittleEndian(samples.data(), samples.size());
}

/// 根据 PCM 字节数和音频格式计算对应的时长字符串。
/// @param byte_count PCM 数据字节数。
/// @param format     音频格式。
/// @return 格式如 "1.234s" 的时长字符串。
std::string FormatDurationFromPcmBytes(uint64_t byte_count,
                                        const AudioFormat& format) {
  if (format.ByteRate() == 0) {
    return "0.000s";
  }

  const double seconds =
      static_cast<double>(byte_count) / static_cast<double>(format.ByteRate());
  std::ostringstream output;
  output << std::fixed << std::setprecision(3) << seconds << "s";
  return output.str();
}

}  // namespace audio_processing_module