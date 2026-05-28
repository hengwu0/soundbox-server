#include "frontend/soundbox/event_dispatcher.hpp"

#include "common/log.hpp"

#include <utility>

namespace xiaoai_server::soundbox {

namespace {

// kLog 是事件层日志器。
const auto kLog = xiaoai_server::GetLogger("soundbox");

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
      kLog->info("小爱收到指令: {}", text);
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
