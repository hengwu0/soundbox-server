#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace audio_processing_module {

/// RAII 封装的 POSIX 文件描述符，析构时自动关闭。
class FileDescriptor {
 public:
  FileDescriptor() = default;

  /// 从已有 fd 构造，接管所有权。
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor();

  /// 禁止拷贝，避免 fd 重复关闭。
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  /// 移动构造，接管源对象的 fd。
  FileDescriptor(FileDescriptor&& other) noexcept;
  /// 移动赋值，先释放自身 fd 再接管源对象的 fd。
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  /// 获取原始文件描述符值。
  int get() const { return fd_; }
  /// 判断 fd 是否有效（>= 0）。
  bool valid() const { return fd_ >= 0; }
  /// 重置文件描述符，先关闭当前 fd（若有效），再设置为新值。
  void reset(int fd = -1);

 private:
  int fd_ = -1;  ///< 原始文件描述符，-1 表示无效。
};

/// RAII 守卫：构造时记录 socket 文件路径，析构时调用 unlink 删除。
class SocketPathGuard {
 public:
  explicit SocketPathGuard(std::string path) : path_(std::move(path)) {}
  ~SocketPathGuard();

  /// 禁止拷贝。
  SocketPathGuard(const SocketPathGuard&) = delete;
  SocketPathGuard& operator=(const SocketPathGuard&) = delete;

 private:
  std::string path_;  ///< 需要自动清理的 Unix Socket 文件路径。
};

/// 创建 Unix Domain Socket 服务端，执行 socket -> bind -> listen。
/// @param path    socket 文件存放路径。
/// @param backlog listen 队列最大长度。
/// @return 包装好的服务端文件描述符。
/// @throws std::runtime_error 任一系统调用失败时抛出。
FileDescriptor CreateUnixServerSocket(const std::string& path, int backlog);

/// 带超时的 accept：在指定时间内等待客户端连接。
/// @param server_fd 已 listen 的 socket 描述符。
/// @param timeout   最大等待时间。
/// @return 包装好的客户端文件描述符。
/// @throws std::runtime_error 超时或出错时抛出。
FileDescriptor AcceptUnixClientWithTimeout(
    int server_fd,
    std::chrono::milliseconds timeout);

/// 循环尝试连接 Unix Socket 服务端，直到成功或超时。
/// 服务端尚未就绪时以固定间隔重试，适用于前端等待后端就绪的场景。
/// @param path           socket 文件路径。
/// @param timeout        总超时时间。
/// @param retry_interval 每次重试的间隔。
/// @return 包装好的已连接客户端文件描述符。
/// @throws std::runtime_error 超时或不可恢复错误时抛出。
FileDescriptor ConnectUnixSocketWithRetry(
    const std::string& path,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds retry_interval);

/// 阻塞式写入全部数据，处理部分写入和 EINTR 信号中断。
/// @param fd         目标文件描述符。
/// @param data       待发送数据缓冲区。
/// @param byte_count 待发送字节数。
/// @throws std::runtime_error 写入失败时抛出。
void WriteAll(int fd, const uint8_t* data, size_t byte_count);

/// 阻塞式读取指定字节数，对端正常关闭时返回已读字节数（可能小于请求值）。
/// @param fd         源文件描述符。
/// @param data       接收缓冲区。
/// @param byte_count 期望读取的字节数。
/// @return 实际读取的字节数。
/// @throws std::runtime_error 读取异常时抛出。
size_t ReadFrameOrEof(int fd, uint8_t* data, size_t byte_count);

/// 创建 TCP 服务端 socket，执行 socket -> bind -> listen。
/// @param host    监听 IP 地址（如 "127.0.0.1"）。
/// @param port    监听端口。
/// @param backlog listen 队列最大长度。
/// @return 包装好的服务端文件描述符。
/// @throws std::runtime_error 任一系统调用失败时抛出。
FileDescriptor CreateTcpServerSocket(const std::string& host, int port, int backlog);

/// 带超时的 accept：在指定时间内等待 TCP 客户端连接。
/// @param server_fd 已 listen 的 socket 描述符。
/// @param timeout   最大等待时间。
/// @return 包装好的客户端文件描述符。
/// @throws std::runtime_error 超时或出错时抛出。
FileDescriptor AcceptTcpClientWithTimeout(
    int server_fd,
    std::chrono::milliseconds timeout);

/// 循环尝试连接 TCP 服务端，直到成功或超时。
/// @param host           服务端 IP 地址。
/// @param port           服务端端口。
/// @param timeout        总超时时间。
/// @param retry_interval 每次重试的间隔。
/// @return 包装好的已连接客户端文件描述符。
/// @throws std::runtime_error 超时或不可恢复错误时抛出。
FileDescriptor ConnectTcpWithRetry(
    const std::string& host,
    int port,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds retry_interval);

}  // namespace audio_processing_module