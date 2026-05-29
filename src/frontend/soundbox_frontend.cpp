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
    case Frontend::State::kSessionInit:
      return "SessionInit";
    case Frontend::State::kSessionStopped:
      return "SessionStopped";
    case Frontend::State::kSessionStarting:
      return "SessionStarting";
    case Frontend::State::kSessionStarted:
      return "SessionStarted";
    case Frontend::State::kSessionStopping:
      return "SessionStopping";
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

// 前端构造函数：存储选项并校验必填 Unix socket 路径非空
// options：前端初始化选项
// 异常：任一 socket 路径为空时抛出 std::runtime_error
Frontend::Frontend(Options options) : options_(std::move(options)) {
  if (options_.kws_socket_path.empty() || options_.aec_socket_path.empty()) {
    throw std::runtime_error("frontend socket paths must not be empty");
  }
  // state_ 保持默认值 kSessionInit，表示初始/重连状态
}

// 前端析构函数：调用 Stop() 安全释放所有资源
Frontend::~Frontend() {
  Stop();
}

// 获取当前状态快照（用于外部观察/测试，不参与业务决策）
// 返回：当前 Frontend::State 枚举值
Frontend::State Frontend::state() const {
  return state_snapshot_.load();
}

// 启动前端主循环，完成以下初始化步骤：
// 1. 重置停止标志
// 2. 进入 kSessionStopped 状态（空闲状态，等待唤醒）
// 3. 连接 KWS 和 AEC Unix socket
// 4. 注册 xiaozhi 客户端的 session_end / 下行播放 PCM 回调
// 5. 初始化 SoundBox 客户端并注册音频回调
// 6. 启动 KWS 控制读取线程
// 7. 启动 SoundBox 客户端
// 8. 阻塞等待停止信号
// 异常：SoundBox 启动失败时抛出 std::runtime_error
void Frontend::Run() {
  // 重置停止标志，允许重新运行
  stop_requested_.store(false);
  kws_uplink_enabled_.store(false);
  aec_uplink_enabled_.store(false);
  llm_uplink_enabled_.store(false);
  playback_enabled_.store(false);
  state_ = State::kSessionInit;
  state_snapshot_.store(State::kSessionInit);
  StartEventLoop();

  // 带重试连接 KWS 控制/音频 socket
  kws_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
      options_.kws_socket_path, kSocketConnectTimeout, kSocketRetryInterval);
  // 带重试连接 AEC 音频 socket
  aec_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
      options_.aec_socket_path, kSocketConnectTimeout, kSocketRetryInterval);

  // 注册 xiaozhi 客户端回调：命令通道 session_end 触发会话停止；音频通道下行 PCM 直接播放。
  if (options_.llm_client) {
    llm_client_ = options_.llm_client;
    llm_client_->set_session_end_callback(
        [this](const std::string& reason) { OnLlmSessionEnd(reason); });
    llm_client_->set_playback_audio_callback([this](const std::vector<uint8_t>& chunk) {
      if (!playback_enabled_.load()) {
        return;
      }
      if (client_ && !client_->PlayPcm(chunk)) {
        kLog->warn("PlayPcm failed xiaozhi_downlink_bytes={}", chunk.size());
      }
    });
  }

  // 配置 SoundBox 客户端回调：
  //   on_wakeup_audio：接收唤醒词的原始音频流
  //   on_audio：接收话筒/喇叭音频用于 AEC 处理
  //   on_connection_closed：SoundBox 连接意外断开时进入重连状态
  xiaoai_server::soundbox::SoundBoxClient::Callbacks callbacks;
  const uint64_t soundbox_generation = soundbox_generation_.load();
  callbacks.on_wakeup_audio = [this, soundbox_generation](const std::vector<uint8_t>& chunk) {
    if (soundbox_generation != soundbox_generation_.load()) {
      return;
    }
    OnWakeupAudio(chunk);
  };
  callbacks.on_audio = [this, soundbox_generation](const std::vector<uint8_t>& chunk) {
    if (soundbox_generation != soundbox_generation_.load()) {
      return;
    }
    OnAecAudio(chunk);
  };
  callbacks.on_connection_closed = [this, soundbox_generation](const std::string& reason) {
    if (soundbox_generation != soundbox_generation_.load()) {
      return;
    }
    kLog->warn("soundbox connection closed reason={}", reason);
    PostEvent(Event{EventType::kSoundboxDisconnected, "soundbox_connection_closed", reason});
  };
  callbacks.on_soundbox_native_kws = [this, soundbox_generation] {
    if (soundbox_generation != soundbox_generation_.load()) {
      return;
    }
    OnSoundboxNativeKws();
  };
  callbacks.on_soundbox_native_text_kws =
      [this, soundbox_generation](const std::string& text, const std::string& trigger) {
        if (soundbox_generation != soundbox_generation_.load()) {
          return;
        }
        OnSoundboxNativeTextKws(text, trigger);
      };
  client_ = std::make_unique<xiaoai_server::soundbox::SoundBoxClient>(
      options_.soundbox_config, std::move(callbacks));

  // 启动 KWS 控制消息读取线程：循环等待 session_start
  kws_control_thread_ = std::thread([this] { KwsControlReaderLoop(); });

  // 启动 SoundBox 客户端，连接到 SoundBox 服务器
  if (!client_->Start()) {
    // 启动失败：进入初始重连状态并停止所有组件
    const std::string error_detail = client_->LastConnectError();
    PostEvent(Event{EventType::kSoundboxDisconnected, "soundbox_start_failed", error_detail});
    Stop();
    throw std::runtime_error(
        error_detail.empty() ? "failed to start soundbox frontend" : error_detail);
  }

  // SoundBox 链路和本地 socket 均已就绪，投递初始化完成事件，由 Frontend
  // 控制面事件线程串行进入空闲态。
  PostEvent(Event{EventType::kFrontendReady, "startup"});

  // 主线程阻塞等待停止信号（Stop() 或 EnterSessionInitAndRestart() 重连失败会通知条件变量）
  std::unique_lock<std::mutex> lock(mu_);
  stop_cv_.wait(lock, [this] { return stop_requested_.load(); });
}

