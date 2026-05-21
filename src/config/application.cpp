#include "config/application.hpp"

#include "apm/aec/aec_stream_processor.hpp"
#include "apm/aec/webrtc_processor.hpp"
#include "frontend/audio_stream_frontend.hpp"
#include "llm/file_recorder.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace audio_processing_module {
namespace {

struct CliOptions {
  std::string input_file;
  std::string output_file;
  std::string config_path;
};

std::string RequireValue(const std::vector<std::string>& args, size_t& index) {
  if (index + 1 >= args.size()) {
    throw std::runtime_error("missing value for option: " + args[index]);
  }
  ++index;
  return args[index];
}

int ParseInt(const std::string& value, const std::string& option_name) {
  try {
    size_t parsed = 0;
    const int result = std::stoi(value, &parsed);
    if (parsed != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return result;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid integer for " + option_name + ": " + value);
  }
}

float ParseFloat(const std::string& value, const std::string& option_name) {
  try {
    size_t parsed = 0;
    const float result = std::stof(value, &parsed);
    if (parsed != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return result;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid float for " + option_name + ": " + value);
  }
}

bool ParseBool(const std::string& value, const std::string& option_name) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw std::runtime_error("invalid bool for " + option_name + ": " + value);
}

WebRtcProcessorOptions::NoiseSuppressionLevel ParseNoiseSuppressionLevel(
    const std::string& value) {
  if (value == "low") {
    return WebRtcProcessorOptions::NoiseSuppressionLevel::kLow;
  }
  if (value == "moderate") {
    return WebRtcProcessorOptions::NoiseSuppressionLevel::kModerate;
  }
  if (value == "high") {
    return WebRtcProcessorOptions::NoiseSuppressionLevel::kHigh;
  }
  if (value == "very-high") {
    return WebRtcProcessorOptions::NoiseSuppressionLevel::kVeryHigh;
  }
  throw std::runtime_error("invalid ns_level: " + value);
}

std::string FormatNoiseSuppressionLevel(
    WebRtcProcessorOptions::NoiseSuppressionLevel level) {
  switch (level) {
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kLow:
      return "low";
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kModerate:
      return "moderate";
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kHigh:
      return "high";
    case WebRtcProcessorOptions::NoiseSuppressionLevel::kVeryHigh:
      return "very-high";
  }
  throw std::runtime_error("unknown noise suppression level");
}

WebRtcProcessorOptions::AgcMode ParseAgcMode(const std::string& value) {
  if (value == "adaptive-digital") {
    return WebRtcProcessorOptions::AgcMode::kAdaptiveDigital;
  }
  if (value == "fixed-digital") {
    return WebRtcProcessorOptions::AgcMode::kFixedDigital;
  }
  throw std::runtime_error("invalid agc_mode: " + value);
}

std::string FormatAgcMode(WebRtcProcessorOptions::AgcMode mode) {
  switch (mode) {
    case WebRtcProcessorOptions::AgcMode::kAdaptiveDigital:
      return "adaptive-digital";
    case WebRtcProcessorOptions::AgcMode::kFixedDigital:
      return "fixed-digital";
  }
  throw std::runtime_error("unknown agc mode");
}

std::string DefaultSocketDir() {
  return "/tmp/audio_processing_module_" + std::to_string(static_cast<long>(::getpid()));
}

bool IsRemovedConfigOption(const std::string& arg) {
  return arg == "--socket-dir" || arg == "--delay-ms" ||
         arg == "--pre-aec-auto-gain-target-rms" ||
         arg == "--pre-aec-auto-gain-max" ||
         arg == "--disable-pre-aec-auto-gain" || arg == "--ns-level" ||
         arg == "--agc-mode" || arg == "--agc-target-dbfs" ||
         arg == "--agc-compression-gain-db";
}

std::string Trim(const std::string& value) {
  const auto is_not_space = [](unsigned char character) {
    return !std::isspace(character);
  };
  const auto begin = std::find_if(value.begin(), value.end(), is_not_space);
  if (begin == value.end()) {
    return "";
  }
  const auto end = std::find_if(value.rbegin(), value.rend(), is_not_space).base();
  return std::string(begin, end);
}

std::string StripInlineComment(const std::string& value) {
  bool in_single_quote = false;
  bool in_double_quote = false;

  for (size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
    } else if (character == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
    } else if (character == '#' && !in_single_quote && !in_double_quote) {
      return value.substr(0, index);
    }
  }

  return value;
}

std::string Unquote(const std::string& value) {
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::filesystem::path ResolveConfigFilePath(const std::string& cli_config_path) {
  if (cli_config_path.empty()) {
    return std::filesystem::current_path() / "apm.yaml";
  }

  const std::filesystem::path path(cli_config_path);
  if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
    return path / "apm.yaml";
  }
  if (!path.has_extension()) {
    return path / "apm.yaml";
  }
  return path;
}

void WriteDefaultConfig(const std::filesystem::path& config_file,
                        const PipelineOptions& defaults) {
  const std::filesystem::path parent = config_file.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(config_file);
  if (!output.good()) {
    throw std::runtime_error("failed to create config file: " + config_file.string());
  }

  const WebRtcProcessorOptions& processor = defaults.processor;
  const PreAecAutoGainConfig& pre_aec = processor.pre_aec_auto_gain;
  output << "# soundbox-server runtime configuration.\n"
         << "# Pass --config <path-or-dir> to select this file; directories use apm.yaml.\n"
         << "socket_dir: \"" << defaults.socket_dir << "\"\n"
         << "delay_ms: " << processor.delay_ms << "\n"
         << "pre_aec_auto_gain:\n"
         << "  enabled: " << (pre_aec.enabled ? "true" : "false") << "\n"
         << "  target_rms: " << pre_aec.target_rms << "\n"
         << "  max_gain: " << pre_aec.max_gain << "\n"
         << "  attack: " << pre_aec.attack << "\n"
         << "  release: " << pre_aec.release << "\n"
         << "ns_level: " << FormatNoiseSuppressionLevel(processor.ns_level) << "\n"
         << "agc_mode: " << FormatAgcMode(processor.agc_mode) << "\n"
         << "agc_target_dbfs: " << processor.agc_target_level_dbfs << "\n"
         << "agc_compression_gain_db: " << processor.agc_compression_gain_db << "\n"
         << "agc_limiter_enabled: "
         << (processor.agc_limiter_enabled ? "true" : "false") << "\n";

  if (!output.good()) {
    throw std::runtime_error("failed to write config file: " + config_file.string());
  }
}

void ApplyConfigEntry(PipelineOptions* options,
                      const std::string& key,
                      const std::string& value) {
  WebRtcProcessorOptions& processor = options->processor;
  PreAecAutoGainConfig& pre_aec = processor.pre_aec_auto_gain;

  if (key == "socket_dir") {
    options->socket_dir = value;
  } else if (key == "delay_ms") {
    processor.delay_ms = ParseInt(value, key);
  } else if (key == "pre_aec_auto_gain.enabled") {
    pre_aec.enabled = ParseBool(value, key);
  } else if (key == "pre_aec_auto_gain.target_rms") {
    pre_aec.target_rms = ParseFloat(value, key);
  } else if (key == "pre_aec_auto_gain.max_gain") {
    pre_aec.max_gain = ParseFloat(value, key);
  } else if (key == "pre_aec_auto_gain.attack") {
    pre_aec.attack = ParseFloat(value, key);
  } else if (key == "pre_aec_auto_gain.release") {
    pre_aec.release = ParseFloat(value, key);
  } else if (key == "ns_level") {
    processor.ns_level = ParseNoiseSuppressionLevel(value);
  } else if (key == "agc_mode") {
    processor.agc_mode = ParseAgcMode(value);
  } else if (key == "agc_target_dbfs" || key == "agc_target_level_dbfs") {
    processor.agc_target_level_dbfs = ParseInt(value, key);
  } else if (key == "agc_compression_gain_db") {
    processor.agc_compression_gain_db = ParseInt(value, key);
  } else if (key == "agc_limiter_enabled") {
    processor.agc_limiter_enabled = ParseBool(value, key);
  } else {
    throw std::runtime_error("unknown config key: " + key);
  }
}

void LoadConfigFile(const std::filesystem::path& config_file,
                    PipelineOptions* options) {
  std::ifstream input(config_file);
  if (!input.good()) {
    throw std::runtime_error("failed to read config file: " + config_file.string());
  }

  std::string line;
  std::string section;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;

    const std::string uncommented = StripInlineComment(line);
    const std::string content = Trim(uncommented);
    if (content.empty()) {
      continue;
    }

    const size_t separator = content.find(':');
    if (separator == std::string::npos) {
      throw std::runtime_error("invalid config line " +
                               std::to_string(line_number) + ": " + content);
    }

    const bool indented =
        !uncommented.empty() &&
        std::isspace(static_cast<unsigned char>(uncommented.front()));
    const std::string raw_key = Trim(content.substr(0, separator));
    const std::string raw_value = Trim(content.substr(separator + 1));
    if (raw_key.empty()) {
      throw std::runtime_error("invalid config line " +
                               std::to_string(line_number) + ": " + content);
    }
    if (raw_value.empty()) {
      if (indented) {
        throw std::runtime_error("invalid config line " +
                                 std::to_string(line_number) + ": " + content);
      }
      section = raw_key;
      continue;
    }

    std::string key = raw_key;
    if (indented) {
      if (section.empty()) {
        throw std::runtime_error("config line " + std::to_string(line_number) +
                                 " is indented without a parent section");
      }
      key = section + "." + raw_key;
    } else {
      section.clear();
    }

    ApplyConfigEntry(options, key, Unquote(raw_value));
  }
}

