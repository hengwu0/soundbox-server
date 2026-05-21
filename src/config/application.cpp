#include "config/application.hpp"

#include "apm/aec/aec_stream_processor.hpp"
#include "apm/kws/kws_socket_server.hpp"
#include "apm/kws/kws_zipformer.hpp"
#include "apm/aec/webrtc_processor.hpp"
#include "common/log.hpp"
#include "frontend/soundbox_frontend.hpp"
#include "llm/file_recorder.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
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

volatile std::sig_atomic_t g_shutdown_signal = 0;

void HandleShutdownSignal(int signal_number) {
  g_shutdown_signal = signal_number;
}

bool ShutdownSignalRequested() {
  return g_shutdown_signal != 0;
}

class SignalHandlerGuard {
 public:
  SignalHandlerGuard()
      : previous_sigint_(std::signal(SIGINT, HandleShutdownSignal)),
        previous_sigterm_(std::signal(SIGTERM, HandleShutdownSignal)) {
    g_shutdown_signal = 0;
  }

  ~SignalHandlerGuard() {
    std::signal(SIGINT, previous_sigint_);
    std::signal(SIGTERM, previous_sigterm_);
  }

  SignalHandlerGuard(const SignalHandlerGuard&) = delete;
  SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;

 private:
  using Handler = void (*)(int);
  Handler previous_sigint_{SIG_DFL};
  Handler previous_sigterm_{SIG_DFL};
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
         << "soundbox:\n"
         << "  ws_url: \"" << defaults.runtime.soundbox.ws_url << "\"\n"
         << "  ws_token: \"" << defaults.runtime.soundbox.ws_token << "\"\n"
         << "  connect_timeout_ms: "
         << defaults.runtime.soundbox.connect_timeout_ms << "\n"
         << "  llm_start_timeout_ms: 1000\n"
         << "  llm_stop_timeout_ms: 1000\n"
         << "wakeup:\n"
         << "  say_hello: \"" << defaults.runtime.wakeup.say_hello << "\"\n"
         << "  keywords_file: \"" << defaults.runtime.wakeup.keywords_file << "\"\n"
         << "  tokens_path: \"" << defaults.runtime.wakeup.tokens_path << "\"\n"
         << "  encoder_path: \"" << defaults.runtime.wakeup.encoder_path << "\"\n"
         << "  decoder_path: \"" << defaults.runtime.wakeup.decoder_path << "\"\n"
         << "  joiner_path: \"" << defaults.runtime.wakeup.joiner_path << "\"\n"
         << "  kws_threshold: " << defaults.runtime.wakeup.kws_threshold << "\n"
         << "  kws_score: " << defaults.runtime.wakeup.kws_score << "\n"
         << "  kws_num_threads: " << defaults.runtime.wakeup.kws_num_threads << "\n"
         << "  kws_max_active_paths: "
         << defaults.runtime.wakeup.kws_max_active_paths << "\n"
         << "  kws_num_trailing_blanks: "
         << defaults.runtime.wakeup.kws_num_trailing_blanks << "\n"
         << "  min_trigger_interval_ms: "
         << defaults.runtime.wakeup.min_trigger_interval_ms << "\n"
         << "playback:\n"
         << "  sample_rate: " << defaults.runtime.xiaozhi.downlink_sample_rate << "\n"
         << "  channels: 1\n"
         << "  bits_per_sample: 16\n"
         << "aec:\n"
         << "  delay_ms: " << processor.delay_ms << "\n"
         << "  pre_aec_auto_gain:\n"
         << "    enabled: " << (pre_aec.enabled ? "true" : "false") << "\n"
         << "    target_rms: " << pre_aec.target_rms << "\n"
         << "    max_gain: " << pre_aec.max_gain << "\n"
         << "    attack: " << pre_aec.attack << "\n"
         << "    release: " << pre_aec.release << "\n"
         << "  ns_level: " << FormatNoiseSuppressionLevel(processor.ns_level) << "\n"
         << "  agc_mode: " << FormatAgcMode(processor.agc_mode) << "\n"
         << "  agc_target_dbfs: " << processor.agc_target_level_dbfs << "\n"
         << "  agc_compression_gain_db: " << processor.agc_compression_gain_db << "\n"
         << "  agc_limiter_enabled: "
         << (processor.agc_limiter_enabled ? "true" : "false") << "\n"
         << "budget:\n"
         << "  input_queue_frames: " << defaults.runtime.budget.input_queue_frames << "\n"
         << "  output_queue_frames: "
         << defaults.runtime.budget.output_queue_frames << "\n"
         << "  reconnect_backoff_min_ms: "
         << defaults.runtime.budget.reconnect_backoff_min_ms << "\n"
         << "  reconnect_backoff_max_ms: "
         << defaults.runtime.budget.reconnect_backoff_max_ms << "\n"
         << "log:\n"
         << "  enable_debug: "
         << (defaults.runtime.log.enable_debug ? "true" : "false") << "\n"
         << "  file_enabled: "
         << (defaults.runtime.log.file_enabled ? "true" : "false") << "\n"
         << "  file_path: \"" << defaults.runtime.log.file_path << "\"\n";

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
  } else if (key == "output_file") {
    options->output_file = value;
  } else if (key == "soundbox.ws_url") {
    options->runtime.soundbox.ws_url = value;
  } else if (key == "soundbox.ws_token") {
    options->runtime.soundbox.ws_token = value;
  } else if (key == "soundbox.connect_timeout_ms") {
    options->runtime.soundbox.connect_timeout_ms = ParseInt(value, key);
  } else if (key == "soundbox.llm_start_timeout_ms") {
    options->runtime.soundbox.llm_start_timeout_ms = ParseInt(value, key);
  } else if (key == "soundbox.llm_stop_timeout_ms") {
    options->runtime.soundbox.llm_stop_timeout_ms = ParseInt(value, key);
  } else if (key == "wakeup.say_hello") {
    options->runtime.wakeup.say_hello = value;
  } else if (key == "wakeup.keywords_file") {
    options->runtime.wakeup.keywords_file = value;
  } else if (key == "wakeup.tokens_path") {
    options->runtime.wakeup.tokens_path = value;
  } else if (key == "wakeup.encoder_path") {
    options->runtime.wakeup.encoder_path = value;
  } else if (key == "wakeup.decoder_path") {
    options->runtime.wakeup.decoder_path = value;
  } else if (key == "wakeup.joiner_path") {
    options->runtime.wakeup.joiner_path = value;
  } else if (key == "wakeup.kws_threshold") {
    options->runtime.wakeup.kws_threshold = ParseFloat(value, key);
  } else if (key == "wakeup.kws_score") {
    options->runtime.wakeup.kws_score = ParseFloat(value, key);
  } else if (key == "wakeup.kws_num_threads") {
    options->runtime.wakeup.kws_num_threads = ParseInt(value, key);
  } else if (key == "wakeup.kws_max_active_paths") {
    options->runtime.wakeup.kws_max_active_paths = ParseInt(value, key);
  } else if (key == "wakeup.kws_num_trailing_blanks") {
    options->runtime.wakeup.kws_num_trailing_blanks = ParseInt(value, key);
  } else if (key == "wakeup.min_trigger_interval_ms") {
    options->runtime.wakeup.min_trigger_interval_ms = ParseInt(value, key);
  } else if (key == "playback.sample_rate") {
    options->runtime.xiaozhi.downlink_sample_rate = ParseInt(value, key);
  } else if (key == "playback.channels" || key == "playback.bits_per_sample") {
    (void)ParseInt(value, key);
  } else if (key == "audio.playback_gain") {
    options->runtime.audio.playback_gain = ParseFloat(value, key);
  } else if (key == "delay_ms" || key == "aec.delay_ms") {
    processor.delay_ms = ParseInt(value, key);
  } else if (key == "pre_aec_auto_gain.enabled" ||
             key == "aec.pre_aec_auto_gain.enabled") {
    pre_aec.enabled = ParseBool(value, key);
  } else if (key == "pre_aec_auto_gain.target_rms" ||
             key == "aec.pre_aec_auto_gain.target_rms") {
    pre_aec.target_rms = ParseFloat(value, key);
  } else if (key == "pre_aec_auto_gain.max_gain" ||
             key == "aec.pre_aec_auto_gain.max_gain") {
    pre_aec.max_gain = ParseFloat(value, key);
  } else if (key == "pre_aec_auto_gain.attack" ||
             key == "aec.pre_aec_auto_gain.attack") {
    pre_aec.attack = ParseFloat(value, key);
  } else if (key == "pre_aec_auto_gain.release" ||
             key == "aec.pre_aec_auto_gain.release") {
    pre_aec.release = ParseFloat(value, key);
  } else if (key == "ns_level" || key == "aec.ns_level") {
    processor.ns_level = ParseNoiseSuppressionLevel(value);
  } else if (key == "agc_mode" || key == "aec.agc_mode") {
    processor.agc_mode = ParseAgcMode(value);
  } else if (key == "agc_target_dbfs" || key == "agc_target_level_dbfs" ||
             key == "aec.agc_target_dbfs" || key == "aec.agc_target_level_dbfs") {
    processor.agc_target_level_dbfs = ParseInt(value, key);
  } else if (key == "agc_compression_gain_db" ||
             key == "aec.agc_compression_gain_db") {
    processor.agc_compression_gain_db = ParseInt(value, key);
  } else if (key == "agc_limiter_enabled" ||
             key == "aec.agc_limiter_enabled") {
    processor.agc_limiter_enabled = ParseBool(value, key);
  } else if (key == "budget.input_queue_frames") {
    options->runtime.budget.input_queue_frames = ParseInt(value, key);
  } else if (key == "budget.output_queue_frames") {
    options->runtime.budget.output_queue_frames = ParseInt(value, key);
  } else if (key == "budget.reconnect_backoff_min_ms") {
    options->runtime.budget.reconnect_backoff_min_ms = ParseInt(value, key);
  } else if (key == "budget.reconnect_backoff_max_ms") {
    options->runtime.budget.reconnect_backoff_max_ms = ParseInt(value, key);
  } else if (key == "log.enable_debug") {
    options->runtime.log.enable_debug = ParseBool(value, key);
  } else if (key == "log.file_enabled") {
    options->runtime.log.file_enabled = ParseBool(value, key);
  } else if (key == "log.file_path") {
    options->runtime.log.file_path = value;
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
  std::string subsection;
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

    const size_t indent = uncommented.find_first_not_of(" \t");
    const bool indented = indent > 0;
    const std::string raw_key = Trim(content.substr(0, separator));
    const std::string raw_value = Trim(content.substr(separator + 1));
    if (raw_key.empty()) {
      throw std::runtime_error("invalid config line " +
                               std::to_string(line_number) + ": " + content);
    }
    if (raw_value.empty()) {
      if (!indented) {
        section = raw_key;
        subsection.clear();
        continue;
      }
      if (section.empty()) {
        throw std::runtime_error("config line " + std::to_string(line_number) +
                                 " is indented without a parent section");
      }
      subsection = raw_key;
      continue;
    }

    std::string key = raw_key;
    if (indented) {
      if (section.empty()) {
        throw std::runtime_error("config line " + std::to_string(line_number) +
                                 " is indented without a parent section");
      }
      if (indent >= 4 && !subsection.empty()) {
        key = section + "." + subsection + "." + raw_key;
      } else {
        key = section + "." + raw_key;
      }
    } else {
      section.clear();
      subsection.clear();
    }

    ApplyConfigEntry(options, key, Unquote(raw_value));
  }
}