// 停止前端：设置停止标志、通知条件变量、断开所有连接、join 子线程
// 可安全重复调用，已停止时不重复 join 线程
void Frontend::Stop() {
  // 原子交换设置停止标志，记录本次调用前是否已停止
  const bool was_stopped = stop_requested_.exchange(true);
  kws_uplink_enabled_.store(false);
  aec_uplink_enabled_.store(false);
  llm_uplink_enabled_.store(false);
  playback_enabled_.store(false);
  // 通知主线程条件变量，解除 Run() 阻塞
  stop_cv_.notify_all();

  // 停止 SoundBox 客户端：断开与 SoundBox 服务器的连接
  if (client_) {
    soundbox_generation_.fetch_add(1);
    client_->Stop();
  }
  // 断开 xiaozhi TCP 客户端连接
  if (llm_client_) {
    llm_client_->Disconnect();
  }

  // 关闭 KWS 和 AEC socket 读写通道并释放文件描述符
  kws_generation_.fetch_add(1);
  ShutdownSocket(kws_socket_.get());
  ShutdownSocket(aec_socket_.get());
  kws_socket_.reset();
  aec_socket_.reset();

  // 停止 Frontend 控制面事件循环。资源先断开，确保事件线程里可能阻塞的
  // NotifyLlmStart/NotifyLlmStop 等操作能够被上游 Stop/Disconnect 唤醒。
  StopEventLoop();

  if (!was_stopped) {
    kLog->info("stop requested");
  }

  // 安全 join 子线程：检查可 join 且避免在子线程内自 join
  const std::thread::id current_thread = std::this_thread::get_id();
  if (kws_control_thread_.joinable() &&
      kws_control_thread_.get_id() != current_thread) {
    kws_control_thread_.join();
  }
}

// 唤醒音频回调：将 KWS 音频数据写入 KWS socket 供唤醒词引擎处理
// chunk：音频数据块（PCM 格式）
// 仅在 kSessionStopped 状态下写入，其他状态丢弃该音频块（避免在会话中向 KWS 引擎注入无关音频）
void Frontend::OnWakeupAudio(const std::vector<uint8_t>& chunk) {
  if (!kws_uplink_enabled_.load()) {
    kLog->debug("drop KWS audio while state={}", StateName(state()));
    return;
  }
  audio_processing_module::WriteAll(kws_socket_.get(), chunk.data(), chunk.size());
}

