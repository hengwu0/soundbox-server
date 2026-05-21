#pragma once
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace xiaoai_server {

// DuplicateFilterSink 包装实际输出 sink，压缩连续重复日志并在下一条不同日志前汇总。
class DuplicateFilterSink final : public spdlog::sinks::sink {
 public:
  // 构造重复日志过滤 sink。
  // 参数说明：
  // - targets: 真正承载 stdout/file 输出的下游 sink 列表。
  // 返回值：
  // - 无。
  explicit DuplicateFilterSink(std::vector<spdlog::sink_ptr> targets)
      : targets_(std::move(targets)) {}

  // 处理一条日志；连续重复时只累计计数，不立即写出。
  // 参数说明：
  // - msg: spdlog 传入的原始日志消息。
  // 返回值：
  // - 无。
  void log(const spdlog::details::log_msg& msg) override {
    if (!should_log(msg.level)) {
      return;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const std::string logger_name(msg.logger_name.data(), msg.logger_name.size());
    const std::string payload(msg.payload.data(), msg.payload.size());
    if (has_last_ && logger_name == last_logger_name_ && msg.level == last_level_ &&
        payload == last_payload_) {
      ++repeat_count_;
      return;
    }

    FlushSummaryLocked();
    ForwardLocked(msg);
    has_last_ = true;
    last_logger_name_ = logger_name;
    last_level_ = msg.level;
    last_payload_ = payload;
  }

  // 刷新过滤器与下游 sink；刷新前会输出尚未汇总的重复日志计数。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void flush() override {
    std::lock_guard<std::mutex> lock(mu_);
    FlushSummaryLocked();
    for (const auto& target : targets_) {
      target->flush();
    }
  }

  // 设置日志格式，并同步传递给所有下游 sink。
  // 参数说明：
  // - pattern: spdlog pattern 字符串。
  // 返回值：
  // - 无。
  void set_pattern(const std::string& pattern) override {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& target : targets_) {
      target->set_pattern(pattern);
    }
  }

  // 设置 formatter，并为每个下游 sink 克隆独立 formatter。
  // 参数说明：
  // - sink_formatter: spdlog formatter 实例。
  // 返回值：
  // - 无。
  void set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) override {
    if (!sink_formatter) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < targets_.size(); ++i) {
      if (i + 1 == targets_.size()) {
        targets_[i]->set_formatter(std::move(sink_formatter));
      } else {
        targets_[i]->set_formatter(sink_formatter->clone());
      }
    }
  }

 private:
  // 把一条日志直接写给所有下游 sink。
  // 参数说明：
  // - msg: 要输出的日志消息。
  // 返回值：
  // - 无。
  void ForwardLocked(const spdlog::details::log_msg& msg) {
    for (const auto& target : targets_) {
      target->log(msg);
    }
  }

  // 如果上一条日志被重复压缩，则输出一条汇总日志。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void FlushSummaryLocked() {
    if (!has_last_ || repeat_count_ == 0) {
      return;
    }
    const std::string summary =
        "previous log repeated " + std::to_string(repeat_count_) + " times";
    spdlog::details::log_msg summary_msg(last_logger_name_, last_level_, summary);
    ForwardLocked(summary_msg);
    repeat_count_ = 0;
  }

  // targets_ 保存真正写 stdout/file 的下游 sink。
  std::vector<spdlog::sink_ptr> targets_;
  // mu_ 保护重复日志状态与下游 sink 配置。
  std::mutex mu_;
  // has_last_ 表示是否已经写出过一条可用于比较的日志。
  bool has_last_{false};
  // last_logger_name_ 保存上一条已写出日志的 logger 名称。
  std::string last_logger_name_;
  // last_payload_ 保存上一条已写出日志的正文。
  std::string last_payload_;
  // last_level_ 保存上一条已写出日志的级别。
  spdlog::level::level_enum last_level_{spdlog::level::off};
  // repeat_count_ 保存上一条日志被连续抑制的次数。
  size_t repeat_count_{0};
};

// 返回全局日志级别存储，用于在 logger 延迟创建时继承当前级别。
// 参数说明：
// - 无。
// 返回值：
// - 返回全局原子日志级别变量的引用。
inline std::atomic<int>& GlobalLogLevelStorage() {
  // level 保存当前全局日志级别，logger 创建较晚时也能继承最新设置。
  static std::atomic<int> level{static_cast<int>(spdlog::level::info)};
  return level;
}

// 返回日志 sink 配置互斥锁，避免运行时重配和 logger 创建并发修改同一组 sink。
// 参数说明：
// - 无。
// 返回值：
// - 返回全局互斥锁引用。
inline std::mutex& GlobalLogMutex() {
  // mu 保护全局 sink 列表和 logger 重配过程。
  static std::mutex mu;
  return mu;
}

