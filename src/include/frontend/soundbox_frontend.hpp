#pragma once

#include "config/config.hpp"
#include "common/unix_socket.hpp"
#include "frontend/soundbox/client.hpp"
#include "llm/llm_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace soundbox_server::frontend {

// 控制消息类型枚举，目前仅支持 KWS 唤醒词命中后发起的 session_start
enum class ControlMessageType {
  kSessionStart,  // KWS 唤醒词命中，发起 LLM/AEC 会话
};

// 从 KWS 控制通道接收的控制消息结构体
struct ControlMessage {
  ControlMessageType type;            // 消息类型
  std::string reason;                 // 触发原因（如 "kws_hit" 表示唤醒词命中）
  std::optional<double> score;        // 可选：KWS 识别置信度分数
  std::optional<int64_t> timestamp_ms; // 可选：事件时间戳（毫秒）
};

// 解析控制消息行，校验类型匹配，返回解析后的 ControlMessage
// line：从 socket 读取的一行 JSON 字符串
// expected_type：期望的消息类型，用于校验
// 返回：解析并校验通过的 ControlMessage
// 异常：JSON 解析失败、类型不匹配、必填字段缺失时抛出 std::runtime_error
ControlMessage ParseControlMessageLine(const std::string& line,
                                        ControlMessageType expected_type);

// Frontend 主控类，管理唤醒词检测 → LLM 会话 → AEC 音频处理的完整生命周期。
//
// 状态机流程：
//   kSessionEnd（默认空闲，无活跃会话）
//     → kKws（启动唤醒词检测，等待 KWS 命中）
//     → kLlmStarting（收到 session_start，通知下游启动 LLM）
//     → kAec（LLM 已启动，正在进行回声消除和音频转发）
//     → kLlmStopping（收到 session_end，通知下游停止 LLM）
//     → kSessionEnd（回到空闲状态，等待下一次唤醒）
//   kFault 为终端故障状态，任何环节遇到不可恢复错误时可进入。
class Frontend {
 public:
  // 前端运行状态枚举
  enum class State {
    kSessionEnd,    // 默认空闲状态：无活跃的 LLM/AEC 会话，等待唤醒
    kKws,           // 唤醒词检测状态：正在监听 KWS 音频流，等待唤醒词命中
    kLlmStarting,   // LLM 启动中：已收到 session_start，正在通知 SoundBox 启用 LLM 通道
    kAec,           // AEC 音频处理中：LLM 已启动，正在进行回声消除并转发音频到 LLM
    kLlmStopping,   // LLM 停止中：收到 session_end，正在通知 SoundBox 关闭 LLM 通道
    kFault,         // 故障终端状态：遇到不可恢复错误，前端需停止并退出
  };

  // 前端初始化选项
  struct Options {
    std::string kws_socket_path;       // KWS 控制/音频 Unix socket 路径
    std::string aec_socket_path;       // AEC 音频 Unix socket 路径
    std::string playback_socket_path;  // 播放 PCM 监听 socket 路径（用于流式播放输出）
    xiaoai_server::config::Config soundbox_config;  // SoundBox 客户端连接配置
    std::shared_ptr<soundbox_server::llm::LlmClient> llm_client;  // LLM 客户端实例（可为空）
    size_t playback_read_chunk_bytes{4096};  // 播放流每次读取的缓冲区大小（字节）
    std::function<ssize_t(int, uint8_t*, size_t)> playback_read;  // 可选的播放读取函数，默认使用 ::read
  };

  // 构造函数：传入 Options 初始化前端各组件，校验 socket 路径非空
  explicit Frontend(Options options);
  // 析构函数：确保 Stop() 被调用以释放资源并安全退出
  ~Frontend();

  // 获取当前状态（线程安全，加锁读取）
  State state() const;

  // 启动前端主循环：连接 KWS/AEC socket、初始化 SoundBox 和 LLM 客户端、
  // 启动 KWS 控制读取线程和播放接受线程，阻塞直到 Stop() 被调用
  void Run();
  // 停止前端：设置停止标志、通知条件变量、断开所有 socket 连接、停止子客户端、
  // join 子线程，可安全重复调用
  void Stop();

