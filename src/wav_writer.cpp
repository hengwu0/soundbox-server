#include "audio_processing_module/wav_writer.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>

namespace audio_processing_module {
namespace {

void WriteAscii(std::ofstream& output, const char* text, size_t length) {
  output.write(text, static_cast<std::streamsize>(length));
}

template <typename T>
void WriteLittleEndian(std::ofstream& output, T value) {
  for (size_t index = 0; index < sizeof(T); ++index) {
    const auto byte = static_cast<char>((value >> (8 * index)) & 0xff);
    output.write(&byte, 1);
  }
}

uint32_t CheckedU32(uint64_t value, const char* field_name) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("wav field exceeds u32: ") + field_name);
  }
  return static_cast<uint32_t>(value);
}

}  // namespace

WavWriter::WavWriter(const std::string& path, const AudioFormat& format)
    : path_(path), format_(format) {
  const std::filesystem::path output_path(path_);
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }

  output_.open(path_, std::ios::binary | std::ios::trunc);
  if (!output_) {
    throw std::runtime_error("failed to open wav output: " + path_);
  }

  WritePlaceholderHeader();
}

WavWriter::~WavWriter() {
  if (!closed_) {
    try {
      Close();
    } catch (...) {
    }
  }
}

void WavWriter::WriteSamples(const std::vector<int16_t>& samples) {
  WriteSamples(samples.data(), samples.size());
}

void WavWriter::WriteSamples(const int16_t* samples, size_t sample_count) {
  if (closed_) {
    throw std::runtime_error("cannot write to closed wav file");
  }
  if (samples == nullptr && sample_count != 0) {
    throw std::invalid_argument("wav samples pointer is null");
  }

  const std::vector<uint8_t> bytes = EncodeS16LittleEndian(samples, sample_count);
  output_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  if (!output_) {
    throw std::runtime_error("failed to write wav samples: " + path_);
  }

  data_bytes_ += bytes.size();
}

void WavWriter::Close() {
  if (closed_) {
    return;
  }

  PatchHeader();
  output_.close();
  closed_ = true;
}

void WavWriter::WritePlaceholderHeader() {
  WriteAscii(output_, "RIFF", 4);
  WriteLittleEndian<uint32_t>(output_, 0);
  WriteAscii(output_, "WAVE", 4);
  WriteAscii(output_, "fmt ", 4);
  WriteLittleEndian<uint32_t>(output_, 16);
  WriteLittleEndian<uint16_t>(output_, 1);
  WriteLittleEndian<uint16_t>(output_, static_cast<uint16_t>(format_.channels));
  WriteLittleEndian<uint32_t>(output_, static_cast<uint32_t>(format_.sample_rate_hz));
  WriteLittleEndian<uint32_t>(output_, static_cast<uint32_t>(format_.ByteRate()));
  WriteLittleEndian<uint16_t>(output_, static_cast<uint16_t>(format_.BlockAlign()));
  WriteLittleEndian<uint16_t>(output_, static_cast<uint16_t>(format_.bits_per_sample));
  WriteAscii(output_, "data", 4);
  WriteLittleEndian<uint32_t>(output_, 0);
}

void WavWriter::PatchHeader() {
  output_.flush();
  if (!output_) {
    throw std::runtime_error("failed to flush wav output: " + path_);
  }

  output_.seekp(4, std::ios::beg);
  WriteLittleEndian<uint32_t>(output_, CheckedU32(36 + data_bytes_, "riff size"));

  output_.seekp(40, std::ios::beg);
  WriteLittleEndian<uint32_t>(output_, CheckedU32(data_bytes_, "data size"));
}

}  // namespace audio_processing_module