PipelineOptions DefaultPipelineOptions() {
  PipelineOptions options;
  options.output_file = "output/aec_processed.wav";
  options.socket_dir = DefaultSocketDir();
  return options;
}

CliOptions ParseCliOptions(const std::vector<std::string>& args) {
  CliOptions options;

  for (size_t index = 1; index < args.size(); ++index) {
    const std::string& arg = args[index];
    if (arg == "--input" || arg == "-i") {
      options.input_file = RequireValue(args, index);
    } else if (arg == "--output" || arg == "-o") {
      options.output_file = RequireValue(args, index);
    } else if (arg == "--config" || arg == "-c") {
      options.config_path = RequireValue(args, index);
    } else if (IsRemovedConfigOption(arg)) {
      throw std::runtime_error(arg + " has moved to apm.yaml; use --config <path-or-dir>");
    } else if (arg == "--help" || arg == "-h") {
      throw std::runtime_error(Usage(args.empty() ? "soundbox-server" : args[0]));
    } else {
      throw std::runtime_error("unknown option: " + arg);
    }
  }

  return options;
}

void RethrowFirstError(const std::vector<std::exception_ptr>& errors) {
  for (const auto& error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }
}

}  // namespace

PipelineOptions ParseOptions(const std::vector<std::string>& args) {
  const CliOptions cli_options = ParseCliOptions(args);
  PipelineOptions options = DefaultPipelineOptions();

  const std::filesystem::path config_file =
      ResolveConfigFilePath(cli_options.config_path);
  if (!std::filesystem::exists(config_file)) {
    WriteDefaultConfig(config_file, options);
  }
  LoadConfigFile(config_file, &options);

  if (!cli_options.output_file.empty()) {
    options.output_file = cli_options.output_file;
  }
  if (!cli_options.input_file.empty()) {
    options.input_file = cli_options.input_file;
  }

  if (options.input_file.empty()) {
    throw std::runtime_error("required option is missing: --input <2ch-s16le-pcm-file>");
  }

  return options;
}

