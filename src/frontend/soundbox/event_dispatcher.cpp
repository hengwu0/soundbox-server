#include "frontend/soundbox/event_dispatcher.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace xiaoai_server::soundbox {

namespace {

// kLog 是事件层日志器。
const auto kLog = xiaoai_server::GetLogger("soundbox");

// 返回 UTF-8 当前位置编码的字节长度，非法字节按单字节处理。
size_t Utf8CharLength(unsigned char lead) {
  if ((lead & 0x80) == 0) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return 2;
  }
  if ((lead & 0xF0) == 0xE0) {
    return 3;
  }
  if ((lead & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

// 判断一个 UTF-8 字符是否应在 native KWS 文本匹配前被去除。
bool IsIgnoredNativeKwsSymbol(const std::string& ch) {
  if (ch.size() == 1) {
    const unsigned char value = static_cast<unsigned char>(ch[0]);
    return std::isspace(value) || std::ispunct(value);
  }

  static const std::vector<std::string> kIgnoredSymbols = {
      "，", "。", "！", "？", "；", "：", "、", "“", "”", "‘", "’",
      "（", "）", "【", "】", "《", "》", "〈", "〉", "「", "」",
      "『", "』", "〔", "〕", "—", "…", "·", "～", "￥", "＂",
      "＇", "＃", "％", "＆", "＊", "＋", "－", "／", "＝", "＠",
      "＼", "＾", "＿", "｀", "｜", "｛", "｝", "［", "］", "＜",
      "＞", "＂", "＇", "　"};
  return std::find(kIgnoredSymbols.begin(), kIgnoredSymbols.end(), ch) !=
         kIgnoredSymbols.end();
}

// 归一化 SoundBox 原生识别文本：去除空白、标点和常见符号。
std::string NormalizeNativeKwsText(const std::string& text) {
  std::string normalized;
  for (size_t pos = 0; pos < text.size();) {
    const size_t char_len = std::min(Utf8CharLength(static_cast<unsigned char>(text[pos])),
                                     text.size() - pos);
    const std::string ch = text.substr(pos, char_len);
    if (!IsIgnoredNativeKwsSymbol(ch)) {
      normalized += ch;
    }
    pos += char_len;
  }
  return normalized;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 读取小爱 RecognizeResult 中第一条识别文本。
std::string FirstResultText(const nlohmann::json& payload) {
  const auto results = payload.find("results");
  if (results == payload.end() || !results->is_array() || results->empty() ||
      !(*results)[0].is_object()) {
    return {};
  }
  return (*results)[0].value("text", std::string());
}

}  // namespace

// 构造 EventDispatcher。
//
// 参数说明：
// - 无。
// 返回值：
// - 无。
EventDispatcher::EventDispatcher()
    : unknown_logger_(kLog, std::chrono::milliseconds(3000), true) {}

// 设置 soundbox 原生 KWS 唤醒事件回调。
void EventDispatcher::set_soundbox_native_kws_callback(
    OnSoundboxNativeKwsCallback callback) {
  on_soundbox_native_kws_ = std::move(callback);
}

// 设置 SoundBox 原生文本 KWS 触发词列表。
void EventDispatcher::set_native_kws_triggers(std::vector<std::string> triggers) {
  native_kws_triggers_.clear();
  native_kws_triggers_.reserve(triggers.size());
  for (auto& trigger : triggers) {
    const std::string normalized = NormalizeNativeKwsText(trigger);
    if (normalized.empty()) {
      continue;
    }
    native_kws_triggers_.push_back(NativeKwsTrigger{std::move(trigger), normalized});
  }
}

// 设置 SoundBox 原生文本 KWS 触发回调。
void EventDispatcher::set_soundbox_native_text_kws_callback(
    OnSoundboxNativeTextKwsCallback callback) {
  on_soundbox_native_text_kws_ = std::move(callback);
}

// 判断识别文本是否以后缀形式命中任一配置触发词。
std::optional<std::string> EventDispatcher::MatchNativeTextKws(
    const std::string& text) const {
  const std::string normalized = NormalizeNativeKwsText(text);
  if (normalized.empty()) {
    return std::nullopt;
  }
  for (const auto& trigger : native_kws_triggers_) {
    if (EndsWith(normalized, trigger.normalized)) {
      return trigger.raw;
    }
  }
  return std::nullopt;
}

// 处理事件包。
//
// 参数说明：
// - packet: Event 包。
// 返回值：
// - 无。
void EventDispatcher::Handle(const Packet& packet) {
  const auto event_type = packet.event.empty() ? packet.json.value("event", std::string())
                                               : packet.event;
  if (event_type == "playing") {
    const auto data = packet.json.find("data");
    if (data != packet.json.end()) {
      kLog->debug("soundbox playing event: {}", data->dump());
    } else {
      kLog->debug("soundbox playing event");
    }
    return;
  }

  if (event_type != "instruction") {
    unknown_logger_.Log(spdlog::level::debug, "unknown-event-" + event_type,
                        "soundbox event ignored event=" + event_type);
    return;
  }

  const auto data = packet.json.find("data");
  if (data == packet.json.end()) {
    return;
  }
  if (data->is_string()) {
    kLog->debug("soundbox instruction marker: {}", data->get<std::string>());
    return;
  }
  if (!data->is_object()) {
    return;
  }

  // NewLine 是小爱原生 instruction.log 的一行 JSON 字符串，必须二次 parse。
  const auto newline = data->value("NewLine", std::string());
  if (newline.empty()) {
    return;
  }

  const auto line = nlohmann::json::parse(newline, nullptr, false);
  if (line.is_discarded() || !line.is_object()) {
    unknown_logger_.Log(spdlog::level::debug, "instruction-parse-failed",
                        "soundbox instruction NewLine parse failed");
    return;
  }

  const auto header = line.value("header", nlohmann::json::object());
  const auto payload = line.value("payload", nlohmann::json::object());
  const auto ns = header.value("namespace", std::string());
  const auto name = header.value("name", std::string());

  if (ns == "SpeechRecognizer" && name == "RecognizeResult") {
    const auto text = FirstResultText(payload);
    const bool is_final = payload.value("is_final", false);
    const bool is_vad_begin = payload.value("is_vad_begin", false);
    if (text.empty() && !is_vad_begin) {
      kLog->info("小爱唤醒事件");
      if (on_soundbox_native_kws_) {
        on_soundbox_native_kws_();
      }
    } else if (is_final && !text.empty()) {
      const auto matched_trigger = MatchNativeTextKws(text);
      if (matched_trigger) {
        kLog->info("小爱原生指令触发 soundbox-server KWS: text={} trigger={}",
                   text, *matched_trigger);
        if (on_soundbox_native_text_kws_) {
          on_soundbox_native_text_kws_(text, *matched_trigger);
        }
      } else {
        kLog->info("小爱收到指令: {}", text);
      }
    } else {
      kLog->debug("小爱过程识别: final={} vad_begin={} text='{}'", is_final, is_vad_begin,
                  text);
    }
    return;
  }

  if (ns == "SpeechSynthesizer" && (name == "Speak" || name == "SpeakStream")) {
    const auto text = payload.value("text", std::string());
    if (!text.empty()) {
      kLog->info("小爱响应: {}", text);
    } else {
      kLog->debug("小爱响应标记: {}", name);
    }
    return;
  }

  unknown_logger_.Log(spdlog::level::debug, "instruction-" + ns + "-" + name,
                      "soundbox instruction ignored ns=" + ns + " name=" + name);
}

}  // namespace xiaoai_server::soundbox
