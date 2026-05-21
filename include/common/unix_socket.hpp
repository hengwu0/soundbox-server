#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace audio_processing_module {

class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor();

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept;
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void reset(int fd = -1);

 private:
  int fd_ = -1;
};

class SocketPathGuard {
 public:
  explicit SocketPathGuard(std::string path) : path_(std::move(path)) {}
  ~SocketPathGuard();

  SocketPathGuard(const SocketPathGuard&) = delete;
  SocketPathGuard& operator=(const SocketPathGuard&) = delete;

 private:
  std::string path_;
};

FileDescriptor CreateUnixServerSocket(const std::string& path, int backlog);

FileDescriptor AcceptUnixClientWithTimeout(
    int server_fd,
    std::chrono::milliseconds timeout);

FileDescriptor ConnectUnixSocketWithRetry(
    const std::string& path,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds retry_interval);

void WriteAll(int fd, const uint8_t* data, size_t byte_count);

size_t ReadFrameOrEof(int fd, uint8_t* data, size_t byte_count);

}  // namespace audio_processing_module
