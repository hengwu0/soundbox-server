#pragma once

#include "apm/aec/webrtc_processor.hpp"
#include "config/config.hpp"

#include <functional>
#include <string>
#include <vector>

namespace audio_processing_module {

struct PipelineOptions {
  std::string input_file;
  std::string output_file;
  std::string socket_dir;
  WebRtcProcessorOptions processor;
  xiaoai_server::config::Config runtime;
};

struct SupervisedWorker {
  std::string name;
  std::function<void()> run;
  std::function<void()> stop;
};

PipelineOptions ParseOptions(const std::vector<std::string>& args);

std::string Usage(const std::string& program_name);

void RunSupervisedWorkers(const std::vector<SupervisedWorker>& workers,
                          const std::function<bool()>& should_stop = {});

int RunPipeline(const PipelineOptions& options);

}  // namespace audio_processing_module
