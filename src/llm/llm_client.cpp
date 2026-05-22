#include "llm/llm_client.hpp"

#include "common/log.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace soundbox_server::llm {
namespace {

/** @brief 连接重试间隔，100 毫秒 */
constexpr auto kConnectRetryInterval = std::chrono::milliseconds(100);

/** @brief 连接总体超时时间，10 秒 */
constexpr auto kConnectTotalTimeout = std::chrono::seconds(10);

/** @brief LLM 模块的日志记录器 */
const auto kLog = xiaoai_server::GetLogger("llm");

/**
 * @brief 将 socket 文件描述符设置为非阻塞模式
 * @param fd 目标 socket 文件描述符
 * @return true 表示设置成功，false 表示失败
 */
bool SetNonBlocking(int fd) {
  // 获取当前文件描述符标志
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  // 追加 O_NONBLOCK 标志使 socket 变为非阻塞
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/**
 * @brief 将 socket 文件描述符设置为阻塞模式
 * @param fd 目标 socket 文件描述符
 * @return true 表示设置成功，false 表示失败
 */
bool SetBlocking(int fd) {
  // 获取当前文件描述符标志
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  // 清除 O_NONBLOCK 标志使 socket 恢复阻塞模式
  return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

/**
 * @brief 判断 connect 错误码是否为致命错误
 * @param error_code connect 返回的 errno 值
 * @return true 表示该错误不可重试（致命），false 表示可重试
 *
 * 以下错误码被视为非致命（可以重试）：
 * - EINPROGRESS: 连接正在进行中
 * - EAGAIN: 资源暂时不可用
 * - EWOULDBLOCK: 操作将阻塞（与非阻塞 socket 配合使用）
 * - ECONNREFUSED: 连接被拒绝（服务器未就绪）
 * - ENOENT: 不存在的实体（某些网络文件系统场景）
 */
bool IsFatalConnectError(int error_code) {
  return error_code != EINPROGRESS && error_code != EAGAIN &&
         error_code != EWOULDBLOCK && error_code != ECONNREFUSED &&
         error_code != ENOENT;
}

/**
 * @brief 从 TCP socket 逐字节读取一行 JSON 文本（以 '\n' 为分隔）
 * @param fd TCP socket 文件描述符
 * @return 不含换行符的单行文本；如果对端关闭连接则返回空字符串
 * @throws std::runtime_error 当 recv 发生非中断错误时抛出
 */
std::string ReadJsonLineFromTcp(int fd) {
  std::string line;
  char ch = '\0';
  while (true) {
    // 每次接收一个字节，直到遇到换行符
    const ssize_t result = ::recv(fd, &ch, 1, 0);
    if (result < 0) {
      // 被信号中断则重试
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("recv tcp control line: ") +
                               std::strerror(errno));
    }
    // 对端关闭连接
    if (result == 0) {
      return "";
    }
    // 遇到换行符表示一行读取完毕
    if (ch == '\n') {
      return line;
    }
    line.push_back(ch);
  }
}

}  // namespace

/**
 * @brief 构造函数：初始化配置与会话结束回调
 * @param options 连接配置选项
 * @param on_session_end 会话结束回调函数
 */
LlmClient::LlmClient(LlmClientOptions options, OnSessionEndCallback on_session_end)
    : options_(std::move(options)),
      on_session_end_(std::move(on_session_end)) {}

/** @brief 析构函数：调用 Stop() 停止后台线程并释放 socket 资源 */
LlmClient::~LlmClient() {
  Stop();
}

bool LlmClient::connected() const {
  return connected_.load();
}

/**
 * @brief 建立与 LLM 服务器的 TCP 连接（带重试机制）
 * @return true 始终返回 true（成功路径），失败时抛出异常
 * @throws std::runtime_error 当地址无效、连接超时或发生致命错误时抛出
 *
 * 连接流程：
 * 1. 在 kConnectTotalTimeout 时间内循环尝试创建 socket 并连接
 * 2. 连接前先将 socket 设为非阻塞模式，通过 poll 等待连接完成
 * 3. 连接成功后切换回阻塞模式并启动响应读取线程
 * 4. 针对 EINPROGRESS 等可重试错误码自动延迟重试
 * 5. 超过总超时时间或发生不可重试错误时抛出异常
 */
bool LlmClient::Connect() {
  // 计算连接截止时间
  const auto deadline = std::chrono::steady_clock::now() + kConnectTotalTimeout;

  while (std::chrono::steady_clock::now() < deadline && !stop_requested_.load()) {
    // 创建 IPv4 TCP socket
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      kLog->warn("tcp socket create failed: {}", std::strerror(errno));
      std::this_thread::sleep_for(kConnectRetryInterval);
      continue;
    }

    // 填充目标地址信息（IPv4 + 端口号）
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(options_.port));
    // 将点分十进制 IP 地址字符串转换为网络字节序
    if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
      ::close(fd);
      throw std::runtime_error("invalid LLM host address: " + options_.host);
    }

    // 设置为非阻塞模式，以便通过 poll 等待连接完成
    SetNonBlocking(fd);
    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
      ::close(fd);
      // 检查是否为致命错误，致命错误直接抛出异常
      if (IsFatalConnectError(errno)) {
        throw std::runtime_error("fatal tcp connect error to " + options_.host + ":" +
                                 std::to_string(options_.port) + ": " +
                                 std::strerror(errno));
      }
      // 非致命错误则等待后重试
      std::this_thread::sleep_for(kConnectRetryInterval);
      continue;
    }

    // 连接立即成功（本地或同一网络常见），直接完成连接
    if (ret == 0) {
      SetBlocking(fd);
      tcp_fd_ = fd;
      connected_.store(true);
      kLog->info("connected to LLM server {}:{}", options_.host, options_.port);
      // 启动响应读取后台线程
      response_thread_ = std::thread([this] { ResponseReaderLoop(); });
      return true;
    }

    // 连接未立即完成（EINPROGRESS 状态），通过 poll 等待连接就绪
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLOUT;  // 监听可写事件，socket 可写即表示连接完成

    // 计算 poll 允许等待的剩余毫秒数
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              std::chrono::steady_clock::now())
            .count());
    if (remaining_ms <= 0) {
      ::close(fd);
      break;  // 剩余时间耗尽，退出循环
    }

    int poll_result = ::poll(&pfd, 1, remaining_ms);
    if (poll_result < 0) {
      ::close(fd);
      if (errno == EINTR) {
        continue;  // 被信号中断则重新尝试
      }
      throw std::runtime_error("poll on tcp connect failed: " + std::string(std::strerror(errno)));
    }
    if (poll_result == 0) {
      ::close(fd);
      continue;  // poll 超时，尝试下一轮
    }

    // poll 返回就绪后，通过 getsockopt 获取最终连接状态
    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0) {
      ::close(fd);
      continue;  // 获取状态失败，丢弃此 socket 重新创建
    }
    if (socket_error != 0) {
      ::close(fd);
      // 根据最终错误码判断是否可重试
      if (IsFatalConnectError(socket_error)) {
        throw std::runtime_error("fatal tcp connect error: " + std::string(std::strerror(socket_error)));
      }
      // 可重试错误等待后继续
      std::this_thread::sleep_for(kConnectRetryInterval);
      continue;
    }

    // 连接成功：切换为阻塞模式并保存 socket 描述符
    SetBlocking(fd);
    tcp_fd_ = fd;
    connected_.store(true);
    kLog->info("connected to LLM server {}:{}", options_.host, options_.port);
    // 启动响应读取后台线程
    response_thread_ = std::thread([this] { ResponseReaderLoop(); });
    return true;
  }

  // 超过总超时时间仍未连接成功
  throw std::runtime_error("timed out connecting to LLM server " + options_.host + ":" +
                           std::to_string(options_.port));
}

