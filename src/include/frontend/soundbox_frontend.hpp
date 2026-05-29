#pragma once

#include "config/config.hpp"
#include "common/unix_socket.hpp"
#include "frontend/soundbox/client.hpp"
#include "llm/llm_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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

// Frontend 主控类，管理初始化 → 空闲 → 会话启动 → 会话进行 → 会话停止的完整生命周期。
//
// 状态机流程：
//   kSessionInit（初始/重连状态：准备建立 WebSocket 音频链路）
//     → kSessionStopped（空闲状态：WebSocket 链路就绪，等待唤醒）
//     → kSessionStarting（会话启动中：KWS 命中，正在通知 SoundBox 启用 LLM 通道）
//     → kSessionStarted（会话进行中：LLM 已启动，正在进行 AEC 和音频转发）
//     → kSessionStopping（会话停止中：收到 session_stop，正在通知 SoundBox 关闭 LLM 通道）
//     → kSessionStopped（回到空闲状态，等待下一次唤醒）
//   任意状态遇到 AEC/KWS socket 断开或 WS 断开时回到 kSessionInit，重连 ws 音频链路。
//
// 控制面状态机采用单线程事件队列串行处理；外部回调只能 PostEvent，不能直接修改 state_。
// 高频音频数据不进入事件队列，只读取状态机维护的 atomic 门控开关。
class Frontend {
 public:
  // 前端运行状态枚举
  enum class State {
    kSessionInit,     // 初始/重连状态：准备建立 WebSocket 音频链路，所有音频丢弃
    kSessionStopped,      // 空闲状态：WebSocket 链路就绪，等待唤醒
    kSessionStarting, // 会话启动中：KWS 命中后正在通知 SoundBox 启用 LLM 通道
    kSessionStarted,  // 会话进行中：LLM 通道已启用，AEC 处理并转发音频到 LLM
    kSessionStopping, // 会话停止中：收到 session_stop 后正在通知 SoundBox 关闭 LLM 通道
  };

  // 前端初始化选项
  struct Options {
    std::string kws_socket_path;       // KWS 控制/音频 Unix socket 路径
    std::string aec_socket_path;       // AEC 音频 Unix socket 路径
    xiaoai_server::config::Config soundbox_config;  // SoundBox 客户端连接配置
    std::shared_ptr<soundbox_server::llm::LlmClient> llm_client;  // LLM 客户端实例（可为空）
  };

  // 构造函数：传入 Options 初始化前端各组件，校验 socket 路径非空
  explicit Frontend(Options options);
  // 析构函数：确保 Stop() 被调用以释放资源并安全退出
  ~Frontend();

  // 获取当前状态快照（仅用于外部观察/测试，不参与业务决策）
  State state() const;

  // 启动前端主循环：连接 KWS/AEC socket、初始化 SoundBox 和 xiaozhi 客户端、
  // 启动 KWS 控制读取线程，阻塞直到 Stop() 被调用
  void Run();
  // 停止前端：设置停止标志、通知条件变量、断开所有 socket 连接、停止子客户端、
  // join 子线程，可安全重复调用
  void Stop();

 private:
  // 唤醒音频回调：将 KWS 音频数据写入 KWS socket 供唤醒词引擎处理
  // chunk：音频数据块（PCM 格式）
  // 仅在 kSessionStopped 状态下写入，其他状态丢弃该音频块
  void OnWakeupAudio(const std::vector<uint8_t>& chunk);
  // AEC 音频回调：将话筒/喇叭音频数据写入 AEC socket 供回声消除引擎处理
  // chunk：音频数据块（PCM 格式）
  // 仅在 kSessionStarted 状态下写入，其他状态丢弃该音频块
  void OnAecAudio(const std::vector<uint8_t>& chunk);
  // AEC 处理后音频回调：将回声消除后的 PCM 数据发送给 LLM 客户端
  // processed_pcm：回声消除处理后的音频数据
  void OnAecProcessedAudio(const std::vector<uint8_t>& processed_pcm);
  // LLM 会话结束回调：由 LLM 客户端在会话结束时调用（如 VAD 静音超时）
  // reason：会话结束原因字符串，由 LLM 客户端提供
  // 投递给 Frontend 事件队列串行处理
  void OnLlmSessionEnd(const std::string& reason);
  // SoundBox 设备自身 KWS 唤醒事件回调：投递给 Frontend 事件队列串行处理。
  void OnSoundboxNativeKws();
  // KWS 控制通道读取线程：循环读取 session_start 消息并投递 Frontend 控制事件
  void KwsControlReaderLoop();

  // Frontend 控制面事件类型：所有会改变状态机的外部信号都必须先进入事件队列。
  enum class EventType {
    kFrontendReady,             // Run() 初始化完成，进入空闲态
    kLocalKwsHit,               // soundbox-server 本地 KWS 命中
    kXiaozhiSessionEnd,         // xiaozhi 命令通道收到 session_end
    kSoundboxNativeKws,         // SoundBox 设备原生 KWS 唤醒事件
    kSoundboxDisconnected,      // SoundBox WS 断开，需要重连
    kKwsControlDisconnected,    // KWS 控制 socket 断开，需要重连
    kReconnectRequired,         // Frontend 内部错误触发重连
  };

