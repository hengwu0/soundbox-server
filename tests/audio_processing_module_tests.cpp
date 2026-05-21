#include "apm/aec/aec_stream_processor.hpp"
#include "apm/aec/webrtc_processor.hpp"
#include "apm/kws/kws_socket_server.hpp"
#include "common/audio_frame.hpp"
#include "common/log.hpp"
#include "common/unix_socket.hpp"
#include "common/wav_writer.hpp"
#include "config/application.hpp"
#include "frontend/soundbox/audio_pipe.hpp"
#include "frontend/soundbox/audio_router.hpp"
#include "frontend/soundbox/client.hpp"
#include "frontend/soundbox/mode_controller.hpp"
#include "frontend/soundbox/packet.hpp"
#include "frontend/soundbox_frontend.hpp"
#include "llm/file_recorder.hpp"
#include "aec/file_audio_stream_frontend.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <openssl/evp.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>

using audio_processing_module::AudioFormat;
using audio_processing_module::AutomaticGainState;
using audio_processing_module::ComputeRms;
using audio_processing_module::ParseOptions;
using audio_processing_module::PreAecAutoGainConfig;
using audio_processing_module::ProcessPreAecAutoGain;
using audio_processing_module::SplitInterleavedStereoS16;
using audio_processing_module::StereoSplit;
using audio_processing_module::WavWriter;
using audio_processing_module::WebRtcProcessorOptions;
using audio_processing_module::apm::aec::AecStreamProcessor;
using audio_processing_module::apm::kws::KwsSocketServer;
using audio_processing_module::llm::FileRecorder;
using audio_processing_module::tests::aec::FileAudioStreamFrontend;
using xiaoai_server::soundbox::AudioMode;
using xiaoai_server::soundbox::AudioPipe;
using xiaoai_server::soundbox::AudioRouter;
using xiaoai_server::soundbox::BuildPlayPcmPacket;
using xiaoai_server::soundbox::ModeController;
using soundbox_server::frontend::Frontend;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T>
T ReadLittleEndian(const std::vector<uint8_t>& bytes, size_t offset) {
  T value = 0;
  for (size_t index = 0; index < sizeof(T); ++index) {
    value |= static_cast<T>(bytes.at(offset + index)) << (8 * index);
  }
  return value;
}

class ScopedCurrentPath {
 public:
  explicit ScopedCurrentPath(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~ScopedCurrentPath() {
    std::filesystem::current_path(previous_);
  }

  ScopedCurrentPath(const ScopedCurrentPath&) = delete;
  ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

 private:
  std::filesystem::path previous_;
};

std::filesystem::path MakeTempDirectory(const std::string& suffix) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("audio_processing_module_test_" + suffix + "_" +
       std::to_string(static_cast<long>(::getpid())));
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

void WriteConfigFile(const std::filesystem::path& path, const std::string& body) {
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  std::ofstream output(path);
  Require(output.good(), "failed to open config file for writing");
  output << body;
  Require(output.good(), "failed to write config file");
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool WaitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  Require(input.good(), "failed to open text file: " + path.string());
  std::string body((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  while (!body.empty() &&
         (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
    body.pop_back();
  }
  return body;
}

std::string ComputeMd5Hex(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "failed to open file for md5: " + path.string());

  EVP_MD_CTX* context = EVP_MD_CTX_new();
  Require(context != nullptr, "failed to allocate md5 context");
  Require(EVP_DigestInit_ex(context, EVP_md5(), nullptr) == 1,
          "failed to initialize md5");

  std::array<char, 8192> buffer{};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytes_read = input.gcount();
    if (bytes_read > 0) {
      Require(EVP_DigestUpdate(context, buffer.data(),
                               static_cast<size_t>(bytes_read)) == 1,
              "failed to update md5");
    }
  }
  Require(input.eof(), "failed while reading file for md5");

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  Require(EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1,
          "failed to finalize md5");
  EVP_MD_CTX_free(context);

  std::ostringstream output;
  for (unsigned int index = 0; index < digest_size; ++index) {
    output << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(digest[index]);
  }
  return output.str();
}

std::string ReadLineFromSocket(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    const ssize_t result = ::read(fd, &ch, 1);
    Require(result >= 0, "failed to read socket line");
    if (result == 0 || ch == '\n') {
      return line;
    }
    line.push_back(ch);
  }
}

std::string ReadLineFromSocketWithTimeout(int fd,
                                          std::chrono::milliseconds timeout) {
  std::string line;
  char ch = '\0';
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error("timed out reading socket line");
    }

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    const int ready = ::poll(&descriptor, 1, remaining_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("poll failed while reading socket line");
    }
    if (ready == 0) {
      continue;
    }

    const ssize_t result = ::read(fd, &ch, 1);
    Require(result >= 0, "failed to read socket line");
    if (result == 0 || ch == '\n') {
      return line;
    }
    line.push_back(ch);
  }
}

std::vector<uint8_t> BuildStereoFrame(int16_t mic_sample,
                                      int16_t reference_sample) {
  std::vector<int16_t> samples;
  samples.reserve(audio_processing_module::kInputSamplesPerFrame);
  for (size_t index = 0; index < audio_processing_module::kSamplesPerChannelPerFrame;
       ++index) {
    samples.push_back(mic_sample);
    samples.push_back(reference_sample);
  }
  return audio_processing_module::EncodeS16LittleEndian(samples);
}

std::vector<uint8_t> BuildRecordPacket(const std::string& id,
                                       const std::vector<uint8_t>& pcm) {
  nlohmann::json packet = {
      {"id", id},
      {"tag", "record"},
      {"bytes", pcm},
  };
  const std::string dumped = packet.dump();
  return std::vector<uint8_t>(dumped.begin(), dumped.end());
}

std::string ResponseMessageForCommand(const std::string& command) {
  if (command == "llm_start") {
    return "llm_start_ok";
  }
  if (command == "llm_stop") {
    return "llm_stop_ok";
  }
  return command + "_ok";
}

size_t ReadAtLeastWithTimeout(int fd,
                              size_t target_bytes,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<uint8_t, 1024> buffer{};
  size_t total = 0;
  while (total < target_bytes) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error("timed out reading fake local socket");
    }
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, remaining_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("poll failed while reading fake local socket");
    }
    if (ready == 0) {
      continue;
    }
    const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("read failed while reading fake local socket");
    }
    if (bytes_read == 0) {
      throw std::runtime_error("fake local socket closed before expected audio");
    }
    total += static_cast<size_t>(bytes_read);
  }
  return total;
}