/**
 * @brief 断开与 LLM 服务器的 TCP 连接
 *
 * 首先执行 shutdown(SHUT_RDWR) 优雅关闭读写通道，
 * 然后 close 释放文件描述符，最后更新连接状态。
 */
void LlmClient::Disconnect() {
  if (tcp_fd_ >= 0) {
    ::shutdown(tcp_fd_, SHUT_RDWR);
    ::close(tcp_fd_);
    tcp_fd_ = -1;
  }
  connected_.store(false);
}

/**
 * @brief 向 LLM 服务器发送音频数据块
 * @param chunk 待发送的字节数组（通常为编码后的音频帧）
 *
 * 采用循环写入确保完整发送所有数据。
 * 遇到连接异常（EPIPE、ECONNRESET、EBADF）或其他写错误时自动断开连接。
 */
void LlmClient::SendAudio(const std::vector<uint8_t>& chunk) {
  // 未连接状态下静默忽略
  if (!connected_.load() || tcp_fd_ < 0) {
    return;
  }

  size_t written = 0;
  // 循环发送直到所有数据写入完毕
  while (written < chunk.size()) {
    ssize_t result = ::send(tcp_fd_, chunk.data() + written, chunk.size() - written,
                            MSG_NOSIGNAL);
    if (result < 0) {
      // 被信号中断则重试
      if (errno == EINTR) {
        continue;
      }
      // 连接已断开：管道破裂、对端重置或无效描述符
      if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
        kLog->warn("LLM tcp connection lost during audio send");
        Disconnect();
        return;
      }
      // 其他不可恢复的写入错误
      kLog->error("LLM tcp audio send failed: {}", std::strerror(errno));
      Disconnect();
      return;
    }
    written += static_cast<size_t>(result);
  }
}

