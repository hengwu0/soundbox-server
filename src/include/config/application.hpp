#pragma once

#include "apm/aec/webrtc_processor.hpp"
#include "config/config.hpp"

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

PipelineOptions ParseOptions(const std::vector<std::string>& args);

std::string Usage(const std::string& program_name);

int RunPipeline(const PipelineOptions& options);

}  // namespace audio_processing_module
