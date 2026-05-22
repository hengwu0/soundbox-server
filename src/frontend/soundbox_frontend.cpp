#include "frontend/soundbox_frontend.hpp"

#include "common/log.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

namespace soundbox_server::frontend {
namespace {

// Unix socket 连接超时时间（秒）
constexpr auto kSocketConnectTimeout = std::chrono::seconds(10);
// Unix socket 连接重试间隔（毫秒）
constexpr auto kSocketRetryInterval = std::chrono::milliseconds(20);
// 播放 accept 轮询超时时间（毫秒），使 accept 循环可响应 stop_requested_
constexpr auto kAcceptPollTimeout = std::chrono::milliseconds(200);

// 前端全局日志记录器
const auto kLog = xiaoai_server::GetLogger("frontend");

// 将 ControlMessageType 枚举值转换为 JSON 中的类型名字符串
// type：控制消息类型枚举值
// 返回：对应的 JSON 类型名字符串
const char* ControlMessageTypeName(ControlMessageType type) {
  switch (type) {
    case ControlMessageType::kSessionStart:
      return "session_start";
  }
  return "unknown";
}

// 从 JSON 对象中提取整数字段并校验
// message：JSON 对象
// field：字段名
// 返回：字段的 int64_t 值
// 异常：字段缺失或类型非整数时抛出 std::runtime_error
int64_t GetIntegerField(const nlohmann::json& message, const char* field) {
  const auto it = message.find(field);
  if (it == message.end()) {
    throw std::runtime_error(std::string("control message missing field: ") + field);
  }
  if (!it->is_number_integer() && !it->is_number_unsigned()) {
    throw std::runtime_error(std::string("control message field must be integer: ") +
                             field);
  }
  return it->get<int64_t>();
}

// 从 JSON 对象中提取可选整数字段，字段不存在时返回 nullopt
// message：JSON 对象
// field：字段名
// 返回：字段存在且为整数时返回其 int64_t 值，否则返回 nullopt
std::optional<int64_t> GetOptionalIntegerField(const nlohmann::json& message,
                                                const char* field) {
  if (!message.contains(field)) {
    return std::nullopt;
  }
  return GetIntegerField(message, field);
}

// 从 JSON 对象中提取字符串字段并校验
// message：JSON 对象
// field：字段名
// 返回：字段的 std::string 值
// 异常：字段缺失或类型非字符串时抛出 std::runtime_error
std::string GetStringField(const nlohmann::json& message, const char* field) {
  const auto it = message.find(field);
  if (it == message.end()) {
    throw std::runtime_error(std::string("control message missing field: ") + field);
  }
  if (!it->is_string()) {
    throw std::runtime_error(std::string("control message field must be string: ") +
                             field);
  }
  return it->get<std::string>();
}

// 从 JSON 对象中提取可选浮点数字段，字段不存在时返回 nullopt
// message：JSON 对象
// field：字段名
// 返回：字段存在且为数值时返回其 double 值，否则返回 nullopt
std::optional<double> GetOptionalNumberField(const nlohmann::json& message,
                                              const char* field) {
  const auto it = message.find(field);
  if (it == message.end()) {
    return std::nullopt;
  }
  if (!it->is_number()) {
    throw std::runtime_error(std::string("control message field must be numeric: ") +
                             field);
  }
  return it->get<double>();
}

// 从 socket fd 逐字节读取一行 JSON 文本（以 '\n' 为终止符）
// fd：已打开的 Unix socket 文件描述符
// 返回：读取到的完整行（不含 '\n'），EOF 时返回空字符串
// 异常：读取系统调用失败时抛出 std::runtime_error
std::string ReadJsonLine(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    const ssize_t result = ::read(fd, &ch, 1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;  // 被信号中断，重试读取
      }
      throw std::runtime_error(std::string("read control line: ") +
                               std::strerror(errno));
    }
    if (result == 0) {
      return "";  // 对端关闭连接，返回空行
    }
    if (ch == '\n') {
      return line;  // 遇到行终止符，返回已读取内容
    }
    line.push_back(ch);
  }
}

// 安全关闭 socket 的读写通道（SHUT_RDWR），fd < 0 时不做任何操作
// fd：要关闭的 socket 文件描述符
void ShutdownSocket(int fd) {
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
  }
}

}  // namespace

