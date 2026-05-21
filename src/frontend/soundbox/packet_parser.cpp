#include "frontend/soundbox/packet_parser.hpp"

#include <algorithm>
#include <sstream>

namespace xiaoai_server::soundbox {

namespace {

// 解析 JSON bytes 数组成原始字节。
//
// 参数说明：
// - value: 形如 [0, 255] 的 JSON 数组。
// 返回值：
// - 格式合法时返回字节数组；格式非法时返回空数组。
std::vector<uint8_t> ParseJsonByteArray(const nlohmann::json& value) {
  if (!value.is_array()) {
    return {};
  }

  std::vector<uint8_t> out;
  out.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_number_integer() && !item.is_number_unsigned()) {
      return {};
    }
    const int v = item.get<int>();
    if (v < 0 || v > 255) {
      return {};
    }
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

// 从 payload 中取出可能存在的协议 wrapper。
//
// 参数说明：
// - root: 外层 JSON 对象。
// - key: wrapper 字段名，例如 Response/Event/Request。
// 返回值：
// - wrapper 存在且是 object 时返回指针，否则返回 nullptr。
const nlohmann::json* FindObjectWrapper(const nlohmann::json& root, const char* key) {
  const auto it = root.find(key);
  if (it == root.end() || !it->is_object()) {
    return nullptr;
  }
  return &*it;
}

// 生成一条不包含完整音频 bytes 的收包摘要。
//
// 参数说明：
// - packet: 已归类的 Packet。
// - raw_len: 原始帧字节数。
// 返回值：
// - 返回统一日志摘要。
std::string BuildSummary(const Packet& packet, size_t raw_len) {
  std::ostringstream out;
  out << "rx soundbox packet frame=" << (packet.binary ? "binary" : "text")
      << " type=" << PacketTypeName(packet.type);

  if (!packet.id.empty()) {
    out << " id=" << packet.id;
  }
  switch (packet.type) {
    case PacketType::Response:
      if (packet.json.contains("code")) {
        out << " code=" << packet.json.value("code", 0);
      }
      if (packet.json.contains("msg")) {
        out << " msg=" << packet.json.value("msg", std::string());
      }
      break;
    case PacketType::Event:
      if (!packet.event.empty()) {
        out << " event=" << packet.event;
      }
      break;
    case PacketType::RecordAudio:
      out << " bytes_len=" << packet.audio_bytes.size();
      break;
    case PacketType::Unknown:
      out << " raw_len=" << raw_len;
      break;
  }
  return out.str();
}

}  // namespace

// 解析 WebSocket 帧为统一 Packet。
//
// 参数说明：
// - binary: 原始帧是否是 binary。
// - payload: 原始帧内容。
// 返回值：
// - 返回分类后的 Packet；失败时为 Unknown。
Packet PacketParser::Parse(bool binary, std::string_view payload) const {
  Packet packet;
  packet.binary = binary;

  const auto root = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    packet.type = PacketType::Unknown;
    packet.raw_summary = BuildSummary(packet, payload.size());
    return packet;
  }

  // Response 优先级最高，且不能绑定到 text 帧；binary JSON Response 也必须唤醒 ResponseHub。
  if (const auto* response = FindObjectWrapper(root, "Response")) {
    packet.type = PacketType::Response;
    packet.json = *response;
    packet.id = response->value("id", std::string());
    packet.command = response->value("command", std::string());
    packet.raw_summary = BuildSummary(packet, payload.size());
    return packet;
  }
  if (root.contains("Response") && root["Response"].is_object()) {
    packet.type = PacketType::Response;
    packet.json = root["Response"];
    packet.id = packet.json.value("id", std::string());
    packet.command = packet.json.value("command", std::string());
    packet.raw_summary = BuildSummary(packet, payload.size());
    return packet;
  }

  // Event 既可能被 AppMessage::Event 包一层，也可能以 event 字段裸露出现。
  if (const auto* event = FindObjectWrapper(root, "Event")) {
    packet.type = PacketType::Event;
    packet.json = *event;
    packet.id = event->value("id", std::string());
    packet.event = event->value("event", std::string());
    packet.raw_summary = BuildSummary(packet, payload.size());
    return packet;
  }
  if (root.contains("event")) {
    packet.type = PacketType::Event;
    packet.json = root;
    packet.id = root.value("id", std::string());
    packet.event = root.value("event", std::string());
    packet.raw_summary = BuildSummary(packet, payload.size());
    return packet;
  }

  packet.tag = root.value("tag", std::string());
  packet.id = root.value("id", std::string());
  if (packet.tag == "record") {
    packet.type = PacketType::RecordAudio;
    packet.json = root;
    packet.audio_bytes = ParseJsonByteArray(root.value("bytes", nlohmann::json::array()));
    packet.raw_summary = BuildSummary(packet, payload.size());
    return packet;
  }

  packet.type = PacketType::Unknown;
  packet.json = root;
  packet.raw_summary = BuildSummary(packet, payload.size());
  return packet;
}

}  // namespace xiaoai_server::soundbox
