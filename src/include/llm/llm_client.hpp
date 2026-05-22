#pragma once

#include "common/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace soundbox_server::llm {

/**
 * @brief LLM 客户端配置选项结构体
 *
 * 封装了连接 LLM 服务器的网络参数。
 */
struct LlmClientOptions {
  std::string host{"127.0.0.1"};  /**< LLM 服务器主机地址，默认为本地回环地址 */
  int port{7799};                  /**< LLM 服务器端口号，默认 7799 */
  size_t read_chunk_bytes{4096};   /**< 读取响应时每次读取的字节数，默认 4096 */
};

/**
 * @brief LLM 客户端类
 *
 * 负责与 LLM 后端服务建立 TCP 连接，发送音频数据，并异步接收
 * 服务端的 JSON 响应消息。支持断线重连与会话结束回调通知。
 */
class LlmClient {
 public:
  /** @brief 会话结束回调函数类型，参数为结束原因字符串 */
  using OnSessionEndCallback = std::function<void(const std::string& reason)>;

  /**
   * @brief 构造 LLM 客户端
   * @param options 连接配置选项
   * @param on_session_end 会话结束回调，当服务端通知会话终止时触发
   */
  LlmClient(LlmClientOptions options, OnSessionEndCallback on_session_end);

  /** @brief 析构函数，自动停止客户端并释放资源 */
  ~LlmClient();

  /**
   * @brief 查询当前是否已连接到 LLM 服务器
   * @return true 表示连接正常，false 表示未连接或已断开
   */
  bool connected() const;

  /**
   * @brief 发起与 LLM 服务器的 TCP 连接
   * @return true 表示连接成功
   * @throws std::runtime_error 当连接超时或发生致命错误时抛出异常
   *
   * 采用非阻塞 socket + poll 方式尝试连接，在总超时时间内支持重试。
   */
  bool Connect();

  /**
   * @brief 主动断开与 LLM 服务器的连接
   *
   * 关闭 TCP socket 并将连接状态标记为已断开。
   */
  void Disconnect();

  /**
   * @brief 发送音频数据块到 LLM 服务器
   * @param chunk 待发送的音频数据（PCM 等原始格式）
   *
   * 如果当前未连接或连接已断开则静默返回。
   * 当检测到管道破裂、连接重置等错误时会自动断开连接。
   */
  void SendAudio(const std::vector<uint8_t>& chunk);

  /**
   * @brief 设置会话结束回调函数
   * @param callback 新的会话结束回调，用于替换构造时传入的回调
   *
   * 该回调在收到服务端的 session_end 消息时被触发。
   */
  void set_session_end_callback(OnSessionEndCallback callback);

  /**
   * @brief 停止客户端并等待后台线程退出
   *
   * 设置停止标志、断开连接、join 响应读取线程。
   */
  void Stop();

 private:
  /**
   * @brief 响应读取循环（运行于独立线程）
   *
   * 持续从 TCP socket 逐行读取 JSON 响应，解析其中的消息类型。
   * 当收到 "session_end" 类型消息时调用注册的回调通知上层。
   * 遇到连接断开或致命读错误时退出循环。
   */
  void ResponseReaderLoop();

  /**
   * @brief 尝试重连到 LLM 服务器
   * @return true 表示重连成功，false 表示重连失败
   */
  bool TryReconnect();

  LlmClientOptions options_;               /**< 连接配置选项 */
  OnSessionEndCallback on_session_end_;    /**< 会话结束回调函数对象 */

  int tcp_fd_{-1};                         /**< TCP socket 文件描述符，-1 表示未建立连接 */
  std::thread response_thread_;            /**< 响应读取后台线程 */
  std::atomic<bool> stop_requested_{false}; /**< 停止标志，用于通知后台线程退出 */
  std::atomic<bool> connected_{false};      /**< 连接状态标志，原子操作保证线程安全 */
};

}  // namespace soundbox_server::llm