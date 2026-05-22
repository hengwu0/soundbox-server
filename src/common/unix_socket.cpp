#include "common/unix_socket.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace audio_processing_module {
namespace {

/// 根据当前 errno 构造包含描述信息的 std::runtime_error。
/// @param action 描述当前操作的字符串。
/// @return 携带 errno 信息的异常对象。
std::runtime_error ErrnoError(const std::string& action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}

/// 构造 Unix Domain Socket 地址结构，并校验路径长度。
/// @param path socket 文件路径。
/// @return 填充好的 sockaddr_un 结构体。
/// @throws std::runtime_error 路径超出系统限制时抛出。
sockaddr_un MakeAddress(const std::string& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;  ///< 使用 Unix Domain Socket 协议族。
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("unix socket path is too long: " + path);
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  return address;
}

/// 计算距离截止时间还剩多少毫秒，已超时则返回 0。
/// @param deadline 绝对截止时间点。
/// @return 剩余毫秒数，超时返回 0。
int RemainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
          .count());
}

}  // namespace

/// 析构文件描述符：如果持有有效 fd，则调用 ::close 释放。
FileDescriptor::~FileDescriptor() {
  reset();
}

/// 移动构造：接管 other 的 fd，并将 other 置为无效状态。
FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

/// 移动赋值：先释放自身 fd，再接管 other 的 fd。
FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    reset(other.fd_);
    other.fd_ = -1;
  }
  return *this;
}

/// 重置文件描述符：先关闭当前 fd（若有效），再设置新 fd。
/// @param fd 新文件描述符值，默认为 -1 表示无效。
void FileDescriptor::reset(int fd) {
  if (fd_ >= 0) {
    ::close(fd_);  ///< 关闭旧的文件描述符。
  }
  fd_ = fd;
}

/// 析构时自动删除 socket 文件，防止残留文件导致下次 bind 失败。
SocketPathGuard::~SocketPathGuard() {
  if (!path_.empty()) {
    ::unlink(path_.c_str());
  }
}

/// 创建 Unix Domain Socket 服务端，执行 socket+bind+listen。
/// @param path    socket 文件路径。
/// @param backlog listen 队列最大长度。
/// @return 包装好的 FileDescriptor（服务端 socket）。
/// @throws std::runtime_error 任一系统调用失败时抛出。
FileDescriptor CreateUnixServerSocket(const std::string& path, int backlog) {
  const std::filesystem::path socket_path(path);
  if (socket_path.has_parent_path()) {
    std::filesystem::create_directories(socket_path.parent_path());
  }

  // 先删除可能存在的旧 socket 文件
  ::unlink(path.c_str());

  // 创建 socket
  FileDescriptor server(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!server.valid()) {
    throw ErrnoError("socket");
  }

  // 绑定到路径
  sockaddr_un address = MakeAddress(path);
  if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw ErrnoError("bind " + path);
  }

  // 开始监听
  if (::listen(server.get(), backlog) < 0) {
    throw ErrnoError("listen " + path);
  }

  return server;
}

/// 在超时内循环 accept 一个客户端连接，支持 EINTR 重试。
/// @param server_fd 已 listen 的 socket 描述符。
/// @param timeout   最大等待时间。
/// @return 包装好的客户端 FileDescriptor。
/// @throws std::runtime_error 超时或系统调用失败时抛出。
FileDescriptor AcceptUnixClientWithTimeout(
    int server_fd,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (true) {
    pollfd descriptor{};
    descriptor.fd = server_fd;
    descriptor.events = POLLIN;  ///< 监听可读事件（即新连接到达）。

    const int poll_timeout = RemainingMilliseconds(deadline);
    if (poll_timeout == 0) {
      throw std::runtime_error("timed out waiting for unix socket client");
    }

    // 用 poll 等待 fd 可读
    const int ready = ::poll(&descriptor, 1, poll_timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;  ///< 被信号中断，重试。
      }
      throw ErrnoError("poll accept");
    }
    if (ready == 0) {
      throw std::runtime_error("timed out waiting for unix socket client");
    }

    // 接受新连接
    const int client_fd = ::accept4(server_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;  ///< 被信号中断，重试。
      }
      throw ErrnoError("accept");
    }
    return FileDescriptor(client_fd);
  }
}

/// 循环尝试连接 Unix Domain Socket 服务端，直到成功或超时。
/// 服务端尚未创建（ENOENT）或连接被拒绝（ECONNREFUSED）时重试。
/// @param path           socket 文件路径。
/// @param timeout        总超时时间。
/// @param retry_interval 每次重试间隔。
/// @return 包装好的已连接客户端 FileDescriptor。
/// @throws std::runtime_error 超时或非可重试的系统错误时抛出。
FileDescriptor ConnectUnixSocketWithRetry(
    const std::string& path,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds retry_interval) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  sockaddr_un address = MakeAddress(path);

  while (std::chrono::steady_clock::now() < deadline) {
    // 每次重试都重新创建 socket，避免 SO_ERROR 缓存问题
    FileDescriptor client(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!client.valid()) {
      throw ErrnoError("socket");
    }

    if (::connect(client.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return client;  ///< 连接成功。
    }

    // 仅对可恢复错误重试
    if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN) {
      throw ErrnoError("connect " + path);
    }
    std::this_thread::sleep_for(retry_interval);
  }

  throw std::runtime_error("timed out connecting unix socket: " + path);
}

/// 阻塞式写入全部数据，处理部分写入和 EINTR。
/// @param fd         目标文件描述符。
/// @param data       待写入数据缓冲区。
/// @param byte_count 待写入字节数。
/// @throws std::runtime_error 写入失败时抛出。
void WriteAll(int fd, const uint8_t* data, size_t byte_count) {
  size_t written = 0;  ///< 已写入字节数。
  while (written < byte_count) {
    const ssize_t result =
        ::send(fd, data + written, byte_count - written, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;  ///< 被信号中断，重试。
      }
      throw ErrnoError("send");
    }
    if (result == 0) {
      throw std::runtime_error("send returned 0 bytes");
    }
    written += static_cast<size_t>(result);
  }
}

/// 阻塞式读取指定字节数，对端关闭时返回已读字节数（可能小于请求值）。
/// @param fd         源文件描述符。
/// @param data       接收缓冲区。
/// @param byte_count 期望读取的字节数。
/// @return 实际读取的字节数（对端正常关闭时可能小于请求值）。
/// @throws std::runtime_error 读取异常时抛出。
size_t ReadFrameOrEof(int fd, uint8_t* data, size_t byte_count) {
  size_t total = 0;  ///< 已读取字节数。
  while (total < byte_count) {
    const ssize_t result = ::read(fd, data + total, byte_count - total);
    if (result < 0) {
      if (errno == EINTR) {
        continue;  ///< 被信号中断，重试。
      }
      throw ErrnoError("read");
    }
    if (result == 0) {
      return total;  ///< 对端正常关闭。
    }
    total += static_cast<size_t>(result);
  }
  return total;
}

}  // namespace audio_processing_module