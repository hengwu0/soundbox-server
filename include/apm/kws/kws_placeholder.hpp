#pragma once

namespace audio_processing_module::apm::kws {

class KwsPlaceholder {
 public:
  constexpr bool implemented() const { return false; }
};

}  // namespace audio_processing_module::apm::kws
