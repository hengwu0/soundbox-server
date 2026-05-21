#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audio_processing_module {

inline constexpr int kSampleRateHz = 16000;
inline constexpr int kBitsPerSample = 16;
inline constexpr int kInputChannels = 2;
inline constexpr int kOutputChannels = 1;
inline constexpr int kFrameDurationMs = 10;
inline constexpr size_t kSamplesPerChannelPerFrame =
    static_cast<size_t>(kSampleRateHz * kFrameDurationMs / 1000);
inline constexpr size_t kInputSamplesPerFrame =
    kSamplesPerChannelPerFrame * static_cast<size_t>(kInputChannels);
inline constexpr size_t kOutputSamplesPerFrame =
    kSamplesPerChannelPerFrame * static_cast<size_t>(kOutputChannels);
inline constexpr size_t kBytesPerSample = kBitsPerSample / 8;
inline constexpr size_t kInputBytesPerFrame =
    kInputSamplesPerFrame * kBytesPerSample;
inline constexpr size_t kOutputBytesPerFrame =
    kOutputSamplesPerFrame * kBytesPerSample;

struct AudioFormat {
  int sample_rate_hz;
  int channels;
  int bits_per_sample;

  static AudioFormat Stereo16kS16();
  static AudioFormat Mono16kS16();

  size_t BytesPerSample() const;
  size_t ByteRate() const;
  size_t BlockAlign() const;
};

struct StereoSplit {
  std::vector<int16_t> mic;
  std::vector<int16_t> reference;
};

StereoSplit SplitInterleavedStereoS16(const int16_t* interleaved,
                                      size_t interleaved_sample_count);

StereoSplit SplitInterleavedStereoS16(const std::vector<int16_t>& interleaved);

std::vector<int16_t> DecodeS16LittleEndian(const uint8_t* bytes,
                                           size_t byte_count);

std::vector<uint8_t> EncodeS16LittleEndian(const int16_t* samples,
                                           size_t sample_count);

std::vector<uint8_t> EncodeS16LittleEndian(const std::vector<int16_t>& samples);

std::string FormatDurationFromPcmBytes(uint64_t byte_count,
                                       const AudioFormat& format);

}  // namespace audio_processing_module