class MockSoundboxWebSocketServer {
 public:
  MockSoundboxWebSocketServer()
      : port_(ix::getFreePort()), server_(port_, "127.0.0.1") {
    server_.disablePerMessageDeflate();
    server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*connection_state*/,
               ix::WebSocket& web_socket,
               const ix::WebSocketMessagePtr& message) {
          HandleMessage(web_socket, message);
        });

    const auto result = server_.listen();
    Require(result.first, "failed to listen mock soundbox websocket: " + result.second);
    server_.start();
  }

  ~MockSoundboxWebSocketServer() { server_.stop(); }

  std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/";
  }

  bool SawCommand(const std::string& command) const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::find(commands_.begin(), commands_.end(), command) != commands_.end();
  }

  bool SawPlayPacket() const {
    std::lock_guard<std::mutex> lock(mu_);
    return saw_play_packet_;
  }

  bool SawRecordHeaderToken() const {
    std::lock_guard<std::mutex> lock(mu_);
    return saw_authorization_header_;
  }

  void SendRecord(const std::string& id, const std::vector<uint8_t>& pcm) {
    const std::vector<uint8_t> packet = BuildRecordPacket(id, pcm);
    const std::string body(packet.begin(), packet.end());
    const auto clients = server_.getClients();
    Require(!clients.empty(), "mock soundbox websocket has no connected clients");
    for (const auto& client : clients) {
      client->send(body, false);
    }
  }

 private:
  void HandleMessage(ix::WebSocket& web_socket,
                     const ix::WebSocketMessagePtr& message) {
    if (message->type == ix::WebSocketMessageType::Open) {
      const auto header = message->openInfo.headers.find("Authorization");
      std::lock_guard<std::mutex> lock(mu_);
      saw_authorization_header_ =
          header != message->openInfo.headers.end() &&
          header->second.find("Bearer ") == 0;
      return;
    }

    if (message->type != ix::WebSocketMessageType::Message) {
      return;
    }

    const auto json =
        nlohmann::json::parse(message->str.begin(), message->str.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return;
    }

    if (message->binary && json.value("tag", std::string()) == "play") {
      std::lock_guard<std::mutex> lock(mu_);
      saw_play_packet_ = !json.value("bytes", std::vector<uint8_t>()).empty();
      return;
    }

    const auto request_it = json.find("Request");
    if (request_it == json.end() || !request_it->is_object()) {
      return;
    }

    const std::string id = request_it->value("id", std::string());
    const std::string command = request_it->value("command", std::string());
    {
      std::lock_guard<std::mutex> lock(mu_);
      commands_.push_back(command);
    }

    nlohmann::json response = {
        {"Response",
         {
             {"id", id},
             {"command", command},
             {"code", 0},
             {"msg", ResponseMessageForCommand(command)},
         }},
    };
    web_socket.sendText(response.dump());
  }

  int port_;
  ix::WebSocketServer server_;
  mutable std::mutex mu_;
  std::vector<std::string> commands_;
  bool saw_play_packet_{false};
  bool saw_authorization_header_{false};
};

class FakeKwsEngine final : public xiaoai_server::wakeup::IKwsEngine {
 public:
  std::optional<xiaoai_server::wakeup::KwsHit> AcceptPcm16(
      const uint8_t* /*pcm*/,
      size_t size_bytes,
      int sample_rate,
      int channels,
      int bits_per_sample) override {
    Require(sample_rate == 16000, "KWS server should pass 16k sample rate");
    Require(channels == 1, "KWS server should pass mono channel count");
    Require(bits_per_sample == 16, "KWS server should pass 16-bit format");
    bytes_seen_ += size_bytes;
    if (bytes_seen_ >= 320) {
      xiaoai_server::wakeup::KwsHit hit;
      hit.keyword = "xiaoai";
      return hit;
    }
    return std::nullopt;
  }

  void Reset() override { bytes_seen_ = 0; }

 private:
  size_t bytes_seen_{0};
};

void TestStereoFrameSplitKeepsMicAndReferenceOrder() {
  // 验证 2ch/16bit 交错 PCM 会被稳定拆成 mic/ref 两路单声道。
  const std::vector<int16_t> interleaved = {
      10, 100,
      20, 200,
      30, 300,
      40, 400,
  };

  const StereoSplit split = SplitInterleavedStereoS16(interleaved);

  Require(split.mic == std::vector<int16_t>({10, 20, 30, 40}),
          "mic channel was not split from channel 0");
  Require(split.reference == std::vector<int16_t>({100, 200, 300, 400}),
          "reference channel was not split from channel 1");
}

void TestNewPipelineModulesExposeExpectedSocketRoles() {
  // 验证新结构的三段职责：frontend 连接 AEC；AEC 监听 frontend 并连接 LLM；LLM 监听 AEC。
  const std::string frontend_to_aec = "/tmp/frontend_to_aec.sock";
  const std::string aec_to_llm = "/tmp/aec_to_llm.sock";
  const std::string output = "/tmp/audio_processing_module_roles.wav";

  FileAudioStreamFrontend frontend("tests/fixtures/aec_2ch_16k.s16", frontend_to_aec);
  AecStreamProcessor aec(frontend_to_aec, aec_to_llm, WebRtcProcessorOptions{});
  FileRecorder recorder(aec_to_llm, output);

  Require(frontend.aec_socket_path() == frontend_to_aec,
          "frontend should connect to the AEC listen socket");
  Require(aec.frontend_listen_socket_path() == frontend_to_aec,
          "AEC should own the frontend-facing listen socket");
  Require(aec.llm_socket_path() == aec_to_llm,
          "AEC should connect to the LLM listen socket");
  Require(recorder.listen_socket_path() == aec_to_llm,
          "LLM recorder should own the AEC-facing listen socket");
}

void TestAecFilePipelineMatchesExpectedWavMd5() {
  // 验证文件型 frontend 只作为 AEC 测试工具使用，并以输出 WAV 的 MD5 作为回归准则。
  const std::filesystem::path temp_root = MakeTempDirectory("aec_md5");
  const std::string frontend_to_aec = (temp_root / "frontend_aec.sock").string();
  const std::string aec_to_llm = (temp_root / "aec_llm.sock").string();
  const std::filesystem::path output_wav = temp_root / "aec_processed.wav";

  FileRecorder recorder(aec_to_llm, output_wav.string());
  AecStreamProcessor aec(frontend_to_aec, aec_to_llm, WebRtcProcessorOptions{});
  FileAudioStreamFrontend frontend("tests/fixtures/aec_2ch_16k.s16",
                                   frontend_to_aec,
                                   false);

  std::vector<std::exception_ptr> errors(3);
  std::thread llm_thread([&] {
    try {
      recorder.RunOneClient();
    } catch (...) {
      errors[0] = std::current_exception();
    }
  });
  std::thread aec_thread([&] {
    try {
      aec.RunOneClient();
    } catch (...) {
      errors[1] = std::current_exception();
    }
  });
  std::thread frontend_thread([&] {
    try {
      frontend.Run();
    } catch (...) {
      errors[2] = std::current_exception();
    }
  });

  frontend_thread.join();
  aec_thread.join();
  llm_thread.join();
  for (const auto& error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }

  const std::string actual_md5 = ComputeMd5Hex(output_wav);
  const std::string expected_md5 =
      ReadTextFile("tests/fixtures/expected_aec_processed.md5");
  Require(actual_md5 == expected_md5,
          "AEC processed wav md5 mismatch: expected " + expected_md5 +
              " got " + actual_md5);
}

