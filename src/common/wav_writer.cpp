#include "common/wav_writer.hpp"

#include <filesystem>
#include <limits>
#include <stdexcept>

namespace audio_processing_module {
namespace {

/// 写入固定长度的 ASCII 字符串到输出流，用于写 WAV RIFF chunk ID。
/// @param output 输出文件流。
/// @param text   待写入的字符串指针。
/// @param length 字符串字节长度。
void WriteAscii(std::ofstream& output, const char* text, size_t length) {
  output.write(text, static_cast<std::streamsize>(length));
}

/// 按小端字节序写入一个数值到输出流。
/// @tparam T    数值类型（uint16_t / uint32_t 等）。
/// @param output 输出文件流。
/// @param value  待写入的数值。
template <typename T>
void WriteLittleEndian(std::ofstream& output, T value) {
  for (size_t index = 0; index < sizeof(T); ++index) {
    const auto byte = static_cast<char>((value >> (8 * index)) & 0xff);
    output.write(&byte, 1);
  }
}

/// 将 uint64_t 转为 uint32_t，溢出时抛出异常。
/// @param value      源数值。
/// @param field_name 字段名（用于错误提示）。
/// @return 裁剪后的 uint32_t 值。
/// @throws std::runtime_error 值超出 uint32_t 范围时抛出。
uint32_t CheckedU32(uint64_t value, const char* field_name) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("wav field exceeds u32: ") + field_name);
  }
  return static_cast<uint32_t>(value);
}

}  // namespace

/// 构造 WAV 写入器：先写占位 header，后续 Close 时回填真实大小。
/// @param path   WAV 输出文件路径。
/// @param format 音频格式（采样率/通道数/位深）。
WavWriter::WavWriter(const std::string& path, const AudioFormat& format)
    : path_(path), format_(format) {
  // 确保输出目录存在
  const std::filesystem::path output_path(path_);
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }

  output_.open(path_, std::ios::binary | std::ios::trunc);
  if (!output_) {
    throw std::runtime_error("failed to open wav output: " + path_);
  }

  // 先写入 chunk size 为 0 的占位 header
  WritePlaceholderHeader();
}

/// 析构时自动补写 header 并关闭文件。
WavWriter::~WavWriter() {
  if (!closed_) {
    try {
      Close();
    } catch (...) {
      // 析构函数中忽略异常，避免 terminate
    }
  }
}

/// 写入一组 S16 采样数据（vector 重载）。
/// @param samples 待写入的 S16 采样点。
void WavWriter::WriteSamples(const std::vector<int16_t>& samples) {
  WriteSamples(samples.data(), samples.size());
}

/// 写入一组 S16 采样数据，内部编码为小端字节流后写入文件。
/// @param samples     待写入的 S16 采样点指针。
/// @param sample_count 采样点数量。
/// @throws std::runtime_error 文件状态异常时抛出。
void WavWriter::WriteSamples(const int16_t* samples, size_t sample_count) {
  if (closed_) {
    throw std::runtime_error("cannot write to closed wav file");
  }
  if (samples == nullptr && sample_count != 0) {
    throw std::invalid_argument("wav samples pointer is null");
  }

  // 编码为小端字节流
  const std::vector<uint8_t> bytes = EncodeS16LittleEndian(samples, sample_count);
  output_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
  if (!output_) {
    throw std::runtime_error("failed to write wav samples: " + path_);
  }

  // 累加已写入的数据字节数，Close 时回填 header 需要
  data_bytes_ += bytes.size();
}

/// 关闭 WAV 文件并回填 header 中的 chunk 大小。
void WavWriter::Close() {
  if (closed_) {
    return;
  }

  // 用实际数据大小更新 header
  PatchHeader();
  output_.close();
  closed_ = true;
}

/// 写入一个占位 WAV header，chunk size 字段填 0，Close 时回填。
void WavWriter::WritePlaceholderHeader() {
  // RIFF chunk
  WriteAscii(output_, "RIFF", 4);
  WriteLittleEndian<uint32_t>(output_, 0);  ///< 文件总大小 - 8，占位。
  WriteAscii(output_, "WAVE", 4);

  // fmt  sub-chunk
  WriteAscii(output_, "fmt ", 4);
  WriteLittleEndian<uint32_t>(output_, 16);  ///< fmt chunk 长度，PCM 固定 16。
  WriteLittleEndian<uint16_t>(output_, 1);   ///< 音频格式，1 = PCM。
  WriteLittleEndian<uint16_t>(output_, static_cast<uint16_t>(format_.channels));
  WriteLittleEndian<uint32_t>(output_, static_cast<uint32_t>(format_.sample_rate_hz));
  WriteLittleEndian<uint32_t>(output_, static_cast<uint32_t>(format_.ByteRate()));
  WriteLittleEndian<uint16_t>(output_, static_cast<uint16_t>(format_.BlockAlign()));
  WriteLittleEndian<uint16_t>(output_, static_cast<uint16_t>(format_.bits_per_sample));

  // data sub-chunk
  WriteAscii(output_, "data", 4);
  WriteLittleEndian<uint32_t>(output_, 0);  ///< 音频数据大小，占位。
}

/// 回填 WAV header 中的 RIFF 总大小和 data chunk 大小。
void WavWriter::PatchHeader() {
  output_.flush();
  if (!output_) {
    throw std::runtime_error("failed to flush wav output: " + path_);
  }

  // 回填 RIFF chunk 大小（文件总大小 - 8）
  output_.seekp(4, std::ios::beg);
  WriteLittleEndian<uint32_t>(output_, CheckedU32(36 + data_bytes_, "riff size"));

  // 回填 data chunk 大小
  output_.seekp(40, std::ios::beg);
  WriteLittleEndian<uint32_t>(output_, CheckedU32(data_bytes_, "data size"));
}

}  // namespace audio_processing_module