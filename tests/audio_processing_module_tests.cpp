#include "apm/aec/aec_stream_processor.hpp"
#include "apm/aec/webrtc_processor.hpp"
#include "apm/kws/kws_socket_server.hpp"
#include "apm/kws/kws_engine.hpp"
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
#include "llm/llm_client.hpp"
#include "file_recorder.hpp"
#include "file_audio_stream_frontend.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <fcntl.h>

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
using audio_processing_module::tests::aec::FileAudioStreamFrontend;
using audio_processing_module::tests::aec::FileRecorder;
using xiaoai_server::soundbox::AudioMode;
using xiaoai_server::soundbox::AudioPipe;
using xiaoai_server::soundbox::AudioRouter;
using xiaoai_server::soundbox::BuildPlayPcmPacket;
using xiaoai_server::soundbox::ModeController;
using soundbox_server::frontend::Frontend;
using soundbox_server::llm::LlmClient;
using soundbox_server::llm::LlmClientOptions;

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
  ~ScopedCurrentPath() { std::filesystem::current_path(previous_); }
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
    if (predicate()) return true;
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
  Require(EVP_DigestInit_ex(context, EVP_md5(), nullptr) == 1, "failed to initialize md5");

  std::array<char, 8192> buffer{};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytes_read = input.gcount();
    if (bytes_read > 0) {
      Require(EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(bytes_read)) == 1,
              "failed to update md5");
    }
  }
  Require(input.eof(), "failed while reading file for md5");

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  Require(EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1, "failed to finalize md5");
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
    if (result == 0 || ch == '\n') return line;
    line.push_back(ch);
  }
}

std::string ReadLineFromSocketWithTimeout(int fd, std::chrono::milliseconds timeout) {
  std::string line;
  char ch = '\0';
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out reading socket line");

    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    const int ready = ::poll(&descriptor, 1, remaining_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("poll failed while reading socket line");
    }
    if (ready == 0) continue;

    const ssize_t result = ::read(fd, &ch, 1);
    Require(result >= 0, "failed to read socket line");
    if (result == 0 || ch == '\n') return line;
    line.push_back(ch);
  }
}

std::vector<uint8_t> BuildStereoFrame(int16_t mic_sample, int16_t reference_sample) {
  std::vector<int16_t> samples;
  samples.reserve(audio_processing_module::kInputSamplesPerFrame);
  for (size_t index = 0; index < audio_processing_module::kSamplesPerChannelPerFrame; ++index) {
    samples.push_back(mic_sample);
    samples.push_back(reference_sample);
  }
  return audio_processing_module::EncodeS16LittleEndian(samples);
}

std::vector<uint8_t> BuildRecordPacket(const std::string& id, const std::vector<uint8_t>& pcm) {
  nlohmann::json packet = {{"id", id}, {"tag", "record"}, {"bytes", pcm}};
  const std::string dumped = packet.dump();
  return std::vector<uint8_t>(dumped.begin(), dumped.end());
}

std::string ResponseMessageForCommand(const std::string& command) {
  if (command == "llm_start") return "llm_start_ok";
  if (command == "llm_stop") return "llm_stop_ok";
  return command + "_ok";
}

size_t ReadAtLeastWithTimeout(int fd, size_t target_bytes, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<uint8_t, 1024> buffer{};
  size_t total = 0;
  while (total < target_bytes) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) throw std::runtime_error("timed out reading local socket");
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, remaining_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("poll failed while reading local socket");
    }
    if (ready == 0) continue;
    const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("read failed while reading local socket");
    }
    if (bytes_read == 0) throw std::runtime_error("local socket closed before expected audio");
    total += static_cast<size_t>(bytes_read);
  }
  return total;
}

int GetFreeTcpPort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  Require(fd >= 0, "failed to create tcp socket for port probe");
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  Require(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0,
          "failed to bind tcp socket for port probe");
  socklen_t addr_len = sizeof(addr);
  Require(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0,
          "failed to get tcp port");
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