void TestAecStreamProcessorReacceptsFrontendClient() {
  // 验证 AEC listen socket 是单客户端循环：当前 frontend 断开后能重新 accept 下一次连接。
  const std::filesystem::path temp_root = MakeTempDirectory("aec_reaccept");
  const std::string frontend_to_aec = (temp_root / "frontend_aec.sock").string();
  const std::string aec_to_llm = (temp_root / "aec_llm.sock").string();

  auto llm_server = audio_processing_module::CreateUnixServerSocket(aec_to_llm, 1);
  AecStreamProcessor aec(frontend_to_aec, aec_to_llm, WebRtcProcessorOptions{});

  std::exception_ptr thread_error;
  std::thread aec_thread([&] {
    try {
      aec.Run();
    } catch (...) {
      thread_error = std::current_exception();
    }
  });

  auto llm_client = audio_processing_module::AcceptUnixClientWithTimeout(
      llm_server.get(), std::chrono::seconds(10));
  const std::vector<uint8_t> stereo_frame(audio_processing_module::kInputBytesPerFrame,
                                          0);

  {
    auto frontend_client = audio_processing_module::ConnectUnixSocketWithRetry(
        frontend_to_aec, std::chrono::seconds(10), std::chrono::milliseconds(20));
    audio_processing_module::WriteAll(frontend_client.get(), stereo_frame.data(),
                                      stereo_frame.size());
  }
  Require(ReadAtLeastWithTimeout(llm_client.get(),
                                 audio_processing_module::kOutputBytesPerFrame,
                                 std::chrono::seconds(10)) >=
              audio_processing_module::kOutputBytesPerFrame,
          "AEC should write processed audio for the first frontend client");

  {
    auto frontend_client = audio_processing_module::ConnectUnixSocketWithRetry(
        frontend_to_aec, std::chrono::seconds(10), std::chrono::milliseconds(20));
    audio_processing_module::WriteAll(frontend_client.get(), stereo_frame.data(),
                                      stereo_frame.size());
  }
  Require(ReadAtLeastWithTimeout(llm_client.get(),
                                 audio_processing_module::kOutputBytesPerFrame,
                                 std::chrono::seconds(10)) >=
              audio_processing_module::kOutputBytesPerFrame,
          "AEC should reaccept and process audio for the second frontend client");

  aec.Stop();
  aec_thread.join();
  if (thread_error) {
    std::rethrow_exception(thread_error);
  }
}

void TestAecStreamProcessorSendsVadSessionEndBeforeFrontendEof() {
  // 验证 AEC 根据近端语音后静音主动发 session_end，而不是等 frontend EOF 后才结束会话。
  const std::filesystem::path temp_root = MakeTempDirectory("aec_vad_session_end");
  const std::string frontend_to_aec = (temp_root / "frontend_aec.sock").string();
  const std::string aec_to_llm = (temp_root / "aec_llm.sock").string();

  auto llm_server = audio_processing_module::CreateUnixServerSocket(aec_to_llm, 1);
  AecStreamProcessor aec(frontend_to_aec, aec_to_llm, WebRtcProcessorOptions{});

  std::exception_ptr thread_error;
  std::thread aec_thread([&] {
    try {
      aec.Run();
    } catch (...) {
      thread_error = std::current_exception();
    }
  });

  auto llm_client = audio_processing_module::AcceptUnixClientWithTimeout(
      llm_server.get(), std::chrono::seconds(10));
  auto frontend_client = audio_processing_module::ConnectUnixSocketWithRetry(
      frontend_to_aec, std::chrono::seconds(10), std::chrono::milliseconds(20));

  const std::vector<uint8_t> speech_frame = BuildStereoFrame(2500, 0);
  const std::vector<uint8_t> silence_frame = BuildStereoFrame(0, 0);
  for (int frame = 0; frame < 20; ++frame) {
    audio_processing_module::WriteAll(frontend_client.get(), speech_frame.data(),
                                      speech_frame.size());
  }
  for (int frame = 0; frame < 70; ++frame) {
    audio_processing_module::WriteAll(frontend_client.get(), silence_frame.data(),
                                      silence_frame.size());
  }

  std::string line;
  try {
    line = ReadLineFromSocketWithTimeout(frontend_client.get(), std::chrono::seconds(5));
  } catch (...) {
    frontend_client.reset();
    aec.Stop();
    if (aec_thread.joinable()) {
      aec_thread.join();
    }
    throw;
  }
  frontend_client.reset();
  aec.Stop();
  aec_thread.join();
  if (thread_error) {
    std::rethrow_exception(thread_error);
  }

  const auto message = nlohmann::json::parse(line);
  Require(message.value("type", std::string()) == "session_end",
          "AEC should send session_end before frontend EOF");
  Require(message.value("reason", std::string()) == "vad_end",
          "AEC session_end reason should be vad_end");
  Require(line.find("input_eof") == std::string::npos,
          "AEC session_end should not use input_eof for VAD end");
}

void TestFileRecorderReacceptsAecClient() {
  // 验证 FileRecorder listen socket 是单客户端循环：当前 AEC 断开后能继续接收下一次连接。
  const std::filesystem::path temp_root = MakeTempDirectory("llm_reaccept");
  const std::string aec_to_llm = (temp_root / "aec_llm.sock").string();
  const std::filesystem::path output_wav = temp_root / "processed.wav";

  FileRecorder recorder(aec_to_llm, output_wav.string());

  std::exception_ptr thread_error;
  std::thread recorder_thread([&] {
    try {
      recorder.Run();
    } catch (...) {
      thread_error = std::current_exception();
    }
  });

  const std::vector<uint8_t> mono_frame(audio_processing_module::kOutputBytesPerFrame,
                                        0);
  {
    auto aec_client = audio_processing_module::ConnectUnixSocketWithRetry(
        aec_to_llm, std::chrono::seconds(10), std::chrono::milliseconds(20));
    audio_processing_module::WriteAll(aec_client.get(), mono_frame.data(),
                                      mono_frame.size());
  }
  {
    auto aec_client = audio_processing_module::ConnectUnixSocketWithRetry(
        aec_to_llm, std::chrono::seconds(10), std::chrono::milliseconds(20));
    audio_processing_module::WriteAll(aec_client.get(), mono_frame.data(),
                                      mono_frame.size());
  }

  Require(WaitUntil([&] { return recorder.frames_written() >= 2; },
                    std::chrono::seconds(10)),
          "FileRecorder should process the second AEC client before stopping");
  recorder.Stop();
  recorder_thread.join();
  if (thread_error) {
    std::rethrow_exception(thread_error);
  }

  Require(std::filesystem::file_size(output_wav) ==
              44 + (2 * audio_processing_module::kOutputBytesPerFrame),
          "FileRecorder should write both accepted AEC client streams into the WAV");
}

