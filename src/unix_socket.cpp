#include "audio_processing_module/unix_socket.hpp"

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

std::runtime_error ErrnoError(const std::string& action) {
  return std::runtime_error(action + ": " + std::strerror(errno));
}

sockaddr_un MakeAddress(const std::string& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("unix socket path is too long: " + path);
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  return address;
}

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

FileDescriptor::~FileDescriptor() {
  reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
  other.fd_ = -1;
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    reset(other.fd_);
    other.fd_ = -1;
  }
  return *this;
}

void FileDescriptor::reset(int fd) {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = fd;
}

SocketPathGuard::~SocketPathGuard() {
  if (!path_.empty()) {
    ::unlink(path_.c_str());
  }
}

FileDescriptor CreateUnixServerSocket(const std::string& path, int backlog) {
  const std::filesystem::path socket_path(path);
  if (socket_path.has_parent_path()) {
    std::filesystem::create_directories(socket_path.parent_path());
  }

  ::unlink(path.c_str());

  FileDescriptor server(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!server.valid()) {
    throw ErrnoError("socket");
  }

  sockaddr_un address = MakeAddress(path);
  if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw ErrnoError("bind " + path);
  }

  if (::listen(server.get(), backlog) < 0) {
    throw ErrnoError("listen " + path);
  }

  return server;
}

FileDescriptor AcceptUnixClientWithTimeout(
    int server_fd,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (true) {
    pollfd descriptor{};
    descriptor.fd = server_fd;
    descriptor.events = POLLIN;

    const int poll_timeout = RemainingMilliseconds(deadline);
    if (poll_timeout == 0) {
      throw std::runtime_error("timed out waiting for unix socket client");
    }

    const int ready = ::poll(&descriptor, 1, poll_timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ErrnoError("poll accept");
    }
    if (ready == 0) {
      throw std::runtime_error("timed out waiting for unix socket client");
    }

    const int client_fd = ::accept4(server_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ErrnoError("accept");
    }
    return FileDescriptor(client_fd);
  }
}

FileDescriptor ConnectUnixSocketWithRetry(
    const std::string& path,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds retry_interval) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  sockaddr_un address = MakeAddress(path);

  while (std::chrono::steady_clock::now() < deadline) {
    FileDescriptor client(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!client.valid()) {
      throw ErrnoError("socket");
    }

    if (::connect(client.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return client;
    }

    if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN) {
      throw ErrnoError("connect " + path);
    }
    std::this_thread::sleep_for(retry_interval);
  }

  throw std::runtime_error("timed out connecting unix socket: " + path);
}

void WriteAll(int fd, const uint8_t* data, size_t byte_count) {
  size_t written = 0;
  while (written < byte_count) {
    const ssize_t result =
        ::send(fd, data + written, byte_count - written, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ErrnoError("send");
    }
    if (result == 0) {
      throw std::runtime_error("send returned 0 bytes");
    }
    written += static_cast<size_t>(result);
  }
}

size_t ReadFrameOrEof(int fd, uint8_t* data, size_t byte_count) {
  size_t total = 0;
  while (total < byte_count) {
    const ssize_t result = ::read(fd, data + total, byte_count - total);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw ErrnoError("read");
    }
    if (result == 0) {
      return total;
    }
    total += static_cast<size_t>(result);
  }
  return total;
}

}  // namespace audio_processing_module
