#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "apm/kws/gate.hpp"
#include "apm/kws/kws_engine.hpp"
#include "apm/kws/trigger.hpp"

namespace xiaoai_server::wakeup {

// 本地唤醒监听器，把 PCM 数据送入 KWS 并在命中时尝试触发门控。
class LocalListener {
 public:
  // 描述本地监听器运行所需的依赖与音频参数。
  struct Config {
    // gate 指向 App 持有的门控对象，用于判断当前是否允许触发唤醒。
    Gate* gate{nullptr};
    // trigger 指向关键词触发器，用于把 KWS 文本转换成唤醒事件。
    Trigger* trigger{nullptr};
    // kws_engine 是具体的 KWS 推理实现。
    std::shared_ptr<IKwsEngine> kws_engine;
    // sample_rate 是传入 PCM 的采样率。
    int sample_rate{24000};
    // channels 是传入 PCM 的通道数；当前 KWS 输入已经是单声道。
    int channels{1};
    // bit_depth 是传入 PCM 的位深；当前只支持 16bit。
    int bit_depth{16};
    // min_trigger_interval_ms 限制连续唤醒的最短间隔，避免同一段音频重复触发。
    int min_trigger_interval_ms{1500};
  };

  // 构造本地监听器。
  // 参数说明：
  // - cfg: 监听器依赖与音频参数配置。
  // 返回值：
  // - 无。
  explicit LocalListener(Config cfg);

  // 接收一块 PCM 数据并尝试做关键词检测。
  // 参数说明：
  // - chunk: 待检测的 PCM 数据块。
  // 返回值：
  // - 无。
  void AcceptPcm(const std::vector<uint8_t>& chunk);

  // 关闭监听器并重置底层 KWS 引擎。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Close();

 private:
  // cfg_ 保存门控、触发器、KWS 引擎和音频格式参数。
  Config cfg_;
  // mu_ 串行化 KWS 引擎访问，避免多线程同时喂入 PCM。
  std::mutex mu_;
  // last_trigger_ 记录上一次成功触发时间，用于做最小触发间隔限制。
  std::chrono::steady_clock::time_point last_trigger_{};
  // closed_ 表示监听器是否已关闭，关闭后忽略新的音频输入。
  bool closed_{false};
};

}  // namespace xiaoai_server::wakeup