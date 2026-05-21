#pragma once

#include <string_view>

#include "frontend/soundbox/packet.hpp"

namespace xiaoai_server::soundbox {

// PacketParser 是 soundbox 收包解析层的唯一入口。
//
// 重要约束：
// - Text/Binary 帧都可能承载 JSON，帧类型只用于日志，不用于判断是否能处理 Response。
// - 分类优先级固定为 Response、Event、tag=record、Unknown。
// - Response 必须优先处理，因为 RPC 等待线程可能正在阻塞；如果同时带 event/tag 字段也不能误分发。
// - 音频摘要只记录 bytes_len，禁止把完整 bytes 数组写入日志。
class PacketParser {
 public:
  // 解析一帧 WebSocket 数据。
  //
  // 参数说明：
  // - binary: 原始 WebSocket 帧是否是二进制帧。
  // - payload: 原始帧内容；当前协议里 text/binary 都是 JSON 文本。
  // 返回值：
  // - 返回统一 Packet；解析失败或不认识的包返回 PacketType::Unknown。
  Packet Parse(bool binary, std::string_view payload) const;
};

}  // namespace xiaoai_server::soundbox