/**
 * @brief 设置会话结束回调
 * @param callback 新的回调函数对象
 */
void LlmClient::set_session_end_callback(OnSessionEndCallback callback) {
  on_session_end_ = std::move(callback);
}

/**
 * @brief 停止客户端运行
 *
 * 设置停止标志通知后台线程退出，断开 TCP 连接，并 join 响应读取线程。
 */
void LlmClient::Stop() {
  stop_requested_.store(true);
  Disconnect();
  if (response_thread_.joinable()) {
    response_thread_.join();
  }
}

/**
 * @brief 响应读取循环（运行在后台线程中）
 *
 * 持续从 TCP socket 读取 JSON 文本行，解析其中的 type 字段：
 * - "session_end"：调用 on_session_end_ 回调通知上层会话已结束
 * - 其他类型：仅以 debug 级别记录日志
 * - 空行表示连接断开
 *
 * 当 stop_requested_ 被置位或 connected_ 变为 false 时退出循环。
 */
void LlmClient::ResponseReaderLoop() {
  try {
    while (!stop_requested_.load() && connected_.load()) {
      // 从 TCP 连接读取一行 JSON 数据
      const std::string line = ReadJsonLineFromTcp(tcp_fd_);
      if (line.empty()) {
        // 空行表示对端已关闭连接
        kLog->warn("LLM tcp server disconnected");
        Disconnect();
        break;
      }

      // 解析 JSON 行
      const auto message = nlohmann::json::parse(line, nullptr, false);
      if (message.is_discarded() || !message.is_object()) {
        // 格式无效或非 JSON 对象，跳过
        kLog->warn("invalid JSON line from LLM server: {}", line);
        continue;
      }

      // 提取消息类型
      const std::string type = message.value("type", "");
      if (type == "session_end") {
        // 服务端通知会话结束
        const std::string reason = message.value("reason", "llm_server");
        kLog->info("received session_end from LLM server reason={}", reason);
        if (on_session_end_) {
          on_session_end_(reason);
        }
      } else {
        // 暂不处理的其他消息类型
        kLog->debug("ignored LLM response type={}", type);
      }
    }
  } catch (const std::exception& error) {
    // 仅在非主动停止时记录错误并断开连接
    if (!stop_requested_.load()) {
      kLog->error("LLM response reader failed: {}", error.what());
      Disconnect();
    }
  }
}

}  // namespace soundbox_server::llm