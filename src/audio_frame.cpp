#include "audio_processing_module/audio_frame.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace audio_processing_module {

AudioFormat AudioFormat::Stereo16kS16() {
  return AudioFormat{kSampleRateHz, kInputChannels, kBitsPerSample};
}

AudioFormat AudioFormat::Mono16kS16() {
  return AudioFormat{kSampleRateHz, kOutputChannels, kBitsPerSample};
}

size_t AudioFormat::BytesPerSample() const {
  return static_cast<size_t>(bits_per_sample / 8);
}

size_t AudioFormat::ByteRate() const {
  return static_cast<size_t>(sample_rate_hz) * static_cast<size_t>(channels) *
         BytesPerSample();
}

size_t AudioFormat::BlockAlign() const {
  return static_cast<size_t>(channels) * BytesPerSample();
}

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
  split.mic.reserve(frames);
  split.reference.reserve(frames);

  for (size_t frame = 0; frame < frames; ++frame) {
    split.mic.push_back(interleaved[frame * kInputChannels]);
    split.reference.push_back(interleaved[frame * kInputChannels + 1]);
  }

  return split;
}

StereoSplit SplitInterleavedStereoS16(const std::vector<int16_t>& interleaved) {
  return SplitInterleavedStereoS16(interleaved.data(), interleaved.size());
}

std::vector<int16_t> DecodeS16LittleEndian(const uint8_t* bytes,
                                           size_t byte_count) {
  if (bytes == nullptr && byte_count != 0) {
    throw std::invalid_argument("pcm byte pointer is null");
  }
  if (byte_count % kBytesPerSample != 0) {
    throw std::invalid_argument("s16 pcm byte count must be even");
  }

  std::vector<int16_t> samples(byte_count / kBytesPerSample);
  for (size_t index = 0; index < samples.size(); ++index) {
    const uint16_t lo = bytes[index * 2];
    const uint16_t hi = bytes[index * 2 + 1];
    samples[index] = static_cast<int16_t>((hi << 8) | lo);
  }

  return samples;
}

std::vector<uint8_t> EncodeS16LittleEndian(const int16_t* samples,
                                           size_t sample_count) {
  if (samples == nullptr && sample_count != 0) {
    throw std::invalid_argument("pcm sample pointer is null");
  }

  std::vector<uint8_t> bytes(sample_count * kBytesPerSample);
  for (size_t index = 0; index < sample_count; ++index) {
    const auto value = static_cast<uint16_t>(samples[index]);
    bytes[index * 2] = static_cast<uint8_t>(value & 0xff);
    bytes[index * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
  }

  return bytes;
}

std::vector<uint8_t> EncodeS16LittleEndian(const std::vector<int16_t>& samples) {
  return EncodeS16LittleEndian(samples.data(), samples.size());
}

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
