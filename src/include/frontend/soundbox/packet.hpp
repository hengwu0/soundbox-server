#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace xiaoai_server::soundbox {

// PacketType 是 soundbox WebSocket 收包后的统一业务分类。
//
// 分层原因：
// - WebSocket 层只知道 text/binary 帧，不能直接把帧类型当业务类型。
// - open-xiaoai-client 可能把 JSON 放在 text 或 binary 里，尤其 Response 不能只在 text 分支处理。
// - 先归一成 Packet，后续 ResponseHub、EventDispatcher、AudioPipe 才能各司其职。
enum class PacketType {
  Response,
  Event,
  RecordAudio,
  Unknown,
};

// Packet 是解析层对外输出的唯一包结构。
//
// 字段说明：
// - type: 固定按 Response -> Event -> RecordAudio -> Unknown 的优先级分类。
// - binary: 原始 WebSocket 帧是否是 binary，用于日志定位协议方向。
// - raw_summary: 不含完整音频 bytes 的摘要日志文本。
// - id/event/tag/command: 常用协议字段，便于上层日志和分发不用反复查 JSON。
// - json: Response/Event/Stream 的业务 JSON 对象；Response 时保存内层 Response。
// - audio_bytes: tag=record 时保存解析出的原始 PCM 字节。
struct Packet {
  PacketType type{PacketType::Unknown};
  bool binary{false};
  std::string raw_summary;

  std::string id;
  std::string event;
  std::string tag;
  std::string command;

  nlohmann::json json;
  std::vector<uint8_t> audio_bytes;
};

// 返回 PacketType 的稳定日志名称。
//
// 参数说明：
// - type: 待转换的包类型。
// 返回值：
// - 返回 response/event/record/unknown。
const char* PacketTypeName(PacketType type);

}  // namespace xiaoai_server::soundbox