// 将 Frontend::State 枚举值转换为可读的字符串名称，用于日志输出
// state：前端状态枚举值
// 返回：对应的中英文字符串名称
const char* StateName(Frontend::State state) {
  switch (state) {
    case Frontend::State::kSessionEnd:
      return "SessionEnd";
    case Frontend::State::kKws:
      return "kws";
    case Frontend::State::kLlmStarting:
      return "LLMStarting";
    case Frontend::State::kAec:
      return "Aec";
    case Frontend::State::kLlmStopping:
      return "LLMStopping";
    case Frontend::State::kFault:
      return "Fault";
  }
  return "Unknown";
}

// 解析控制消息行：将 JSON 行解析为 ControlMessage，校验类型匹配，根据类型
// 提取对应字段（session_start 需要 reason 和可选 score）
// line：从 KWS socket 读取的一行 JSON 字符串
// expected_type：期望的消息类型
// 返回：解析并校验通过的 ControlMessage
// 异常：JSON 格式错误、类型不匹配、必填字段缺失时抛出 std::runtime_error
ControlMessage ParseControlMessageLine(const std::string& line,
                                        ControlMessageType expected_type) {
  // 解析 JSON 行，容错处理非法 JSON
  const auto message = nlohmann::json::parse(line, nullptr, false);
  if (message.is_discarded() || !message.is_object()) {
    throw std::runtime_error("control message line is not a JSON object");
  }

  // 校验消息类型字段必须匹配期望类型
  const std::string type = GetStringField(message, "type");
  const std::string expected_type_name = ControlMessageTypeName(expected_type);
  if (type != expected_type_name) {
    throw std::runtime_error("unexpected control message type: " + type +
                             ", expected: " + expected_type_name);
  }

  ControlMessage parsed;
  parsed.type = expected_type;
  parsed.timestamp_ms = GetOptionalIntegerField(message, "timestamp_ms");

  // session_start 消息：reason 必须为 "kws_hit"，可选 score 置信度
  if (expected_type == ControlMessageType::kSessionStart) {
    parsed.reason = GetStringField(message, "reason");
    if (parsed.reason != "kws_hit") {
      throw std::runtime_error("session_start reason must be kws_hit");
    }
    parsed.score = GetOptionalNumberField(message, "score");
    return parsed;
  }

  // 通用字段提取（其他消息类型直接提取存在的 reason 和 score）
  if (message.contains("reason")) {
    parsed.reason = GetStringField(message, "reason");
  }
  if (message.contains("score")) {
    parsed.score = GetOptionalNumberField(message, "score");
  }
  return parsed;
}

// 前端构造函数：存储选项并校验必填 socket 路径非空
// options：前端初始化选项
// 异常：任一 socket 路径为空时抛出 std::runtime_error
Frontend::Frontend(Options options) : options_(std::move(options)) {
  if (options_.kws_socket_path.empty() || options_.aec_socket_path.empty() ||
      options_.playback_socket_path.empty()) {
    throw std::runtime_error("frontend socket paths must not be empty");
  }
  // state_ 保持默认值 kSessionEnd，表示初始空闲状态
}

// 前端析构函数：调用 Stop() 安全释放所有资源
Frontend::~Frontend() {
  Stop();
}

// 获取当前状态（线程安全）
// 返回：当前 Frontend::State 枚举值
Frontend::State Frontend::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

