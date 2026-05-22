#include "apm/kws/trigger.hpp"

#include <algorithm>
#include <cctype>

namespace xiaoai_server::wakeup {

namespace {

// 把字符串统一转成小写，便于做大小写无关的匹配。
// 参数说明：
// - s: 待转换的字符串。
// 返回值：
// - 返回转换后的小写字符串。
std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

// 构造关键词触发器，并把关键词列表预处理成规范化查找表。
// 参数说明：
// - keywords: 原始关键词列表。
// - on_wake: 命中关键词后执行的唤醒回调。
// 返回值：
// - 无。
Trigger::Trigger(std::vector<std::string> keywords,
                 std::function<bool(const std::string&)> on_wake)
    : allowed_(NormalizeKeywords(keywords)), on_wake_(std::move(on_wake)) {}

// 规范化关键词文本，统一大小写并移除空白字符。
// 参数说明：
// - text: 原始关键词文本。
// 返回值：
// - 返回可用于匹配的规范化字符串。
std::string Trigger::NormalizeKeyword(const std::string& text) {
  // s 是小写化后的候选文本。
  std::string s = ToLower(text);
  // out 保存去掉空白后的规范化文本。
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// 尝试用一段识别文本触发唤醒。
// 参数说明：
// - text: KWS 或 ASR 产生的候选文本。
// 返回值：
// - 命中允许关键词且上层接受唤醒时返回 true。
bool Trigger::FireFromText(const std::string& text) {
  // norm 是待匹配文本的规范化结果。
  const auto norm = NormalizeKeyword(text);
  if (norm.empty()) {
    return false;
  }

  // matched 保存命中的原始关键词文本。
  std::string matched;
  for (const auto& it : allowed_) {
    // 子串匹配：规范化的识别文本中任意位置包含规范化关键词即命中
    if (norm.find(it.first) != std::string::npos) {
      matched = it.second;
      break;
    }
  }

  // 命中但无回调或未匹配到时返回 false
  if (!on_wake_ || matched.empty()) {
    return false;
  }
  return on_wake_(matched);
}

// 把原始关键词列表转换成规范化关键词映射。
// 参数说明：
// - keywords: 原始关键词列表。
// 返回值：
// - 返回规范化文本到原始文本的映射表。
std::unordered_map<std::string, std::string> Trigger::NormalizeKeywords(
    const std::vector<std::string>& keywords) {
  // out 以规范化关键词为 key，原始关键词为 value。
  std::unordered_map<std::string, std::string> out;
  for (const auto& keyword : keywords) {
    auto norm = NormalizeKeyword(keyword);
    if (norm.empty()) {
      continue;  ///< 跳过规范化后为空的关键词。
    }
    out[norm] = keyword;
  }
  return out;
}

}  // namespace xiaoai_server::wakeup