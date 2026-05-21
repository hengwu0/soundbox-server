#pragma once

// 兼容旧 include 路径。
//
// 重构后 SoundBoxClient 的真实实现位于 client.hpp/cpp：
// - soundbox.hpp 只保留转发，避免 App 和外部调用点一次性改名造成大范围 churn。
// - 新代码应优先 include "frontend/soundbox/client.hpp"。
#include "frontend/soundbox/client.hpp"
