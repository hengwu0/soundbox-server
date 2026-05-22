#include "apm/aec/aec_stream_processor.hpp"

#include "common/audio_frame.hpp"
#include "common/log.hpp"
#include "common/unix_socket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace audio_processing_module::apm::aec {
namespace {

// =============================================================================
// 匿名命名空间 —— 常量定义
// =============================================================================

/// @brief accept 客户端连接的超时时间（RunOneClient 使用）
constexpr auto kSocketAcceptTimeout = std::chrono::seconds(10);

/// @brief accept 轮询间隔（Run 循环中每次 poll 的等待时长）
constexpr auto kSocketAcceptPollTimeout = std::chrono::milliseconds(200);

/// @brief 每次从 socket 读取的最大字节数
constexpr size_t kSocketReadChunkBytes = 4096;

/// @brief 模块日志句柄，tag 为 "apm/aec"
const auto kLog = xiaoai_server::GetLogger("apm/aec");

// =============================================================================
// 匿名命名空间 —— 辅助函数
// =============================================================================

/// @brief 将 errno 包装为带描述信息的 std::runtime_error
/// @param action 发生错误时正在执行的操作描述（如 "read AEC pcm"）
/// @return 包含 errno 错误消息的 runtime_error 对象
std::runtime_error ErrnoError(const std::string& action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}

/// @brief 获取当前单调时钟的毫秒时间戳（不受系统时间调整影响）
/// @return 自时钟纪元（通常为系统启动时刻）以来的毫秒数
int64_t MonotonicMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// @brief 尝试向文件描述符写入一条 session_end JSON 行，失败只记录日志不抛异常
/// @param fd 目标文件描述符（通常是对端 frontend 的 socket）
/// @param reason session 结束的原因描述字符串
///
/// 写入的 JSON 格式为：
///   {"type":"session_end","reason":"<reason>","timestamp_ms":<毫秒>}\n
///
/// 此函数设计为 "尽力而为"（best-effort），写入失败仅输出 warn 日志，
/// 不会向上层传播异常，避免影响主处理流程。
[[maybe_unused]] void TryWriteSessionEnd(int fd, const std::string& reason) {
  // 构建 session_end 的 JSON 行
  const std::string line =
      "{\"type\":\"session_end\",\"reason\":\"" + reason +
      "\",\"timestamp_ms\":" + std::to_string(MonotonicMilliseconds()) + "}\n";

  // 尝试写入；WriteAll 可能抛异常，由 catch 兜底
  try {
    WriteAll(fd, reinterpret_cast<const uint8_t*>(line.data()), line.size());
  } catch (const std::exception& e) {
    // 写入失败仅记录 warn 日志，不中断上层调用
    kLog->warn("failed to write session_end: {} fd={}", e.what(), fd);
  }
}

}  // namespace

// =============================================================================
// AecStreamProcessor —— 构造与析构
// =============================================================================

/// @brief 构造 AEC 流处理器
/// @param frontend_listen_socket_path frontend 端连接时需要监听的 Unix domain socket 路径
/// @param audio_sink 处理完成后输出的回调（接收处理后的单声道 16kHz S16LE PCM 数据）
/// @param options WebRTC 音频处理器的初始化选项（AEC/NS/AGC 开关及参数）
AecStreamProcessor::AecStreamProcessor(
    std::string frontend_listen_socket_path,
    AudioSink audio_sink,
    audio_processing_module::WebRtcProcessorOptions options)
    : frontend_listen_socket_path_(std::move(frontend_listen_socket_path)),
      audio_sink_(std::move(audio_sink)),
      options_(std::move(options)) {}

// =============================================================================
// AecStreamProcessor —— Run（多客户端循环模式）
// =============================================================================

