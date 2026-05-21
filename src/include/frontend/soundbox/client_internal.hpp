#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "frontend/soundbox/mode_controller.hpp"

namespace xiaoai_server::soundbox::internal {

// kRestartSoundboxAudioFailedReason 是恢复链路失败后通知 App 的稳定原因。
inline constexpr const char* kRestartSoundboxAudioFailedReason =
    "restart_soundbox_audio_failed";

// 生成 record 音频日志聚合 key。
//
// 参数说明：
// - mode: 收到 record 音频时的当前 AudioMode。
// 返回值：
// - 返回带 mode 后缀的聚合 key，避免不同模式共享首条日志正文。
inline std::string RecordAudioLogKey(AudioMode mode) {
  return std::string("rx-record-audio-") + AudioModeName(mode);
}

// 生成 soundbox 普通 JSON debug 日志正文。
//
// 参数说明：
// - prefix: 日志方向前缀，例如 [SOUNDBOX][SEND] 或 [SOUNDBOX][RAW]。
// - payload: 要打印的 JSON 原文。
// 返回值：
// - 返回包含完整 JSON 的 debug 日志正文。
inline std::string SoundboxJsonDebugLog(std::string_view prefix, std::string_view payload) {
  std::string out(prefix);
  out.push_back(' ');
  out.append(payload.data(), payload.size());
  return out;
}

// 从 soundbox 音频 JSON 文本中提取 bytes 数组长度。
//
// 参数说明：
// - payload: soundbox 音频包 JSON 文本。
// 返回值：
// - 解析成功且存在 bytes 数组时返回数组长度，否则返回 std::nullopt。
inline std::optional<size_t> ExtractSoundboxAudioBytesLength(std::string_view payload) {
  const auto json = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return std::nullopt;
  }
  const auto bytes = json.find("bytes");
  if (bytes == json.end() || !bytes->is_array()) {
    return std::nullopt;
  }
  return bytes->size();
}

// 生成 soundbox 音频 debug 日志正文，只输出字节数，不展开 JSON bytes 数组。
//
// 参数说明：
// - prefix: 音频日志前缀，例如 [SOUNDBOX][SEND_AUDIO] 或 [SOUNDBOX][AUDIO]。
// - audio_bytes: 业务音频字节数。
// - payload_bytes: WebSocket payload 字节数。
// 返回值：
// - 返回只含字节统计的 debug 日志正文。
inline std::string SoundboxAudioDebugLog(std::string_view prefix, size_t audio_bytes,
                                         size_t payload_bytes) {
  std::string out(prefix);
  out += " audio_bytes=" + std::to_string(audio_bytes);
  out += " payload_bytes=" + std::to_string(payload_bytes);
  return out;
}

// 生成 soundbox 音频 JSON debug 日志正文，只提取 bytes 数组长度。
//
// 参数说明：
// - prefix: 音频日志前缀。
// - payload: soundbox 音频包 JSON 文本。
// 返回值：
// - 返回只含字节统计的 debug 日志正文；解析失败时只输出 payload 字节数。
inline std::string SoundboxAudioDebugLog(std::string_view prefix, std::string_view payload) {
  const auto audio_bytes = ExtractSoundboxAudioBytesLength(payload);
  if (audio_bytes) {
    return SoundboxAudioDebugLog(prefix, *audio_bytes, payload.size());
  }

  std::string out(prefix);
  out += " audio_bytes=unknown payload_bytes=" + std::to_string(payload.size());
  return out;
}

// 处理 soundbox audio 链路恢复失败后的关闭通知。
//
// 参数说明：
// - close_connection: 关闭 WebSocket 连接的回调。
// - notify_connection_closed: 通知 App 连接已关闭的回调。
// 返回值：
// - 无。
template <typename CloseConnection, typename NotifyConnectionClosed>
void NotifyRestartSoundboxAudioFailed(CloseConnection&& close_connection,
                                      NotifyConnectionClosed&& notify_connection_closed) {
  std::forward<CloseConnection>(close_connection)();
  std::forward<NotifyConnectionClosed>(notify_connection_closed)(
      kRestartSoundboxAudioFailedReason);
}

}  // namespace xiaoai_server::soundbox::internal