std::string Usage(const std::string& program_name) {
  std::ostringstream output;
  output << "Usage: " << program_name
         << " --input <2ch_16bit_16k.s16> [--output <processed.wav>]"
         << " [--config <apm.yaml|config-dir>]\n";
  return output.str();
}

int RunPipeline(const PipelineOptions& options) {
  std::filesystem::create_directories(options.socket_dir);
  const std::string frontend_to_aec_socket =
      (std::filesystem::path(options.socket_dir) / "frontend_to_aec.sock").string();
  const std::string aec_to_llm_socket =
      (std::filesystem::path(options.socket_dir) / "aec_to_llm.sock").string();

  frontend::AudioStreamFrontend frontend(options.input_file, frontend_to_aec_socket);
  apm::aec::AecStreamProcessor aec(frontend_to_aec_socket, aec_to_llm_socket,
                                   options.processor);
  llm::FileRecorder llm(aec_to_llm_socket, options.output_file);

  std::vector<std::exception_ptr> errors(3);
  std::mutex error_mutex;

  auto run_thread = [&](size_t index, auto&& task) {
    try {
      task();
    } catch (...) {
      std::lock_guard<std::mutex> lock(error_mutex);
      errors[index] = std::current_exception();
    }
  };

  std::thread llm_thread([&] { run_thread(2, [&] { llm.Run(); }); });
  std::thread aec_thread([&] { run_thread(1, [&] { aec.Run(); }); });
  std::thread frontend_thread([&] { run_thread(0, [&] { frontend.Run(); }); });

  frontend_thread.join();
  aec_thread.join();
  llm_thread.join();

  RethrowFirstError(errors);
  std::cout << "[main] pipeline completed successfully\n";
  return 0;
}

}  // namespace audio_processing_module
