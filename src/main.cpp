#include "audio_processing_module/application.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  try {
    const audio_processing_module::PipelineOptions options =
        audio_processing_module::ParseOptions(args);
    return audio_processing_module::RunPipeline(options);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (argc > 0) {
      std::cerr << audio_processing_module::Usage(argv[0]);
    }
    return 1;
  }
}
