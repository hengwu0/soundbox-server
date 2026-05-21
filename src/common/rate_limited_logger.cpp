#include "common/rate_limited_logger.hpp"

#include <sstream>
#include <utility>

namespace xiaoai_server {

// 构造 RateLimitedLogger。
//
// 参数说明：
// - logger: 真实 spdlog logger。
// - window: 聚合窗口。
// - use_background_flush: 预留后台 flush 开关。
// 返回值：
// - 无。
RateLimitedLogger::RateLimitedLogger(std::shared_ptr<spdlog::logger> logger,
                                     std::chrono::milliseconds window,
                                     bool use_background_flush)
    : logger_(std::move(logger)), window_(window), background_flush_(use_background_flush) {
  if (background_flush_) {
    StartBackgroundFlush();
  }
}

// 析构时刷新重复累计。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
RateLimitedLogger::~RateLimitedLogger() {
  StopBackgroundFlush();
  Flush();
}

// 记录可聚合日志。
//
// 参数说明：
// - level: 日志级别。
// - key: 聚合键。
// - message: 日志正文。
// - bytes: 本次关联字节数。
// 返回值：
// - 无。
void RateLimitedLogger::Log(spdlog::level::level_enum level, const std::string& key,
                            const std::string& message, size_t bytes) {
  if (!logger_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mu_);
  auto& entry = entries_[key];
  if (entry.message.empty()) {
    entry.level = level;
    entry.message = message;
    entry.first_seen = now;
    entry.last_seen = now;
    entry.total_bytes = 0;
    logger_->log(level, "{}", message);
    return;
  }

  if (now - entry.first_seen >= window_) {
    FlushLocked(key, entry);
    entry.level = level;
    entry.message = message;
    entry.first_seen = now;
    entry.last_seen = now;
    entry.repeated = 0;
    entry.total_bytes = 0;
    logger_->log(level, "{}", message);
    return;
  }

  ++entry.repeated;
  entry.total_bytes += bytes;
  entry.last_seen = now;
}

// 刷新所有重复累计。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void RateLimitedLogger::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [key, entry] : entries_) {
    FlushLocked(key, entry);
  }
}

// 输出单个 key 的聚合摘要。
//
// 参数说明：
// - key: 聚合键。
// - entry: 聚合状态。
// 返回值：
// - 无。
void RateLimitedLogger::FlushLocked(const std::string& /*key*/, Entry& entry) {
  if (!logger_ || entry.message.empty() || entry.repeated == 0) {
    return;
  }
  const auto duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(entry.last_seen - entry.first_seen)
          .count();
  logger_->log(entry.level, "{} repeated={} total_bytes={} duration_ms={}", entry.message,
               entry.repeated, entry.total_bytes, duration_ms);
  entry.repeated = 0;
  entry.total_bytes = 0;
  entry.first_seen = std::chrono::steady_clock::now();
  entry.last_seen = entry.first_seen;
}

// 启动后台周期 flush。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void RateLimitedLogger::StartBackgroundFlush() {
  flush_thread_ = std::thread([this]() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_flush_) {
      flush_cv_.wait_for(lock, window_, [this]() { return stop_flush_; });
      if (stop_flush_) {
        break;
      }
      for (auto& [key, entry] : entries_) {
        FlushLocked(key, entry);
      }
    }
  });
}

// 停止后台周期 flush。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void RateLimitedLogger::StopBackgroundFlush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_flush_ = true;
  }
  flush_cv_.notify_all();
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }
}

}  // namespace xiaoai_server