void TestKwsSocketServerSendsSessionStartJsonLine() {
  // 验证 KWS socket 接收单声道 PCM 后，会把 fake engine 的命中转成 session_start JSON line。
  const std::filesystem::path temp_root = MakeTempDirectory("kws_socket");
  const std::string socket_path = (temp_root / "frontend_kws.sock").string();
  auto engine = std::make_shared<FakeKwsEngine>();

  KwsSocketServer::Options options;
  options.listen_socket_path = socket_path;
  KwsSocketServer server(options, engine);
  std::exception_ptr thread_error;
  std::thread server_thread([&] {
    try {
      server.RunOneClient();
    } catch (...) {
      thread_error = std::current_exception();
    }
  });

  auto client = audio_processing_module::ConnectUnixSocketWithRetry(
      socket_path, std::chrono::seconds(10), std::chrono::milliseconds(20));
  const std::vector<uint8_t> pcm(320, 1);
  audio_processing_module::WriteAll(client.get(), pcm.data(), pcm.size());
  const std::string line = ReadLineFromSocket(client.get());
  client.reset();
  server_thread.join();
  if (thread_error) {
    std::rethrow_exception(thread_error);
  }

  Require(line.find("\"type\":\"session_start\"") != std::string::npos,
          "KWS server should send a session_start control message");
  Require(line.find("\"reason\":\"kws_hit\"") != std::string::npos,
          "KWS server should include kws_hit reason");
  Require(line.find("\"keyword\":\"xiaoai\"") != std::string::npos,
          "KWS server should include the hit keyword");
}

void TestSoundboxAudioRouterRoutesOnlyCurrentMode() {
  // 验证 soundbox record 音频在 KWS/AEC 模式间单路由，切换态和故障态直接丢弃。
  AudioPipe pipe;
  ModeController mode;
  std::atomic<size_t> kws_chunks{0};
  std::atomic<size_t> aec_chunks{0};

  AudioRouter::Callbacks callbacks;
  callbacks.on_wakeup_audio = [&](const std::vector<uint8_t>& chunk) {
    Require(chunk == std::vector<uint8_t>({1, 2, 3}),
            "KWS callback should receive the KWS-mode chunk");
    ++kws_chunks;
  };
  callbacks.on_audio = [&](const std::vector<uint8_t>& chunk) {
    Require(chunk == std::vector<uint8_t>({7, 8, 9}),
            "AEC callback should receive the AEC-mode chunk");
    ++aec_chunks;
  };
  AudioRouter router(pipe, mode, callbacks);

  pipe.Start();
  router.Start();

  mode.ForceEnter(AudioMode::Kws, "test_kws");
  Require(pipe.Push(std::vector<uint8_t>{1, 2, 3}), "failed to push KWS chunk");
  Require(WaitUntil([&] { return kws_chunks.load() == 1; },
                    std::chrono::milliseconds(500)),
          "KWS mode should route only to KWS callback");
  Require(aec_chunks.load() == 0, "KWS mode must not route to AEC callback");

  mode.ForceEnter(AudioMode::LlmStarting, "test_starting");
  Require(pipe.Push(std::vector<uint8_t>{4, 5, 6}), "failed to push starting chunk");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Require(kws_chunks.load() == 1 && aec_chunks.load() == 0,
          "LLMStarting should drop record audio");

  mode.ForceEnter(AudioMode::LlmWorking, "test_aec");
  Require(pipe.Push(std::vector<uint8_t>{7, 8, 9}), "failed to push AEC chunk");
  Require(WaitUntil([&] { return aec_chunks.load() == 1; },
                    std::chrono::milliseconds(500)),
          "Aec mode should route only to AEC callback");
  Require(kws_chunks.load() == 1, "Aec mode must not route to KWS callback");

  mode.ForceEnter(AudioMode::LlmStopping, "test_stopping");
  Require(pipe.Push(std::vector<uint8_t>{10, 11, 12}), "failed to push stopping chunk");
  mode.ForceEnter(AudioMode::Fault, "test_fault");
  Require(pipe.Push(std::vector<uint8_t>{13, 14, 15}), "failed to push fault chunk");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Require(kws_chunks.load() == 1 && aec_chunks.load() == 1,
          "LLMStopping and Fault should drop record audio");
  Require(router.dropped_packets() >= 3,
          "router should count dropped transition/fault packets");

  router.Stop();
}

void TestPlaybackPcmBuildsTagPlayBinaryPayload() {
  // 验证 playback PCM 会被打包成旧 soundbox 约定的 tag=play 二进制 JSON 载荷。
  const std::vector<uint8_t> pcm = {0x01, 0x02, 0x7f, 0xff};
  const std::vector<uint8_t> packet = BuildPlayPcmPacket("playback-test", pcm);
  const std::string body(packet.begin(), packet.end());
  const auto json = nlohmann::json::parse(body);

  Require(json.value("id", "") == "playback-test",
          "playback packet should preserve id");
  Require(json.value("tag", "") == "play",
          "playback packet should use tag=play");
  Require(json.at("bytes").get<std::vector<uint8_t>>() == pcm,
          "playback packet should preserve pcm bytes");
}

void TestFrontendControlMessageParserRequiresTypedJsonFields() {
  // 验证 frontend 控制消息按 JSON line 解析，并校验 type/reason/score/timestamp_ms 字段。
  using soundbox_server::frontend::ControlMessageType;
  using soundbox_server::frontend::ParseControlMessageLine;

  const auto start = ParseControlMessageLine(
      "{\"type\":\"session_start\",\"reason\":\"kws_hit\","
      "\"score\":3.8,\"timestamp_ms\":123456}",
      ControlMessageType::kSessionStart);
  Require(start.reason == "kws_hit", "session_start reason should be parsed");
  Require(start.score.has_value() && std::abs(*start.score - 3.8) < 0.001,
          "session_start score should be parsed as a number");
  Require(start.timestamp_ms.has_value() && *start.timestamp_ms == 123456,
          "session_start timestamp_ms should be parsed as an integer");

  const auto end = ParseControlMessageLine(
      "{\"type\":\"session_end\",\"reason\":\"vad_end\",\"timestamp_ms\":654321}",
      ControlMessageType::kSessionEnd);
  Require(end.reason == "vad_end", "session_end reason should be parsed");
  Require(!end.score.has_value(), "session_end should not invent a score field");
  Require(end.timestamp_ms.has_value() && *end.timestamp_ms == 654321,
          "session_end timestamp_ms should be parsed as an integer");

  const auto rejects = [](const std::string& line,
                          ControlMessageType expected_type) {
    try {
      (void)ParseControlMessageLine(line, expected_type);
      return false;
    } catch (const std::exception&) {
      return true;
    }
  };

  Require(rejects("not-json", ControlMessageType::kSessionStart),
          "invalid JSON control line should be rejected");
  Require(rejects("{\"type\":\"session_end\",\"reason\":\"vad_end\"}",
                  ControlMessageType::kSessionStart),
          "unexpected control type should be rejected");
  Require(rejects("{\"type\":\"session_start\",\"score\":3.8,\"timestamp_ms\":1}",
                  ControlMessageType::kSessionStart),
          "session_start without reason should be rejected");
  Require(rejects("{\"type\":\"session_start\",\"reason\":\"vad_end\","
                  "\"score\":3.8,\"timestamp_ms\":1}",
                  ControlMessageType::kSessionStart),
          "session_start reason other than kws_hit should be rejected");
  Require(rejects("{\"type\":\"session_start\",\"reason\":\"kws_hit\","
                  "\"score\":\"3.8\",\"timestamp_ms\":1}",
                  ControlMessageType::kSessionStart),
          "string score should be rejected");
  Require(rejects("{\"type\":\"session_end\",\"reason\":\"vad_end\","
                  "\"timestamp_ms\":\"2\"}",
                  ControlMessageType::kSessionEnd),
          "string timestamp_ms should be rejected");
}