  // Frontend 控制面事件载荷。
  struct Event {
    EventType type{};
    std::string reason{};
    std::string source{};
    std::optional<double> score{};
    std::optional<int64_t> timestamp_ms{};
  };

  // 启停 Frontend 控制面事件循环线程。
  void StartEventLoop();
  void StopEventLoop();
  // 投递控制面事件。该方法可由任意回调线程调用，但不会直接修改 state_。
  void PostEvent(Event event);
  // 控制面事件循环：唯一允许执行状态机流转的线程。
  void EventLoop();
  // 分发单个控制面事件。
  void DispatchEvent(const Event& event);
  // 处理初始化完成事件：进入 kSessionStopped 空闲态。
  void HandleFrontendReady(const Event& event);
  // 处理 SoundBox WS / KWS socket 断开事件。
  void HandleReconnectRequired(const Event& event);
  // 处理 session_start 消息：仅在 kSessionStopped 状态下有效
  // event：本地 KWS session_start 控制事件
  // 成功后进入 kSessionStarting → kSessionStarted，失败则回到 kSessionStopped 继续等待唤醒
  void HandleSessionStart(const Event& event);
  // 处理 session_stop 消息：仅在 kSessionStarted 状态下有效，其他状态输出警告并忽略
  // event：session 结束事件
  // 进入 kSessionStopping，通知下游停止 LLM，最终回到 kSessionStopped 空闲状态
  void HandleSessionStop(const Event& event);
  // SoundBox 原生 KWS 打断当前会话：先向 xiaozhi 主动发送 session_end，再走本地 session_stop。
  void HandleSoundboxNativeKws(const Event& event);
  void StopSessionBySoundboxNativeKws(const Event& event);
  // 完成已经进入 kSessionStopping 后的本地 session_stop 清理。
  void FinishSessionStop(const char* reason);
  // 进入初始重连状态：设置 kSessionInit 状态、断开当前链路、
  // 尝试重连 WebSocket 音频链路。重连成功后回到 kSessionStopped 空闲状态。
  // reason：断连原因描述，用于日志诊断
  void EnterSessionInitAndRestart(const char* reason);
  // 设置状态并记录日志，相同状态不重复设置。只能在 Frontend 事件线程内调用。
  // next：目标状态
  // reason：状态变更原因描述
  void SetState(State next, const char* reason);
  // 检查是否已收到停止请求（无锁读取原子变量）
  bool IsStopping() const;
  // 重连 WebSocket 音频链路：重新启动 SoundBox 客户端并恢复远端音频
  bool RestartSoundboxAudioLink();

  Options options_;  // 前端配置选项副本
  std::unique_ptr<xiaoai_server::soundbox::SoundBoxClient> client_;  // SoundBox 客户端实例
  std::shared_ptr<soundbox_server::llm::LlmClient> llm_client_;      // LLM 客户端实例
  audio_processing_module::FileDescriptor kws_socket_;   // KWS 控制和音频 Unix socket
  audio_processing_module::FileDescriptor aec_socket_;   // AEC 音频 Unix socket
  std::thread kws_control_thread_;   // KWS 控制消息读取线程
  std::thread event_thread_;         // Frontend 控制面事件循环线程
  mutable std::mutex mu_;            // 保护停止条件变量
  std::condition_variable stop_cv_;  // 停止条件变量，用于阻塞 Run() 主循环等待停止信号
  std::mutex event_mu_;              // 保护 Frontend 控制面事件队列
  std::condition_variable event_cv_; // Frontend 控制面事件队列条件变量
  std::deque<Event> event_queue_;    // Frontend 控制面事件队列
  bool event_loop_running_{false};   // Frontend 控制面事件循环是否运行
  State state_{State::kSessionInit};  // 当前运行状态，默认为 kSessionInit 初始/重连状态
  std::atomic<State> state_snapshot_{State::kSessionInit};  // 供外部只读观察的状态快照
  std::atomic<bool> kws_uplink_enabled_{false};      // 数据面门控：是否允许向 KWS socket 写入唤醒音频
  std::atomic<bool> aec_uplink_enabled_{false};      // 数据面门控：是否允许向 AEC socket 写入会话音频
  std::atomic<bool> llm_uplink_enabled_{false};      // 数据面门控：是否允许向 xiaozhi 音频通道上行 PCM
  std::atomic<bool> playback_enabled_{false};        // 数据面门控：是否允许播放 xiaozhi 下行 PCM
  std::atomic<uint64_t> soundbox_generation_{0};      // SoundBox 客户端代际，用于丢弃旧连接的异步回调
  std::atomic<uint64_t> kws_generation_{0};           // KWS 控制 socket 代际，用于丢弃旧连接的异步事件
  std::atomic<bool> stop_requested_{false};  // 停止请求标志，原子变量保证多线程可见性
};

// 将 State 枚举值转换为可读的字符串名称，用于日志输出和调试
const char* StateName(Frontend::State state);

}  // namespace soundbox_server::frontend