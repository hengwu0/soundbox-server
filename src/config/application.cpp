#include "config/application.hpp"

#include "apm/aec/aec_stream_processor.hpp"
#include "apm/kws/kws_socket_server.hpp"
#include "apm/kws/kws_zipformer.hpp"
#include "apm/aec/webrtc_processor.hpp"
#include "common/log.hpp"
#include "frontend/soundbox_frontend.hpp"

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

/// CLI 命令行选项，从 main() 解析后传入配置加载流程。
struct CliOptions {
  std::string config_path;  ///< 用户通过 --config 指定的 YAML 配置文件路径。
};

/// 进程级关闭信号标志，由信号处理器写入，主循环轮询读取。
volatile std::sig_atomic_t g_shutdown_signal = 0;

/// 信号处理器回调：将收到的信号编号写入全局标志。
/// @param signal_number 操作系统传入的信号编号（SIGINT 或 SIGTERM）。
void HandleShutdownSignal(int signal_number) {
  g_shutdown_signal = signal_number;
}

/// 判断是否收到了关闭信号。
/// @return 收到关闭信号时返回 true。
bool ShutdownSignalRequested() {
  return g_shutdown_signal != 0;
}

/// RAII 守卫：构造时注册 SIGINT/SIGTERM 处理器，析构时恢复原处理器。
class SignalHandlerGuard {
 public:
  /// 构造守卫：保存原有信号处理器并注册新的处理函数。
  SignalHandlerGuard()
      : previous_sigint_(std::signal(SIGINT, HandleShutdownSignal)),
        previous_sigterm_(std::signal(SIGTERM, HandleShutdownSignal)) {
    g_shutdown_signal = 0;  ///< 重置关闭信号标志，确保干净启动。
  }

  /// 析构守卫：恢复原始信号处理器。
  ~SignalHandlerGuard() {
    std::signal(SIGINT, previous_sigint_);
    std::signal(SIGTERM, previous_sigterm_);
  }

  /// 禁止拷贝，保证 RAII 语义不被意外破坏。
  SignalHandlerGuard(const SignalHandlerGuard&) = delete;
  SignalHandlerGuard& operator=(const SignalHandlerGuard&) = delete;

 private:
  using Handler = void (*)(int);
  Handler previous_sigint_{SIG_DFL};   ///< 启动前原始的 SIGINT 处理器。
  Handler previous_sigterm_{SIG_DFL};  ///< 启动前原始的 SIGTERM 处理器。
};

/// 从命令行参数列表中取出下一个参数作为选项值。
/// @param args  命令行参数列表。
/// @param index 当前参数索引，调用后会自增指向值。
/// @return 选项对应的字符串值。
/// @throws std::runtime_error 如果当前选项后没有值。
std::string RequireValue(const std::vector<std::string>& args, size_t& index) {
  if (index + 1 >= args.size()) {
    throw std::runtime_error("missing value for option: " + args[index]);
  }
  ++index;
  return args[index];
}

/// 将字符串解析为整数，若格式非法则抛出异常。
/// @param value       待解析的字符串。
/// @param option_name 选项名（用于错误提示）。
/// @return 解析后的整数。
/// @throws std::runtime_error 格式非法时抛出。
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

/// 将字符串解析为浮点数，若格式非法则抛出异常。
/// @param value       待解析的字符串。
/// @param option_name 选项名（用于错误提示）。
/// @return 解析后的浮点数。
/// @throws std::runtime_error 格式非法时抛出。
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

/// 将字符串 "true"/"false" 解析为布尔值。
/// @param value       待解析的字符串。
/// @param option_name 选项名（用于错误提示）。
/// @return 解析后的布尔值。
/// @throws std::runtime_error 格式非法时抛出。
bool ParseBool(const std::string& value, const std::string& option_name) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  throw std::runtime_error("invalid bool for " + option_name + ": " + value);
}

/// 将噪声抑制级别字符串转换为枚举值。
/// @param value 噪声抑制级别字符串（"low" / "moderate" / "high" / "very-high"）。
/// @return 对应的枚举值。
/// @throws std::runtime_error 未知级别时抛出。
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

/// 将噪声抑制级别枚举值转换为 YAML 可显示的字符串。
/// @param level 噪声抑制级别枚举值。
/// @return 对应的字符串表示。
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

/// 将 AGC 模式字符串转换为枚举值。
/// @param value AGC 模式字符串（"adaptive-digital" / "fixed-digital"）。
/// @return 对应的枚举值。
/// @throws std::runtime_error 未知模式时抛出。
WebRtcProcessorOptions::AgcMode ParseAgcMode(const std::string& value) {
  if (value == "adaptive-digital") {
    return WebRtcProcessorOptions::AgcMode::kAdaptiveDigital;
  }
  if (value == "fixed-digital") {
    return WebRtcProcessorOptions::AgcMode::kFixedDigital;
  }
  throw std::runtime_error("invalid agc_mode: " + value);
}