void TestFrontendControlSocketDisconnectsEnterFault() {
  // 验证 KWS/AEC 控制 socket EOF 或空行会让 frontend 进入 Fault，并停止整体 frontend。
  struct Case {
    std::string name;
    bool break_kws;
    bool send_empty_line;
  };

  const std::vector<Case> cases = {
      {"kws_eof", true, false},
      {"aec_empty_line", false, true},
  };

  for (const Case& test_case : cases) {
    const std::filesystem::path temp_root =
        MakeTempDirectory("frontend_control_fault_" + test_case.name);
    const std::string kws_socket = (temp_root / "frontend_kws.sock").string();
    const std::string aec_socket = (temp_root / "frontend_aec.sock").string();
    const std::string playback_socket = (temp_root / "frontend_playback.sock").string();

    MockSoundboxWebSocketServer mock_soundbox;
    std::atomic<bool> trigger_fault{false};
    std::atomic<bool> stop_servers{false};
    std::exception_ptr kws_error;
    std::exception_ptr aec_error;

    const auto server_loop = [&](const std::string& socket_path,
                                 bool is_broken_socket,
                                 std::exception_ptr* out_error) {
      try {
        auto server = audio_processing_module::CreateUnixServerSocket(socket_path, 1);
        audio_processing_module::SocketPathGuard guard(socket_path);
        auto client = audio_processing_module::AcceptUnixClientWithTimeout(
            server.get(), std::chrono::seconds(10));
        while (!trigger_fault.load() && !stop_servers.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (is_broken_socket && !stop_servers.load()) {
          if (test_case.send_empty_line) {
            const std::string empty_line = "\n";
            audio_processing_module::WriteAll(
                client.get(),
                reinterpret_cast<const uint8_t*>(empty_line.data()),
                empty_line.size());
          }
          client.reset();
        } else {
          while (!stop_servers.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }
      } catch (...) {
        *out_error = std::current_exception();
      }
    };

    std::thread kws_server_thread(
        [&] { server_loop(kws_socket, test_case.break_kws, &kws_error); });
    std::thread aec_server_thread(
        [&] { server_loop(aec_socket, !test_case.break_kws, &aec_error); });

    Frontend::Options options;
    options.kws_socket_path = kws_socket;
    options.aec_socket_path = aec_socket;
    options.playback_socket_path = playback_socket;
    options.soundbox_config.soundbox.ws_url = mock_soundbox.url();
    options.soundbox_config.soundbox.ws_token = "mock-token";
    options.soundbox_config.soundbox.connect_timeout_ms = 3000;

    Frontend frontend(options);
    std::exception_ptr frontend_error;
    std::atomic<bool> frontend_returned{false};
    std::thread frontend_thread([&] {
      try {
        frontend.Run();
      } catch (...) {
        frontend_error = std::current_exception();
      }
      frontend_returned.store(true);
    });

    try {
      Require(WaitUntil([&] { return mock_soundbox.SawCommand("fast_recording"); },
                        std::chrono::seconds(5)),
              "frontend should finish soundbox startup before control fault test");
      trigger_fault.store(true);
      Require(WaitUntil([&] { return frontend.state() == Frontend::State::kFault; },
                        std::chrono::seconds(2)),
              test_case.name + " should put frontend into Fault");
      Require(WaitUntil([&] { return frontend_returned.load(); },
                        std::chrono::seconds(2)),
              test_case.name + " should stop frontend after control socket fault");
    } catch (...) {
      frontend.Stop();
      stop_servers.store(true);
      if (frontend_thread.joinable()) {
        frontend_thread.join();
      }
      if (kws_server_thread.joinable()) {
        kws_server_thread.join();
      }
      if (aec_server_thread.joinable()) {
        aec_server_thread.join();
      }
      throw;
    }

    frontend.Stop();
    stop_servers.store(true);
    if (frontend_thread.joinable()) {
      frontend_thread.join();
    }
    kws_server_thread.join();
    aec_server_thread.join();

    if (frontend_error) {
      std::rethrow_exception(frontend_error);
    }
    if (kws_error) {
      std::rethrow_exception(kws_error);
    }
    if (aec_error) {
      std::rethrow_exception(aec_error);
    }
  }
}

void TestFrontendPlaybackReadErrorKeepsAcceptingClients() {
  // 验证 playback client 的非 timeout 读异常不会杀死 playback 线程，后续 client 仍可接入播放。
  const std::filesystem::path temp_root = MakeTempDirectory("frontend_playback_recover");
  const std::string kws_socket = (temp_root / "frontend_kws.sock").string();
  const std::string aec_socket = (temp_root / "frontend_aec.sock").string();
  const std::string playback_socket = (temp_root / "frontend_playback.sock").string();

  MockSoundboxWebSocketServer mock_soundbox;
  std::atomic<bool> stop_servers{false};
  std::exception_ptr kws_error;
  std::exception_ptr aec_error;

  const auto hold_control_socket = [&](const std::string& socket_path,
                                       std::exception_ptr* out_error) {
    try {
      auto server = audio_processing_module::CreateUnixServerSocket(socket_path, 1);
      audio_processing_module::SocketPathGuard guard(socket_path);
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          server.get(), std::chrono::seconds(10));
      while (!stop_servers.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {
      *out_error = std::current_exception();
    }
  };

  std::thread kws_server_thread(
      [&] { hold_control_socket(kws_socket, &kws_error); });
  std::thread aec_server_thread(
      [&] { hold_control_socket(aec_socket, &aec_error); });

  std::atomic<bool> fail_next_playback_read{true};
  Frontend::Options options;
  options.kws_socket_path = kws_socket;
  options.aec_socket_path = aec_socket;
  options.playback_socket_path = playback_socket;
  options.soundbox_config.soundbox.ws_url = mock_soundbox.url();
  options.soundbox_config.soundbox.ws_token = "mock-token";
  options.soundbox_config.soundbox.connect_timeout_ms = 3000;
  options.playback_read_chunk_bytes = 320;
  options.playback_read = [&](int fd, uint8_t* data, size_t size) -> ssize_t {
    if (fail_next_playback_read.exchange(false)) {
      errno = EIO;
      return -1;
    }
    return ::read(fd, data, size);
  };

  Frontend frontend(options);
  std::exception_ptr frontend_error;
  std::thread frontend_thread([&] {
    try {
      frontend.Run();
    } catch (...) {
      frontend_error = std::current_exception();
    }
  });

  try {
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("fast_recording"); },
                      std::chrono::seconds(5)),
            "frontend should finish soundbox startup before playback recovery test");
    auto failing_client = audio_processing_module::ConnectUnixSocketWithRetry(
        playback_socket, std::chrono::seconds(5), std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    failing_client.reset();

    auto playback_client = audio_processing_module::ConnectUnixSocketWithRetry(
        playback_socket, std::chrono::seconds(5), std::chrono::milliseconds(20));
    const std::vector<uint8_t> playback_pcm(320, 0x44);
    audio_processing_module::WriteAll(
        playback_client.get(), playback_pcm.data(), playback_pcm.size());
    playback_client.reset();
    Require(WaitUntil([&] { return mock_soundbox.SawPlayPacket(); },
                      std::chrono::seconds(5)),
            "playback thread should accept a second client after a read error");
  } catch (...) {
    frontend.Stop();
    stop_servers.store(true);
    if (frontend_thread.joinable()) {
      frontend_thread.join();
    }
    if (kws_server_thread.joinable()) {
      kws_server_thread.join();
    }
    if (aec_server_thread.joinable()) {
      aec_server_thread.join();
    }
    throw;
  }

  frontend.Stop();
  stop_servers.store(true);
  frontend_thread.join();
  kws_server_thread.join();
  aec_server_thread.join();

  if (frontend_error) {
    std::rethrow_exception(frontend_error);
  }
  if (kws_error) {
    std::rethrow_exception(kws_error);
  }
  if (aec_error) {
    std::rethrow_exception(aec_error);
  }
}

void TestFrontendMockSoundboxSmokeLoop() {
  // 验证 frontend 在 mock 小爱 WebSocket 和 fake 本地 socket 下完成 KWS->AEC->KWS 闭环。
  const std::filesystem::path temp_root = MakeTempDirectory("frontend_smoke");
  const std::string kws_socket = (temp_root / "frontend_kws.sock").string();
  const std::string aec_socket = (temp_root / "frontend_aec.sock").string();
  const std::string playback_socket = (temp_root / "frontend_playback.sock").string();

  MockSoundboxWebSocketServer mock_soundbox;
  std::exception_ptr kws_error;
  std::exception_ptr aec_error;
  std::atomic<size_t> kws_bytes{0};
  std::atomic<size_t> aec_bytes{0};
  std::atomic<bool> keep_control_open{true};

  std::thread kws_server_thread([&] {
    try {
      // 单客户端 fake listen：本测试只模拟一个 frontend 连接，验证路由和控制消息。
      auto server = audio_processing_module::CreateUnixServerSocket(kws_socket, 1);
      audio_processing_module::SocketPathGuard guard(kws_socket);
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          server.get(), std::chrono::seconds(10));
      kws_bytes.store(ReadAtLeastWithTimeout(client.get(), 320, std::chrono::seconds(5)));
      const std::string line =
          "{\"type\":\"session_start\",\"reason\":\"kws_hit\","
          "\"score\":3.8,\"timestamp_ms\":1}\n";
      audio_processing_module::WriteAll(
          client.get(), reinterpret_cast<const uint8_t*>(line.data()), line.size());
      while (keep_control_open.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {
      kws_error = std::current_exception();
    }
  });

  std::thread aec_server_thread([&] {
    try {
      // 单客户端 fake listen：本测试只模拟一个 frontend 连接，收到一帧 AEC 音频后结束会话。
      auto server = audio_processing_module::CreateUnixServerSocket(aec_socket, 1);
      audio_processing_module::SocketPathGuard guard(aec_socket);
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          server.get(), std::chrono::seconds(10));
      aec_bytes.store(ReadAtLeastWithTimeout(client.get(), 640, std::chrono::seconds(5)));
      const std::string line =
          "{\"type\":\"session_end\",\"reason\":\"vad_end\",\"timestamp_ms\":2}\n";
      audio_processing_module::WriteAll(
          client.get(), reinterpret_cast<const uint8_t*>(line.data()), line.size());
      while (keep_control_open.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {
      aec_error = std::current_exception();
    }
  });

  Frontend::Options options;
  options.kws_socket_path = kws_socket;
  options.aec_socket_path = aec_socket;
  options.playback_socket_path = playback_socket;
  options.soundbox_config.soundbox.ws_url = mock_soundbox.url();
  options.soundbox_config.soundbox.ws_token = "mock-token";
  options.soundbox_config.soundbox.connect_timeout_ms = 3000;
  options.playback_read_chunk_bytes = 320;

  Frontend frontend(options);
  std::exception_ptr frontend_error;
  std::thread frontend_thread([&] {
    try {
      frontend.Run();
    } catch (...) {
      frontend_error = std::current_exception();
    }
  });

  try {
    Require(WaitUntil([&] { return mock_soundbox.SawRecordHeaderToken(); },
                      std::chrono::seconds(5)),
            "frontend should connect websocket with bearer token header");
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("start_play"); },
                      std::chrono::seconds(5)),
            "frontend should send start_play after websocket connect");
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("fast_recording"); },
                      std::chrono::seconds(5)),
            "frontend should send fast_recording after start_play");
    Require(WaitUntil([&] { return frontend.state() == Frontend::State::kKws; },
                      std::chrono::seconds(5)),
            "frontend should enter KWS mode after remote audio starts");

    mock_soundbox.SendRecord("kws-record", std::vector<uint8_t>(320, 0x11));
    Require(WaitUntil([&] { return kws_bytes.load() >= 320; },
                      std::chrono::seconds(5)),
            "KWS-mode record audio should be routed only to KWS socket");
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("llm_start"); },
                      std::chrono::seconds(5)),
            "session_start should trigger llm_start");
    Require(WaitUntil([&] { return frontend.state() == Frontend::State::kAec; },
                      std::chrono::seconds(5)),
            "llm_start_ok should switch frontend into AEC mode");

    mock_soundbox.SendRecord("aec-record", std::vector<uint8_t>(640, 0x22));
    Require(WaitUntil([&] { return aec_bytes.load() >= 640; },
                      std::chrono::seconds(5)),
            "AEC-mode record audio should be routed only to AEC socket");
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("llm_stop"); },
                      std::chrono::seconds(5)),
            "session_end should trigger llm_stop");
    Require(WaitUntil([&] { return frontend.state() == Frontend::State::kKws; },
                      std::chrono::seconds(5)),
            "llm_stop_ok should return frontend to KWS mode");

    auto playback_client = audio_processing_module::ConnectUnixSocketWithRetry(
        playback_socket, std::chrono::seconds(5), std::chrono::milliseconds(20));
    const std::vector<uint8_t> playback_pcm(320, 0x33);
    audio_processing_module::WriteAll(
        playback_client.get(), playback_pcm.data(), playback_pcm.size());
    playback_client.reset();
    Require(WaitUntil([&] { return mock_soundbox.SawPlayPacket(); },
                      std::chrono::seconds(5)),
            "playback socket PCM should be forwarded as websocket tag=play packet");
  } catch (...) {
    frontend.Stop();
    keep_control_open.store(false);
    if (frontend_thread.joinable()) {
      frontend_thread.join();
    }
    if (kws_server_thread.joinable()) {
      kws_server_thread.join();
    }
    if (aec_server_thread.joinable()) {
      aec_server_thread.join();
    }
    throw;
  }

  frontend.Stop();
  keep_control_open.store(false);
  frontend_thread.join();
  kws_server_thread.join();
  aec_server_thread.join();

  if (frontend_error) {
    std::rethrow_exception(frontend_error);
  }
  if (kws_error) {
    std::rethrow_exception(kws_error);
  }
  if (aec_error) {
    std::rethrow_exception(aec_error);
  }
}

