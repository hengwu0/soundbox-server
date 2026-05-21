#include "frontend/soundbox/packet.hpp"

namespace xiaoai_server::soundbox {

// 返回统一包类型的日志名称。
//
// 参数说明：
// - type: 统一解析后的 PacketType。
// 返回值：
// - 返回小写英文名称，未知枚举值返回 unknown。
const char* PacketTypeName(PacketType type) {
  switch (type) {
    case PacketType::Response:
      return "response";
    case PacketType::Event:
      return "event";
    case PacketType::RecordAudio:
      return "record";
    case PacketType::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::vector<uint8_t> BuildPlayPcmPacket(const std::string& id,
                                        const std::vector<uint8_t>& chunk) {
  nlohmann::json stream = {
      {"id", id},
      {"tag", "play"},
      {"bytes", chunk},
  };
  const auto dumped = stream.dump();
  return std::vector<uint8_t>(dumped.begin(), dumped.end());
}

}  // namespace xiaoai_server::soundbox