// AEC 音频回调：将话筒/喇叭音频数据写入 AEC socket 供回声消除引擎处理
// chunk：音频数据块（PCM 格式）
// 仅在 kSessionStarted 状态下写入，其他状态丢弃（非会话期间无需 AEC 处理）
void Frontend::OnAecAudio(const std::vector<uint8_t>& chunk) {
  if (!aec_uplink_enabled_.load()) {
    kLog->debug("drop AEC audio while state={}", StateName(state()));
    return;
  }
  audio_processing_module::WriteAll(aec_socket_.get(), chunk.data(), chunk.size());
}

// AEC 处理后音频回调：将回声消除后的 PCM 数据发送给 xiaozhi 客户端
// processed_pcm：回声消除处理后的音频数据
// 仅在 xiaozhi 客户端已连接时发送，未连接时丢弃
void Frontend::OnAecProcessedAudio(const std::vector<uint8_t>& processed_pcm) {
  if (llm_uplink_enabled_.load() && llm_client_ && llm_client_->connected()) {
    llm_client_->SendAudio(processed_pcm);
  }
}

// xiaozhi 会话停止回调：由 xiaozhi 客户端在会话结束时调用（如 VAD 静音超时）
// reason：会话停止原因字符串
// 投递给 Frontend 控制面事件队列串行处理。
void Frontend::OnLlmSessionEnd(const std::string& reason) {
  PostEvent(Event{EventType::kXiaozhiSessionEnd, reason, "xiaozhi"});
}

// SoundBox 设备自身 KWS 唤醒事件回调：投递给 Frontend 控制面事件队列串行处理。
void Frontend::OnSoundboxNativeKws() {
  PostEvent(Event{EventType::kSoundboxNativeKws, "soundbox_native_kws", "soundbox"});
}

// SoundBox 原生识别文本命中配置触发词后，复用本地 KWS session_start 事件。
void Frontend::OnSoundboxNativeTextKws(const std::string& text,
                                       const std::string& trigger) {
  kLog->info("soundbox native text triggers KWS text={} trigger={}", text, trigger);
  PostEvent(Event{EventType::kLocalKwsHit, "soundbox_native_text_kws", "soundbox"});
}

// KWS 控制通道读取线程主循环：
// 循环从 KWS socket 读取 JSON 行消息，解析为 session_start 控制消息，
// 投递给 Frontend 控制面事件队列处理。线程退出条件为 IsStopping() 返回 true
// 或在读取/解析过程中发生异常（此时进入故障状态）。
void Frontend::KwsControlReaderLoop() {
  const uint64_t kws_generation = kws_generation_.load();
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
      if (kws_generation != kws_generation_.load()) {
        return;
      }
      PostEvent(Event{EventType::kLocalKwsHit, message.reason, "local_kws",
                      message.score, message.timestamp_ms});
    }
  } catch (const std::exception& error) {
    // 非正常停止时的异常：记录错误、进入初始重连状态并重连音频链路
    if (!IsStopping() && kws_generation == kws_generation_.load()) {
      kLog->error("KWS control reader failed: {}", error.what());
      PostEvent(Event{EventType::kKwsControlDisconnected,
                      "kws_control_socket_disconnected", error.what()});
    }
  }
}

// 启动 Frontend 控制面事件循环。事件循环是唯一允许驱动 state_ 流转的上下文。
void Frontend::StartEventLoop() {
  {
    std::lock_guard<std::mutex> lock(event_mu_);
    if (event_loop_running_) {
      return;
    }
    event_loop_running_ = true;
    event_queue_.clear();
  }
  event_thread_ = std::thread([this] { EventLoop(); });
}

// 停止 Frontend 控制面事件循环并等待线程退出。
void Frontend::StopEventLoop() {
  {
    std::lock_guard<std::mutex> lock(event_mu_);
    event_loop_running_ = false;
    event_queue_.clear();
  }
  event_cv_.notify_all();

  const std::thread::id current_thread = std::this_thread::get_id();
  if (event_thread_.joinable() && event_thread_.get_id() != current_thread) {
    event_thread_.join();
  }
}

// 投递控制面事件；状态机不会在调用方线程内直接流转。
void Frontend::PostEvent(Event event) {
  {
    std::lock_guard<std::mutex> lock(event_mu_);
    if (!event_loop_running_ || stop_requested_.load()) {
      kLog->debug("drop frontend event reason={} while event loop stopped",
                  event.reason);
      return;
    }
    event_queue_.push_back(std::move(event));
  }
  event_cv_.notify_one();
}