/// 将 AGC 模式枚举值转换为 YAML 可显示的字符串。
/// @param mode AGC 模式枚举值。
/// @return 对应的字符串表示。
std::string FormatAgcMode(WebRtcProcessorOptions::AgcMode mode) {
  switch (mode) {
    case WebRtcProcessorOptions::AgcMode::kAdaptiveDigital:
      return "adaptive-digital";
    case WebRtcProcessorOptions::AgcMode::kFixedDigital:
      return "fixed-digital";
  }
  throw std::runtime_error("unknown agc mode");
}

/// 生成默认的 Unix Domain Socket 目录路径，以进程 PID 区分多实例。
/// @return 格式为 "/tmp/audio_processing_module_<pid>" 的路径。
std::string DefaultSocketDir() {
  return "/tmp/audio_processing_module_" + std::to_string(static_cast<long>(::getpid()));
}

/// 判断命令行参数是否属于已迁移到 YAML 配置的废弃选项。
/// @param arg 命令行参数。
/// @return 如果是已废弃的选项则返回 true。
bool IsRemovedConfigOption(const std::string& arg) {
  return arg == "--socket-dir" || arg == "--delay-ms" ||
         arg == "--pre-aec-auto-gain-target-rms" ||
         arg == "--pre-aec-auto-gain-max" ||
         arg == "--disable-pre-aec-auto-gain" || arg == "--ns-level" ||
         arg == "--agc-mode" || arg == "--agc-target-dbfs" ||
         arg == "--agc-compression-gain-db" || arg == "--input" || arg == "-i";
}

/// 去除字符串首尾空白字符。
/// @param value 原始字符串。
/// @return 去掉首尾空白后的新字符串。
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

/// 去掉 YAML 行中的内联注释（# 及其之后的内容），但保留引号内的 #。
/// @param value 原始行内容。
/// @return 去掉注释后的内容。
std::string StripInlineComment(const std::string& value) {
  bool in_single_quote = false;   ///< 是否正在单引号内。
  bool in_double_quote = false;   ///< 是否正在双引号内。

  for (size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
    } else if (character == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
    } else if (character == '#' && !in_single_quote && !in_double_quote) {
      // 在引号外遇到 #，截断为注释
      return value.substr(0, index);
    }
  }

  return value;
}

