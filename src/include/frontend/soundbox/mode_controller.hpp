#pragma once

#include <mutex>
#include <string_view>

namespace xiaoai_server::soundbox {

// AudioMode 是 soundbox 上行音频的唯一状态机。
//
// 状态含义：
// - Stopped: 录音未启动或连接未就绪，所有音频丢弃。
// - Kws: client 输出 KWS 单声道 16k/16bit 音频，路由到 wakeup 模块。
// - LlmStarting: 已发 llm_start，等待 llm_start_ok，期间音频格式正在切换，必须丢弃。
// - LlmWorking: 已确认 LLM raw 双通道音频，路由到 audio/VAD/AEC。
// - LlmStopping: 已发 llm_stop，等待 llm_stop_ok，期间音频格式正在切换，必须丢弃。
// - Fault: 切换超时或状态不一致，只丢弃音频，并触发 soundbox audio 链路重启。
//
// LlmStarting/LlmStopping 丢弃音频的原因：
// 切换窗口里 client 可能输出旧格式、半切换格式或新格式，如果误送给 KWS/VAD/AEC 会污染状态机。
enum class AudioMode {
  Stopped,
  Kws,
  LlmStarting,
  LlmWorking,
  LlmStopping,
  Fault,
};

// 返回 AudioMode 的稳定日志名称。
//
// 参数说明：
// - mode: 待转换状态。
// 返回值：
// - 返回 Stopped/Kws/LlmStarting/LlmWorking/LlmStopping/Fault。
const char* AudioModeName(AudioMode mode);

// ModeController 负责所有 AudioMode 读写和状态切换日志。
//
// 合法主路径：
// - Stopped -> Kws: start_recording_ok
// - Kws -> LlmStarting -> LlmWorking: llm_start 成功
// - LlmWorking -> LlmStopping -> Kws: llm_stop 成功
// - LlmStarting/LlmStopping -> Fault: 切换超时或失败
// - Fault -> Kws/Stopped: soundbox audio 链路恢复成功/失败
class ModeController {
 public:
  // 按期望状态做 CAS 式切换。
  //
  // 参数说明：
  // - expected: 调用方认为当前必须处于的状态。
  // - next: 要切入的新状态。
  // - reason: 状态变化原因，写入日志。
  // 返回值：
  // - 当前状态等于 expected 并完成切换时返回 true，否则返回 false。
  bool TryEnter(AudioMode expected, AudioMode next, std::string_view reason = "");

  // 强制切换到指定状态。
  //
  // 参数说明：
  // - next: 要切入的新状态。
  // - reason: 状态变化原因，写入日志。
  // 返回值：
  // - 状态发生变化返回 true；原本就是该状态返回 false。
  bool ForceEnter(AudioMode next, std::string_view reason);

  // 判断当前状态是否等于给定状态。
  //
  // 参数说明：
  // - mode: 目标状态。
  // 返回值：
  // - 相等返回 true。
  bool Is(AudioMode mode) const;

  // 读取当前状态。
  //
  // 参数说明：
  // - 无。
  // 返回值：
  // - 返回当前 AudioMode。
  AudioMode Current() const;

 private:
  // mu_ 保护 mode_。
  mutable std::mutex mu_;
  // mode_ 保存当前音频状态，初始为 Stopped。
  AudioMode mode_{AudioMode::Stopped};
};

}  // namespace xiaoai_server::soundbox
