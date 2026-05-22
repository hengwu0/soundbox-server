#pragma once

#include "common/audio_frame.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace audio_processing_module {

/// WAV 格式音频文件写入器。
/// 先写占位 header，后续持续追加采样数据，Close 时回填真实的 chunk 大小。
class WavWriter {
 public:
  /// 构造 WAV 写入器：创建输出目录，写入占位 header。
  /// @param path   WAV 输出文件路径。
  /// @param format 音频格式（采样率/通道数/位深）。
  WavWriter(const std::string& path, const AudioFormat& format);
  /// 析构时自动调用 Close() 回填 header 并关闭文件。
  ~WavWriter();

  /// 禁止拷贝。
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  /// 写入 S16 采样数据（vector 重载）。
  void WriteSamples(const std::vector<int16_t>& samples);
  /// 写入 S16 采样数据（指针 + 长度）。
  /// @param samples     采样点指针。
  /// @param sample_count 采样点数量。
  void WriteSamples(const int16_t* samples, size_t sample_count);
  /// 回填 header 并关闭文件，可重复调用（幂等）。
  void Close();

  /// 获取已写入的 PCM 数据字节数（不含 header）。
  uint64_t data_bytes() const { return data_bytes_; }
  /// 获取输出文件路径。
  const std::string& path() const { return path_; }

 private:
  /// 写入 chunk size 为 0 的占位 WAV header。
  void WritePlaceholderHeader();
  /// 用实际数据大小回填 header 中的 RIFF 总大小和 data chunk 大小。
  void PatchHeader();

  std::string path_;           ///< 输出文件路径。
  AudioFormat format_;         ///< 音频格式参数。
  std::ofstream output_;       ///< 二进制输出文件流。
  uint64_t data_bytes_ = 0;    ///< 已写入的 PCM 数据字节数（不含 header）。
  bool closed_ = false;        ///< 标记文件是否已关闭。
};

}  // namespace audio_processing_module