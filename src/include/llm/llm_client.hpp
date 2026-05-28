#pragma once

#include "common/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace soundbox_server::llm {

/**
 * @brief xiaozhi TCP 协议客户端配置。
 *
 * 音频通道连接 xiaozhi 的 TCP Server 7799：上行 16kHz PCM，下行 24kHz PCM。
 * 命令通道连接 xiaozhi 的 TCP Server 7789：JSON line 控制消息。
 */
struct LlmClientOptions {
  std::string host{"127.0.0.1"};          /**< xiaozhi 音频通道主机地址 */
  int port{7799};                          /**< xiaozhi 音频通道端口，默认 7799 */
  std::string command_host{"127.0.0.1"};  /**< xiaozhi 命令通道主机地址 */
  int command_port{7789};                  /**< xiaozhi 命令通道端口，默认 7789 */
  size_t read_chunk_bytes{4096};           /**< 下行 PCM 每次读取的缓冲区大小 */
};

/**
 * @brief xiaozhi TCP 协议客户端。
 *
 * 负责同时连接 xiaozhi 的音频通道和命令通道：
 * - SendAudio() 将 AEC 后的 1ch/S16_LE/16kHz PCM 写入音频通道；
 * - AudioReaderLoop() 从音频通道读取 1ch/S16_LE/24kHz PCM 并回调给 playback；
 * - SendSessionStart() 将 KWS 命中的 session_start JSON line 写入命令通道；
 * - SendSessionEnd() 将本地中断产生的 session_end JSON line 写入命令通道；
 * - CommandReaderLoop() 从命令通道读取 session_end JSON line 并通知上层状态机。
 */
class LlmClient {
 public:
  /** @brief 会话结束回调函数类型，参数为结束原因字符串 */
  using OnSessionEndCallback = std::function<void(const std::string& reason)>;
  /** @brief 下行播放音频回调，参数为 1ch/S16_LE/24kHz PCM */
  using OnPlaybackAudioCallback = std::function<void(const std::vector<uint8_t>& chunk)>;

  /**
   * @brief 构造 xiaozhi TCP 客户端
   * @param options 连接配置选项
   * @param on_session_end 收到 session_end 时触发的回调
   * @param on_playback_audio 收到下行播放 PCM 时触发的回调，可为空
   */
  LlmClient(LlmClientOptions options,
            OnSessionEndCallback on_session_end,
            OnPlaybackAudioCallback on_playback_audio = {});

  /** @brief 析构函数，自动停止客户端并释放资源 */
  ~LlmClient();

  /**
   * @brief 查询音频通道与命令通道是否均已连接
   * @return true 表示两个 TCP 通道均连接正常
   */
  bool connected() const;

  /**
   * @brief 发起与 xiaozhi 音频/命令 TCP Server 的连接
   * @return true 表示连接成功
   * @throws std::runtime_error 当连接超时或发生致命错误时抛出
   */
  bool Connect();

  /** @brief 主动断开两个 TCP 通道 */
  void Disconnect();

  /**
   * @brief 发送上行音频到 xiaozhi 音频通道
   * @param chunk 1ch/S16_LE/16kHz PCM
   */
  void SendAudio(const std::vector<uint8_t>& chunk);

  /**
   * @brief 发送 session_start JSON line 到 xiaozhi 命令通道
   * @param reason 触发原因，KWS 命中时为 kws_hit
   * @param score 可选 KWS 置信度
   * @param timestamp_ms 可选事件时间戳
   * @return 发送成功返回 true，未连接或写入失败返回 false
   */
  bool SendSessionStart(const std::string& reason,
                        std::optional<double> score = std::nullopt,
                        std::optional<int64_t> timestamp_ms = std::nullopt);

  /**
   * @brief 发送 session_end JSON line 到 xiaozhi 命令通道
   * @param reason 结束原因，soundbox 原生 KWS 打断时为 soundbox_native_kws
   * @param source 结束来源，soundbox 原生 KWS 打断时为 soundbox
   * @return 发送成功返回 true，未连接或写入失败返回 false
   */
  bool SendSessionEnd(const std::string& reason, const std::string& source);

  /** @brief 设置会话结束回调函数 */
  void set_session_end_callback(OnSessionEndCallback callback);

  /** @brief 设置下行播放 PCM 回调函数 */
  void set_playback_audio_callback(OnPlaybackAudioCallback callback);

  /** @brief 停止客户端并等待后台线程退出 */
  void Stop();

 private:
  /** @brief 下行 PCM 读取循环（运行于独立线程） */
  void AudioReaderLoop();

  /** @brief 命令 JSON line 读取循环（运行于独立线程） */
  void CommandReaderLoop();

  /** @brief 向指定 socket 完整发送数据 */
  bool SendAllLocked(int fd, const uint8_t* data, size_t byte_count, const char* channel_name);

  LlmClientOptions options_;                            /**< 连接配置选项 */
  OnSessionEndCallback on_session_end_;                 /**< 会话结束回调函数对象 */
  OnPlaybackAudioCallback on_playback_audio_;           /**< 下行 PCM 回调函数对象 */

  int audio_fd_{-1};                                    /**< 音频通道 TCP socket */
  int command_fd_{-1};                                  /**< 命令通道 TCP socket */
  std::thread audio_thread_;                            /**< 音频读取后台线程 */
  std::thread command_thread_;                          /**< 命令读取后台线程 */
  mutable std::mutex audio_mu_;                         /**< 保护音频通道写入和回调 */
  mutable std::mutex command_mu_;                       /**< 保护命令通道写入 */
  mutable std::mutex callback_mu_;                      /**< 保护回调替换 */
  std::atomic<bool> stop_requested_{false};             /**< 停止标志 */
  std::atomic<bool> connected_{false};                  /**< 两个通道均连接成功时为 true */
};

}  // namespace soundbox_server::llm