// 启动前端主循环，完成以下初始化步骤：
// 1. 重置停止标志
// 2. 进入 kKws 状态（启动唤醒词检测）
// 3. 连接 KWS 和 AEC Unix socket
// 4. 注册 LLM 客户端的 session_end 回调
// 5. 初始化 SoundBox 客户端并注册音频回调
// 6. 启动 KWS 控制读取线程和播放接受线程
// 7. 启动 SoundBox 客户端
// 8. 阻塞等待停止信号
// 异常：SoundBox 启动失败时抛出 std::runtime_error
void Frontend::Run() {
  // 重置停止标志，允许重新运行
  stop_requested_.store(false);
  // 启动时立即进入 KWS 状态开始唤醒词检测
  SetState(State::kKws, "startup");

  // 带重试连接 KWS 控制/音频 socket
  kws_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
      options_.kws_socket_path, kSocketConnectTimeout, kSocketRetryInterval);
  // 带重试连接 AEC 音频 socket
  aec_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
      options_.aec_socket_path, kSocketConnectTimeout, kSocketRetryInterval);

  // 注册 LLM 客户端会话结束回调：当 VAD 检测到静音超时或其他原因结束时触发
  if (options_.llm_client) {
    llm_client_ = options_.llm_client;
    llm_client_->set_session_end_callback(
        [this](const std::string& reason) { OnLlmSessionEnd(reason); });
  }

  // 配置 SoundBox 客户端回调：
  //   on_wakeup_audio：接收唤醒词的原始音频流
  //   on_audio：接收话筒/喇叭音频用于 AEC 处理
  //   on_connection_closed：SoundBox 连接意外断开时的异常处理
  xiaoai_server::soundbox::SoundBoxClient::Callbacks callbacks;
  callbacks.on_wakeup_audio = [this](const std::vector<uint8_t>& chunk) {
    OnWakeupAudio(chunk);
  };
  callbacks.on_audio = [this](const std::vector<uint8_t>& chunk) {
    OnAecAudio(chunk);
  };
  callbacks.on_connection_closed = [this](const std::string& reason) {
    kLog->warn("soundbox connection closed reason={}", reason);
    EnterFaultAndStop("soundbox_connection_closed");
  };
  client_ = std::make_unique<xiaoai_server::soundbox::SoundBoxClient>(
      options_.soundbox_config, std::move(callbacks));

  // 启动 KWS 控制消息读取线程：循环等待 session_start
  kws_control_thread_ = std::thread([this] { KwsControlReaderLoop(); });
  // 启动播放接受线程：创建 playback Unix socket 并接受播放客户端
  playback_thread_ = std::thread([this] { PlaybackAcceptLoop(); });

  // 启动 SoundBox 客户端，连接到 SoundBox 服务器
  if (!client_->Start()) {
    // 启动失败：进入故障状态并停止所有组件
    const std::string error_detail = client_->LastConnectError();
    SetState(State::kFault, "soundbox_start_failed");
    Stop();
    throw std::runtime_error(
        error_detail.empty() ? "failed to start soundbox frontend" : error_detail);
  }

  // 主线程阻塞等待停止信号（Stop() 或 EnterFaultAndStop() 会通知条件变量）
  std::unique_lock<std::mutex> lock(mu_);
  stop_cv_.wait(lock, [this] { return stop_requested_.load(); });
}

// 停止前端：设置停止标志、通知条件变量、断开所有连接、join 子线程
// 可安全重复调用，已停止时不重复 join 线程
void Frontend::Stop() {
  // 原子交换设置停止标志，记录本次调用前是否已停止
  const bool was_stopped = stop_requested_.exchange(true);
  // 通知主线程条件变量，解除 Run() 阻塞
  stop_cv_.notify_all();

  // 停止 SoundBox 客户端：断开与 SoundBox 服务器的连接
  if (client_) {
    client_->Stop();
  }
  // 断开 LLM 客户端连接
  if (llm_client_) {
    llm_client_->Disconnect();
  }

  // 关闭 KWS 和 AEC socket 读写通道并释放文件描述符
  ShutdownSocket(kws_socket_.get());
  ShutdownSocket(aec_socket_.get());
  kws_socket_.reset();
  aec_socket_.reset();

  if (!was_stopped) {
    kLog->info("stop requested");
  }

  // 安全 join 子线程：检查可 join 且避免在子线程内自 join
  const std::thread::id current_thread = std::this_thread::get_id();
  if (kws_control_thread_.joinable() &&
      kws_control_thread_.get_id() != current_thread) {
    kws_control_thread_.join();
  }
  if (playback_thread_.joinable() && playback_thread_.get_id() != current_thread) {
    playback_thread_.join();
  }
}

// 唤醒音频回调：将 KWS 音频数据写入 KWS socket 供唤醒词引擎处理
// chunk：音频数据块（PCM 格式）
// 仅在 kKws 状态下写入，其他状态丢弃该音频块（避免在会话中向 KWS 引擎注入无关音频）
void Frontend::OnWakeupAudio(const std::vector<uint8_t>& chunk) {
  if (state() != State::kKws) {
    kLog->debug("drop KWS audio while state={}", StateName(state()));
    return;
  }
  audio_processing_module::WriteAll(kws_socket_.get(), chunk.data(), chunk.size());
}

// AEC 音频回调：将话筒/喇叭音频数据写入 AEC socket 供回声消除引擎处理
// chunk：音频数据块（PCM 格式）
// 仅在 kAec 状态下写入，其他状态丢弃（非会话期间无需 AEC 处理）
void Frontend::OnAecAudio(const std::vector<uint8_t>& chunk) {
  if (state() != State::kAec) {
    kLog->debug("drop AEC audio while state={}", StateName(state()));
    return;
  }
  audio_processing_module::WriteAll(aec_socket_.get(), chunk.data(), chunk.size());
}

// AEC 处理后音频回调：将回声消除后的 PCM 数据发送给 LLM 客户端
// processed_pcm：回声消除处理后的音频数据
// 仅在 LLM 客户端已连接时发送，未连接时丢弃
void Frontend::OnAecProcessedAudio(const std::vector<uint8_t>& processed_pcm) {
  if (llm_client_ && llm_client_->connected()) {
    llm_client_->SendAudio(processed_pcm);
  }
}