 private:
  // 唤醒音频回调：将 KWS 音频数据写入 KWS socket 供唤醒词引擎处理
  // chunk：音频数据块（PCM 格式）
  // 仅在 kKws 状态下写入，其他状态丢弃该音频块
  void OnWakeupAudio(const std::vector<uint8_t>& chunk);
  // AEC 音频回调：将话筒/喇叭音频数据写入 AEC socket 供回声消除引擎处理
  // chunk：音频数据块（PCM 格式）
  // 仅在 kAec 状态下写入，其他状态丢弃该音频块
  void OnAecAudio(const std::vector<uint8_t>& chunk);
  // AEC 处理后音频回调：将回声消除后的 PCM 数据发送给 LLM 客户端
  // processed_pcm：回声消除处理后的音频数据
  void OnAecProcessedAudio(const std::vector<uint8_t>& processed_pcm);
  // LLM 会话结束回调：由 LLM 客户端在会话结束时调用（如 VAD 静音超时）
  // reason：会话结束原因字符串，由 LLM 客户端提供
  // 转发给 HandleSessionEnd 执行状态转换逻辑
  void OnLlmSessionEnd(const std::string& reason);
  // KWS 控制通道读取线程：循环读取 session_start 消息并分发给 HandleSessionStart
  void KwsControlReaderLoop();
  // 播放接受线程：创建 playback Unix socket 监听，循环接受播放客户端连接
  void PlaybackAcceptLoop();
  // 播放客户端处理循环：从客户端 fd 读取 PCM 数据并写入 SoundBox 播放队列
  // client_fd：已接受的播放客户端 socket 文件描述符
  void PlaybackClientLoop(int client_fd);
  // 处理 session_start 消息：仅在 kKws 状态下有效
  // message：已解析的 session_start 控制消息
  // 成功后进入 kLlmStarting → kAec，失败则回到 kKws 继续等待唤醒
  void HandleSessionStart(const ControlMessage& message);
  // 处理 session_end 消息：仅在 kAec 状态下有效，其他状态输出警告并忽略
  // reason：session 结束原因字符串
  // 进入 kLlmStopping，通知下游停止 LLM，最终回到 kSessionEnd 空闲状态
  void HandleSessionEnd(const std::string& reason);
  // 进入故障状态并停止所有组件：设置 kFault 状态、关闭所有 socket、
  // 断开 LLM 和 SoundBox 客户端、通知条件变量唤醒主线程
  // reason：故障原因描述
  void EnterFaultAndStop(const char* reason);
  // 线程安全地设置状态并记录日志，相同状态不重复设置
  // next：目标状态
  // reason：状态变更原因描述
  void SetState(State next, const char* reason);
  // 检查是否已收到停止请求（无锁读取原子变量）
  bool IsStopping() const;

  Options options_;  // 前端配置选项副本
  std::unique_ptr<xiaoai_server::soundbox::SoundBoxClient> client_;  // SoundBox 客户端实例
  std::shared_ptr<soundbox_server::llm::LlmClient> llm_client_;      // LLM 客户端实例
  audio_processing_module::FileDescriptor kws_socket_;   // KWS 控制和音频 Unix socket
  audio_processing_module::FileDescriptor aec_socket_;   // AEC 音频 Unix socket
  std::thread kws_control_thread_;   // KWS 控制消息读取线程
  std::thread playback_thread_;      // 播放接受线程
  mutable std::mutex mu_;            // 保护 state_ 和条件变量的互斥锁
  std::condition_variable stop_cv_;  // 停止条件变量，用于阻塞 Run() 主循环等待停止信号
  State state_{State::kSessionEnd};  // 当前运行状态，默认为 kSessionEnd 空闲状态
  std::atomic<bool> stop_requested_{false};  // 停止请求标志，原子变量保证多线程可见性
};

// 将 State 枚举值转换为可读的字符串名称，用于日志输出和调试
const char* StateName(Frontend::State state);

}  // namespace soundbox_server::frontend