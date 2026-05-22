#pragma once

#include <string>
#include <vector>

namespace xiaoai_server::wakeup {

// 从关键词文件中提取所有合法的唤醒词标签。
// 参数说明：
// - keywords_file: 关键词文件路径。
// 返回值：
// - 返回去重后的关键词标签列表；文件异常时抛出异常。
std::vector<std::string> LoadWakeupKeywords(const std::string& keywords_file);

}  // namespace xiaoai_server::wakeup