void TestWavWriterPatchesHeaderAfterStreamingSamples() {
  // 验证 WAV sink 能先流式写样本，关闭时再回填 RIFF/data 长度。
  const std::string path = "/tmp/audio_processing_module_test.wav";
  {
    WavWriter writer(path, AudioFormat::Mono16kS16());
    writer.WriteSamples(std::vector<int16_t>{1, -2, 3, -4});
    writer.Close();
  }

  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "test wav file was not created");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());

  Require(bytes.size() == 44 + 8, "wav size should be 44-byte header plus 8-byte data");
  Require(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF", "missing RIFF tag");
  Require(std::string(bytes.begin() + 8, bytes.begin() + 12) == "WAVE", "missing WAVE tag");
  Require(ReadLittleEndian<uint16_t>(bytes, 20) == 1, "wav format should be PCM");
  Require(ReadLittleEndian<uint16_t>(bytes, 22) == 1, "wav should be mono");
  Require(ReadLittleEndian<uint32_t>(bytes, 24) == 16000, "wav sample rate should be 16k");
  Require(ReadLittleEndian<uint16_t>(bytes, 34) == 16, "wav bit depth should be 16");
  Require(ReadLittleEndian<uint32_t>(bytes, 40) == 8, "wav data chunk size should match samples");
}

void TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory() {
  // 验证未指定配置参数时，程序会在当前工作目录生成 apm.yaml，并使用默认配置启动。
  const std::filesystem::path original_input =
      std::filesystem::absolute("tests/fixtures/aec_2ch_16k.s16");
  const std::filesystem::path temp_root = MakeTempDirectory("cwd");
  ScopedCurrentPath current_path_guard(temp_root);

  const std::vector<std::string> args = {
      "soundbox-server",
      "--input", original_input.string(),
  };

  const auto options = ParseOptions(args);

  Require(std::filesystem::exists(temp_root / "apm.yaml"),
          "default config file was not created in cwd");
  Require(StartsWith(options.socket_dir, "/tmp/audio_processing_module_"),
          "default socket dir was not loaded from generated config");
  Require(options.processor.delay_ms == 2, "default delay_ms should remain two milliseconds");
  Require(options.processor.ns_level == WebRtcProcessorOptions::NoiseSuppressionLevel::kHigh,
          "default ns level should remain high");
  Require(options.processor.agc_mode == WebRtcProcessorOptions::AgcMode::kAdaptiveDigital,
          "default agc mode should remain adaptive digital");
}

void TestParseOptionsLoadsConfigurationFromDirectory() {
  // 验证传入配置文件夹时，会默认读取其中的 apm.yaml 并应用文件里的配置。
  const std::filesystem::path temp_root = MakeTempDirectory("dir");
  const std::filesystem::path config_dir = temp_root / "config";
  const std::filesystem::path config_file = config_dir / "apm.yaml";
  const std::filesystem::path input_file =
      std::filesystem::absolute("tests/fixtures/aec_2ch_16k.s16");
  WriteConfigFile(config_file,
                  "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                  "delay_ms: 32\n"
                  "pre_aec_auto_gain:\n"
                  "  enabled: false\n"
                  "  target_rms: 1800\n"
                  "  max_gain: 4.5\n"
                  "  attack: 0.5\n"
                  "  release: 0.25\n"
                  "ns_level: very-high\n"
                  "agc_mode: fixed-digital\n"
                  "agc_target_dbfs: 4\n"
                  "agc_compression_gain_db: 12\n"
                  "agc_limiter_enabled: false\n");

  const std::vector<std::string> args = {
      "soundbox-server",
      "--config", config_dir.string(),
      "--input", input_file.string(),
  };

  const auto options = ParseOptions(args);

  Require(options.socket_dir == (temp_root / "sockets").string(),
          "socket dir was not loaded from config file");
  Require(options.processor.delay_ms == 32, "delay_ms was not loaded from config file");
  Require(!options.processor.pre_aec_auto_gain.enabled,
          "pre-aec auto gain enable flag was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.target_rms - 1800.0f) < 0.01f,
          "pre-aec target rms was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.max_gain - 4.5f) < 0.01f,
          "pre-aec max gain was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.attack - 0.5f) < 0.01f,
          "pre-aec attack was not loaded");
  Require(std::abs(options.processor.pre_aec_auto_gain.release - 0.25f) < 0.01f,
          "pre-aec release was not loaded");
  Require(options.processor.ns_level == WebRtcProcessorOptions::NoiseSuppressionLevel::kVeryHigh,
          "ns level was not loaded");
  Require(options.processor.agc_mode == WebRtcProcessorOptions::AgcMode::kFixedDigital,
          "agc mode was not loaded");
  Require(options.processor.agc_target_level_dbfs == 4, "agc target was not loaded");
  Require(options.processor.agc_compression_gain_db == 12, "agc compression was not loaded");
  Require(!options.processor.agc_limiter_enabled, "agc limiter was not loaded");
}