class MockSoundboxWebSocketServer {
 public:
  MockSoundboxWebSocketServer()
      : port_(ix::getFreePort()), server_(port_, "127.0.0.1") {
    server_.disablePerMessageDeflate();
    server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& web_socket,
               const ix::WebSocketMessagePtr& message) {
          HandleMessage(web_socket, message);
        });
    const auto result = server_.listen();
    Require(result.first, "failed to listen mock soundbox websocket: " + result.second);
    server_.start();
  }
  ~MockSoundboxWebSocketServer() { server_.stop(); }

  std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/"; }

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
  void HandleMessage(ix::WebSocket& web_socket, const ix::WebSocketMessagePtr& message) {
    if (message->type == ix::WebSocketMessageType::Open) {
      const auto header = message->openInfo.headers.find("Authorization");
      std::lock_guard<std::mutex> lock(mu_);
      saw_authorization_header_ =
          header != message->openInfo.headers.end() &&
          header->second.find("Bearer ") == 0;
      return;
    }
    if (message->type != ix::WebSocketMessageType::Message) return;

    const auto json =
        nlohmann::json::parse(message->str.begin(), message->str.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) return;

    if (message->binary && json.value("tag", std::string()) == "play") {
      std::lock_guard<std::mutex> lock(mu_);
      saw_play_packet_ = !json.value("bytes", std::vector<uint8_t>()).empty();
      return;
    }

    const auto request_it = json.find("Request");
    if (request_it == json.end() || !request_it->is_object()) return;

    const std::string id = request_it->value("id", std::string());
    const std::string command = request_it->value("command", std::string());
    {
      std::lock_guard<std::mutex> lock(mu_);
      commands_.push_back(command);
    }

    nlohmann::json response = {
        {"Response", {{"id", id}, {"command", command}, {"code", 0},
                      {"msg", ResponseMessageForCommand(command)}}},
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
      const uint8_t*, size_t size_bytes, int sample_rate,
      int channels, int bits_per_sample) override {
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

class MockLlmTcpServer {
 public:
  MockLlmTcpServer() : listen_port_(GetFreeTcpPort()) {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    Require(server_fd_ >= 0, "failed to create mock LLM TCP server socket");

    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(listen_port_));
    Require(::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0,
            "failed to bind mock LLM TCP server");
    Require(::listen(server_fd_, 1) == 0, "failed to listen mock LLM TCP server");
  }

  ~MockLlmTcpServer() {
    Stop();
    if (server_fd_ >= 0) ::close(server_fd_);
  }

  int port() const { return listen_port_; }
  std::string host() const { return "127.0.0.1"; }
  size_t audio_bytes_received() const { return audio_bytes_received_.load(); }

  void Start() {
    accept_thread_ = std::thread([this] {
      while (!stop_requested_.load()) {
        pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, 200);
        if (ret < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (ret == 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        client_fd_ = ::accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
        if (client_fd_ < 0) {
          if (errno == EINTR) continue;
          break;
        }
        break;
      }
      if (client_fd_ < 0) return;

      std::array<uint8_t, 4096> buffer{};
      while (!stop_requested_.load()) {
        const ssize_t bytes_read = ::recv(client_fd_, buffer.data(), buffer.size(), 0);
        if (bytes_read < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (bytes_read == 0) break;
        audio_bytes_received_.fetch_add(static_cast<size_t>(bytes_read));
      }
    });
  }

  void SendSessionEnd() {
    const std::string line =
        "{\"type\":\"session_end\",\"reason\":\"vad_end\",\"timestamp_ms\":2}\n";
    if (client_fd_ >= 0) {
      ssize_t result = ::send(client_fd_, line.data(), line.size(), MSG_NOSIGNAL);
      Require(result >= 0, "failed to send session_end from mock LLM TCP server");
    }
  }

  void Stop() {
    stop_requested_.store(true);
    if (server_fd_ >= 0) {
      ::shutdown(server_fd_, SHUT_RDWR);
    }
    if (client_fd_ >= 0) {
      ::shutdown(client_fd_, SHUT_RDWR);
      ::close(client_fd_);
      client_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
    if (accept_error_) std::rethrow_exception(accept_error_);
  }

 private:
  int listen_port_;
  int server_fd_{-1};
  int client_fd_{-1};
  std::atomic<bool> stop_requested_{false};
  std::atomic<size_t> audio_bytes_received_{0};
  std::thread accept_thread_;
  std::exception_ptr accept_error_;
};

}  // namespace

// 测试 2ch/16bit PCM 数据被稳定拆成 mic 和 ref 两路单声道。
static void TestStereoFrameSplitKeepsMicAndReferenceOrder() {
  const std::vector<int16_t> interleaved = {10, 100, 20, 200, 30, 300, 40, 400};
  const StereoSplit split = SplitInterleavedStereoS16(interleaved);
  Require(split.mic == std::vector<int16_t>({10, 20, 30, 40}),
          "mic channel was not split from channel 0");
  Require(split.reference == std::vector<int16_t>({100, 200, 300, 400}),
          "reference channel was not split from channel 1");
}

// 测试新流水线模块暴露出预期角色的 socket 路径。
static void TestNewPipelineModulesExposeExpectedSocketRoles() {
  const std::string frontend_to_aec = "/tmp/frontend_to_aec.sock";
  const std::string aec_to_llm = "/tmp/aec_to_llm.sock";
  const std::string output = "/tmp/audio_processing_module_roles.wav";

  FileAudioStreamFrontend frontend("tests/fixtures/aec_2ch_16k.s16", frontend_to_aec);

  auto aec_audio_sink = [](const std::vector<uint8_t>&) {};
  AecStreamProcessor aec(frontend_to_aec, aec_audio_sink, WebRtcProcessorOptions{});
  FileRecorder recorder(aec_to_llm, output);

  Require(frontend.aec_socket_path() == frontend_to_aec,
          "frontend should connect to the AEC listen socket");
  Require(aec.frontend_listen_socket_path() == frontend_to_aec,
          "AEC should own the frontend-facing listen socket");
  Require(recorder.listen_socket_path() == aec_to_llm,
          "FileRecorder should own the AEC-facing listen socket");
}

// 测试 AEC 文件流水线输出 WAV 文件 MD5 与预期一致。
static void TestAecFilePipelineMatchesExpectedWavMd5() {
  const std::filesystem::path temp_root = MakeTempDirectory("aec_md5");
  const std::string frontend_to_aec = (temp_root / "frontend_aec.sock").string();
  const std::string aec_to_llm = (temp_root / "aec_llm.sock").string();
  const std::filesystem::path output_wav = "tests/fixtures/aec_processed.wav";

  FileRecorder recorder(aec_to_llm, output_wav.string());

  std::vector<std::exception_ptr> errors(3);
  std::thread recorder_thread([&] {
    try { recorder.RunOneClient(); }
    catch (...) { errors[0] = std::current_exception(); }
  });

  auto aec_to_llm_client = audio_processing_module::ConnectUnixSocketWithRetry(
      aec_to_llm, std::chrono::seconds(10), std::chrono::milliseconds(20));
  auto aec_audio_sink = [&aec_to_llm_client](const std::vector<uint8_t>& processed_pcm) {
    audio_processing_module::WriteAll(aec_to_llm_client.get(), processed_pcm.data(),
                                      processed_pcm.size());
  };
  AecStreamProcessor aec(frontend_to_aec, aec_audio_sink, WebRtcProcessorOptions{});
  FileAudioStreamFrontend frontend("tests/fixtures/aec_2ch_16k.s16", frontend_to_aec, false);

  std::thread aec_thread([&] {
    try { aec.RunOneClient(); }
    catch (...) { errors[1] = std::current_exception(); }
  });
  std::thread frontend_thread([&] {
    try { frontend.Run(); }
    catch (...) { errors[2] = std::current_exception(); }
  });

  frontend_thread.join();
  aec_thread.join();
  aec_to_llm_client.reset();
  recorder_thread.join();
  for (const auto& error : errors) {
    if (error) std::rethrow_exception(error);
  }

  const std::string actual_md5 = ComputeMd5Hex(output_wav);
  const std::string expected_md5 =
      ReadTextFile("tests/fixtures/expected_aec_processed.md5");
  Require(actual_md5 == expected_md5,
          "AEC processed wav md5 mismatch: expected " + expected_md5 + " got " + actual_md5);
}

// 测试 AEC 处理器在前端客户端断开后可重新接受连接。
static void TestAecStreamProcessorReacceptsFrontendClient() {
  const std::filesystem::path temp_root = MakeTempDirectory("aec_reaccept");
  const std::string frontend_to_aec = (temp_root / "frontend_aec.sock").string();

  std::atomic<size_t> audio_bytes_sent{0};
  auto aec_audio_sink = [&audio_bytes_sent](const std::vector<uint8_t>& processed_pcm) {
    audio_bytes_sent.fetch_add(processed_pcm.size());
  };
  AecStreamProcessor aec(frontend_to_aec, aec_audio_sink, WebRtcProcessorOptions{});

  std::exception_ptr thread_error;
  std::thread aec_thread([&] {
    try { aec.Run(); }
    catch (...) { thread_error = std::current_exception(); }
  });

  const std::vector<uint8_t> stereo_frame(audio_processing_module::kInputBytesPerFrame, 0);

  {
    auto frontend_client = audio_processing_module::ConnectUnixSocketWithRetry(
        frontend_to_aec, std::chrono::seconds(10), std::chrono::milliseconds(20));
    audio_processing_module::WriteAll(frontend_client.get(), stereo_frame.data(),
                                      stereo_frame.size());
  }
  WaitUntil([&] { return audio_bytes_sent.load() >= audio_processing_module::kOutputBytesPerFrame; },
            std::chrono::seconds(10));

  size_t bytes_before_second = audio_bytes_sent.load();
  {
    auto frontend_client = audio_processing_module::ConnectUnixSocketWithRetry(
        frontend_to_aec, std::chrono::seconds(10), std::chrono::milliseconds(20));
    audio_processing_module::WriteAll(frontend_client.get(), stereo_frame.data(),
                                      stereo_frame.size());
  }
  WaitUntil([&] { return audio_bytes_sent.load() >= bytes_before_second +
                      audio_processing_module::kOutputBytesPerFrame; },
            std::chrono::seconds(10));

  aec.Stop();
  aec_thread.join();
  if (thread_error) std::rethrow_exception(thread_error);
}

// 测试 AEC 处理器通过 AudioSink 回调输出处理后的 PCM 数据。
static void TestAecStreamProcessorCallsAudioSinkWithProcessedPcm() {
  const std::filesystem::path temp_root = MakeTempDirectory("aec_audio_sink");
  const std::string frontend_to_aec = (temp_root / "frontend_aec.sock").string();

  std::atomic<bool> audio_sink_called{false};
  std::atomic<size_t> received_bytes{0};
  auto aec_audio_sink = [&](const std::vector<uint8_t>& processed_pcm) {
    audio_sink_called.store(true);
    received_bytes.fetch_add(processed_pcm.size());
  };
  AecStreamProcessor aec(frontend_to_aec, aec_audio_sink, WebRtcProcessorOptions{});

  std::exception_ptr thread_error;
  std::thread aec_thread([&] {
    try { aec.Run(); }
    catch (...) { thread_error = std::current_exception(); }
  });

  auto frontend_client = audio_processing_module::ConnectUnixSocketWithRetry(
      frontend_to_aec, std::chrono::seconds(10), std::chrono::milliseconds(20));
  const std::vector<uint8_t> stereo_frame(audio_processing_module::kInputBytesPerFrame, 0);
  audio_processing_module::WriteAll(frontend_client.get(), stereo_frame.data(),
                                    stereo_frame.size());
  frontend_client.reset();

  Require(WaitUntil([&] { return audio_sink_called.load(); }, std::chrono::seconds(10)),
          "AEC should call AudioSink callback after processing audio");
  Require(received_bytes.load() >= audio_processing_module::kOutputBytesPerFrame,
          "AEC should forward processed PCM bytes to AudioSink");

  aec.Stop();
  aec_thread.join();
  if (thread_error) std::rethrow_exception(thread_error);
}

// 测试 LLM 客户端连接 TCP 服务器并接收 session_end 消息。
static void TestLlmClientConnectsToTcpServerAndReceivesSessionEnd() {
  MockLlmTcpServer mock_server;
  mock_server.Start();

  std::atomic<bool> session_end_received{false};
  std::string session_end_reason;

  LlmClient client(LlmClientOptions{mock_server.host(), mock_server.port()},
                   [&](const std::string& reason) {
                     session_end_received.store(true);
                     session_end_reason = reason;
                   });

  Require(client.Connect(), "LlmClient should connect to mock TCP server");

  const std::vector<uint8_t> audio_data(320, 0x11);
  client.SendAudio(audio_data);

  WaitUntil([&] { return mock_server.audio_bytes_received() >= 320; },
            std::chrono::seconds(5));
  Require(mock_server.audio_bytes_received() >= 320,
          "mock LLM server should receive audio from LlmClient");

  mock_server.SendSessionEnd();
  WaitUntil([&] { return session_end_received.load(); }, std::chrono::seconds(5));
  Require(session_end_received.load(), "LlmClient should receive session_end from TCP server");
  Require(session_end_reason == "vad_end", "session_end reason should be vad_end");

  client.Stop();
  mock_server.Stop();
}

// 测试 KWS socket 服务器在唤醒命中后发送 session_start JSON 行。
static void TestKwsSocketServerSendsSessionStartJsonLine() {
  const std::filesystem::path temp_root = MakeTempDirectory("kws_socket");
  const std::string socket_path = (temp_root / "frontend_kws.sock").string();
  auto engine = std::make_shared<FakeKwsEngine>();

  KwsSocketServer::Options options;
  options.listen_socket_path = socket_path;
  KwsSocketServer server(options, engine);
  std::exception_ptr thread_error;
  std::thread server_thread([&] {
    try { server.RunOneClient(); }
    catch (...) { thread_error = std::current_exception(); }
  });

  auto client = audio_processing_module::ConnectUnixSocketWithRetry(
      socket_path, std::chrono::seconds(10), std::chrono::milliseconds(20));
  const std::vector<uint8_t> pcm(320, 1);
  audio_processing_module::WriteAll(client.get(), pcm.data(), pcm.size());
  const std::string line = ReadLineFromSocket(client.get());
  client.reset();
  server_thread.join();
  if (thread_error) std::rethrow_exception(thread_error);

  Require(line.find("\"type\":\"session_start\"") != std::string::npos,
          "KWS server should send a session_start control message");
  Require(line.find("\"reason\":\"kws_hit\"") != std::string::npos,
          "KWS server should include kws_hit reason");
  Require(line.find("\"keyword\":\"xiaoai\"") != std::string::npos,
          "KWS server should include the hit keyword");
}

// 测试 Soundbox 音频路由器仅路由当前模式对应的回调。
static void TestSoundboxAudioRouterRoutesOnlyCurrentMode() {
  AudioPipe pipe;
  ModeController mode;
  std::atomic<size_t> kws_chunks{0};
  std::atomic<size_t> aec_chunks{0};

  AudioRouter::Callbacks callbacks;
  callbacks.on_wakeup_audio = [&](const std::vector<uint8_t>& chunk) {
    Require(chunk == std::vector<uint8_t>({1, 2, 3}), "KWS callback should receive KWS chunk");
    ++kws_chunks;
  };
  callbacks.on_audio = [&](const std::vector<uint8_t>& chunk) {
    Require(chunk == std::vector<uint8_t>({7, 8, 9}), "AEC callback should receive AEC chunk");
    ++aec_chunks;
  };
  AudioRouter router(pipe, mode, callbacks);

  pipe.Start();
  router.Start();

  mode.ForceEnter(AudioMode::Kws, "test_kws");
  Require(pipe.Push(std::vector<uint8_t>{1, 2, 3}), "failed to push KWS chunk");
  Require(WaitUntil([&] { return kws_chunks.load() == 1; }, std::chrono::milliseconds(500)),
          "KWS mode should route only to KWS callback");
  Require(aec_chunks.load() == 0, "KWS mode must not route to AEC callback");

  mode.ForceEnter(AudioMode::LlmStarting, "test_starting");
  Require(pipe.Push(std::vector<uint8_t>{4, 5, 6}), "failed to push starting chunk");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Require(kws_chunks.load() == 1 && aec_chunks.load() == 0, "LLMStarting should drop record audio");

  mode.ForceEnter(AudioMode::LlmWorking, "test_aec");
  Require(pipe.Push(std::vector<uint8_t>{7, 8, 9}), "failed to push AEC chunk");
  Require(WaitUntil([&] { return aec_chunks.load() == 1; }, std::chrono::milliseconds(500)),
          "Aec mode should route only to AEC callback");
  Require(kws_chunks.load() == 1, "Aec mode must not route to KWS callback");

  mode.ForceEnter(AudioMode::LlmStopping, "test_stopping");
  Require(pipe.Push(std::vector<uint8_t>{10, 11, 12}), "failed to push stopping chunk");
  mode.ForceEnter(AudioMode::Fault, "test_fault");
  Require(pipe.Push(std::vector<uint8_t>{13, 14, 15}), "failed to push fault chunk");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Require(kws_chunks.load() == 1 && aec_chunks.load() == 1,
          "LLMStopping and Fault should drop record audio");
  Require(router.dropped_packets() >= 3, "router should count dropped transition packets");

  router.Stop();
}

// 测试播放 PCM 数据构建正确的 tag=play 二进制负载。
static void TestPlaybackPcmBuildsTagPlayBinaryPayload() {
  const std::vector<uint8_t> pcm = {0x01, 0x02, 0x7f, 0xff};
  const std::vector<uint8_t> packet = BuildPlayPcmPacket("playback-test", pcm);
  const std::string body(packet.begin(), packet.end());
  const auto json = nlohmann::json::parse(body);

  Require(json.value("id", "") == "playback-test", "playback packet should preserve id");
  Require(json.value("tag", "") == "play", "playback packet should use tag=play");
  Require(json.at("bytes").get<std::vector<uint8_t>>() == pcm,
          "playback packet should preserve pcm bytes");
}

// 测试前端控制消息解析器对 JSON 字段类型做严格校验。
static void TestFrontendControlMessageParserRequiresTypedJsonFields() {
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

  const auto rejects = [](const std::string& line, ControlMessageType expected_type) {
    try {
      (void)ParseControlMessageLine(line, expected_type);
      return false;
    } catch (const std::exception&) {
      return true;
    }
  };

  Require(rejects("not-json", ControlMessageType::kSessionStart),
          "invalid JSON control line should be rejected");
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
}

// 测试前端控制 socket 连接断开时系统进入 Fault 状态。
static void TestFrontendControlSocketDisconnectsEnterFault() {
  const std::filesystem::path temp_root = MakeTempDirectory("frontend_control_fault");
  const std::string kws_socket = (temp_root / "frontend_kws.sock").string();
  const std::string aec_socket = (temp_root / "frontend_aec.sock").string();
  const std::string playback_socket = (temp_root / "frontend_playback.sock").string();

  MockSoundboxWebSocketServer mock_soundbox;
  MockLlmTcpServer mock_llm;
  mock_llm.Start();

  std::atomic<bool> trigger_fault{false};
  std::atomic<bool> stop_servers{false};
  std::exception_ptr kws_error;

  const auto kws_server_func = [&](const std::string& socket_path) {
    try {
      auto server = audio_processing_module::CreateUnixServerSocket(socket_path, 1);
      audio_processing_module::SocketPathGuard guard(socket_path);
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          server.get(), std::chrono::seconds(10));
      while (!trigger_fault.load() && !stop_servers.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!stop_servers.load()) {
        client.reset();
      } else {
        while (!stop_servers.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
    } catch (...) {
      kws_error = std::current_exception();
    }
  };

  std::thread kws_thread([&] { kws_server_func(kws_socket); });

  auto aec_server = audio_processing_module::CreateUnixServerSocket(aec_socket, 1);
  audio_processing_module::SocketPathGuard aec_guard(aec_socket);
  std::thread aec_thread([&] {
    try {
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          aec_server.get(), std::chrono::seconds(10));
      while (!trigger_fault.load() && !stop_servers.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {}
  });

  Frontend::Options options;
  options.kws_socket_path = kws_socket;
  options.aec_socket_path = aec_socket;
  options.playback_socket_path = playback_socket;
  options.soundbox_config.soundbox.ws_url = mock_soundbox.url();
  options.soundbox_config.soundbox.ws_token = "mock-token";
  options.soundbox_config.soundbox.connect_timeout_ms = 3000;
  options.llm_client = std::make_shared<LlmClient>(
      LlmClientOptions{mock_llm.host(), mock_llm.port()}, [](const std::string&) {});

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
            "frontend should finish soundbox startup");
    trigger_fault.store(true);
    Require(WaitUntil([&] { return frontend.state() == Frontend::State::kFault; },
                      std::chrono::seconds(2)),
            "KWS control socket disconnect should put frontend into Fault");
    Require(WaitUntil([&] { return frontend_returned.load(); }, std::chrono::seconds(2)),
            "frontend should stop after KWS control socket fault");
  } catch (...) {
    frontend.Stop();
    stop_servers.store(true);
    if (frontend_thread.joinable()) frontend_thread.join();
    if (kws_thread.joinable()) kws_thread.join();
    if (aec_thread.joinable()) aec_thread.join();
    mock_llm.Stop();
    throw;
  }

  frontend.Stop();
  stop_servers.store(true);
  if (frontend_thread.joinable()) frontend_thread.join();
  kws_thread.join();
  aec_thread.join();
  mock_llm.Stop();

  if (frontend_error) std::rethrow_exception(frontend_error);
  if (kws_error) std::rethrow_exception(kws_error);
}

// 测试前端播放读错误后仍能继续接受新客户端连接。
static void TestFrontendPlaybackReadErrorKeepsAcceptingClients() {
  const std::filesystem::path temp_root = MakeTempDirectory("frontend_playback_recover");
  const std::string kws_socket = (temp_root / "frontend_kws.sock").string();
  const std::string aec_socket = (temp_root / "frontend_aec.sock").string();
  const std::string playback_socket = (temp_root / "frontend_playback.sock").string();

  MockSoundboxWebSocketServer mock_soundbox;
  MockLlmTcpServer mock_llm;
  mock_llm.Start();

  std::atomic<bool> stop_servers{false};
  std::exception_ptr kws_error;

  auto kws_server = audio_processing_module::CreateUnixServerSocket(kws_socket, 1);
  audio_processing_module::SocketPathGuard kws_guard(kws_socket);
  std::thread kws_thread([&] {
    try {
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          kws_server.get(), std::chrono::seconds(10));
      while (!stop_servers.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {
      kws_error = std::current_exception();
    }
  });

  auto aec_server = audio_processing_module::CreateUnixServerSocket(aec_socket, 1);
  audio_processing_module::SocketPathGuard aec_guard(aec_socket);
  std::thread aec_thread([&] {
    try {
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          aec_server.get(), std::chrono::seconds(10));
      while (!stop_servers.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {}
  });

  std::atomic<bool> fail_next_playback_read{true};
  Frontend::Options options;
  options.kws_socket_path = kws_socket;
  options.aec_socket_path = aec_socket;
  options.playback_socket_path = playback_socket;
  options.soundbox_config.soundbox.ws_url = mock_soundbox.url();
  options.soundbox_config.soundbox.ws_token = "mock-token";
  options.soundbox_config.soundbox.connect_timeout_ms = 3000;
  options.llm_client = std::make_shared<LlmClient>(
      LlmClientOptions{mock_llm.host(), mock_llm.port()}, [](const std::string&) {});
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
    try { frontend.Run(); }
    catch (...) { frontend_error = std::current_exception(); }
  });

  try {
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("fast_recording"); },
                      std::chrono::seconds(5)),
            "frontend should finish soundbox startup");

    auto failing_client = audio_processing_module::ConnectUnixSocketWithRetry(
        playback_socket, std::chrono::seconds(5), std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    failing_client.reset();

    auto playback_client = audio_processing_module::ConnectUnixSocketWithRetry(
        playback_socket, std::chrono::seconds(5), std::chrono::milliseconds(20));
    const std::vector<uint8_t> playback_pcm(320, 0x44);
    audio_processing_module::WriteAll(playback_client.get(), playback_pcm.data(),
                                      playback_pcm.size());
    playback_client.reset();
    Require(WaitUntil([&] { return mock_soundbox.SawPlayPacket(); }, std::chrono::seconds(5)),
            "playback thread should accept a second client after a read error");
  } catch (...) {
    frontend.Stop();
    stop_servers.store(true);
    if (frontend_thread.joinable()) frontend_thread.join();
    if (kws_thread.joinable()) kws_thread.join();
    if (aec_thread.joinable()) aec_thread.join();
    mock_llm.Stop();
    throw;
  }

  frontend.Stop();
  stop_servers.store(true);
  frontend_thread.join();
  kws_thread.join();
  aec_thread.join();
  mock_llm.Stop();
  if (frontend_error) std::rethrow_exception(frontend_error);
  if (kws_error) std::rethrow_exception(kws_error);
}

// 测试前端与模拟 Soundbox 完整交互流程：KWS → AEC → 恢复。
static void TestFrontendMockSoundboxSmokeLoop() {
  const std::filesystem::path temp_root = MakeTempDirectory("frontend_smoke");
  const std::string kws_socket = (temp_root / "frontend_kws.sock").string();
  const std::string aec_socket = (temp_root / "frontend_aec.sock").string();
  const std::string playback_socket = (temp_root / "frontend_playback.sock").string();

  MockSoundboxWebSocketServer mock_soundbox;
  MockLlmTcpServer mock_llm;
  mock_llm.Start();

  auto shared_llm = std::make_shared<LlmClient>(
      LlmClientOptions{mock_llm.host(), mock_llm.port()}, [](const std::string&) {});
  shared_llm->Connect();

  std::exception_ptr kws_error;
  std::atomic<size_t> kws_bytes{0};
  std::atomic<size_t> aec_bytes{0};
  std::atomic<bool> keep_control_open{true};

  std::thread kws_server_thread([&] {
    try {
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

  auto aec_server = audio_processing_module::CreateUnixServerSocket(aec_socket, 1);
  audio_processing_module::SocketPathGuard aec_guard(aec_socket);
  std::thread aec_server_thread([&] {
    try {
      auto client = audio_processing_module::AcceptUnixClientWithTimeout(
          aec_server.get(), std::chrono::seconds(10));
      aec_bytes.store(ReadAtLeastWithTimeout(client.get(), 640, std::chrono::seconds(5)));
      while (keep_control_open.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (...) {}
  });

  Frontend::Options options;
  options.kws_socket_path = kws_socket;
  options.aec_socket_path = aec_socket;
  options.playback_socket_path = playback_socket;
  options.soundbox_config.soundbox.ws_url = mock_soundbox.url();
  options.soundbox_config.soundbox.ws_token = "mock-token";
  options.soundbox_config.soundbox.connect_timeout_ms = 3000;
  options.llm_client = shared_llm;
  options.playback_read_chunk_bytes = 320;

  Frontend frontend(options);
  std::exception_ptr frontend_error;
  std::thread frontend_thread([&] {
    try { frontend.Run(); }
    catch (...) { frontend_error = std::current_exception(); }
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
    Require(WaitUntil([&] { return kws_bytes.load() >= 320; }, std::chrono::seconds(5)),
            "KWS-mode record audio should be routed only to KWS socket");
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("llm_start"); },
                      std::chrono::seconds(5)),
            "session_start should trigger llm_start");
    Require(WaitUntil([&] { return frontend.state() == Frontend::State::kAec; },
                      std::chrono::seconds(5)),
            "llm_start_ok should switch frontend into AEC mode");

    mock_soundbox.SendRecord("aec-record", std::vector<uint8_t>(640, 0x22));
    Require(WaitUntil([&] { return aec_bytes.load() >= 640; }, std::chrono::seconds(5)),
            "AEC-mode record audio should be routed to AEC socket");

    mock_llm.SendSessionEnd();
    Require(WaitUntil([&] { return mock_soundbox.SawCommand("llm_stop"); },
                      std::chrono::seconds(5)),
            "session_end should trigger llm_stop");
    Require(WaitUntil([&] { return frontend.state() == Frontend::State::kKws; },
                      std::chrono::seconds(5)),
            "llm_stop_ok should return frontend to KWS mode");

    auto playback_client = audio_processing_module::ConnectUnixSocketWithRetry(
        playback_socket, std::chrono::seconds(5), std::chrono::milliseconds(20));
    const std::vector<uint8_t> playback_pcm(320, 0x33);
    audio_processing_module::WriteAll(playback_client.get(), playback_pcm.data(),
                                      playback_pcm.size());
    playback_client.reset();
    Require(WaitUntil([&] { return mock_soundbox.SawPlayPacket(); }, std::chrono::seconds(5)),
            "playback socket PCM should be forwarded as websocket tag=play packet");
  } catch (...) {
    frontend.Stop();
    keep_control_open.store(false);
    if (frontend_thread.joinable()) frontend_thread.join();
    if (kws_server_thread.joinable()) kws_server_thread.join();
    if (aec_server_thread.joinable()) aec_server_thread.join();
    mock_llm.Stop();
    throw;
  }

  frontend.Stop();
  keep_control_open.store(false);
  frontend_thread.join();
  kws_server_thread.join();
  aec_server_thread.join();
  mock_llm.Stop();

  if (frontend_error) std::rethrow_exception(frontend_error);
  if (kws_error) std::rethrow_exception(kws_error);
}

// 测试 WAV 写入器在流式写入采样后正确修复文件头。
static void TestWavWriterPatchesHeaderAfterStreamingSamples() {
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

// 测试未指定配置参数时，程序在当前工作目录生成 apm.yaml 并使用默认配置启动。
static void TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory() {
  const std::filesystem::path temp_root = MakeTempDirectory("cwd");
  ScopedCurrentPath current_path_guard(temp_root);

  const std::vector<std::string> args = {"soundbox-server"};

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

// 测试传入配置文件夹时，程序默认读取其中的 apm.yaml 并应用文件里的配置。
static void TestParseOptionsLoadsConfigurationFromDirectory() {
  const std::filesystem::path temp_root = MakeTempDirectory("dir");
  const std::filesystem::path config_dir = temp_root / "config";
  const std::filesystem::path config_file = config_dir / "apm.yaml";
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

// 测试已移除的 --input 命令行参数会被拒绝并提示改用配置文件。
static void TestParseOptionsRejectsRemovedTuningFlags() {
  const std::vector<std::string> args = {
      "soundbox-server",
      "--input", "tests/fixtures/aec_2ch_16k.s16",
  };
  bool failed = false;
  try {
    (void)ParseOptions(args);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("config") != std::string::npos;
  }
  Require(failed, "removed --input flag should be rejected with a config-file hint");
}

// 测试 ParseOptions 加载 Soundbox llm_start/llm_stop 超时配置。
static void TestParseOptionsLoadsSoundboxControlTimeouts() {
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

// 测试 ParseOptions 加载播放和 LLM 配置项。
static void TestParseOptionsLoadsPlaybackAndLlmConfig() {
  const std::filesystem::path temp_root = MakeTempDirectory("playback_llm_config");
  const std::filesystem::path config_file = temp_root / "apm.yaml";
  WriteConfigFile(config_file,
                  "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                  "soundbox:\n"
                  "  ws_url: \"ws://127.0.0.1:9/\"\n"
                  "  ws_token: \"mock-token\"\n"
                  "playback:\n"
                  "  sample_rate: 44100\n"
                  "  channels: 2\n"
                  "  bits_per_sample: 24\n"
                  "llm:\n"
                  "  host: \"10.0.0.1\"\n"
                  "  port: 9876\n");

  const auto options = ParseOptions({"soundbox-server", "--config", config_file.string()});

  Require(options.runtime.playback.sample_rate == 44100,
          "playback sample_rate should be loaded from config");
  Require(options.runtime.playback.channels == 2,
          "playback channels should be loaded from config");
  Require(options.runtime.playback.bits_per_sample == 24,
          "playback bits_per_sample should be loaded from config");
  Require(options.runtime.llm.host == "10.0.0.1",
          "llm host should be loaded from config");
  Require(options.runtime.llm.port == 9876,
          "llm port should be loaded from config");
}

// 测试 RunPipeline 在缺少 soundbox.ws_url 时启动失败并给出明确错误。
static void TestRunPipelineRejectsMissingSoundboxUrl() {
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
    failed = std::string(error.what()).find("missing or invalid config: soundbox.ws_url") !=
             std::string::npos;
  }
  Require(failed, "missing soundbox.ws_url should fail production startup clearly");
}

// 测试 RunPipeline 在缺少 soundbox.ws_token 时启动失败并给出明确错误。
static void TestRunPipelineRejectsMissingSoundboxToken() {
  const std::filesystem::path temp_root = MakeTempDirectory("missing_ws_token");
  const std::filesystem::path config_file = temp_root / "apm.yaml";
  WriteConfigFile(config_file,
                   "socket_dir: \"" + (temp_root / "sockets").string() + "\"\n"
                   "soundbox:\n"
                   "  ws_url: \"ws://127.0.0.1:9/\"\n"
                   "  ws_token: \"\"\n");

  const auto options = ParseOptions({"soundbox-server", "--config", config_file.string()});
  bool failed = false;
  try {
    (void)audio_processing_module::RunPipeline(options);
  } catch (const std::exception& error) {
    failed = std::string(error.what()).find("missing or invalid config: soundbox.ws_token") !=
             std::string::npos;
  }
  Require(failed, "missing soundbox.ws_token should fail production startup clearly");
}

// 测试 supervisor 在 worker 发生 fatal 错误时立即停止所有 worker 并退出。
static void TestRunPipelineStopsImmediatelyOnFatalWorkerError() {
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
                              std::chrono::steady_clock::now() - start).count();
  Require(failed, "supervisor should rethrow the first worker fatal error");
  Require(slow_worker_stopped.load(), "supervisor should call stop on other workers");
  Require(slow_worker_exited.load(), "supervisor should wait for stopped workers to exit");
  Require(elapsed_ms < 1000, "supervisor fatal error should stop promptly");
}

// 测试配置日志器写入文件且运行时源文件不使用 std::cout/cerr 直接输出。
static void TestConfiguredLoggerWritesFileAndRuntimeSourcesUseIt() {
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
      "src/llm/llm_client.cpp",
  };
  for (const auto& source : runtime_sources) {
    const std::string body = ReadTextFile(source);
    Require(body.find("std::cout") == std::string::npos,
            source.string() + " should not use std::cout directly");
    Require(body.find("std::cerr") == std::string::npos,
            source.string() + " should not use std::cerr directly");
  }
}

// 测试前置 AEC 自动增益对小信号帧放大到目标 RMS 且不削波。
static void TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping() {
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

int main() {
  try {
    std::cerr << "[ RUN      ] TestStereoFrameSplitKeepsMicAndReferenceOrder\n";
    TestStereoFrameSplitKeepsMicAndReferenceOrder();
    std::cerr << "[ RUN      ] TestNewPipelineModulesExposeExpectedSocketRoles\n";
    TestNewPipelineModulesExposeExpectedSocketRoles();
    std::cerr << "[ RUN      ] TestAecFilePipelineMatchesExpectedWavMd5\n";
    TestAecFilePipelineMatchesExpectedWavMd5();
    std::cerr << "[ RUN      ] TestAecStreamProcessorReacceptsFrontendClient\n";
    TestAecStreamProcessorReacceptsFrontendClient();
    std::cerr << "[ RUN      ] TestAecStreamProcessorCallsAudioSinkWithProcessedPcm\n";
    TestAecStreamProcessorCallsAudioSinkWithProcessedPcm();
    std::cerr << "[ RUN      ] TestLlmClientConnectsToTcpServerAndReceivesSessionEnd\n";
    TestLlmClientConnectsToTcpServerAndReceivesSessionEnd();
    std::cerr << "[ RUN      ] TestKwsSocketServerSendsSessionStartJsonLine\n";
    TestKwsSocketServerSendsSessionStartJsonLine();
    std::cerr << "[ RUN      ] TestSoundboxAudioRouterRoutesOnlyCurrentMode\n";
    TestSoundboxAudioRouterRoutesOnlyCurrentMode();
    std::cerr << "[ RUN      ] TestPlaybackPcmBuildsTagPlayBinaryPayload\n";
    TestPlaybackPcmBuildsTagPlayBinaryPayload();
    std::cerr << "[ RUN      ] TestFrontendControlMessageParserRequiresTypedJsonFields\n";
    TestFrontendControlMessageParserRequiresTypedJsonFields();
    std::cerr << "[ RUN      ] TestFrontendControlSocketDisconnectsEnterFault\n";
    TestFrontendControlSocketDisconnectsEnterFault();
    std::cerr << "[ RUN      ] TestFrontendPlaybackReadErrorKeepsAcceptingClients\n";
    TestFrontendPlaybackReadErrorKeepsAcceptingClients();
    std::cerr << "[ RUN      ] TestFrontendMockSoundboxSmokeLoop\n";
    TestFrontendMockSoundboxSmokeLoop();
    std::cerr << "[ RUN      ] TestWavWriterPatchesHeaderAfterStreamingSamples\n";
    TestWavWriterPatchesHeaderAfterStreamingSamples();
    std::cerr << "[ RUN      ] TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory\n";
    TestParseOptionsCreatesDefaultConfigInCurrentWorkingDirectory();
    std::cerr << "[ RUN      ] TestParseOptionsLoadsConfigurationFromDirectory\n";
    TestParseOptionsLoadsConfigurationFromDirectory();
    std::cerr << "[ RUN      ] TestParseOptionsRejectsRemovedTuningFlags\n";
    TestParseOptionsRejectsRemovedTuningFlags();
    std::cerr << "[ RUN      ] TestParseOptionsLoadsSoundboxControlTimeouts\n";
    TestParseOptionsLoadsSoundboxControlTimeouts();
    std::cerr << "[ RUN      ] TestParseOptionsLoadsPlaybackAndLlmConfig\n";
    TestParseOptionsLoadsPlaybackAndLlmConfig();
    std::cerr << "[ RUN      ] TestRunPipelineRejectsMissingSoundboxUrl\n";
    TestRunPipelineRejectsMissingSoundboxUrl();
    std::cerr << "[ RUN      ] TestRunPipelineRejectsMissingSoundboxToken\n";
    TestRunPipelineRejectsMissingSoundboxToken();
    std::cerr << "[ RUN      ] TestRunPipelineStopsImmediatelyOnFatalWorkerError\n";
    TestRunPipelineStopsImmediatelyOnFatalWorkerError();
    std::cerr << "[ RUN      ] TestConfiguredLoggerWritesFileAndRuntimeSourcesUseIt\n";
    TestConfiguredLoggerWritesFileAndRuntimeSourcesUseIt();
    std::cerr << "[ RUN      ] TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping\n";
    TestPreAecAutoGainRaisesSmallMicFrameWithoutClipping();
    std::cerr << "[       OK ] All tests passed.\n";
  } catch (const std::exception& error) {
    std::cerr << "Test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}