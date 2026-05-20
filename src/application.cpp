#include "audio_processing_module/application.hpp"

#include "audio_processing_module/modules.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace audio_processing_module {
namespace {

std::string RequireValue(const std::vector<std::string>& args, size_t& index) {
  if (index + 1 >= args.size()) {
    throw std::runtime_error("missing value for option: " + args[index]);
  }
  ++index;
  return args[index];
}

std::string DefaultSocketDir() {
  return "/tmp/audio_processing_module_" + std::to_string(static_cast<long>(::getpid()));
}

void RethrowFirstError(const std::vector<std::exception_ptr>& errors) {
  for (const auto& error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }
}

}  // namespace

PipelineOptions ParseOptions(const std::vector<std::string>& args) {
  PipelineOptions options;
  options.output_file = "output/aec_processed.wav";
  options.socket_dir = DefaultSocketDir();

  for (size_t index = 1; index < args.size(); ++index) {
    const std::string& arg = args[index];
    if (arg == "--input" || arg == "-i") {
      options.input_file = RequireValue(args, index);
    } else if (arg == "--output" || arg == "-o") {
      options.output_file = RequireValue(args, index);
    } else if (arg == "--socket-dir") {
      options.socket_dir = RequireValue(args, index);
    } else if (arg == "--help" || arg == "-h") {
      throw std::runtime_error(Usage(args.empty() ? "AudioProcessingModule" : args[0]));
    } else {
      throw std::runtime_error("unknown option: " + arg);
    }
  }

  if (options.input_file.empty()) {
    throw std::runtime_error("required option is missing: --input <2ch-s16le-pcm-file>");
  }

  return options;
}

std::string Usage(const std::string& program_name) {
  std::ostringstream output;
  output << "Usage: " << program_name
         << " --input <2ch_16bit_16k.s16> [--output <processed.wav>]"
         << " [--socket-dir <dir>]\n";
  return output.str();
}

int RunPipeline(const PipelineOptions& options) {
  std::filesystem::create_directories(options.socket_dir);
  const std::string capture_socket =
      (std::filesystem::path(options.socket_dir) / "capture.sock").string();
  const std::string processed_socket =
      (std::filesystem::path(options.socket_dir) / "processed.sock").string();

  ModuleAFileSource module_a(options.input_file, capture_socket);
  ModuleBWebRtcProcessor module_b(capture_socket, processed_socket);
  ModuleCWavSink module_c(processed_socket, options.output_file);

  std::vector<std::exception_ptr> errors(3);
  std::mutex error_mutex;

  auto run_thread = [&](size_t index, auto&& task) {
    try {
      task();
    } catch (...) {
      std::lock_guard<std::mutex> lock(error_mutex);
      errors[index] = std::current_exception();
    }
  };

  std::thread thread_a([&] { run_thread(0, [&] { module_a.Run(); }); });
  std::thread thread_b([&] { run_thread(1, [&] { module_b.Run(); }); });
  std::thread thread_c([&] { run_thread(2, [&] { module_c.Run(); }); });

  thread_a.join();
  thread_b.join();
  thread_c.join();

  RethrowFirstError(errors);
  std::cout << "[main] pipeline completed successfully\n";
  return 0;
}

}  // namespace audio_processing_module