// LLM 会话结束回调：由 LLM 客户端在会话结束时调用（如 VAD 静音超时）
// reason：会话结束原因字符串
// 转发给 HandleSessionEnd 执行状态转换，仅在 kAec 状态时有效
void Frontend::OnLlmSessionEnd(const std::string& reason) {
  HandleSessionEnd(reason);
}

// KWS 控制通道读取线程主循环：
// 循环从 KWS socket 读取 JSON 行消息，解析为 session_start 控制消息，
// 分发给 HandleSessionStart 处理。线程退出条件为 IsStopping() 返回 true
// 或在读取/解析过程中发生异常（此时进入故障状态）。
void Frontend::KwsControlReaderLoop() {
  try {
    while (!IsStopping()) {
      // 阻塞读取一行 JSON（对端 KWS 引擎发送控制消息）
      const std::string line = ReadJsonLine(kws_socket_.get());
      if (line.empty()) {
        // 对端关闭连接：KWS socket 断开
        throw std::runtime_error("KWS control socket disconnected");
      }
      // 解析并校验 JSON 行，期望类型为 session_start
      const ControlMessage message =
          ParseControlMessageLine(line, ControlMessageType::kSessionStart);
      HandleSessionStart(message);
    }
  } catch (const std::exception& error) {
    // 非正常停止时的异常：记录错误、进入故障状态并停止所有组件
    if (!IsStopping()) {
      kLog->error("KWS control reader failed: {}", error.what());
      SetState(State::kFault, "kws_control_reader_failed");
      EnterFaultAndStop("kws_control_reader_failed");
    }
  }
}

// 播放接受线程主循环：
// 创建 playback Unix socket 服务器并循环接受播放客户端连接。
// 每次只保持一个活跃播放客户端；客户端断开后等待下一个连接。
// 使用带超时的 accept 以响应 stop_requested_ 标志。
void Frontend::PlaybackAcceptLoop() {
  // 创建 Unix socket 服务器并绑定到 playback_socket_path
  audio_processing_module::FileDescriptor server =
      audio_processing_module::CreateUnixServerSocket(options_.playback_socket_path, 1);
  // SocketPathGuard 在作用域结束时自动清理 socket 文件
  audio_processing_module::SocketPathGuard guard(options_.playback_socket_path);
  kLog->info("playback listen ready socket={}", options_.playback_socket_path);

  while (!IsStopping()) {
    try {
      // 单客户端 listen：每次只接一个 playback writer，断开后重新 accept
      audio_processing_module::FileDescriptor client =
          audio_processing_module::AcceptUnixClientWithTimeout(server.get(),
                                                               kAcceptPollTimeout);
      try {
        // 进入客户端读取循环：读取 PCM 数据并写入 SoundBox 播放队列
        PlaybackClientLoop(client.get());
      } catch (const std::runtime_error& error) {
        // 客户端断开或读取失败，等待下一个播放客户端连接
        if (IsStopping()) {
          break;
        }
        kLog->warn("playback client failed: {}; waiting for next client",
                   error.what());
      }
    } catch (const std::runtime_error& error) {
      if (IsStopping()) {
        break;  // 停止时 accept 超时是预期行为，正常退出
      }
      if (std::string(error.what()).find("timed out waiting") !=
          std::string::npos) {
        continue;  // accept 超时，继续循环检查 stop_requested_
      }
      throw;  // 其他异常向上传播
    }
  }
}

// 播放客户端处理循环：从客户端 fd 循环读取 PCM 数据块并调用 SoundBox
// 客户端的 PlayPcm() 方法输出。
// client_fd：播放客户端 socket 文件描述符
// 使用 options_.playback_read 自定义读取函数（如果有）或默认 ::read。
void Frontend::PlaybackClientLoop(int client_fd) {
  // 预分配读取缓冲区
  std::vector<uint8_t> buffer(options_.playback_read_chunk_bytes);
  while (!IsStopping()) {
    // 使用自定义读取函数或默认 ::read 系统调用
    const ssize_t bytes_read = options_.playback_read
                                    ? options_.playback_read(client_fd, buffer.data(),
                                                             buffer.size())
                                    : ::read(client_fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;  // 被信号中断，重试读取
      }
      throw std::runtime_error(std::string("read playback pcm: ") +
                               std::strerror(errno));
    }
    if (bytes_read == 0) {
      break;  // 对端关闭连接，退出读取循环
    }
    // 拷贝实际读取的数据块到 chunk 并写入播放队列
    std::vector<uint8_t> chunk(buffer.begin(), buffer.begin() + bytes_read);
    if (client_ && !client_->PlayPcm(chunk)) {
      kLog->warn("PlayPcm failed playback_chunk_bytes={}", bytes_read);
    }
  }
}

