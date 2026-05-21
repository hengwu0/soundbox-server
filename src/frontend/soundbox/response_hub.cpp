#include "frontend/soundbox/response_hub.hpp"

#include <algorithm>

namespace xiaoai_server::soundbox {

// 重置 ResponseHub。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void ResponseHub::Reset() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    responses_.clear();
    closed_ = false;
  }
  cv_.notify_all();
}

// 关闭 ResponseHub 并唤醒等待者。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void ResponseHub::Close() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    responses_.clear();
  }
  cv_.notify_all();
}

// 发布一条 Response。
//
// 参数说明：
// - response: 内层 Response JSON。
// 返回值：
// - 无。
void ResponseHub::Notify(const nlohmann::json& response) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      return;
    }
    responses_.push_back(response);
    while (responses_.size() > kMaxRetainedResponses) {
      responses_.pop_front();
    }
  }
  cv_.notify_all();
}

// 等待并消费指定 request_id 的响应。
//
// 参数说明：
// - request_id: 目标请求 ID。
// - timeout: 最长等待时间。
// 返回值：
// - 命中返回响应 JSON；超时或关闭返回空。
std::optional<nlohmann::json> ResponseHub::Wait(const std::string& request_id,
                                                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock<std::mutex> lock(mu_);

  auto pop_response = [this, &request_id]() -> std::optional<nlohmann::json> {
    auto it = std::find_if(responses_.begin(), responses_.end(), [&request_id](const auto& item) {
      return item.value("id", std::string()) == request_id;
    });
    if (it == responses_.end()) {
      return std::nullopt;
    }
    auto response = *it;
    responses_.erase(it);
    return response;
  };

  while (!closed_) {
    if (auto response = pop_response()) {
      return response;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    cv_.wait_until(lock, deadline);
  }
  return std::nullopt;
}

// 返回当前保留响应数量。
//
// 参数说明：
// - 无。
// 返回值：
// - 返回 responses_ 长度。
size_t ResponseHub::PendingCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return responses_.size();
}

}  // namespace xiaoai_server::soundbox
