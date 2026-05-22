#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xiaoai_server::wakeup {

// 描述一次关键词命中的结果。
struct KwsHit {
  // keyword 是模型命中的唤醒词标签。
  std::string keyword;
};

// 关键词检测引擎接口，屏蔽具体模型实现差异。
class IKwsEngine {
 public:
  // 虚析构函数，保证通过接口指针释放派生类时行为正确。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  virtual ~IKwsEngine() = default;

  // 输入一段 PCM 数据并尝试检测关键词。
  // 参数说明：
  // - pcm: PCM 原始数据地址。
  // - size_bytes: PCM 原始数据字节数。
  // - sample_rate: PCM 采样率。
  // - channels: PCM 通道数。
  // - bits_per_sample: PCM 位宽。
  // 返回值：
  // - 命中时返回 KwsHit，否则返回 std::nullopt。
  virtual std::optional<KwsHit> AcceptPcm16(const uint8_t* pcm, size_t size_bytes, int sample_rate,
                                            int channels, int bits_per_sample) = 0;

  // 重置内部状态，清空历史流上下文。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  virtual void Reset() = 0;
};

}  // namespace xiaoai_server::wakeup