/// @brief 启动 AEC 处理循环（持续 accept，可服务多个 frontend 连接）
///
/// 工作流程：
/// 1. 创建 Unix domain socket 服务器，监听 frontend_listen_socket_path_
/// 2. 进入 accept 循环，每接受一个客户端就调用 HandleClient 处理其音频数据
/// 3. 客户端断开后回到 accept，等待下一个连接
/// 4. 通过 Stop() 设置停止标志后可优雅退出
///
/// @note AEC frontend socket 被设计为单客户端模式：
///       每次只有一个 frontend 占用活跃的 LLM 对话轮次；
///       frontend 断开后处理器回到 accept 状态，而非维护客户端列表。
void AecStreamProcessor::Run() {
  // 清除停止标记，允许循环继续执行
  stop_requested_.store(false);

  // 创建 Unix domain socket 服务器，backlog = 1（单客户端）
  FileDescriptor frontend_server =
      CreateUnixServerSocket(frontend_listen_socket_path_, 1);
  // 作用域结束时自动清理 socket 路径文件
  SocketPathGuard frontend_guard(frontend_listen_socket_path_);
  kLog->info("frontend listen ready socket={}", frontend_listen_socket_path_);

  while (!stop_requested_.load()) {
    try {
      // 带超时的阻塞 accept；超时后抛出异常以检查停止标志
      FileDescriptor frontend_socket =
          AcceptUnixClientWithTimeout(frontend_server.get(),
                                      kSocketAcceptPollTimeout);
      // 处理单个客户端的 AEC 音频流
      HandleClient(frontend_socket.get());
    } catch (const std::runtime_error& error) {
      // 如果已收到停止请求，跳出主循环
      if (stop_requested_.load()) {
        break;
      }
      // accept 超时是正常行为（用于定期检查 stop_requested_），跳过此次循环
      if (std::string(error.what()).find("timed out waiting") !=
          std::string::npos) {
        continue;
      }
      // 其他异常向上传播
      throw;
    }
  }
}

// =============================================================================
// AecStreamProcessor —— RunOneClient（单客户端模式）
// =============================================================================

/// @brief 启动 AEC 处理并仅服务一个 frontend 客户端（用于测试/一次性场景）
///
/// 与 Run() 的区别：
/// - Run() 持续 accept 多个客户端，直到 Stop() 调用
/// - RunOneClient() 只 accept 一个客户端（最长等待 kSocketAcceptTimeout），
///   处理完该客户端即返回
void AecStreamProcessor::RunOneClient() {
  // 清除停止标记
  stop_requested_.store(false);

  // 创建 Unix domain socket 服务器，backlog = 1
  FileDescriptor frontend_server =
      CreateUnixServerSocket(frontend_listen_socket_path_, 1);
  SocketPathGuard frontend_guard(frontend_listen_socket_path_);
  kLog->info("frontend listen ready socket={}", frontend_listen_socket_path_);

  // 阻塞等待一个客户端连接（10 秒超时）
  FileDescriptor frontend_socket =
      AcceptUnixClientWithTimeout(frontend_server.get(), kSocketAcceptTimeout);
  // 处理该客户端的 AEC 音频流
  HandleClient(frontend_socket.get());
}

// =============================================================================
// AecStreamProcessor —— Stop（停止）
// =============================================================================

/// @brief 请求停止 AEC 处理循环
///
/// 设置原子标志 stop_requested_，使 Run() 主循环在下一次 accept 超时后退出。
/// 此方法可以在任何线程调用，线程安全。
void AecStreamProcessor::Stop() {
  stop_requested_.store(true);
}

// =============================================================================
// AecStreamProcessor —— HandleClient（核心处理逻辑）
// =============================================================================