// Frontend 控制面事件循环：串行消费所有会改变状态机的事件。
void Frontend::EventLoop() {
  while (true) {
    Event event;
    {
      std::unique_lock<std::mutex> lock(event_mu_);
      event_cv_.wait(lock, [this] {
        return !event_loop_running_ || !event_queue_.empty();
      });
      if (!event_loop_running_ && event_queue_.empty()) {
        break;
      }
      event = std::move(event_queue_.front());
      event_queue_.pop_front();
    }

    try {
      DispatchEvent(event);
    } catch (const std::exception& error) {
      kLog->error("frontend event failed reason={} error={}", event.reason,
                  error.what());
      if (!IsStopping()) {
        HandleReconnectRequired(Event{EventType::kReconnectRequired,
                                      "frontend_event_failed", error.what()});
      }
    }
  }
}

// 分发单个 Frontend 控制面事件。
void Frontend::DispatchEvent(const Event& event) {
  switch (event.type) {
    case EventType::kFrontendReady:
      HandleFrontendReady(event);
      return;
    case EventType::kLocalKwsHit:
      HandleSessionStart(event);
      return;
    case EventType::kXiaozhiSessionEnd:
      HandleSessionStop(event);
      return;
    case EventType::kSoundboxNativeKws:
      HandleSoundboxNativeKws(event);
      return;
    case EventType::kSoundboxDisconnected:
    case EventType::kKwsControlDisconnected:
    case EventType::kReconnectRequired:
      HandleReconnectRequired(event);
      return;
  }
}

// 初始化完成后进入空闲状态并打开 KWS 数据面门控。
void Frontend::HandleFrontendReady(const Event& event) {
  if (state_ != State::kSessionInit && state_ != State::kSessionStopped) {
    kLog->warn("ignore frontend ready while state={}", StateName(state_));
    return;
  }
  SetState(State::kSessionStopped, event.reason.empty() ? "startup" : event.reason.c_str());
}

// SoundBox WS 或本地控制 socket 异常时进入重连流程。
void Frontend::HandleReconnectRequired(const Event& event) {
  const char* reason = event.reason.empty() ? "reconnect_required" : event.reason.c_str();
  EnterSessionInitAndRestart(reason);
}

// 处理 session_start 事件：仅在 kSessionStopped 状态下有效，表示唤醒词命中，
// 需要启动 xiaozhi 会话。调用 SoundBox NotifyLlmStart() 通知下游启动 LLM raw 通道。
// event：本地 KWS session_start 控制事件
// 成功后进入 kSessionStarted 状态开始 AEC 处理，失败则回到 kSessionStopped 继续等待下一次唤醒。
void Frontend::HandleSessionStart(const Event& event) {
  if (state_ != State::kSessionStopped) {
    kLog->warn("ignore duplicate session_start while state={}", StateName(state_));
    return;
  }

  // 进入会话启动中状态
  SetState(State::kSessionStarting,
           event.reason.empty() ? "session_start" : event.reason.c_str());
  // 按 xiaozhi 命令通道协议发送 session_start JSON line。
  const std::string reason = event.reason.empty() ? "kws_hit" : event.reason;
  if (!llm_client_ || !llm_client_->SendSessionStart(reason, event.score,
                                                     event.timestamp_ms)) {
    SetState(State::kSessionStopped, "xiaozhi_session_start_failed");
    return;
  }
  // 通知 SoundBox 启用 LLM raw 通道
  if (client_ && client_->NotifyLlmStart()) {
    // LLM raw 通道启用成功：进入会话进行中状态，开始 AEC 处理和音频转发。
    SetState(State::kSessionStarted, "llm_start_ok");
    return;
  }
  // LLM raw 通道启用失败：回到空闲状态继续等待下一次唤醒
  SetState(State::kSessionStopped, "llm_start_failed");
}

// SoundBox 设备自身 KWS 唤醒事件：仅在 kSessionStarted 中作为打断当前
// xiaozhi 会话的信号；如果事件发生在 kSessionStarting 期间，会先排队，
// 等启动处理完成后再看到 kSessionStarted 并执行停止。
void Frontend::HandleSoundboxNativeKws(const Event& event) {
  if (state_ != State::kSessionStarted) {
    kLog->debug("ignore soundbox native KWS while state={}", StateName(state_));
    return;
  }
  StopSessionBySoundboxNativeKws(event);
}