// 返回进程级 stdout sink。所有 logger 共享同一个 stdout sink，避免重复创建终端输出端。
// 参数说明：
// - 无。
// 返回值：
// - 返回 stdout 彩色 sink。
inline spdlog::sink_ptr StdoutSink() {
  // sink 是所有 logger 共享的彩色 stdout 输出端。
  static spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  return sink;
}

// 返回当前所有 logger 应使用的 sink 列表。
// 参数说明：
// - 无。
// 返回值：
// - 返回 sink 列表引用；调用方修改前必须持有 GlobalLogMutex。
inline std::vector<spdlog::sink_ptr>& GlobalLogSinks() {
  // sinks 保存当前所有 logger 应绑定的输出端列表。
  static std::vector<spdlog::sink_ptr> sinks{
      std::make_shared<DuplicateFilterSink>(std::vector<spdlog::sink_ptr>{StdoutSink()})};
  return sinks;
}

// 对单个 logger 应用统一格式、级别、flush 策略和 sink 列表。
// 参数说明：
// - logger: 待更新的 logger；为空时直接返回。
// - sinks: 要绑定到 logger 的 sink 列表。
// 返回值：
// - 无。
inline void ApplyLoggerSettings(const std::shared_ptr<spdlog::logger>& logger,
                                const std::vector<spdlog::sink_ptr>& sinks) {
  if (!logger) {
    return;
  }
  logger->sinks().clear();
  logger->sinks().insert(logger->sinks().end(), sinks.begin(), sinks.end());
  logger->set_pattern("[%H:%M:%S.%e] [%-8n] [%^%-5l%$] %v");
  logger->set_level(static_cast<spdlog::level::level_enum>(GlobalLogLevelStorage().load()));
  logger->flush_on(spdlog::level::info);
}

// 根据配置切换日志级别和输出位置。
// 参数说明：
// - debug_enabled: 为 true 时开启 debug 级别，否则使用 info 级别。
// - file_enabled: 为 true 时同时写入普通日志文件。
// - file_path: 普通日志文件路径；相对路径会按当前工作目录解析。
// 返回值：
// - 文件日志启用且成功创建时返回 true；未启用文件日志或创建失败时返回 false。
inline bool ConfigureLogging(bool debug_enabled,
                             bool file_enabled,
                             const std::string& file_path) {
  // level 是根据配置计算出的进程级日志级别。
  const auto level = debug_enabled ? spdlog::level::debug : spdlog::level::info;
  GlobalLogLevelStorage().store(static_cast<int>(level));
  spdlog::set_level(level);

  // target_sinks 从 stdout 开始，文件日志开启时再追加 file sink。
  std::vector<spdlog::sink_ptr> target_sinks{StdoutSink()};
  // file_ready 表示文件 sink 是否成功创建。
  bool file_ready = false;
  if (file_enabled) {
    try {
      // path 是普通日志文件输出路径，空配置时使用默认 logs 目录。
      std::filesystem::path path = file_path.empty() ? "logs/open-xiaoai-server.log" : file_path;
      if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
      }
      target_sinks.push_back(
          std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true));
      file_ready = true;
    } catch (const std::exception& e) {
      std::cerr << "failed to enable file logging: " << e.what() << std::endl;
    }
  }

  // sinks 是 logger 实际绑定的过滤 sink；它会把非重复日志扇出到 target_sinks。
  std::vector<spdlog::sink_ptr> sinks{
      std::make_shared<DuplicateFilterSink>(std::move(target_sinks))};
  {
    std::lock_guard<std::mutex> lock(GlobalLogMutex());
    GlobalLogSinks() = sinks;
    spdlog::apply_all([&sinks](const std::shared_ptr<spdlog::logger>& logger) {
      ApplyLoggerSettings(logger, sinks);
    });
  }
  return file_ready;
}

// 兼容旧调用点：只切换 debug 级别，不启用文件日志。
// 参数说明：
// - enabled: 为 true 时开启 debug 级别，否则使用 info 级别。
// 返回值：
// - 无。
inline void SetDebugLogging(bool enabled) {
  (void)ConfigureLogging(enabled, false, "");
}

// 获取指定名称的 logger；若尚未创建则按统一格式新建一个。
// 参数说明：
// - name: logger 名称，同时会体现在日志输出前缀中。
// 返回值：
// - 返回可直接使用的 spdlog logger 共享指针。
inline std::shared_ptr<spdlog::logger> GetLogger(const char* name) {
  if (auto logger = spdlog::get(name)) return logger;
  try {
    std::lock_guard<std::mutex> lock(GlobalLogMutex());
    // logger 复用当前全局 sink 列表，保证后续 ConfigureLogging 能统一重配。
    auto logger = std::make_shared<spdlog::logger>(name, GlobalLogSinks().begin(),
                                                   GlobalLogSinks().end());
    ApplyLoggerSettings(logger, GlobalLogSinks());
    spdlog::register_logger(logger);
    return logger;
  } catch (...) {
    return spdlog::get(name);
  }
}

}  // namespace xiaoai_server
