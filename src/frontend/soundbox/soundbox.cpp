#include "frontend/soundbox/soundbox.hpp"

// 兼容旧构建入口。
//
// SoundBoxClient 已拆到 client.cpp；保留空翻译单元可以避免外部脚本仍显式引用
// src/soundbox/soundbox.cpp 时直接失败。CMake 新目标会编译 client.cpp。
