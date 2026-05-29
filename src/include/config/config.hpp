#pragma once

#include <string>
#include <vector>

namespace xiaoai_server::config {

// open-xiaoai-client fast_recording 在唤醒前用于 KWS 的固定输出格式。
inline constexpr int kFastRecordingOutputSampleRate = 16000;
inline constexpr int kFastRecordingOutputChannels = 1;
inline constexpr int kFastRecordingOutputBitsPerSample = 16;
// open-xiaoai-client 在 llm_start 后切换到的双通道 S16_LE 输出格式。
inline constexpr int kLlmRawSampleRate = 16000;
inline constexpr int kLlmRawChannels = 2;
inline constexpr int kLlmRawBitsPerSample = 16;
// xiaozhi TCP 音频通道上行固定为 1ch/S16_LE/16kHz PCM。
inline constexpr int kXiaozhiUploadSampleRate = 16000;
// xiaozhi TCP 音频通道下行播放固定为 1ch/S16_LE/24kHz PCM。
inline constexpr int kPlaybackSampleRate = 24000;
inline constexpr int kPlaybackChannels = 1;
inline constexpr int kPlaybackBitsPerSample = 16;

// 描述本地播放链路可调参数；采集格式由 open-xiaoai-client 固定，不从配置读取。
struct AudioPreset {
  // playback_gain 是下行播放前的线性增益，1.0 表示不放大也不衰减。
  float playback_gain{1.0f};
};
using Audio = AudioPreset;

// 描述 soundbox 音箱桥接所需的连接参数。
struct SoundBoxPreset {
  // ws_url 指向小爱音箱侧 open-xiaoai-client 暴露的 WebSocket 地址，默认直连本机网段服务。
  std::string ws_url{"ws://192.168.0.50:4399/"};
  // ws_token 是 open-xiaoai-client -l 输出的 Listen code，用作 Bearer token，默认沿用仓库样板值。
  std::string ws_token{"whsn1oeo"};
  // connect_timeout_ms 是连接小爱音箱 WebSocket 的最长等待时间。
  int connect_timeout_ms{10000};
  // llm_start_timeout_ms 是发送 llm_start 后等待 llm_start_ok 的最长时间。
  int llm_start_timeout_ms{1000};
  // llm_stop_timeout_ms 是发送 llm_stop 后等待 llm_stop_ok 的最长时间。
  int llm_stop_timeout_ms{1000};
  // native_kws_triggers 是 SoundBox 原生识别文本可触发 soundbox-server KWS 的后缀词列表。
  std::vector<std::string> native_kws_triggers{"小杜老师", "小度老师"};
};

// 描述 xiaozhi 上游服务连接与音频协商参数。
struct XiaozhiPreset {
  // ota_url 是 xiaozhi OTA/设备信息接口，当前默认使用公共服务地址。
  std::string ota_url{"https://api.tenclass.net/xiaozhi/ota/"};
  // ws_url 是 xiaozhi 对话 WebSocket 地址，默认配置可直接做公共服务握手。
  std::string ws_url{"wss://api.tenclass.net/xiaozhi/v1/"};
  // access_token 是可选上游鉴权 token，公共服务通常留空。
  std::string access_token;
  // verification_code 是首次绑定设备时使用的验证码，服务端可能在交互中更新。
  std::string verification_code{"678327"};
  // device_id 是 xiaozhi 识别设备的 ID；若绑定流程异常可按配置说明清空后重试。
  std::string device_id{"4c:c6:4c:17:2d:9a"};
  // client_id 是客户端实例 ID；留空时 normalize 会自动生成 UUID 形式的值。
  std::string client_id;
  // hello_timeout_ms 是 WebSocket 打开后等待服务端 hello 的最长时间。
  int hello_timeout_ms{10000};
  // session_idle_timeout_ms 是唤醒后无有效语音时主动重建 session 的空闲时间。
  int session_idle_timeout_ms{30000};
  // ping_interval_ms 是后台心跳间隔，默认 10 秒，避免长时间空闲连接被上游关闭。
  int ping_interval_ms{10000};
  // opus_frame_duration_ms 是本地 Opus 编码帧长，也会写入 hello 的 frame_duration。
  int opus_frame_duration_ms{60};
};

// 描述本地 VAD 的阈值、滑动窗口和预卷参数。
struct VadPreset {
  // threshold_high 是从能量概率进入"有人声"状态的高阈值。
  float threshold_high{0.50f};
  // threshold_low 是从能量概率退出"有人声"状态的低阈值。
  float threshold_low{0.30f};
  // rms_floor 是 RMS 映射到概率 0 的下限，用于适配现场底噪。
  float rms_floor{120.0f};
  // rms_full 是 RMS 映射到概率 1 的上限，用于适配近讲音量。
  float rms_full{1800.0f};
  // barge_in_rms 是 AI 播报中允许插话的近端 RMS 下限。
  float barge_in_rms{1100.0f};
  // window_frames 是 VAD 滑动窗口帧数。
  int window_frames{5};
  // trigger_frames 是窗口内达到多少有人声帧才进入稳定语音状态。
  int trigger_frames{3};
  // min_speech_ms 是触发一次语音开始所需的最短持续人声时间。
  int min_speech_ms{180};
  // min_silence_ms 是判定语音结束所需的最短静音时间。
  int min_silence_ms{650};
  // pre_roll_ms 是真正开始上传前保留的预卷音频时长。
  int pre_roll_ms{300};
};

// 描述唤醒词模型与唤醒成功后的提示文案。
struct Wakeup {
  // say_hello 是唤醒成功后通过小爱音箱播报的提示文本。
  std::string say_hello{"在"};
  // keywords_file 是 sherpa-onnx KWS 关键词文件路径。
  std::string keywords_file{"assets/keywords.txt"};
  // tokens_path 是 KWS 模型使用的 token 表路径。
  std::string tokens_path{"assets/tokens.txt"};
  // encoder_path 是 Zipformer KWS encoder ONNX 模型路径。
  std::string encoder_path{"assets/encoder.onnx"};
  // decoder_path 是 Zipformer KWS decoder ONNX 模型路径。
  std::string decoder_path{"assets/decoder.onnx"};
  // joiner_path 是 Zipformer KWS joiner ONNX 模型路径。
  std::string joiner_path{"assets/joiner.onnx"};
  // kws_threshold 是 KWS 解码候选命中的内部阈值。
  float kws_threshold{0.20f};
  // kws_score 是关键词命中分数门限。
  float kws_score{3.0f};
  // kws_num_threads 是 KWS 推理线程数。
  int kws_num_threads{2};
  // kws_max_active_paths 是 KWS beam search 中保留的最大活跃路径数。
  int kws_max_active_paths{8};
  // kws_num_trailing_blanks 是关键词末尾允许的 blank 数。
  int kws_num_trailing_blanks{0};
  // min_trigger_interval_ms 是两次唤醒命中之间的最短间隔。
  int min_trigger_interval_ms{800};
};

// 描述 xiaozhi 命令通道 TCP Server 的连接参数。
struct CommandPreset {
  // host 是 xiaozhi 命令通道 TCP Server 地址，默认本机回环地址。
  std::string host{"127.0.0.1"};
  // port 是 xiaozhi 命令通道 TCP Server 端口，默认 7789。
  int port{7789};
};

// 描述 xiaozhi 音频通道 TCP Server 的连接参数。
struct LlmPreset {
  // host 是 xiaozhi 音频通道 TCP Server 地址，默认本机回环地址。
  std::string host{"127.0.0.1"};
  // port 是 xiaozhi 音频通道 TCP Server 端口，默认 7799。
  int port{7799};
};

// 描述各类内部队列与节流参数。
struct BudgetPreset {
  // input_queue_frames 是 xiaozhi 上行 PCM 队列最多缓存的音频块数量。
  int input_queue_frames{64};
  // output_queue_frames 是预留的下行播放队列预算，供后续扩展使用。
  int output_queue_frames{128};
  // reconnect_backoff_min_ms 是 xiaozhi 断线重连退避的最小等待时间。
  int reconnect_backoff_min_ms{300};
  // reconnect_backoff_max_ms 是 xiaozhi 断线重连退避的最大等待时间。
  int reconnect_backoff_max_ms{4000};
};

// 描述日志输出相关参数。
struct LogPreset {
  // enable_debug 控制 debug 级别日志是否输出。
  bool enable_debug{false};
  // file_enabled 控制普通日志是否额外写入文件。
  bool file_enabled{false};
  // file_path 是日志文件相对配置目录的保存路径。
  std::string file_path{"logs/open-xiaoai-server.log"};
};

// 描述上传给 xiaozhi 的音频留存选项。
struct UploadPreset {
  // save_audio 控制是否保存实际上传给 xiaozhi 的有效语音 WAV。
  bool save_audio{false};
};

// 聚合程序启动所需的全部配置项。
struct Config {
  // soundbox 保存音箱侧 open-xiaoai-client 连接参数。
  SoundBoxPreset soundbox;
  // xiaozhi 保存上游 xiaozhi 服务连接和协议参数。
  XiaozhiPreset xiaozhi;
  // audio 保存本地采集和播放 PCM 参数。
  AudioPreset audio;
  // vad 保存上行语音裁剪参数。
  VadPreset vad;
  // wakeup 保存本地唤醒模型和提示语参数。
  Wakeup wakeup;
  // command 保存 xiaozhi 命令通道 TCP Server 连接参数。
  CommandPreset command;
  // llm 保存 xiaozhi 音频通道 TCP Server 连接参数。
  LlmPreset llm;
  // budget 保存内部队列和重连退避预算。
  BudgetPreset budget;
  // log 保存日志输出配置。
  LogPreset log;
  // upload 保存上行音频留存配置。
  UploadPreset upload;

  // 对解析后的配置做归一化，补齐默认值并修正非法范围。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void normalize();

  // 校验关键配置是否完整，缺失时抛出异常阻止启动。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void validate() const;
};

// 从 ini 文件加载配置。
// 参数说明：
// - path: 要读取的配置文件路径；主程序要求必须通过 -c/--config 显式传入。
// 返回值：
// - 返回解析、归一化后的 Config 对象。
Config load(const std::string& path);

}  // namespace xiaoai_server::config