// SoundBox 原生 KWS 打断当前会话：主动通知 xiaozhi 结束会话，
// 然后复用本地 session_stop 流程关闭 SoundBox LLM raw 通道。
void Frontend::StopSessionBySoundboxNativeKws(const Event& event) {
  SetState(State::kSessionStopping, "soundbox_native_kws");
  if (llm_client_ &&
      !llm_client_->SendSessionEnd("soundbox_native_kws", "soundbox")) {
    kLog->warn("failed to send soundbox_native_kws session_end to xiaozhi");
  }
  FinishSessionStop(event.reason.empty() ? "soundbox_native_kws" : event.reason.c_str());
}

// 处理 session_stop 事件：仅在 kSessionStarted 状态下有效，表示当前 xiaozhi 会话已结束
//（如 VAD 静音超时或显式结束）。通知 SoundBox 停止 LLM raw 通道，
// 最终回到 kSessionStopped 状态继续等待下一次唤醒。
// event：session 停止事件，reason 为停止原因字符串
// 成功后：kSessionStopping → kSessionStopped（回到空闲状态，等待下一次唤醒）
// 失败后：kSessionStopping → kSessionInit（重连 ws 音频链路）
void Frontend::HandleSessionStop(const Event& event) {
  if (state_ != State::kSessionStarted) {
    // 非 kSessionStarted 状态收到 session_stop 是异常情况，记录警告并忽略
    kLog->warn("ignore session_stop from LLM while state={}", StateName(state_));
    return;
  }
  // 进入会话停止中状态
  SetState(State::kSessionStopping,
           event.reason.empty() ? "session_stop" : event.reason.c_str());
  FinishSessionStop(event.reason.empty() ? "session_stop" : event.reason.c_str());
}

// 完成已经进入 kSessionStopping 后的本地停止流程。
void Frontend::FinishSessionStop(const char* reason) {
  // 通知 SoundBox 停止 LLM raw 通道
  if (client_ && client_->NotifyLlmStop()) {
    // LLM raw 通道停止成功：回到空闲状态，继续等待下一次唤醒
    SetState(State::kSessionStopped, "llm_stop_ok");
    return;
  }
  // LLM raw 通道停止失败：进入初始状态并重连 ws 音频链路
  kLog->warn("llm_stop failed after session_stop reason={}; reconnecting soundbox audio link",
             reason);
  EnterSessionInitAndRestart("llm_stop_failed");
}

// 进入初始重连状态并尝试恢复音频链路：
// 设置状态为 kSessionInit、断开当前 SoundBox 客户端连接、
// 尝试重连 WebSocket 音频链路。重连成功后回到 kSessionStopped 空闲状态，
// 重连失败则设置停止标志并通知 Run() 主循环退出。
// reason：断连原因描述，用于日志诊断
void Frontend::EnterSessionInitAndRestart(const char* reason) {
  // 设置初始重连状态
  SetState(State::kSessionInit, reason);
  // 断开当前 SoundBox 客户端连接
  if (client_) {
    soundbox_generation_.fetch_add(1);
    client_->Stop();
  }
  // 关闭 KWS 和 AEC socket 读写通道
  kws_generation_.fetch_add(1);
  ShutdownSocket(kws_socket_.get());
  ShutdownSocket(aec_socket_.get());
  kws_socket_.reset();
  aec_socket_.reset();
  // 尝试重连 WebSocket 音频链路
  if (RestartSoundboxAudioLink()) {
    // 重连成功：回到空闲状态，等待下一次唤醒
    SetState(State::kSessionStopped, "reconnect_ok");
    return;
  }
  // 重连失败：设置停止标志，通知 Run() 主循环退出
  kLog->error("failed to reconnect soundbox audio link; stopping");
  const bool was_stopped = stop_requested_.exchange(true);
  stop_cv_.notify_all();
  if (!was_stopped) {
    kLog->info("stop requested");
  }
}

