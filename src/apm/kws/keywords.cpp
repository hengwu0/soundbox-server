#include "apm/kws/keywords.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace xiaoai_server::wakeup {

namespace {

// 去掉字符串首尾空白，便于解析关键词文件。
// 参数说明：
// - s: 待裁剪的原始字符串。
// 返回值：
// - 返回裁剪后的字符串副本。
std::string TrimSpace(std::string s) {
  // not_space 用于定位首尾第一个非空白字符。
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

}  // namespace

// 从 sherpa-onnx keywords.txt 中读取唤醒词标签并去重。
// 参数说明：
// - keywords_file: 关键词文件路径。
// 返回值：
// - 返回合法关键词标签列表；文件打不开或格式非法时抛出异常。
std::vector<std::string> LoadWakeupKeywords(const std::string& keywords_file) {
  // in 负责按文本行读取 sherpa-onnx KWS keywords 文件。
  std::ifstream in(keywords_file);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open wakeup.keywords_file: " + keywords_file);
  }

  // keywords 保存解析出的所有关键词标签，后续会排序去重。
  std::vector<std::string> keywords;
  // line 保存当前读取的原始行文本。
  std::string line;
  // lineno 保存当前行号，用于错误提示。
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    line = TrimSpace(std::move(line));
    // 跳过空行和注释行（# 或 ; 开头）
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }

    // sherpa-onnx keywords.txt 每行通常是 token 序列加 @label，应用只需要 label。
    const auto at = line.rfind('@');
    if (at == std::string::npos) {
      throw std::runtime_error("invalid wakeup.keywords_file line " + std::to_string(lineno) +
                                ": missing '@' label");
    }
    auto keyword = TrimSpace(line.substr(at + 1));
    if (keyword.empty()) {
      throw std::runtime_error("invalid wakeup.keywords_file line " + std::to_string(lineno) +
                                ": empty keyword label");
    }
    keywords.push_back(std::move(keyword));
  }

  // 排序去重后交给 Trigger，避免同一个关键词标签重复注册。
  std::sort(keywords.begin(), keywords.end());
  keywords.erase(std::unique(keywords.begin(), keywords.end()), keywords.end());
  if (keywords.empty()) {
    throw std::runtime_error("wakeup.keywords_file has no valid keyword label: " +
                             keywords_file);
  }
  return keywords;
}

}  // namespace xiaoai_server::wakeup