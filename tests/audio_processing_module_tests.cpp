#include "audio_processing_module/application.hpp"
#include "audio_processing_module/audio_frame.hpp"
#include "audio_processing_module/wav_writer.hpp"
#include "audio_processing_module/webrtc_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
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

class ScopedCurrentPath {
 public:
  explicit ScopedCurrentPath(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~ScopedCurrentPath() {
    std::filesystem::current_path(previous_);
  }

  ScopedCurrentPath(const ScopedCurrentPath&) = delete;
  ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

 private:
  std::filesystem::path previous_;
};

std::filesystem::path MakeTempDirectory(const std::string& suffix) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("audio_processing_module_test_" + suffix + "_" +
       std::to_string(static_cast<long>(::getpid())));
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void WriteConfigFile(const std::filesystem::path& path, const std::string& body) {
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(path);
  Require(output.good(), "failed to open config file for writing");
  output << body;
  Require(output.good(), "failed to write config file");
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
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

void TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory() {
  // 验证未指定配置参数时，程序会在当前工作目录生成 apm.yaml，并使用默认配置启动。
  const std::filesystem::path original_input =
      std::filesystem::absolute("tests/aec_2ch_16k.s16");
  const std::filesystem::path temp_root = MakeTempDirectory("cwd");
  ScopedCurrentPath current_path_guard(temp_root);

  const std::vector<std::string> args = {
      "AudioProcessingModule",
      "--input", original_input.string(),
  };

  const auto options = ParseOptions(args);

  Require(std::filesystem::exists(temp_root / "apm.yaml"),
          "default config file was not created in cwd");
  Require(StartsWith(options.socket_dir, "/tmp/audio_processing_module_"),
          "default socket dir was not loaded from generated config");
  Require(options.processor.delay_ms == 0, "default delay_ms should remain zero");
  Require(options.processor.ns_level == WebRtcProcessorOptions::NoiseSuppressionLevel::kHigh,
          "default ns level should remain high");
  Require(options.processor.agc_mode == WebRtcProcessorOptions::AgcMode::kAdaptiveDigital,
          "default agc mode should remain adaptive digital");
}

void TestParseOptionsLoadsConfigurationFromDirectory() {
  // 验证传入配置文件夹时，会默认读取其中的 apm.yaml 并应用文件里的配置。
  const std::filesystem::path temp_root = MakeTempDirectory("dir");
  const std::filesystem::path config_dir = temp_root / "config";
  const std::filesystem::path config_file = config_dir / "apm.yaml";
  const std::filesystem::path input_file = std::filesystem::absolute("tests/aec_2ch_16k.s16");
  WriteConfigFile(config_file,
                  "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                  "delay_ms: 32\n"
                  "pre_aec_auto_gain:\n"
                  "  enabled: false\n"
                  "  target_rms: 1800\n"
                  "  max_gain: 4.5\n"
                  "  attack: 0.5\n"
                  "  release: 0.25\n"
                  "ns_level: very-high\n"
                  "agc_mode: fixed-digital\n"
                  "agc_target_dbfs: 4\n"
                  "agc_compression_gain_db: 12\n"
                  "agc_limiter_enabled: false\n");

  const std::vector<std::string> args = {
      "AudioProcessingModule",
      "--config", config_dir.string(),
      "--input", input_file.string(),
  };

  const auto options = ParseOptions(args);

  Require(options.socket_dir == (temp_root / "sockets").string(),
          "socket dir was not loaded from config file");
  Require(options.processor.delay_ms == 32, "delay_ms was not loaded from config file");
  Require(!options.processor.pre_aec_auto_gain.enabled,
          "pre-aec auto gain enable flag was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.target_rms - 1800.0f) < 0.01f,
          "pre-aec target rms was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.max_gain - 4.5f) < 0.01f,
          "pre-aec max gain was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.attack - 0.5f) < 0.01f,
          "pre-aec attack was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.release - 0.25f) < 0.01f,
          "pre-aec release was not loaded");
  Require(options.processor.ns_level == WebRtcProcessorOptions::NoiseSuppressionLevel::kVeryHigh,
          "ns level was not loaded");
  Require(options.processor.agc_mode == WebRtcProcessorOptions::AgcMode::kFixedDigital,
          "agc mode was not loaded");
  Require(options.processor.agc_target_level_dbfs == 4, "agc target was not loaded");
  Require(options.processor.agc_compression_gain_db == 12, "agc compression was not loaded");
  Require(!options.processor.agc_limiter_enabled, "agc limiter was not loaded");
}

void TestParseOptionsCreatesConfigFileAtExplicitYamlPath() {
  // 验证传入配置文件绝对路径时，程序会直接在该路径创建 apm.yaml 并使用默认配置。
  const std::filesystem::path temp_root = MakeTempDirectory("file");
  const std::filesystem::path config_file = temp_root / "custom-apm.yaml";
  const std::filesystem::path input_file = std::filesystem::absolute("tests/aec_2ch_16k.s16");

  const std::vector<std::string> args = {
      "AudioProcessingModule",
      "--config", config_file.string(),
      "--input", input_file.string(),
  };

  const auto options = ParseOptions(args);

  Require(std::filesystem::exists(config_file),
          "explicit config file path was not created");
  Require(StartsWith(options.socket_dir, "/tmp/audio_processing_module_"),
          "generated config should still use default socket dir");

  std::ifstream input(config_file);
  const std::string body((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  Require(body.find("pre_aec_auto_gain:\n") != std::string::npos,
          "generated config should use nested pre-aec auto gain yaml");
}

void TestParseOptionsRejectsRemovedTuningFlags() {
  // 验证原来直接从命令行传入的调参项已经被移除，并会提示改用配置文件。
  const std::vector<std::string> args = {
      "AudioProcessingModule",
      "--input", "tests/aec_2ch_16k.s16",
      "--delay-ms", "32",
  };

  bool failed = false;
  try {
    (void)ParseOptions(args);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("config") != std::string::npos;
  }

  Require(failed, "removed tuning flags should be rejected with a config-file hint");
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
    TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory();
    TestParseOptionsLoadsConfigurationFromDirectory();
    TestParseOptionsCreatesConfigFileAtExplicitYamlPath();
    TestParseOptionsRejectsRemovedTuningFlags();
    TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping();
  } catch (const std::exception& error) {
    std::cerr << "Test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