// 处理 session_start 消息：仅在 kKws 状态下有效，表示唤醒词命中，
// 需要启动 LLM 会话。调用 SoundBox NotifyLlmStart() 通知下游启动 LLM 通道。
// message：已解析的 session_start 控制消息，reason 为 "kws_hit"
// 成功后进入 kAec 状态开始 AEC 处理，失败则回到 kKws 继续等待下一次唤醒。
void Frontend::HandleSessionStart(const ControlMessage& message) {
  if (state() != State::kKws) {
    kLog->warn("ignore duplicate session_start while state={}", StateName(state()));
    return;
  }
  // 进入 LLM 启动中状态
  SetState(State::kLlmStarting,
           message.reason.empty() ? "session_start" : message.reason.c_str());
  // 通知 SoundBox 启用 LLM 通道
  if (client_ && client_->NotifyLlmStart()) {
    // LLM 启动成功：进入 AEC 状态，开始回声消除和音频转发
    SetState(State::kAec, "llm_start_ok");
    return;
  }
  // LLM 启动失败：回到 KWS 状态继续等待下一次唤醒
  SetState(State::kKws, "llm_start_failed");
}

// 处理 session_end 消息：仅在 kAec 状态下有效，表示当前 LLM 会话已结束
//（如 VAD 静音超时或显式结束）。通知 SoundBox 停止 LLM 通道，
// 最终回到 kKws 状态继续等待下一次唤醒。
// reason：session 结束原因字符串
// 成功后：kLlmStopping → kKws（回到唤醒词检测，等待下一次唤醒）
// 失败后：kLlmStopping → kKws（即使停止失败也回到 kKws，重置信道）
void Frontend::HandleSessionEnd(const std::string& reason) {
  if (state() != State::kAec) {
    // 非 kAec 状态收到 session_end 是异常情况，记录警告并忽略
    kLog->warn("ignore session_end from LLM while state={}", StateName(state()));
    return;
  }
  // 进入 LLM 停止中状态
  SetState(State::kLlmStopping,
           reason.empty() ? "session_end" : reason.c_str());
  // 通知 SoundBox 停止 LLM 通道
  if (client_ && client_->NotifyLlmStop()) {
    // LLM 停止成功：回到 kKws 状态，继续等待下一次唤醒
    SetState(State::kKws, "llm_stop_ok");
    return;
  }
  // LLM 停止失败：仍然回到 kKws 状态，SoundBox 音频链路可能需重置
  kLog->warn("llm_stop failed after session_end; resetting soundbox audio link");
  SetState(State::kKws, "llm_stop_failed_reset");
}

// 进入故障状态并停止所有组件：
// 设置状态为 kFault、关闭所有 socket 连接、断开 LLM 和 SoundBox 客户端、
// 通知条件变量唤醒 Run() 主循环以退出。
// reason：故障原因描述，用于日志诊断
void Frontend::EnterFaultAndStop(const char* reason) {
  // 设置故障状态
  SetState(State::kFault, reason);
  // 设置停止标志并通知条件变量，使 Run() 主循环退出
  const bool was_stopped = stop_requested_.exchange(true);
  stop_cv_.notify_all();
  // 立即断开所有 socket 和客户端连接，无需等待线程 join
  ShutdownSocket(kws_socket_.get());
  ShutdownSocket(aec_socket_.get());
  kws_socket_.reset();
  aec_socket_.reset();
  if (llm_client_) {
    llm_client_->Disconnect();
  }
  if (client_) {
    client_->Stop();
  }
  if (!was_stopped) {
    kLog->info("stop requested");
  }
}

// 线程安全地设置状态并记录日志
// next：目标状
// reason：状态变更原因描述
// 相同状态不重复设置（避免日志刷屏）
void Frontend::SetState(State next, const char* reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == next) {
    return;  // 状态未变化，无需处理
  }
  // 记录状态变更日志，包含旧状态、新状态和变更原因
  kLog->info("state change from={} to={} reason={}",
             StateName(state_), StateName(next), reason);
  state_ = next;
}

// 检查是否已收到停止请求（原子变量读取，无锁线程安全）
// 返回：true 表示已请求停止，false 表示未请求
bool Frontend::IsStopping() const {
  return stop_requested_.load();
}

}  // namespace soundbox_server::frontend