void TestParseOptionsCreatesConfigFileAtExplicitYamlPath() {
  // 验证传入配置文件绝对路径时，程序会直接在该路径创建 apm.yaml 并使用默认配置。
  const std::filesystem::path temp_root = MakeTempDirectory("file");
  const std::filesystem::path config_file = temp_root / "custom-apm.yaml";
  const std::filesystem::path input_file =
      std::filesystem::absolute("tests/fixtures/aec_2ch_16k.s16");

  const std::vector<std::string> args = {
      "soundbox-server",
      "--config", config_file.string(),
      "--input", input_file.string(),
  };

  const auto options = ParseOptions(args);

  Require(std::filesystem::exists(config_file),
          "explicit config file path was not created");
  Require(StartsWith(options.socket_dir, "/tmp/audio_processing_module_"),
          "generated config should still use default socket dir");

  std::ifstream input(config_file);
  const std::string body((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  Require(body.find("pre_aec_auto_gain:\n") != std::string::npos,
          "generated config should use nested pre-aec auto gain yaml");
}

void TestParseOptionsRejectsRemovedTuningFlags() {
  // 验证原来直接从命令行传入的调参项已经被移除，并会提示改用配置文件。
  const std::vector<std::string> args = {
      "soundbox-server",
      "--input", "tests/fixtures/aec_2ch_16k.s16",
      "--delay-ms", "32",
  };

  bool failed = false;
  try {
    (void)ParseOptions(args);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("config") != std::string::npos;
  }

  Require(failed, "removed tuning flags should be rejected with a config-file hint");
}

void TestParseOptionsLoadsSoundboxControlTimeouts() {
  // 验证 llm_start/llm_stop 的等待超时来自配置，而不是固定写死在客户端。
  const std::filesystem::path temp_root = MakeTempDirectory("soundbox_timeouts");
  const std::filesystem::path config_file = temp_root / "apm.yaml";
  WriteConfigFile(config_file,
                  "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                  "soundbox:\n"
                  "  ws_url: \"ws://127.0.0.1:9/\"\n"
                  "  ws_token: \"mock-token\"\n"
                  "  connect_timeout_ms: 1234\n"
                  "  llm_start_timeout_ms: 456\n"
                  "  llm_stop_timeout_ms: 789\n");

  const auto options = ParseOptions({"soundbox-server", "--config", config_file.string()});

  Require(options.runtime.soundbox.connect_timeout_ms == 1234,
          "connect timeout should be loaded from config");
  Require(options.runtime.soundbox.llm_start_timeout_ms == 456,
          "llm_start timeout should be loaded from config");
  Require(options.runtime.soundbox.llm_stop_timeout_ms == 789,
          "llm_stop timeout should be loaded from config");
}

void TestRunPipelineRejectsMissingSoundboxUrl() {
  // 验证生产启动缺少 soundbox.ws_url 时，在连接任何模块前给出明确错误。
  const std::filesystem::path temp_root = MakeTempDirectory("missing_ws_url");
  const std::filesystem::path config_file = temp_root / "apm.yaml";
  WriteConfigFile(config_file,
                  "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                  "soundbox:\n"
                  "  ws_token: \"mock-token\"\n");

  const auto options = ParseOptions({"soundbox-server", "--config", config_file.string()});
  bool failed = false;
  try {
    (void)audio_processing_module::RunPipeline(options);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("missing required config: soundbox.ws_url") !=
             std::string::npos;
  }

  Require(failed, "missing soundbox.ws_url should fail production startup clearly");
}

void TestRunPipelineRejectsMissingSoundboxToken() {
  // 验证生产启动缺少 soundbox.ws_token 时，在连接任何模块前给出明确错误。
  const std::filesystem::path temp_root = MakeTempDirectory("missing_ws_token");
  const std::filesystem::path config_file = temp_root / "apm.yaml";
  WriteConfigFile(config_file,
                  "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                  "soundbox:\n"
                  "  ws_url: \"ws://127.0.0.1:9/\"\n");

  const auto options = ParseOptions({"soundbox-server", "--config", config_file.string()});
  bool failed = false;
  try {
    (void)audio_processing_module::RunPipeline(options);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("missing required config: soundbox.ws_token") !=
             std::string::npos;
  }

  Require(failed, "missing soundbox.ws_token should fail production startup clearly");
}

void TestRunPipelineStopsImmediatelyOnFatalWorkerError() {
  // 验证 RunPipeline 使用的监督器会在任一 worker fatal error 后立即通知其它 worker 停止。
  std::atomic<bool> slow_worker_stopped{false};
  std::atomic<bool> slow_worker_exited{false};
  const auto start = std::chrono::steady_clock::now();
  bool failed = false;
  try {
    audio_processing_module::RunSupervisedWorkers({
        audio_processing_module::SupervisedWorker{
            "fatal",
            [] {
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
              throw std::runtime_error("fatal worker failed");
            },
            [] {},
        },
        audio_processing_module::SupervisedWorker{
            "slow",
            [&] {
              while (!slow_worker_stopped.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
              slow_worker_exited.store(true);
            },
            [&] { slow_worker_stopped.store(true); },
        },
    });
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("fatal worker failed") != std::string::npos;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

  Require(failed, "supervisor should rethrow the first worker fatal error");
  Require(slow_worker_stopped.load(), "supervisor should call stop on other workers");
  Require(slow_worker_exited.load(), "supervisor should wait for stopped workers to exit");
  Require(elapsed_ms < 1000, "supervisor fatal error should stop promptly");
}

void TestConfiguredLoggerWritesFileAndRuntimeSourcesUseIt() {
  // 验证配置化 logger 能写文件，且关键运行链路源码不再绕过 logger 直接写 stdout/stderr。
  const std::filesystem::path temp_root = MakeTempDirectory("logger");
  const std::filesystem::path log_file = temp_root / "soundbox_server.log";

  Require(xiaoai_server::ConfigureLogging(true, true, log_file.string()),
          "ConfigureLogging should enable file logging");
  auto logger = xiaoai_server::GetLogger("logger-test");
  logger->info("configured logger file sink test");
  logger->flush();

  const std::string log_body = ReadTextFile(log_file);
  Require(log_body.find("configured logger file sink test") != std::string::npos,
          "configured logger should write to the requested log file");
  (void)xiaoai_server::ConfigureLogging(false, false, "");

  const std::vector<std::filesystem::path> runtime_sources = {
      "src/apm/aec/aec_stream_processor.cpp",
      "src/apm/kws/kws_socket_server.cpp",
      "src/config/application.cpp",
      "src/frontend/soundbox_frontend.cpp",
      "src/llm/file_recorder.cpp",
  };
  for (const auto& source : runtime_sources) {
    const std::string body = ReadTextFile(source);
    Require(body.find("std::cout") == std::string::npos,
            source.string() + " should not use std::cout directly");
    Require(body.find("std::cerr") == std::string::npos,
            source.string() + " should not use std::cerr directly");
  }
}

void TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping() {
  // 验证 AEC 前自动增益只拉升较小的 ch0 音量，并在 max_gain 内避免削波。
  std::vector<int16_t> mic(160, 0);
  for (size_t index = 0; index < mic.size(); ++index) {
    mic[index] = (index % 2 == 0) ? 400 : -400;
  }

  PreAecAutoGainConfig config;
  config.enabled = true;
  config.target_rms = 2400.0f;
  config.max_gain = 6.0f;
  config.attack = 1.0f;
  config.release = 1.0f;
  AutomaticGainState state;

  const float before = ComputeRms(mic);
  ProcessPreAecAutoGain(&mic, config, &state);
  const float after = ComputeRms(mic);

  Require(before < 450.0f, "test setup should start with a small mic frame");
  Require(after > 2300.0f && after < 2500.0f, "pre-aec gain should approach target rms");
  Require(std::all_of(mic.begin(), mic.end(), [](int16_t sample) {
            return sample < 32767 && sample > -32768;
          }),
          "pre-aec gain should not clip this frame");
  Require(state.current_gain > 5.9f && state.current_gain <= 6.0f,
          "pre-aec gain should honor max gain");
}

}  // namespace

int main() {
  try {
    TestStereoFrameSplitKeepsMicAndReferenceOrder();
    TestNewPipelineModulesExposeExpectedSocketRoles();
    TestAecFilePipelineMatchesExpectedWavMd5();
    TestAecStreamProcessorReacceptsFrontendClient();
    TestAecStreamProcessorSendsVadSessionEndBeforeFrontendEof();
    TestFileRecorderReacceptsAecClient();
    TestKwsSocketServerSendsSessionStartJsonLine();
    TestSoundboxAudioRouterRoutesOnlyCurrentMode();
    TestPlaybackPcmBuildsTagPlayBinaryPayload();
    TestFrontendControlMessageParserRequiresTypedJsonFields();
    TestFrontendControlSocketDisconnectsEnterFault();
    TestFrontendPlaybackReadErrorKeepsAcceptingClients();
    TestFrontendMockSoundboxSmokeLoop();
    TestWavWriterPatchesHeaderAfterStreamingSamples();
    TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory();
    TestParseOptionsLoadsConfigurationFromDirectory();
    TestParseOptionsCreatesConfigFileAtExplicitYamlPath();
    TestParseOptionsRejectsRemovedTuningFlags();
    TestParseOptionsLoadsSoundboxControlTimeouts();
    TestRunPipelineRejectsMissingSoundboxUrl();
    TestRunPipelineRejectsMissingSoundboxToken();
    TestRunPipelineStopsImmediatelyOnFatalWorkerError();
    TestConfiguredLoggerWritesFileAndRuntimeSourcesUseIt();
    TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping();
  } catch (const std::exception& error) {
    std::cerr << "Test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
