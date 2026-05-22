#pragma once

#include "apm/aec/webrtc_processor.hpp"
#include "config/config.hpp"
#include "llm/llm_client.hpp"

#include <functional>
#include <string>
#include <vector>

namespace audio_processing_module {

/// 流水线运行选项，集合了命令行参数和 YAML 配置的全部运行时设置。
struct PipelineOptions {
  std::string output_file;                    ///< 调试输出 WAV 文件路径。
  std::string socket_dir;                     ///< Unix Domain Socket 存放目录。
  WebRtcProcessorOptions processor;           ///< WebRTC AEC / NS / AGC 处理器选项。
  xiaoai_server::config::Config runtime;      ///< 业务层运行时配置。
};

/// 描述一个需要被监管运行的工作线程。
struct SupervisedWorker {
  std::string name;                 ///< Worker 名称，用于日志和错误提示。
  std::function<void()> run;        ///< 主循环函数。
  std::function<void()> stop;       ///< 停止通知函数（逆序调用）。
};

/// 解析命令行参数并加载 YAML 配置，返回最终流水线选项。
/// @param args 命令行参数列表（含 argv[0]）。
/// @return 合并后的 PipelineOptions。
PipelineOptions ParseOptions(const std::vector<std::string>& args);

/// 返回命令行使用说明字符串。
/// @param program_name 程序名。
/// @return 使用说明文本。
std::string Usage(const std::string& program_name);

/// 监管多条工作线程，任一异常或停止信号时通知全部退出。
/// @param workers     需要同时运行的工作线程列表。
/// @param should_stop 外部停止条件回调，返回 true 时请求全体退出。
void RunSupervisedWorkers(const std::vector<SupervisedWorker>& workers,
                           const std::function<bool()>& should_stop = {});

/// 根据配置启动整个音频处理流水线（日志 -> 验证 -> 创建模块 -> 监管运行）。
/// @param options 已解析的流水线配置。
/// @return 正常退出返回 0。
/// @throws std::runtime_error 关键配置缺失或资源不存在时抛出。
int RunPipeline(const PipelineOptions& options);

}  // namespace audio_processing_module