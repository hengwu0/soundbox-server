#include "frontend/soundbox/command_guard.hpp"

#include <utility>

namespace xiaoai_server::soundbox {

namespace {

// 构造命令幂等结果。
CommandGuardDecision Decision(CommandGuardAction action, std::string reason) {
  return CommandGuardDecision{action, std::move(reason)};
}

// 判断普通入口是否因为 Fault 被禁止。
bool IsFaultBlocked(const ModeController& mode, bool recovery) {
  return mode.Is(AudioMode::Fault) && !recovery;
}

}  // namespace

// 检查 llm_start 并在 Kws 下预切到 LlmStarting。
//
// 参数说明：
// - mode: 音频模式控制器。
// 返回值：
// - 返回 Send/IdempotentSuccess/Ignore/Reject。
CommandGuardDecision CommandGuard::PrepareLlmStart(ModeController& mode) const {
  switch (mode.Current()) {
    case AudioMode::Kws:
      if (mode.TryEnter(AudioMode::Kws, AudioMode::LlmStarting, "kws_hit")) {
        return Decision(CommandGuardAction::Send, "send_llm_start");
      }
      return Decision(CommandGuardAction::Ignore, "llm_start_race");
    case AudioMode::LlmStarting:
      return Decision(CommandGuardAction::Ignore, "duplicate_llm_start_pending");
    case AudioMode::LlmWorking:
      return Decision(CommandGuardAction::IdempotentSuccess, "already_llm_working");
    case AudioMode::LlmStopping:
      return Decision(CommandGuardAction::Ignore, "llm_stop_pending");
    case AudioMode::Stopped:
      return Decision(CommandGuardAction::Reject, "stopped");
    case AudioMode::Fault:
      return Decision(CommandGuardAction::Reject, "fault");
  }
  return Decision(CommandGuardAction::Reject, "unknown_mode");
}

// 检查 llm_stop 并在 LlmWorking 下预切到 LlmStopping。
//
// 参数说明：
// - mode: 音频模式控制器。
// 返回值：
// - 返回 Send/IdempotentSuccess/Ignore/Reject。
CommandGuardDecision CommandGuard::PrepareLlmStop(ModeController& mode) const {
  switch (mode.Current()) {
    case AudioMode::Kws:
      return Decision(CommandGuardAction::IdempotentSuccess, "already_kws");
    case AudioMode::LlmStarting:
      return Decision(CommandGuardAction::Ignore, "llm_start_pending");
    case AudioMode::LlmWorking:
      if (mode.TryEnter(AudioMode::LlmWorking, AudioMode::LlmStopping, "session_end")) {
        return Decision(CommandGuardAction::Send, "send_llm_stop");
      }
      return Decision(CommandGuardAction::Ignore, "llm_stop_race");
    case AudioMode::LlmStopping:
      return Decision(CommandGuardAction::Ignore, "duplicate_llm_stop_pending");
    case AudioMode::Stopped:
      return Decision(CommandGuardAction::IdempotentSuccess, "already_stopped");
    case AudioMode::Fault:
      return Decision(CommandGuardAction::Reject, "fault");
  }
  return Decision(CommandGuardAction::Reject, "unknown_mode");
}

// 检查 start_play。
//
// 参数说明：
// - mode: 当前状态。
// - recovery: true 表示 Fault 恢复流程内部调用。
// 返回值：
// - Fault 普通入口拒绝，其余允许发送。
CommandGuardDecision CommandGuard::PrepareStartPlay(const ModeController& mode,
                                                    bool recovery) {
  if (IsFaultBlocked(mode, recovery)) {
    return Decision(CommandGuardAction::Reject, "fault");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (play_started_ && !recovery) {
    return Decision(CommandGuardAction::IdempotentSuccess, "play_already_started");
  }
  return Decision(CommandGuardAction::Send, recovery ? "recovery_start_play" : "send_start_play");
}

// 检查 fast_recording。
//
// 参数说明：
// - mode: 当前状态。
// - recovery: true 表示 Fault 恢复流程内部调用。
// 返回值：
// - Fault 普通入口拒绝，其余允许发送。
CommandGuardDecision CommandGuard::PrepareFastRecording(const ModeController& mode,
                                                        bool recovery) {
  if (IsFaultBlocked(mode, recovery)) {
    return Decision(CommandGuardAction::Reject, "fault");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (recording_started_ && !recovery) {
    return Decision(CommandGuardAction::IdempotentSuccess, "recording_already_started");
  }
  return Decision(CommandGuardAction::Send,
                  recovery ? "recovery_fast_recording" : "send_fast_recording");
}

// 检查 stop_recording。
//
// 参数说明：
// - mode: 当前状态。
// - recovery: true 表示 Fault 恢复流程内部调用。
// 返回值：
// - Stopped 幂等成功；Fault 普通入口拒绝；其余允许发送。
CommandGuardDecision CommandGuard::PrepareStopRecording(const ModeController& mode,
                                                        bool recovery) {
  if (mode.Is(AudioMode::Stopped) && !recovery) {
    return Decision(CommandGuardAction::IdempotentSuccess, "already_stopped");
  }
  if (IsFaultBlocked(mode, recovery)) {
    return Decision(CommandGuardAction::Reject, "fault");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!recording_started_ && !recovery) {
    return Decision(CommandGuardAction::IdempotentSuccess, "recording_already_stopped");
  }
  return Decision(CommandGuardAction::Send,
                  recovery ? "recovery_stop_recording" : "send_stop_recording");
}

// 检查 stop_play。
//
// 参数说明：
// - mode: 当前状态。
// - recovery: true 表示 Fault 恢复流程内部调用。
// 返回值：
// - Stopped 幂等成功；Fault 普通入口拒绝；其余允许发送。
CommandGuardDecision CommandGuard::PrepareStopPlay(const ModeController& mode,
                                                   bool recovery) {
  if (mode.Is(AudioMode::Stopped) && !recovery) {
    return Decision(CommandGuardAction::IdempotentSuccess, "already_stopped");
  }
  if (IsFaultBlocked(mode, recovery)) {
    return Decision(CommandGuardAction::Reject, "fault");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!play_started_ && !recovery) {
    return Decision(CommandGuardAction::IdempotentSuccess, "play_already_stopped");
  }
  return Decision(CommandGuardAction::Send, recovery ? "recovery_stop_play" : "send_stop_play");
}

// 标记播放链路启动成功。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void CommandGuard::MarkStartPlaySucceeded() {
  std::lock_guard<std::mutex> lock(mu_);
  play_started_ = true;
}

// 标记录音链路启动成功。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void CommandGuard::MarkFastRecordingSucceeded() {
  std::lock_guard<std::mutex> lock(mu_);
  recording_started_ = true;
}

// 标记录音链路停止成功。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void CommandGuard::MarkStopRecordingSucceeded() {
  std::lock_guard<std::mutex> lock(mu_);
  recording_started_ = false;
}

// 标记播放链路停止成功。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void CommandGuard::MarkStopPlaySucceeded() {
  std::lock_guard<std::mutex> lock(mu_);
  play_started_ = false;
}

// 重置远端链路幂等状态。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
void CommandGuard::ResetAudioState() {
  std::lock_guard<std::mutex> lock(mu_);
  play_started_ = false;
  recording_started_ = false;
}

}  // namespace xiaoai_server::soundbox
