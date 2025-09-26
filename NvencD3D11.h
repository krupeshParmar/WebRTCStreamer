// Copyright (c) Krupesh Parmar
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include <cstdint>
#include <vector>

#include "IUnityInterface.h"
struct ID3D11Device;
struct ID3D11Texture2D;

#if defined(_WIN32)
#define NVWR_API extern "C" __declspec(dllexport)
#else
#define NVWR_API extern "C"
#endif

typedef void (*NvencLogCB)(const char* msg);
NVWR_API void Nvenc_SetLogger(NvencLogCB cb);

typedef void (*NvencEncodedFrameSink)(const uint8_t* annexB, int bytes, uint64_t pts90k, bool keyframe);
NVWR_API void Nvenc_SetEncodedFrameSink(NvencEncodedFrameSink cb);

NVWR_API bool Nvenc_Open(ID3D11Device* dev, int w, int h, int fps, int bitrateKbps);
NVWR_API bool Nvenc_EncodeTexture(ID3D11Texture2D* tex,
	std::uint64_t pts100ns,
	std::vector<std::uint8_t>& outAnnexB,
	bool& outKeyframe,
	std::vector<std::uint8_t>* outSPS = nullptr,
	std::vector<std::uint8_t>* outPPS = nullptr);
NVWR_API void Nvenc_RequestIDR();
NVWR_API void Nvenc_SetBitrate(int kbps);
NVWR_API void Nvenc_Close();

NVWR_API void NWR_GetVideoDesc(int* width, int* height, int* fps, int* bitratekbps);


NVWR_API void NWR_SetOutputDir(const char* path);


// Render-thread callback
typedef void (*UnityRenderingEventAndData)(int eventId, void* data);
NVWR_API void* NWR_GetD3D11Device();
NVWR_API void NWR_SetPendingPTS(long long pts100ns);
NVWR_API void UNITY_INTERFACE_API NWR_OnRenderEvent(int eventId, void* data);
NVWR_API UnityRenderingEventAndData NWR_GetRenderEventFunc();
NVWR_API bool NWR_InitVideoWithD3D11Device(void* d3d11Device, int width, int height, int fps, int bitrateKbps, bool saveLocally);