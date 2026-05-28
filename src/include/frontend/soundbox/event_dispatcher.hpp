#pragma once

#include "common/rate_limited_logger.hpp"
#include "frontend/soundbox/packet.hpp"

#include <functional>

namespace xiaoai_server::soundbox {

// EventDispatcher 统一处理 soundbox 事件包。
//
// 当前事件只做日志观察，不直接驱动核心状态机：
// - event=playing: 记录播放状态。
// - event=instruction: data.NewLine 里还有一层 JSON 字符串，需要二次解析 header/payload。
// - SpeechRecognizer/RecognizeResult: 记录唤醒、过程识别、最终识别。
// - SpeechSynthesizer/Speak/SpeakStream: 记录小爱响应文本。
// - 其它 namespace/name: debug 聚合，避免高频刷屏。
class EventDispatcher {
 public:
  // soundbox 原生 KWS 唤醒事件回调。
  using OnSoundboxNativeKwsCallback = std::function<void()>;

  // 构造事件分发器。
  EventDispatcher();

  // 设置 soundbox 原生 KWS 唤醒事件回调。
  void set_soundbox_native_kws_callback(OnSoundboxNativeKwsCallback callback);

  // 处理一条事件 Packet。
  //
  // 参数说明：
  // - packet: PacketParser 解析出的 Event 包。
  // 返回值：
  // - 无。
  void Handle(const Packet& packet);

 private:
  // unknown_logger_ 聚合非关键 instruction 或未知事件日志。
  xiaoai_server::RateLimitedLogger unknown_logger_;
  // on_soundbox_native_kws_ 在识别到 soundbox 设备自身 KWS 事件时通知上层。
  OnSoundboxNativeKwsCallback on_soundbox_native_kws_;
};

}  // namespace xiaoai_server::soundbox
