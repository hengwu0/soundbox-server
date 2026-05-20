#pragma once

#include "audio_processing_module/audio_frame.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace audio_processing_module {

class WavWriter {
 public:
  WavWriter(const std::string& path, const AudioFormat& format);
  ~WavWriter();

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  void WriteSamples(const std::vector<int16_t>& samples);
  void WriteSamples(const int16_t* samples, size_t sample_count);
  void Close();

  uint64_t data_bytes() const { return data_bytes_; }
  const std::string& path() const { return path_; }

 private:
  void WritePlaceholderHeader();
  void PatchHeader();

  std::string path_;
  AudioFormat format_;
  std::ofstream output_;
  uint64_t data_bytes_ = 0;
  bool closed_ = false;
};

}  // namespace audio_processing_module