// 重连 WebSocket 音频链路：重新启动 SoundBox 客户端并恢复远端音频。
// 重连流程：重新建立 WS 连接 → 重新连接 KWS/AEC Unix socket → 启动 KWS 控制读取线程
// 返回：重连成功返回 true，失败返回 false（所有异常被捕获，不会向上抛出）。
bool Frontend::RestartSoundboxAudioLink() {
  try {
    // 重新注册 SoundBox 客户端回调
    xiaoai_server::soundbox::SoundBoxClient::Callbacks callbacks;
    const uint64_t soundbox_generation = soundbox_generation_.load();
    callbacks.on_wakeup_audio = [this, soundbox_generation](const std::vector<uint8_t>& chunk) {
      if (soundbox_generation != soundbox_generation_.load()) {
        return;
      }
      OnWakeupAudio(chunk);
    };
    callbacks.on_audio = [this, soundbox_generation](const std::vector<uint8_t>& chunk) {
      if (soundbox_generation != soundbox_generation_.load()) {
        return;
      }
      OnAecAudio(chunk);
    };
    callbacks.on_connection_closed = [this, soundbox_generation](const std::string& reason) {
      if (soundbox_generation != soundbox_generation_.load()) {
        return;
      }
      kLog->warn("soundbox connection closed during reconnect reason={}", reason);
      PostEvent(Event{EventType::kSoundboxDisconnected,
                      "soundbox_connection_closed_reconnect", reason});
    };
    callbacks.on_soundbox_native_kws = [this, soundbox_generation] {
      if (soundbox_generation != soundbox_generation_.load()) {
        return;
      }
      OnSoundboxNativeKws();
    };
    callbacks.on_soundbox_native_text_kws =
        [this, soundbox_generation](const std::string& text, const std::string& trigger) {
          if (soundbox_generation != soundbox_generation_.load()) {
            return;
          }
          OnSoundboxNativeTextKws(text, trigger);
        };
    client_ = std::make_unique<xiaoai_server::soundbox::SoundBoxClient>(
        options_.soundbox_config, std::move(callbacks));

    // 重新建立 WS 连接
    if (!client_->Start()) {
      kLog->error("soundbox reconnect failed: {}", client_->LastConnectError());
      return false;
    }

    // 重新连接 KWS/AEC Unix socket
    kws_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
        options_.kws_socket_path, kSocketConnectTimeout, kSocketRetryInterval);
    aec_socket_ = audio_processing_module::ConnectUnixSocketWithRetry(
        options_.aec_socket_path, kSocketConnectTimeout, kSocketRetryInterval);

    // 重启 KWS 控制读取线程
    if (kws_control_thread_.joinable()) {
      kws_control_thread_.join();
    }
    kws_control_thread_ = std::thread([this] { KwsControlReaderLoop(); });

    kLog->info("soundbox audio link reconnected");
    return true;
  } catch (const std::exception& e) {
    kLog->error("soundbox reconnect exception: {}", e.what());
    return false;
  }
}

// 设置状态并记录日志。该方法只能在 Frontend 控制面事件线程内调用；
// 数据面通过 atomic gate 观察是否允许上行/播放，不直接参与状态流转。
// next：目标状
// reason：状态变更原因描述
// 相同状态不重复设置（避免日志刷屏）
void Frontend::SetState(State next, const char* reason) {
  if (state_ == next) {
    state_snapshot_.store(next);
    kws_uplink_enabled_.store(next == State::kSessionStopped);
    aec_uplink_enabled_.store(next == State::kSessionStarted);
    llm_uplink_enabled_.store(next == State::kSessionStarted);
    playback_enabled_.store(next == State::kSessionStarted);
    return;  // 状态未变化，无需处理
  }
  // 记录状态变更日志，包含旧状态、新状态和变更原因
  kLog->info("state change from={} to={} reason={}",
             StateName(state_), StateName(next), reason);
  state_ = next;
  state_snapshot_.store(next);

  // 数据面门控：高频音频不进入 Frontend 事件队列，只读取这些开关。
  kws_uplink_enabled_.store(next == State::kSessionStopped);
  aec_uplink_enabled_.store(next == State::kSessionStarted);
  llm_uplink_enabled_.store(next == State::kSessionStarted);
  playback_enabled_.store(next == State::kSessionStarted);
}

// 检查是否已收到停止请求（原子变量读取，无锁线程安全）
// 返回：true 表示已请求停止，false 表示未请求
bool Frontend::IsStopping() const {
  return stop_requested_.load();
}

}  // namespace soundbox_server::frontend