/// 去掉 YAML 值两端的引号（单引号或双引号）。
/// @param value 原始值字符串。
/// @return 去掉引号（如果有）后的值。
std::string Unquote(const std::string& value) {
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

/// 根据 CLI 传入路径解析最终配置文件路径。
/// 空路径时默认返回当前目录下的 apm.yaml；传入目录则在目录内查找 apm.yaml。
/// @param cli_config_path 命令行传入的配置路径（可为空、目录或文件路径）。
/// @return 最终确定的配置文件路径。
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

/// 当配置文件不存在时，用当前默认值写入一份默认 apm.yaml。
/// @param config_file 目标文件路径。
/// @param defaults    当前默认流水线选项。
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
          << "  host: \"" << defaults.runtime.playback.host << "\"\n"
          << "  port: " << defaults.runtime.playback.port << "\n"
          << "  sample_rate: " << defaults.runtime.playback.sample_rate << "\n"
          << "  channels: " << defaults.runtime.playback.channels << "\n"
          << "  bits_per_sample: " << defaults.runtime.playback.bits_per_sample << "\n"
         << "llm:\n"
         << "  host: \"" << defaults.runtime.llm.host << "\"\n"
         << "  port: " << defaults.runtime.llm.port << "\n"
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

/// 解析 YAML 中的单个键值对，应用到 PipelineOptions 的对应字段。
/// @param options 输出参数，当前流水线配置。
/// @param key     YAML 键（可能带层级前缀，如 "aec.pre_aec_auto_gain.enabled"）。
/// @param value   YAML 值（已去掉引号和注释）。
/// @throws std::runtime_error 未知键或值格式非法时抛出。
void ApplyConfigEntry(PipelineOptions* options,
                      const std::string& key,
                      const std::string& value) {
  WebRtcProcessorOptions& processor = options->processor;
  PreAecAutoGainConfig& pre_aec = processor.pre_aec_auto_gain;

  if (key == "socket_dir") {
    options->socket_dir = value;
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
  } else if (key == "playback.host") {
    options->runtime.playback.host = value;
  } else if (key == "playback.port") {
    options->runtime.playback.port = ParseInt(value, key);
  } else if (key == "playback.sample_rate") {
    options->runtime.playback.sample_rate = ParseInt(value, key);
  } else if (key == "playback.channels") {
    options->runtime.playback.channels = ParseInt(value, key);
  } else if (key == "playback.bits_per_sample") {
    options->runtime.playback.bits_per_sample = ParseInt(value, key);
  } else if (key == "llm.host") {
    options->runtime.llm.host = value;
  } else if (key == "llm.port") {
    options->runtime.llm.port = ParseInt(value, key);
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

/// 读取 YAML 配置文件，逐行解析并应用到 PipelineOptions。
/// 支持无缩进的顶层键作为 section，2 空格缩进为 subsection，4 空格缩进为深层键。
/// @param config_file 配置文件路径。
/// @param options     输出参数，用于填充配置值。
void LoadConfigFile(const std::filesystem::path& config_file,
                    PipelineOptions* options) {
  std::ifstream input(config_file);
  if (!input.good()) {
    throw std::runtime_error("failed to read config file: " + config_file.string());
  }

  std::string line;
  std::string section;     ///< 当前顶层 section（如 "aec"）。
  std::string subsection;  ///< 当前子 section（如 "pre_aec_auto_gain"）。
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;

    const std::string uncommented = StripInlineComment(line);
    const std::string content = Trim(uncommented);
    if (content.empty()) {
      continue;
    }

    // 查找冒号分隔的键值对
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
      // 空值表示这是一个 section 声明行
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

    // 构建完整的层级键名（e.g. aec.pre_aec_auto_gain.enabled）
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

/// 返回一套携带默认值的 PipelineOptions。
/// @return 默认配置好的流水线选项。
PipelineOptions DefaultPipelineOptions() {
  PipelineOptions options;
  options.socket_dir = DefaultSocketDir();
  options.runtime.soundbox.ws_url.clear();
  options.runtime.log.file_path = "logs/soundbox_server.log";
  return options;
}

/// 解析命令行参数，返回 CLI 选项结构体。
/// @param args 命令行参数列表（含 argv[0]）。
/// @return CLI 解析结果。
/// @throws std::runtime_error 参数非法或请求帮助时抛出。
CliOptions ParseCliOptions(const std::vector<std::string>& args) {
  CliOptions options;

  for (size_t index = 1; index < args.size(); ++index) {
    const std::string& arg = args[index];
    if (arg == "--config" || arg == "-c") {
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

/// 清理 socket 目录下的旧 socket 文件，防止 bind 失败。
/// @param socket_dir socket 文件所在目录。
void CleanupStaleSocketFiles(const std::filesystem::path& socket_dir) {
  const std::vector<std::string> socket_names = {
      "frontend_kws.sock",       ///< KWS 唤醒词前端通信 socket。
      "frontend_aec.sock",       ///< AEC 回声消除前端通信 socket。
      "aec_llm.sock",            ///< AEC 到 LLM 音频转发 socket。
  };
  for (const auto& name : socket_names) {
    const std::filesystem::path path = socket_dir / name;
    if (std::filesystem::exists(path)) {
      std::filesystem::remove(path);
      const auto log = xiaoai_server::GetLogger("main");
      log->info("cleaned stale socket file {}", path.string());
    }
  }
}

}  // namespace

/// 解析命令行参数并加载 YAML 配置，返回最终流水线选项。
/// 如果配置文件不存在，会先生成一份默认配置。
/// @param args 命令行参数列表。
/// @return 合并后的 PipelineOptions。
PipelineOptions ParseOptions(const std::vector<std::string>& args) {
  const CliOptions cli_options = ParseCliOptions(args);
  PipelineOptions options = DefaultPipelineOptions();

  const std::filesystem::path config_file =
      ResolveConfigFilePath(cli_options.config_path);
  if (!std::filesystem::exists(config_file)) {
    WriteDefaultConfig(config_file, options);
  }
  LoadConfigFile(config_file, &options);

  return options;
}

/// 返回命令行使用说明字符串。
/// @param program_name 程序名（通常为 argv[0]）。
/// @return 使用说明文本。
std::string Usage(const std::string& program_name) {
  std::ostringstream output;
  output << "Usage: " << program_name
         << " [--config <apm.yaml|config-dir>]\n";
  return output.str();
}

/// 监管多条工作线程，任一异常或停止信号时通知全部线程退出。
/// @param workers     需要同时运行的 Worker 列表。
/// @param should_stop 外部停止条件回调，返回 true 时请求全体退出。
void RunSupervisedWorkers(const std::vector<SupervisedWorker>& workers,
                           const std::function<bool()>& should_stop) {
  if (workers.empty()) {
    return;
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::exception_ptr first_error;  ///< 首个被捕获的工作线程异常。
  std::atomic<bool> stop_requested{false};  ///< 向所有线程发出停止请求的标记。
  size_t completed = 0;  ///< 已完成的工作线程计数。

  /// 将首次错误记录下来并通知所有线程停止。
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
        // worker 正常返回视为异常退出
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
    // 短暂等待后检查是否有 worker 已失败
    cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
      return first_error || completed == workers.size() ||
             (should_stop && should_stop());
    });
    // 持续监听直到满足退出条件
    while (!first_error && completed != workers.size() &&
           !(should_stop && should_stop())) {
      cv.wait_for(lock, std::chrono::milliseconds(200));
    }
    if (first_error || (should_stop && should_stop())) {
      stop_requested.store(true);
    }
  }

  // 逆序调用 stop() 对外通知各子模块停止
  if (stop_requested.load()) {
    for (auto it = workers.rbegin(); it != workers.rend(); ++it) {
      if (it->stop) {
        try {
          it->stop();
        } catch (...) {
          // 忽略 stop 过程中的异常
        }
      }
    }
  }

  // 等待所有线程结束
  for (std::thread& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  // 有异常则重新抛出第一个异常
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

/// 根据 PipelineOptions 启动整个音频处理流水线。
/// 初始化日志、验证配置、创建各子模块并监管运行。
/// @param options 已解析的流水线配置。
/// @return 正常退出返回 0。
/// @throws std::runtime_error 关键配置缺失或资源文件不存在时抛出。
int RunPipeline(const PipelineOptions& options) {
  // 初始化日志系统
  const bool file_log_ready = xiaoai_server::ConfigureLogging(
      options.runtime.log.enable_debug, options.runtime.log.file_enabled,
      options.runtime.log.file_path);
  const auto log = xiaoai_server::GetLogger("main");
  log->info("logging configured debug={} file_enabled={} file_ready={} file_path={}",
            options.runtime.log.enable_debug, options.runtime.log.file_enabled,
            file_log_ready, options.runtime.log.file_path);

  // 验证必要配置项
  if (options.runtime.soundbox.ws_url.empty()) {
    throw std::runtime_error("missing or invalid config: soundbox.ws_url is empty");
  }
  if (options.runtime.soundbox.ws_url.find("ws://") != 0 &&
      options.runtime.soundbox.ws_url.find("wss://") != 0) {
    throw std::runtime_error("invalid config: soundbox.ws_url must start with ws:// or wss://");
  }
  if (options.runtime.soundbox.ws_token.empty()) {
    throw std::runtime_error("missing or invalid config: soundbox.ws_token is empty");
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

  // 创建 socket 目录并清理残留文件
  std::filesystem::create_directories(options.socket_dir);
  CleanupStaleSocketFiles(options.socket_dir);

  // 拼接各子模块的 Unix Socket 路径
  const std::string frontend_kws_socket =
      (std::filesystem::path(options.socket_dir) / "frontend_kws.sock").string();
  const std::string frontend_aec_socket =
      (std::filesystem::path(options.socket_dir) / "frontend_aec.sock").string();

  // 创建 LLM 客户端并连接
  auto llm_client = std::make_shared<soundbox_server::llm::LlmClient>(
      soundbox_server::llm::LlmClientOptions{
          options.runtime.llm.host,
          options.runtime.llm.port,
      },
      [](const std::string&) {});
  llm_client->Connect();

  // AEC 处理后的音频通过该回调送入 LLM
  auto aec_audio_sink = [llm_client](const std::vector<uint8_t>& processed_pcm) {
    llm_client->SendAudio(processed_pcm);
  };

  // 创建 AEC 处理流
  apm::aec::AecStreamProcessor aec(frontend_aec_socket, aec_audio_sink,
                                     options.processor);
  // 创建 KWS 引擎
  auto kws_engine =
      std::make_shared<xiaoai_server::wakeup::ZipformerKwsEngine>(
          options.runtime.wakeup);
  // 创建 KWS Socket 服务器
  apm::kws::KwsSocketServer::Options kws_options;
  kws_options.listen_socket_path = frontend_kws_socket;
  apm::kws::KwsSocketServer kws(kws_options, kws_engine);

  // 创建 Frontend（前端音频采集/播放模块）
  soundbox_server::frontend::Frontend::Options frontend_options;
  frontend_options.kws_socket_path = frontend_kws_socket;
  frontend_options.aec_socket_path = frontend_aec_socket;
  frontend_options.playback_host = options.runtime.playback.host;
  frontend_options.playback_port = options.runtime.playback.port;
  frontend_options.soundbox_config = options.runtime;
  frontend_options.llm_client = llm_client;
  soundbox_server::frontend::Frontend frontend(frontend_options);

  // 注册信号处理器并监管运行 aec / kws / frontend 三条工作线程
  SignalHandlerGuard signal_handler_guard;
  RunSupervisedWorkers(
      {
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