PipelineOptions DefaultPipelineOptions() {
  PipelineOptions options;
  options.output_file = "output/aec_processed.wav";
  options.socket_dir = DefaultSocketDir();
  options.runtime.soundbox.ws_url.clear();
  options.runtime.soundbox.ws_token.clear();
  options.runtime.log.file_path = "logs/soundbox_server.log";
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

  return options;
}

std::string Usage(const std::string& program_name) {
  std::ostringstream output;
  output << "Usage: " << program_name
         << " [--output <processed.wav>]"
         << " [--config <apm.yaml|config-dir>]\n";
  return output.str();
}

void RunSupervisedWorkers(const std::vector<SupervisedWorker>& workers,
                          const std::function<bool()>& should_stop) {
  if (workers.empty()) {
    return;
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::exception_ptr first_error;
  std::atomic<bool> stop_requested{false};
  size_t completed = 0;

  auto record_error = [&](std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!first_error) {
        first_error = error;
      }
    }
    stop_requested.store(true);
    cv.notify_all();
  };

  std::vector<std::thread> threads;
  threads.reserve(workers.size());
  for (size_t index = 0; index < workers.size(); ++index) {
    threads.emplace_back([&, index] {
      try {
        workers[index].run();
        if (!stop_requested.load()) {
          record_error(std::make_exception_ptr(
              std::runtime_error("worker exited unexpectedly: " +
                                 workers[index].name)));
        }
      } catch (...) {
        record_error(std::current_exception());
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
      }
      cv.notify_all();
    });
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
      return first_error || completed == workers.size() ||
             (should_stop && should_stop());
    });
    while (!first_error && completed != workers.size() &&
           !(should_stop && should_stop())) {
      cv.wait_for(lock, std::chrono::milliseconds(200));
    }
    if (first_error || (should_stop && should_stop())) {
      stop_requested.store(true);
    }
  }

  if (stop_requested.load()) {
    for (auto it = workers.rbegin(); it != workers.rend(); ++it) {
      if (it->stop) {
        try {
          it->stop();
        } catch (...) {
        }
      }
    }
  }

  for (std::thread& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

int RunPipeline(const PipelineOptions& options) {
  const bool file_log_ready = xiaoai_server::ConfigureLogging(
      options.runtime.log.enable_debug, options.runtime.log.file_enabled,
      options.runtime.log.file_path);
  const auto log = xiaoai_server::GetLogger("main");
  log->info("logging configured debug={} file_enabled={} file_ready={} file_path={}",
            options.runtime.log.enable_debug, options.runtime.log.file_enabled,
            file_log_ready, options.runtime.log.file_path);

  if (options.runtime.soundbox.ws_url.empty()) {
    throw std::runtime_error("missing required config: soundbox.ws_url");
  }
  if (options.runtime.soundbox.ws_token.empty()) {
    throw std::runtime_error("missing required config: soundbox.ws_token");
  }
  const std::vector<std::string> required_kws_files = {
      options.runtime.wakeup.keywords_file,
      options.runtime.wakeup.tokens_path,
      options.runtime.wakeup.encoder_path,
      options.runtime.wakeup.decoder_path,
      options.runtime.wakeup.joiner_path,
  };
  for (const std::string& path : required_kws_files) {
    if (!std::filesystem::exists(path)) {
      throw std::runtime_error("missing required KWS asset: " + path);
    }
  }

  std::filesystem::create_directories(options.socket_dir);
  const std::string frontend_kws_socket =
      (std::filesystem::path(options.socket_dir) / "frontend_kws.sock").string();
  const std::string frontend_aec_socket =
      (std::filesystem::path(options.socket_dir) / "frontend_aec.sock").string();
  const std::string aec_llm_socket =
      (std::filesystem::path(options.socket_dir) / "aec_llm.sock").string();
  const std::string frontend_playback_socket =
      (std::filesystem::path(options.socket_dir) / "frontend_playback.sock").string();

  llm::FileRecorder llm(aec_llm_socket, options.output_file);
  apm::aec::AecStreamProcessor aec(frontend_aec_socket, aec_llm_socket,
                                   options.processor);
  auto kws_engine =
      std::make_shared<xiaoai_server::wakeup::ZipformerKwsEngine>(
          options.runtime.wakeup);
  apm::kws::KwsSocketServer::Options kws_options;
  kws_options.listen_socket_path = frontend_kws_socket;
  apm::kws::KwsSocketServer kws(kws_options, kws_engine);

  soundbox_server::frontend::Frontend::Options frontend_options;
  frontend_options.kws_socket_path = frontend_kws_socket;
  frontend_options.aec_socket_path = frontend_aec_socket;
  frontend_options.playback_socket_path = frontend_playback_socket;
  frontend_options.soundbox_config = options.runtime;
  soundbox_server::frontend::Frontend frontend(frontend_options);

  SignalHandlerGuard signal_handler_guard;
  RunSupervisedWorkers(
      {
          SupervisedWorker{"llm", [&] { llm.Run(); }, [&] { llm.Stop(); }},
          SupervisedWorker{"aec", [&] { aec.Run(); }, [&] { aec.Stop(); }},
          SupervisedWorker{"kws", [&] { kws.Run(); }, [&] { kws.Stop(); }},
          SupervisedWorker{"frontend", [&] { frontend.Run(); },
                           [&] { frontend.Stop(); }},
      },
      [] { return ShutdownSignalRequested(); });
  log->info("pipeline completed successfully");
  return 0;
}

}  // namespace audio_processing_module
