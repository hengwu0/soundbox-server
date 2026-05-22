#include "apm/kws/kws_socket_server.hpp"

#include "common/log.hpp"
#include "common/unix_socket.hpp"

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace audio_processing_module::apm::kws {
namespace {

/// accept 超时轮询间隔：每次 200ms 检查一次是否收到停止信号。
constexpr auto kAcceptPollTimeout = std::chrono::milliseconds(200);

/// apm/kws 模块专用日志器。
const auto kLog = xiaoai_server::GetLogger("apm/kws");

/// 将当前 errno 封装为 std::runtime_error。
/// @param action 操作描述字符串。
/// @return 携带有 errno 信息的异常。
std::runtime_error ErrnoError(const std::string& action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}

/// 获取当前单调时钟的毫秒数，用于 session_start 消息的时间戳。
/// @return 自系统启动以来的毫秒数。
int64_t MonotonicMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// 向客户端回写一条 session_start JSON 消息，告知前端已检测到唤醒词。
/// @param client_fd 已连接的前端 socket fd。
/// @param hit       KWS 命中结果（含唤醒词文本）。
void WriteSessionStart(int client_fd, const xiaoai_server::wakeup::KwsHit& hit) {
  const std::string line =
      "{\"type\":\"session_start\",\"reason\":\"kws_hit\",\"keyword\":\"" +
      hit.keyword + "\",\"score\":0,\"timestamp_ms\":" +
      std::to_string(MonotonicMilliseconds()) + "}\n";
  // 将 JSON 行写入 socket
  WriteAll(client_fd, reinterpret_cast<const uint8_t*>(line.data()), line.size());
}

}  // namespace

/// 构造 KWS Socket 服务端，校验配置合法性。
/// @param options socket 和音频格式配置。
/// @param engine  KWS 引擎实例。
KwsSocketServer::KwsSocketServer(
    Options options,
    std::shared_ptr<xiaoai_server::wakeup::IKwsEngine> engine)
    : options_(std::move(options)), engine_(std::move(engine)) {
  if (options_.listen_socket_path.empty()) {
    throw std::runtime_error("KWS listen socket path is empty");
  }
  if (!engine_) {
    throw std::runtime_error("KWS engine is null");
  }
  if (options_.bits_per_sample != 16 || options_.channels <= 0 ||
      options_.sample_rate <= 0 || options_.read_chunk_bytes == 0) {
    throw std::runtime_error("invalid KWS socket server audio options");
  }
}

/// 进入 KWS Socket 服务端主循环。
/// 单客户端模式：一次只服务一个前端连接，断开后回到 accept 等待下一次连接。
void KwsSocketServer::Run() {
  FileDescriptor server = CreateUnixServerSocket(options_.listen_socket_path, 1);
  SocketPathGuard guard(options_.listen_socket_path);
  kLog->info("frontend listen ready socket={}", options_.listen_socket_path);

  while (!stop_requested_.load()) {
    try {
      // 单客户端 listen：一次只服务一个 frontend；断开后回到 accept 等下一次连接。
      FileDescriptor client =
          AcceptUnixClientWithTimeout(server.get(), kAcceptPollTimeout);
      HandleClient(client.get());
    } catch (const std::runtime_error& error) {
      if (stop_requested_.load()) {
        break;  ///< 收到停止信号，退出循环。
      }
      if (std::string(error.what()).find("timed out waiting") !=
          std::string::npos) {
        continue;  ///< accept 超时是正常的轮询结果，继续等待。
      }
      throw;
    }
  }
}

/// 单客户端模式（调试用）：只 accept 一次，处理完就退出。
void KwsSocketServer::RunOneClient() {
  FileDescriptor server = CreateUnixServerSocket(options_.listen_socket_path, 1);
  SocketPathGuard guard(options_.listen_socket_path);
  FileDescriptor client =
      AcceptUnixClientWithTimeout(server.get(), std::chrono::seconds(10));
  HandleClient(client.get());
}

/// 设置停止标记，通知 Run() 主循环退出。
void KwsSocketServer::Stop() {
  stop_requested_.store(true);
}

/// 处理单个客户端连接：循环读取 PCM 数据帧送入 KWS 引擎。
/// 检测到关键词命中时通过 socket 回写 session_start 事件并重置引擎状态。
/// @param client_fd 已连接的前端客户端 socket fd。
void KwsSocketServer::HandleClient(int client_fd) {
  kLog->info("frontend connected; KWS PCM stream enabled");
  engine_->Reset();  ///< 新连接到来时重置 KWS 引擎状态。

  std::vector<uint8_t> buffer(options_.read_chunk_bytes);
  while (!stop_requested_.load()) {
    const ssize_t bytes_read = ::read(client_fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;  ///< 被信号中断，重试。
      }
      throw ErrnoError("read KWS pcm");
    }
    if (bytes_read == 0) {
      break;  ///< 对端关闭连接，退出循环。
    }
    if (bytes_read % 2 != 0) {
      throw std::runtime_error("KWS PCM stream ended on an odd byte");
    }

    // 送入 KWS 引擎做关键词检测
    const auto hit = engine_->AcceptPcm16(
        buffer.data(), static_cast<size_t>(bytes_read), options_.sample_rate,
        options_.channels, options_.bits_per_sample);
    if (hit.has_value()) {
      // 关键词命中：通知前端并重置引擎
      WriteSessionStart(client_fd, *hit);
      engine_->Reset();
    }
  }

  kLog->info("frontend disconnected");
}

}  // namespace audio_processing_module::apm::kws