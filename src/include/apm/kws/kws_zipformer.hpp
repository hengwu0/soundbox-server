#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "apm/kws/kws_engine.hpp"

namespace xiaoai_server::wakeup {

// 基于 sherpa-onnx Zipformer2 的关键词检测实现。
class ZipformerKwsEngine final : public IKwsEngine {
 public:
  // 构造 Zipformer KWS 引擎。
  // 参数说明：
  // - cfg: 模型路径和关键词文件配置。
  // 返回值：
  // - 无；模型加载失败时抛出异常。
  explicit ZipformerKwsEngine(config::Wakeup cfg);

  // 析构 KWS 引擎。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  ~ZipformerKwsEngine() override;

  // 输入一段 PCM 数据并尝试检测关键词。
  // 参数说明：
  // - pcm: PCM 原始数据地址。
  // - size_bytes: PCM 数据字节数。
  // - sample_rate: PCM 采样率。
  // - channels: PCM 通道数。
  // - bits_per_sample: PCM 位宽。
  // 返回值：
  // - 命中时返回 KwsHit，否则返回 std::nullopt。
  std::optional<KwsHit> AcceptPcm16(const uint8_t* pcm, size_t size_bytes, int sample_rate,
                                    int channels, int bits_per_sample) override;

  // 重置当前流状态。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Reset() override;

 private:
  // Impl 隐藏 sherpa-onnx C++ API 具体类型，避免头文件暴露第三方细节。
  struct Impl;

  // cfg_ 保存模型路径、关键词文件和 KWS 解码参数。
  config::Wakeup cfg_;
  // impl_ 持有 sherpa-onnx recognizer/stream 等底层对象。
  std::unique_ptr<Impl> impl_;
};

}  // namespace xiaoai_server::wakeup