#pragma once

#include "audio_processing_module/modules.hpp"

#include <string>
#include <vector>

namespace audio_processing_module {

PipelineOptions ParseOptions(const std::vector<std::string>& args);

std::string Usage(const std::string& program_name);

int RunPipeline(const PipelineOptions& options);

}  // namespace audio_processing_module
