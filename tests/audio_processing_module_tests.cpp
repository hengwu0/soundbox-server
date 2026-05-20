#include "audio_processing_module/audio_frame.hpp"
#include "audio_processing_module/wav_writer.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using audio_processing_module::AudioFormat;
using audio_processing_module::SplitInterleavedStereoS16;
using audio_processing_module::StereoSplit;
using audio_processing_module::WavWriter;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T>
T ReadLittleEndian(const std::vector<uint8_t>& bytes, size_t offset) {
  T value = 0;
  for (size_t index = 0; index < sizeof(T); ++index) {
    value |= static_cast<T>(bytes.at(offset + index)) << (8 * index);
  }
  return value;
}

void TestStereoFrameSplitKeepsMicAndReferenceOrder() {
  // 验证 2ch/16bit 交错 PCM 会被稳定拆成 mic/ref 两路单声道。
  const std::vector<int16_t> interleaved = {
      10, 100,
      20, 200,
      30, 300,
      40, 400,
  };

  const StereoSplit split = SplitInterleavedStereoS16(interleaved);

  Require(split.mic == std::vector<int16_t>({10, 20, 30, 40}),
          "mic channel was not split from channel 0");
  Require(split.reference == std::vector<int16_t>({100, 200, 300, 400}),
          "reference channel was not split from channel 1");
}

void TestWavWriterPatchesHeaderAfterStreamingSamples() {
  // 验证 WAV sink 能先流式写样本，关闭时再回填 RIFF/data 长度。
  const std::string path = "/tmp/audio_processing_module_test.wav";
  {
    WavWriter writer(path, AudioFormat::Mono16kS16());
    writer.WriteSamples(std::vector<int16_t>{1, -2, 3, -4});
    writer.Close();
  }

  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "test wav file was not created");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());

  Require(bytes.size() == 44 + 8, "wav size should be 44-byte header plus 8-byte data");
  Require(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF", "missing RIFF tag");
  Require(std::string(bytes.begin() + 8, bytes.begin() + 12) == "WAVE", "missing WAVE tag");
  Require(ReadLittleEndian<uint16_t>(bytes, 20) == 1, "wav format should be PCM");
  Require(ReadLittleEndian<uint16_t>(bytes, 22) == 1, "wav should be mono");
  Require(ReadLittleEndian<uint32_t>(bytes, 24) == 16000, "wav sample rate should be 16k");
  Require(ReadLittleEndian<uint16_t>(bytes, 34) == 16, "wav bit depth should be 16");
  Require(ReadLittleEndian<uint32_t>(bytes, 40) == 8, "wav data chunk size should match samples");
}

}  // namespace

int main() {
  try {
    TestStereoFrameSplitKeepsMicAndReferenceOrder();
    TestWavWriterPatchesHeaderAfterStreamingSamples();
  } catch (const std::exception& error) {
    std::cerr << "Test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
