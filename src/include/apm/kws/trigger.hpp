#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xiaoai_server::wakeup {

// 负责把命中的关键词文本映射成真正的唤醒事件。
class Trigger {
 public:
  // 构造触发器对象。
  // 参数说明：
  // - keywords: 支持的关键词列表。
  // - on_wake: 命中后触发唤醒的回调。
  // 返回值：
  // - 无。
  Trigger(std::vector<std::string> keywords,
          std::function<bool(const std::string&)> on_wake);

  // 规范化关键词文本，便于做大小写与空白无关的匹配。
  // 参数说明：
  // - text: 原始关键词文本。
  // 返回值：
  // - 返回规范化后的关键词字符串。
  static std::string NormalizeKeyword(const std::string& text);

  // 用一段文本尝试触发唤醒。
  // 参数说明：
  // - text: 待匹配的识别结果文本。
  // 返回值：
  // - 成功触发唤醒返回 true，否则返回 false。
  bool FireFromText(const std::string& text);

 private:
  // 把原始关键词列表转换成规范化查找表。
  // 参数说明：
  // - keywords: 原始关键词列表。
  // 返回值：
  // - 返回规范化后的映射表。
  static std::unordered_map<std::string, std::string> NormalizeKeywords(
      const std::vector<std::string>& keywords);

  // allowed_ 以规范化关键词为 key，保留原始展示文本作为 value。
  std::unordered_map<std::string, std::string> allowed_;
  // on_wake_ 是命中关键词后回调 App/Gate 的触发函数。
  std::function<bool(const std::string&)> on_wake_;
};

}  // namespace xiaoai_server::wakeup