/// @brief 处理一个 frontend 客户端的音频流
/// @param frontend_fd 已连接的 frontend 客户端 socket 文件描述符
///
/// 处理流程：
/// 1. 初始化 WebRTC 音频处理器（AEC/NS/AGC）
/// 2. 循环从 socket 读取原始 PCM 数据
/// 3. 将读取的字节流累积到 pending 缓冲区
/// 4. 当 pending 缓冲区 ≥ 一帧（640 字节，即 10ms 立体声 16kHz S16LE）时：
///    a. 解码为 int16_t 样本
///    b. 分离为麦克风通道和参考通道（立体声拆分）
///    c. 送入 WebRTC 处理器执行 AEC/NS/AGC
///    d. 将处理后的单声道 PCM 通过 audio_sink_ 回调输出
///    e. 从 pending 中移除已处理的一帧数据
/// 5. 客户端断开（read 返回 0）后，检查 pending 不应有残留（帧边界对齐）
/// 6. 若有残留字节（未勘界到一帧），抛出异常
void AecStreamProcessor::HandleClient(int frontend_fd) {
  kLog->info("frontend connected; WebRTC AEC/NS/AGC enabled");

  // 初始化 WebRTC 音频处理器（回声消除 / 降噪 / 自动增益控制）
  audio_processing_module::WebRtcProcessor processor(options_);

  // read_buffer: 每次 read 系统调用使用的临时缓冲区
  std::vector<uint8_t> read_buffer(kSocketReadChunkBytes, 0);

  // pending: 尚未处理的原始字节累积缓冲区（用于帧边界对齐）
  // reserve 预分配 2 帧空间以减少 reallocation
  std::vector<uint8_t> pending;
  pending.reserve(kInputBytesPerFrame * 2);

  // frames_processed: 已成功处理的完整帧计数器（用于日志统计）
  uint64_t frames_processed = 0;

  // 主读取循环
  while (true) {
    // 从 frontend socket 读取 PCM 原始数据
    const ssize_t bytes_read =
        ::read(frontend_fd, read_buffer.data(), read_buffer.size());
    if (bytes_read < 0) {
      // 被信号中断（EINTR），重新尝试读取
      if (errno == EINTR) {
        continue;
      }
      // 致命读取错误，抛出异常
      throw ErrnoError("read AEC pcm");
    }
    // bytes_read == 0：对端关闭连接，正常退出循环
    if (bytes_read == 0) {
      break;
    }

    // 将本次读取的字节追加到 pending 缓冲区末尾
    pending.insert(pending.end(), read_buffer.begin(),
                   read_buffer.begin() + bytes_read);

    // 帧边界对齐循环：只要 pending 中有足够一帧的数据就立即处理
    while (pending.size() >= kInputBytesPerFrame) {
      // 解码一帧 PCM 字节数据为 int16_t 样本向量（小端序）
      const std::vector<int16_t> interleaved =
          DecodeS16LittleEndian(pending.data(), kInputBytesPerFrame);

      // 将交织的立体声样本拆分为麦克风通道和参考（扬声器）通道
      const StereoSplit split = SplitInterleavedStereoS16(interleaved);

      // 通过 WebRTC 处理器执行 AEC/NS/AGC，输出处理后的单声道 PCM
      const std::vector<int16_t> processed =
          processor.Process10Ms(split.mic, split.reference);

      // 将处理后的 int16_t 样本编码回小端序字节流
      const std::vector<uint8_t> processed_bytes =
          EncodeS16LittleEndian(processed);

      // 如果设置了输出回调，将处理后的音频数据发送到下游
      if (audio_sink_) {
        audio_sink_(processed_bytes);
      }

      // 从 pending 缓冲区头部移除已处理的一帧数据
      pending.erase(pending.begin(), pending.begin() + kInputBytesPerFrame);

      // 帧计数器递增
      ++frames_processed;
    }
  }

  // 流结束后，pending 缓冲区不应有残留数据
  // 若仍有残留，说明 PCM 流未在完整帧边界处结束
  if (!pending.empty()) {
    throw std::runtime_error(
        "AEC PCM stream ended before a complete 10ms frame");
  }

  kLog->info("finished frames={}", frames_processed);
}

}  // namespace audio_processing_module::apm::aec