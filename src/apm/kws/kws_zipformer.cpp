#include "apm/kws/kws_zipformer.hpp"

#include <stdexcept>

#include <sherpa-onnx/c-api/cxx-api.h>

#include "common/log.hpp"

namespace xiaoai_server::wakeup {

namespace {

// kLog 是本地关键词检测模块专用日志器。
const auto kLog = xiaoai_server::GetLogger("kws");

}  // namespace

struct ZipformerKwsEngine::Impl {
  // spotter 是 sherpa-onnx 提供的关键词检测器实例。
  sherpa_onnx::cxx::KeywordSpotter spotter;
  // stream 是在线检测流，保存当前音频上下文。
  sherpa_onnx::cxx::OnlineStream stream;
  // samples 复用为 float PCM 缓冲区，避免每帧重复分配。
  std::vector<float> samples;

  // 构造 Zipformer 内部实现并加载模型文件。
  // 参数说明：
  // - cfg: 关键词模型与词表配置。
  // 返回值：
  // - 无；模型加载失败时抛出异常。
  explicit Impl(const config::Wakeup& cfg)
      : spotter([&]() {
          // c 是传给 sherpa-onnx 的完整 KWS 模型和解码配置。
          sherpa_onnx::cxx::KeywordSpotterConfig c;
          c.model_config.model_type = "zipformer2";
          c.model_config.tokens = cfg.tokens_path;
          c.model_config.transducer.encoder = cfg.encoder_path;
          c.model_config.transducer.decoder = cfg.decoder_path;
          c.model_config.transducer.joiner = cfg.joiner_path;
          c.model_config.num_threads = cfg.kws_num_threads;
          c.model_config.provider = "cpu";
          c.keywords_threshold = cfg.kws_threshold;
          c.keywords_score = cfg.kws_score;
          c.num_trailing_blanks = cfg.kws_num_trailing_blanks;
          c.max_active_paths = cfg.kws_max_active_paths;
          c.keywords_file = cfg.keywords_file;
          kLog->info(
              "kws config: threshold={:.2f}, score={:.2f}, threads={}, max_active_paths={}, trailing_blanks={}",
              cfg.kws_threshold, cfg.kws_score, cfg.kws_num_threads,
              cfg.kws_max_active_paths, cfg.kws_num_trailing_blanks);
          // s 是创建完成的 spotter 句柄，空句柄表示模型加载失败。
          auto s = sherpa_onnx::cxx::KeywordSpotter::Create(c);
          if (s.Get() == nullptr) {
            kLog->error("kws model load failed: spotter is null "
                        "(check model files exist and memory/disk is sufficient)");
            throw std::runtime_error(
                "kws model load failed: "
                "check model files exist and memory/disk is sufficient");
          }
          return s;
        }()),
        stream(spotter.CreateStream()) {
    if (stream.Get() == nullptr) {
      kLog->error("kws model load failed: CreateStream returned null");
      throw std::runtime_error("kws model load failed: CreateStream returned null");
    }
  }

  // 送入一段 PCM 并尝试返回一次关键词命中结果。
  // 参数说明：
  // - pcm: PCM 原始数据地址。
  // - size_bytes: PCM 数据字节数。
  // - sample_rate: PCM 采样率。
  // - channels: PCM 通道数。
  // - bits_per_sample: PCM 位宽。
  // 返回值：
  // - 命中时返回 KwsHit，否则返回 std::nullopt。
  std::optional<KwsHit> Accept(const uint8_t* pcm, size_t size_bytes, int sample_rate,
                               int channels, int bits_per_sample) {
    if (!pcm || size_bytes < 2 || bits_per_sample != 16 || channels != 1 || sample_rate <= 0) {
      return std::nullopt;
    }

    // n 是当前 PCM 块包含的 S16 采样数。
    const size_t n = size_bytes / sizeof(int16_t);
    if (n == 0) {
      return std::nullopt;
    }
    // in 指向原始 S16 PCM 采样。
    const auto* in = reinterpret_cast<const int16_t*>(pcm);

    samples.resize(n);
    for (size_t i = 0; i < n; ++i) {
      samples[i] = static_cast<float>(in[i]) / 32768.0f;
    }

    stream.AcceptWaveform(sample_rate, samples.data(), static_cast<int32_t>(n));

    while (spotter.IsReady(&stream)) {
      spotter.Decode(&stream);
      // r 是当前解码步的关键词检测结果。
      auto r = spotter.GetResult(&stream);
      if (!r.keyword.empty()) {
        spotter.Reset(&stream);
        return KwsHit{.keyword = r.keyword};
      }
    }

    return std::nullopt;
  }

  // 重置在线流状态，清空此前的上下文。
  // 参数说明：
  // - 无。
  // 返回值：
  // - 无。
  void Reset() { stream = spotter.CreateStream(); }
};

// 构造 Zipformer KWS 引擎外层对象。
// 参数说明：
// - cfg: 模型路径、关键词文件和 KWS 解码参数。
// 返回值：
// - 无；模型加载失败时抛出异常。
ZipformerKwsEngine::ZipformerKwsEngine(config::Wakeup cfg)
    : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>(cfg_)) {}

// 析构 Zipformer KWS 引擎。
// 参数说明：
// - 无。
// 返回值：
// - 无。
ZipformerKwsEngine::~ZipformerKwsEngine() = default;

// 输入一段 S16 PCM 并尝试检测唤醒词。
// 参数说明：
// - pcm: PCM 字节流地址。
// - size_bytes: PCM 字节数。
// - sample_rate: PCM 采样率。
// - channels: PCM 通道数，当前要求为 1。
// - bits_per_sample: PCM 位深，当前要求为 16。
// 返回值：
// - 命中时返回 KwsHit，否则返回 std::nullopt。
std::optional<KwsHit> ZipformerKwsEngine::AcceptPcm16(const uint8_t* pcm, size_t size_bytes,
                                                       int sample_rate, int channels,
                                                       int bits_per_sample) {
  if (!impl_) {
    return std::nullopt;
  }
  return impl_->Accept(pcm, size_bytes, sample_rate, channels, bits_per_sample);
}

// 重置底层在线检测流。
// 参数说明：
// - 无。
// 返回值：
// - 无。
void ZipformerKwsEngine::Reset() {
  if (impl_) {
    impl_->Reset();
  }
}

}  // namespace xiaoai_server::wakeup
