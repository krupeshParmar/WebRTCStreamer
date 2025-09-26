// Copyright (c) Krupesh Parmar
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#ifdef _WIN32
#define WEBRTC_STREAMER_API extern "C" __declspec(dllexport)
#else
#define WEBRTC_STREAMER_API extern "C"
#endif
#include <iostream>
#include <cstdint>
#include <d3d11.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <memory>
#include <string>
#include <filesystem> // C++17
#include <fstream>
#include <atomic>