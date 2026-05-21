#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

namespace xiaoai_server {

// RateLimitedLogger 是全局重复日志聚合器。
//
// 聚合规则：
// - key 表示一类重复日志，例如 transition drop、unknown packet、idle ping skip。
// - 同一 key 在窗口期内只立即打印第一条，后续累计 repeated 和 total_bytes。
// - Flush 会输出尚未打印的聚合结果；生产环境可按 3000ms 周期调用。
// - 它是 common 模块能力，不能放在 soundbox 内部，方便 xiaozhi/audio 等模块复用。
class RateLimitedLogger {
 public:
  // 构造聚合 logger。
  //
  // 参数说明：
  // - logger: 真实 spdlog logger。
  // - window: 相同 key 的聚合窗口。
  // - use_background_flush: 是否启用后台 flush；当前实现保留参数，调用方可显式 Flush。
  // 返回值：
  // - 无。
  RateLimitedLogger(std::shared_ptr<spdlog::logger> logger,
                    std::chrono::milliseconds window = std::chrono::milliseconds(3000),
                    bool use_background_flush = false);

  // 析构前刷新聚合日志。
  ~RateLimitedLogger();

  // 记录一条可聚合日志。
  //
  // 参数说明：
  // - level: 输出级别。
  // - key: 聚合 key。
  // - message: 首条日志正文，也是聚合摘要前缀。
  // - bytes: 本条日志关联的字节数，可为 0。
  // 返回值：
  // - 无。
  void Log(spdlog::level::level_enum level, const std::string& key, const std::string& message,
           size_t bytes = 0);

  // 输出所有已有重复累计。
  //
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Flush();

 private:
  // Entry 保存一个 key 的聚合状态。
  struct Entry {
    spdlog::level::level_enum level{spdlog::level::debug};
    std::string message;
    std::chrono::steady_clock::time_point first_seen;
    std::chrono::steady_clock::time_point last_seen;
    size_t repeated{0};
    size_t total_bytes{0};
  };

  // FlushLocked 输出单个 key 的聚合摘要。
  void FlushLocked(const std::string& key, Entry& entry);

  // 启动后台周期 flush 线程。
  void StartBackgroundFlush();

  // 停止后台周期 flush 线程。
  void StopBackgroundFlush();

  // logger_ 是真实输出日志器。
  std::shared_ptr<spdlog::logger> logger_;
  // window_ 是重复聚合窗口。
  std::chrono::milliseconds window_;
  // mu_ 保护 entries_。
  std::mutex mu_;
  // entries_ 保存每个 key 的聚合状态。
  std::unordered_map<std::string, Entry> entries_;
  // background_flush_ 表示是否启动后台强制 flush。
  bool background_flush_{false};
  // stop_flush_ 通知后台线程退出。
  bool stop_flush_{false};
  // flush_cv_ 用于唤醒后台线程退出。
  std::condition_variable flush_cv_;
  // flush_thread_ 按窗口周期输出聚合摘要，满足“无新日志也强制 flush”的要求。
  std::thread flush_thread_;
};

}  // namespace xiaoai_server
