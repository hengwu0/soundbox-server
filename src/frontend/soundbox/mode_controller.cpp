#include "frontend/soundbox/mode_controller.hpp"

#include "common/log.hpp"

namespace xiaoai_server::soundbox {

namespace {

// kLog 是 soundbox 状态机日志器。
const auto kLog = xiaoai_server::GetLogger("soundbox");

}  // namespace

// 返回 AudioMode 的日志名称。
//
// 参数说明：
// - mode: 音频状态。
// 返回值：
// - 返回稳定字符串。
const char* AudioModeName(AudioMode mode) {
  switch (mode) {
    case AudioMode::Stopped:
      return "Stopped";
    case AudioMode::Kws:
      return "Kws";
    case AudioMode::LlmStarting:
      return "LlmStarting";
    case AudioMode::LlmWorking:
      return "LlmWorking";
    case AudioMode::LlmStopping:
      return "LlmStopping";
    case AudioMode::Fault:
      return "Fault";
  }
  return "Unknown";
}

// 尝试按 expected -> next 切换状态。
//
// 参数说明：
// - expected: 期望当前状态。
// - next: 目标状态。
// - reason: 切换原因。
// 返回值：
// - 切换成功返回 true。
bool ModeController::TryEnter(AudioMode expected, AudioMode next, std::string_view reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (mode_ != expected) {
    return false;
  }
  if (mode_ == next) {
    return true;
  }
  const auto old = mode_;
  mode_ = next;
  kLog->info("soundbox mode change from={} to={} reason={}", AudioModeName(old),
             AudioModeName(next), reason);
  return true;
}

// 强制切换状态。
//
// 参数说明：
// - next: 目标状态。
// - reason: 切换原因。
// 返回值：
// - 状态变化返回 true。
bool ModeController::ForceEnter(AudioMode next, std::string_view reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (mode_ == next) {
    return false;
  }
  const auto old = mode_;
  mode_ = next;
  kLog->info("soundbox mode change from={} to={} reason={}", AudioModeName(old),
             AudioModeName(next), reason);
  return true;
}

// 判断当前状态。
//
// 参数说明：
// - mode: 目标状态。
// 返回值：
// - 当前状态相等返回 true。
bool ModeController::Is(AudioMode mode) const {
  std::lock_guard<std::mutex> lock(mu_);
  return mode_ == mode;
}

// 读取当前状态。
//
// 参数说明：
// - 无。
// 返回值：
// - 返回 mode_。
AudioMode ModeController::Current() const {
  std::lock_guard<std::mutex> lock(mu_);
  return mode_;
}

}  // namespace xiaoai_server::soundbox
