#include "audio_processing_module/application.hpp"
#include "audio_processing_module/audio_frame.hpp"
#include "audio_processing_module/wav_writer.hpp"
#include "audio_processing_module/webrtc_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using audio_processing_module::AudioFormat;
using audio_processing_module::AutomaticGainState;
using audio_processing_module::ComputeRms;
using audio_processing_module::ParseOptions;
using audio_processing_module::PreAecAutoGainConfig;
using audio_processing_module::ProcessPreAecAutoGain;
using audio_processing_module::SplitInterleavedStereoS16;
using audio_processing_module::StereoSplit;
using audio_processing_module::WavWriter;
using audio_processing_module::WebRtcProcessorOptions;

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

void TestParseOptionsAcceptsAec3TuningParameters() {
  // 验证系统测试可以通过命令行稳定指定 AEC3 delay、AEC 前自动增益、NS 与 AGC 参数。
  const std::vector<std::string> args = {
      "AudioProcessingModule",
      "--input", "tests/aec_2ch_16k.s16",
      "--delay-ms", "32",
      "--pre-aec-auto-gain-target-rms", "2400",
      "--pre-aec-auto-gain-max", "6.5",
      "--ns-level", "very-high",
      "--agc-mode", "adaptive-digital",
      "--agc-target-dbfs", "4",
      "--agc-compression-gain-db", "12",
  };

  const auto options = ParseOptions(args);

  Require(options.processor.delay_ms == 32, "delay_ms was not parsed");
  Require(std::abs(options.processor.pre_aec_auto_gain.target_rms - 2400.0f) < 0.01f,
          "pre-aec target rms was not parsed");
  Require(std::abs(options.processor.pre_aec_auto_gain.max_gain - 6.5f) < 0.01f,
          "pre-aec max gain was not parsed");
  Require(options.processor.ns_level == WebRtcProcessorOptions::NoiseSuppressionLevel::kVeryHigh,
          "ns level was not parsed");
  Require(options.processor.agc_mode == WebRtcProcessorOptions::AgcMode::kAdaptiveDigital,
          "agc mode was not parsed");
  Require(options.processor.agc_target_level_dbfs == 4, "agc target was not parsed");
  Require(options.processor.agc_compression_gain_db == 12, "agc compression was not parsed");
}

void TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping() {
  // 验证 AEC 前自动增益只拉升较小的 ch0 音量，并在 max_gain 内避免削波。
  std::vector<int16_t> mic(160, 0);
  for (size_t index = 0; index < mic.size(); ++index) {
    mic[index] = (index % 2 == 0) ? 400 : -400;
  }

  PreAecAutoGainConfig config;
  config.enabled = true;
  config.target_rms = 2400.0f;
  config.max_gain = 6.0f;
  config.attack = 1.0f;
  config.release = 1.0f;
  AutomaticGainState state;

  const float before = ComputeRms(mic);
  ProcessPreAecAutoGain(&mic, config, &state);
  const float after = ComputeRms(mic);

  Require(before < 450.0f, "test setup should start with a small mic frame");
  Require(after > 2300.0f && after < 2500.0f, "pre-aec gain should approach target rms");
  Require(std::all_of(mic.begin(), mic.end(), [](int16_t sample) {
            return sample < 32767 && sample > -32768;
          }),
          "pre-aec gain should not clip this frame");
  Require(state.current_gain > 5.9f && state.current_gain <= 6.0f,
          "pre-aec gain should honor max gain");
}

}  // namespace

int main() {
  try {
    TestStereoFrameSplitKeepsMicAndReferenceOrder();
    TestWavWriterPatchesHeaderAfterStreamingSamples();
    TestParseOptionsAcceptsAec3TuningParameters();
    TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping();
  } catch (const std::exception& error) {
    std::cerr << "Test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
