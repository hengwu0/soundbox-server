#pragma once

#include <string>
#include <mutex>

#include "frontend/soundbox/mode_controller.hpp"

namespace xiaoai_server::soundbox {

// CommandGuardAction 描述幂等层对一次命令调用的处理决定。
enum class CommandGuardAction {
  // Send 表示允许发送真实 RPC；必要时已完成状态预切换。
  Send,
  // IdempotentSuccess 表示当前状态已经满足目标语义，直接返回成功，不发 RPC。
  IdempotentSuccess,
  // Ignore 表示切换中或重复请求应静默忽略，不发 RPC，不报错。
  Ignore,
  // Reject 表示当前状态禁止普通入口调用，典型场景是 Fault。
  Reject,
};

// CommandGuardDecision 是幂等检查结果。
struct CommandGuardDecision {
  // action 是本次命令要采取的处理动作。
  CommandGuardAction action{CommandGuardAction::Reject};
  // reason 是日志可读原因。
  std::string reason;
};

// CommandGuard 对 start/stop 类命令做状态检查和幂等保护。
//
// 幂等的原因：
// - Response 丢失、超时重试、KWS 重复触发、VAD/TTS 并发都可能重复调用 start/stop。
// - 如果重复 llm_start/llm_stop 继续下发，会让 client 反复 reopen ALSA 或打乱采集格式。
// - Fault 下普通入口全部拒绝，只允许恢复流程带 recovery 标记调用 stop/start 链路。
class CommandGuard {
 public:
  // 准备发送 llm_start。
  CommandGuardDecision PrepareLlmStart(ModeController& mode) const;

  // 准备发送 llm_stop。
  CommandGuardDecision PrepareLlmStop(ModeController& mode) const;

  // 准备发送 start_play。
  CommandGuardDecision PrepareStartPlay(const ModeController& mode, bool recovery = false);

  // 准备发送 fast_recording。
  CommandGuardDecision PrepareFastRecording(const ModeController& mode,
                                            bool recovery = false);

  // 准备发送 stop_recording。
  CommandGuardDecision PrepareStopRecording(const ModeController& mode,
                                            bool recovery = false);

  // 准备发送 stop_play。
  CommandGuardDecision PrepareStopPlay(const ModeController& mode, bool recovery = false);

  // 标记 start_play 成功。
  void MarkStartPlaySucceeded();

  // 标记 fast_recording 成功。
  void MarkFastRecordingSucceeded();

  // 标记 stop_recording 成功。
  void MarkStopRecordingSucceeded();

  // 标记 stop_play 成功。
  void MarkStopPlaySucceeded();

  // 连接重建或 Stop 后清空幂等层记录的远端音频链路状态。
  void ResetAudioState();

 private:
  // mu_ 保护 play_started_ 和 recording_started_。
  std::mutex mu_;
  // play_started_ 表示 server 认为远端播放链路已经启动。
  bool play_started_{false};
  // recording_started_ 表示 server 认为远端录音链路已经启动。
  bool recording_started_{false};
};

}  // namespace xiaoai_server::soundbox
