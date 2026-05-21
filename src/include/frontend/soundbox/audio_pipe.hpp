#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace xiaoai_server::soundbox {

// AudioPipeStats 保存 record 音频队列的累计和当前统计。
struct AudioPipeStats {
  // received_packets 是收到的非空 record 包数量。
  uint64_t received_packets{0};
  // dropped_packets 是因未运行、队列满或字节预算超限而丢弃的包数量。
  uint64_t dropped_packets{0};
  // received_bytes 是累计收到的音频字节数。
  uint64_t received_bytes{0};
  // dropped_bytes 是累计丢弃的音频字节数。
  uint64_t dropped_bytes{0};
  // queued_packets 是当前队列内待路由包数量。
  size_t queued_packets{0};
  // queued_bytes 是当前队列内待路由总字节数。
  size_t queued_bytes{0};
};

// AudioPipe 是 soundbox record 音频的线程安全队列。
//
// 它只负责承接和背压，不做 KWS/VAD/AEC 业务路由：
// - 最大 100 包。
// - 最大 10 MiB。
// - 队列满时丢弃最旧包，保留实时性。
// - 统计收到包数、丢弃包数、累计字节数和当前队列长度。
class AudioPipe {
 public:
  // 构造音频队列。
  //
  // 参数说明：
  // - max_packets: 队列最大包数，小于 1 时按 1 处理。
  // - max_bytes: 队列最大字节数，小于 1 时按 1 处理。
  // 返回值：
  // - 无。
  explicit AudioPipe(size_t max_packets = 100, size_t max_bytes = 10 * 1024 * 1024);

  // 析构并停止队列。
  ~AudioPipe();

  // 允许 Push/Pop 继续工作。
  void Start();

  // 停止队列并清空积压音频。
  void Stop();

  // 清空当前队列，常用于 Fault 恢复前丢弃过渡态积压音频。
  void Clear();

  // 推入一块 record 音频。
  //
  // 参数说明：
  // - chunk: record 包中的原始 PCM 字节。
  // 返回值：
  // - 接收入队返回 true；空包或未运行返回 false。
  bool Push(std::vector<uint8_t>&& chunk);

  // 阻塞弹出一块音频。
  //
  // 参数说明：
  // - 无。
  // 返回值：
  // - 成功返回音频字节；停止且队列空时返回 std::nullopt。
  std::optional<std::vector<uint8_t>> Pop();

  // 返回当前队列统计快照。
  AudioPipeStats Stats() const;

  // 返回累计丢弃包数，兼容旧测试和日志调用。
  uint64_t dropped_chunks() const;

 private:
  // 丢弃队首旧包并更新统计。
  void DropOldestLocked();

  // max_packets_ 是队列最大包数。
  const size_t max_packets_;
  // max_bytes_ 是队列最大字节数。
  const size_t max_bytes_;

  // mu_ 保护 running_、queue_、queued_bytes_ 和统计。
  mutable std::mutex mu_;
  // cv_ 在新音频入队或停止时唤醒 Pop。
  std::condition_variable cv_;
  // queue_ 保存等待 AudioRouter 路由的 record 音频。
  std::deque<std::vector<uint8_t>> queue_;
  // running_ 表示队列是否接受新音频。
  bool running_{false};
  // stats_ 保存累计统计。
  AudioPipeStats stats_;
};

}  // namespace xiaoai_server::soundbox
