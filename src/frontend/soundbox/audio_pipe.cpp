#include "frontend/soundbox/audio_pipe.hpp"

#include <algorithm>

namespace xiaoai_server::soundbox {

// 构造 AudioPipe。
//
// 参数说明：
// - max_packets: 最大包数。
// - max_bytes: 最大字节数。
// 返回值：
// - 无。
AudioPipe::AudioPipe(size_t max_packets, size_t max_bytes)
    : max_packets_(std::max<size_t>(1, max_packets)),
      max_bytes_(std::max<size_t>(1, max_bytes)) {}

// 析构 AudioPipe。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
AudioPipe::~AudioPipe() { Stop(); }

// 启动 AudioPipe。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void AudioPipe::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  running_ = true;
}

// 停止 AudioPipe。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void AudioPipe::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
    while (!queue_.empty()) {
      DropOldestLocked();
    }
  }
  cv_.notify_all();
}

// 清空积压音频。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void AudioPipe::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  while (!queue_.empty()) {
    DropOldestLocked();
  }
}

// 推入音频包。
//
// 参数说明：
// - chunk: 原始 PCM 字节。
// 返回值：
// - 入队成功返回 true。
bool AudioPipe::Push(std::vector<uint8_t>&& chunk) {
  if (chunk.empty()) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.received_packets;
    stats_.received_bytes += chunk.size();
    if (!running_) {
      ++stats_.dropped_packets;
      stats_.dropped_bytes += chunk.size();
      return false;
    }
    while (!queue_.empty() &&
           (queue_.size() >= max_packets_ || stats_.queued_bytes + chunk.size() > max_bytes_)) {
      DropOldestLocked();
    }
    if (chunk.size() > max_bytes_) {
      ++stats_.dropped_packets;
      stats_.dropped_bytes += chunk.size();
      return false;
    }
    stats_.queued_bytes += chunk.size();
    queue_.push_back(std::move(chunk));
    stats_.queued_packets = queue_.size();
  }
  cv_.notify_one();
  return true;
}

// 弹出音频包。
//
// 参数说明：
// - 无。
// 返回值：
// - 成功返回音频字节；停止且队列空返回空。
std::optional<std::vector<uint8_t>> AudioPipe::Pop() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [this]() { return !running_ || !queue_.empty(); });
  if (queue_.empty()) {
    return std::nullopt;
  }
  auto chunk = std::move(queue_.front());
  queue_.pop_front();
  stats_.queued_bytes -= chunk.size();
  stats_.queued_packets = queue_.size();
  return chunk;
}

// 返回统计快照。
//
// 参数说明：
// - 无。
// 返回值：
// - 返回 AudioPipeStats。
AudioPipeStats AudioPipe::Stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  auto stats = stats_;
  stats.queued_packets = queue_.size();
  return stats;
}

// 返回累计丢弃包数。
//
// 参数说明：
// - 无。
// 返回值：
// - 返回 dropped_packets。
uint64_t AudioPipe::dropped_chunks() const { return Stats().dropped_packets; }

// 丢弃最旧音频包。
//
// 参数说明：
// - 无，调用方必须持有 mu_。
// 返回值：
// - 无。
void AudioPipe::DropOldestLocked() {
  if (queue_.empty()) {
    return;
  }
  stats_.queued_bytes -= queue_.front().size();
  stats_.dropped_bytes += queue_.front().size();
  ++stats_.dropped_packets;
  queue_.pop_front();
  stats_.queued_packets = queue_.size();
}

}  // namespace xiaoai_server::soundbox
