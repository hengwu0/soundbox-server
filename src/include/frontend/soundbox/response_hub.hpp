#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace xiaoai_server::soundbox {

// ResponseHub 管理 soundbox 同步 RPC 的响应匹配与唤醒。
//
// 设计原因：
// - request_id 是唯一可靠的匹配键，不能靠命令名或到达顺序猜测。
// - Response 可能来自 Text JSON，也可能来自 Binary JSON；解析层统一识别后都调用 Notify。
// - Wait 必须有超时，避免远端丢包或连接抖动时永久卡住唤醒线程。
// - 未消费响应最多保留 256 条，超过后删除最旧响应，防止异常流量导致内存增长。
class ResponseHub {
 public:
  // 重置 hub 为可用状态，并清空旧响应。
  //
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Reset();

  // 关闭 hub 并唤醒所有等待者。
  //
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Close();

  // 发布一条 RPC Response。
  //
  // 参数说明：
  // - response: 解析层提取出的内层 Response JSON。
  // 返回值：
  // - 无。
  void Notify(const nlohmann::json& response);

  // 等待指定 request_id 的 Response。
  //
  // 参数说明：
  // - request_id: 要匹配的请求 ID。
  // - timeout: 最长等待时间，超时返回空。
  // 返回值：
  // - 命中时返回 Response JSON；超时或关闭时返回 std::nullopt。
  std::optional<nlohmann::json> Wait(const std::string& request_id,
                                     std::chrono::milliseconds timeout);

  // 返回当前未消费响应数量。
  //
  // 参数说明：
  // - 无。
  // 返回值：
  // - 当前保留响应条数。
  size_t PendingCount() const;

 private:
  // kMaxRetainedResponses 是异常情况下最多暂存的未消费 Response 数。
  static constexpr size_t kMaxRetainedResponses = 256;

  // mu_ 保护 responses_ 与 closed_。
  mutable std::mutex mu_;
  // cv_ 用于唤醒等待指定 request_id 的调用方。
  std::condition_variable cv_;
  // responses_ 保存尚未被 Wait 消费的 Response JSON。
  std::deque<nlohmann::json> responses_;
  // closed_ 表示连接关闭或客户端停止后不应继续等待。
  bool closed_{false};
};

}  // namespace xiaoai_server::soundbox
