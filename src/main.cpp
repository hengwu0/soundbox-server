#include "config/application.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

/// soundbox-server 程序入口。
/// 解析命令行参数与 YAML 配置，启动音频处理流水线。
/// @param argc 命令行参数个数。
/// @param argv 命令行参数字符串数组。
/// @return 正常退出返回 0，异常退出返回 1。
int main(int argc, char** argv) {
  // 将原始 argv 收集到 std::vector<std::string> 中方便传递
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  try {
    // 解析命令行参数并加载 YAML 配置
    const audio_processing_module::PipelineOptions options =
        audio_processing_module::ParseOptions(args);
    // 启动整个音频处理流水线
    return audio_processing_module::RunPipeline(options);
  } catch (const std::exception& error) {
    const std::string message = error.what();
    std::cerr << message << '\n';
    // "--help" / "-h" 触发的异常以 "Usage:" 开头，此时正常退出
    if (message.rfind("Usage:", 0) == 0) {
      return 0;
    }
    // 异常退出时打印使用说明
    if (argc > 0) {
      std::cerr << audio_processing_module::Usage(argv[0]);
    }
    return 1;
  }
}