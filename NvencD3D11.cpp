// Copyright (c) Krupesh Parmar
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include "Common.h"
#include "NvencD3D11.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include "nvEncodeAPI.h"

// Unity interfaces (device getter)
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#pragma comment(lib, "nvencodeapi.lib")

using Microsoft::WRL::ComPtr;
static IUnityInterfaces* s_UnityInterfaces = nullptr;
static IUnityGraphics* s_GFX = nullptr;
static ID3D11Device* s_D3D11 = nullptr;
static ID3D11Texture2D* gLastTex = nullptr;
static bool s_SaveLocally = true;


// Called when the plugin is loaded by Unity
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    s_UnityInterfaces = unityInterfaces;
    s_GFX = s_UnityInterfaces->Get<IUnityGraphics>();
    if (s_GFX && s_GFX->GetRenderer() == kUnityGfxRendererD3D11)
    {
        auto d3d = s_UnityInterfaces->Get<IUnityGraphicsD3D11>();
        s_D3D11 = d3d ? d3d->GetDevice() : nullptr;
    }
}

//static std::vector<NV_ENC_OUTPUT_PTR> gBitstreams;
//static size_t gBsIndex = 0;
//static const int kNumBitstreams = 4;
NVWR_API void* NWR_GetD3D11Device() { return (void*)s_D3D11; }
static std::atomic<long long> gPendingPTS100ns{ 0 };

NVWR_API void NWR_SetPendingPTS(long long pts100ns) {
    gPendingPTS100ns.store(pts100ns, std::memory_order_relaxed);
}

static NvencLogCB g_log = nullptr;
static inline void Log(const char* s) { if (g_log) g_log(s); }
static NvencEncodedFrameSink g_frameSink = nullptr;
NVWR_API void Nvenc_SetEncodedFrameSink(NvencEncodedFrameSink cb) { g_frameSink = cb; }

static inline void NVENC_LOG(const char* where, NVENCSTATUS st) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[NVENC] %s failed: 0x%08x", where, st);
    Log(buf);
}

NVWR_API void Nvenc_SetLogger(NvencLogCB cb) { g_log = cb; }

// --------- Small helpers ----------
static inline void NVENC_THROW(const char* where, NVENCSTATUS st) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[NVENC] %s failed: 0x%08x", where, st);
    throw std::runtime_error(buf);
}
static inline void NVENC_CHK(const char* where, NVENCSTATUS st) {
    if (st != NV_ENC_SUCCESS) NVENC_THROW(where, st);
}

#define NVENC_DO_FALSE(call)                                                   \
    do {                                                                       \
        NVENCSTATUS _st = (call);                                              \
        if (_st != NV_ENC_SUCCESS) { NVENC_LOG(#call, _st); return false; }    \
    } while (0)

#define NVENC_DO_VOID(call)                                                    \
    do {                                                                       \
        NVENCSTATUS _st = (call);                                              \
        if (_st != NV_ENC_SUCCESS) { NVENC_LOG(#call, _st); return; }          \
    } while (0)

#define NVENC_DO_GOTO(call, label)                                             \
    do {                                                                       \
        NVENCSTATUS _st = (call);                                              \
        if (_st != NV_ENC_SUCCESS) { NVENC_LOG(#call, _st); goto label; }      \
    } while (0)

static inline std::string NowTag() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{}; localtime_s(&tm, &t);
    char b[64]; std::snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d",
        1900 + tm.tm_year, 1 + tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}


class NvencD3D11 {
public:
    bool open(ID3D11Device* dev, int w, int h, int fps, int kbps);
    bool encodeTexture(ID3D11Texture2D* tex, uint64_t pts100ns, bool& outKeyframe);
    void requestIDR();
    void setBitrate(int kbps);
    void close();
    void GetVideoDesc(int* width, int* height, int* fps, int* bitratekbps);

    static NvencD3D11& I() { static NvencD3D11 s; return s; }

    std::string                     outDir = "EncodedOut";

private:
    bool opened = false;
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;

    NV_ENCODE_API_FUNCTION_LIST    f = { NV_ENCODE_API_FUNCTION_LIST_VER };
    void*                          enc = nullptr;          // NV_ENC_OPEN_ENCODE_SESSION_EX::device

    GUID                           codec = NV_ENC_CODEC_H264_GUID;
    NV_ENC_INITIALIZE_PARAMS       init = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG                  cfg = { NV_ENC_CONFIG_VER };

    FILE*                          fp = nullptr;           // output .h264
    int                            W = 0, H = 0, FPS = 0, Bitrate = 0;
    bool                           forceIDR = false;
    // Bitstream pool
    std::vector<NV_ENC_OUTPUT_PTR> bitstreams; size_t bsIndex = 0; const int kNumBS = 4;
    // Per-frame mapped resource handle
    ComPtr<ID3D11Texture2D> encodeTex;  // GPU-side copy
    NV_ENC_REGISTERED_PTR          reg = nullptr;
    NV_ENC_INPUT_PTR               mapped = nullptr;

private:
    void writeBytes(const void* p, size_t n) {
        if (!fp) return;
        std::fwrite(p, 1, n, fp);
    }
    void ensureOutFile() {
        if (!s_SaveLocally) return;
        if (fp) return;
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec) { Log("[NVENC] create_directories failed"); }

        const auto path = std::string(outDir + "/stream_") + NowTag() + ".h264";
        if (fopen_s(&fp, path.c_str(), "wb") != 0) {
            fp = nullptr;
            Log((std::string("[NVENC] fopen_s failed: ") + path).c_str());
            return;
        }
        Log((std::string("[NVENC] Writing to ") + path).c_str());
    }
    void getParamsForPreset();
    bool ensureEncodeSurface();
    bool ensureRegisteredSurface();
    void registerTexture(ID3D11Texture2D* tex);
    void unregisterTexture();
};


void NvencD3D11::getParamsForPreset() {
    NV_ENC_PRESET_CONFIG pc{}; 
    pc.version = NV_ENC_PRESET_CONFIG_VER; 
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENC_DO_VOID(f.nvEncGetEncodePresetConfigEx(enc, codec, NV_ENC_PRESET_P3_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc));
    //cfg.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;    
    //cfg.gopLength = FPS * 2;                                
    //cfg.frameIntervalP = 1;                                      
    //cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    //cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR; 
    //cfg.rcParams.averageBitRate = Bitrate;                   
    //cfg.rcParams.maxBitRate = Bitrate;
    //cfg.rcParams.vbvBufferSize = Bitrate / 4;               
    //cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    //cfg.rcParams.enableAQ = 0;                               
    //cfg.encodeCodecConfig.h264Config.idrPeriod = cfg.gopLength;
    //cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    //cfg.encodeCodecConfig.h264Config.enableIntraRefresh = 0;
    //cfg.encodeCodecConfig.h264Config.disableDeblockingFilterIDC = 0;

    cfg = pc.presetCfg; // FIRST (once)
    cfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;      
    cfg.gopLength = FPS * 2;
    cfg.frameIntervalP = 1;                               
    cfg.encodeCodecConfig.h264Config.idrPeriod = FPS;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;    
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = Bitrate;
    cfg.rcParams.maxBitRate = Bitrate;
    cfg.rcParams.vbvBufferSize = Bitrate / 4;
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;

    /*
    cfg = pc.presetCfg; 
    cfg.version = NV_ENC_CONFIG_VER;

    cfg.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    cfg.gopLength = FPS * 2; // ~2s GOP
    cfg.frameIntervalP = 1; // IPPP…
    cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = Bitrate;
    cfg.rcParams.maxBitRate = Bitrate;
    cfg.rcParams.vbvBufferSize = Bitrate / 4;
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    cfg.rcParams.enableAQ = 0;

    cfg.encodeCodecConfig.h264Config.idrPeriod = cfg.gopLength;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.h264Config.disableDeblockingFilterIDC = 0;
   // cfg.encodeCodecConfig.h264Config.outputAUD = 1;
   */
}

bool NvencD3D11::ensureEncodeSurface() {
    if (encodeTex) { D3D11_TEXTURE2D_DESC d{}; encodeTex->GetDesc(&d); if ((int)d.Width == W && (int)d.Height == H) return true; encodeTex.Reset(); }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = W; desc.Height = H; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
    auto hr = device->CreateTexture2D(&desc, nullptr, encodeTex.GetAddressOf());
    if (FAILED(hr)) { Log("[NVENC] CreateTexture2D encode surface failed"); return false; }
    return true;
}


bool NvencD3D11::ensureRegisteredSurface() {
    if (reg) return true;
    NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER };
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.resourceToRegister = encodeTex.Get();
    rr.width = W; rr.height = H; rr.pitch = 0;
    rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR; // R8G8B8A8_UNORM → ABGR for NVENC
    NVENCSTATUS st = f.nvEncRegisterResource(enc, &rr);
    if (st != NV_ENC_SUCCESS) { NVENC_LOG("f.nvEncRegisterResource(enc,&rr)", st); return false; }
    reg = rr.registeredResource; return true;
}

bool NvencD3D11::open(ID3D11Device* dev, int w, int h, int fps, int kbps) {
    if (opened) close();
    device = dev; device->GetImmediateContext(context.GetAddressOf());
    W = w; H = h; FPS = fps; Bitrate = kbps;


    NVENC_DO_FALSE(NvEncodeAPICreateInstance(&f));


    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS ex{ NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    ex.device = dev; ex.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; ex.apiVersion = NVENCAPI_VERSION;
    NVENC_DO_FALSE(f.nvEncOpenEncodeSessionEx(&ex, &enc));


    init = {}; 
    init.version = NV_ENC_INITIALIZE_PARAMS_VER; 
    init.encodeGUID = codec;
    init.presetGUID = NV_ENC_PRESET_P3_GUID; // low latency perf
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = W; init.encodeHeight = H; init.darWidth = W; init.darHeight = H;
    init.frameRateNum = FPS; init.frameRateDen = 1; init.enablePTD = 1; init.reportSliceOffsets = 0; init.enableSubFrameWrite = 0;
    init.encodeConfig = &cfg; 
    getParamsForPreset();

    NVENC_DO_FALSE(f.nvEncInitializeEncoder(enc, &init));


    // bitstream buffers
    bitstreams.clear(); bitstreams.reserve(kNumBS);
    for (int i = 0; i < kNumBS; i++) {
        NV_ENC_CREATE_BITSTREAM_BUFFER cbb{ NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
        NVENC_DO_FALSE(f.nvEncCreateBitstreamBuffer(enc, &cbb));
        bitstreams.push_back(cbb.bitstreamBuffer);
    }
    bsIndex = 0;


    ensureOutFile();
    opened = true; Log("[NVENC] Open OK");
    return true;
}

void NvencD3D11::registerTexture(ID3D11Texture2D* tex) {
    if (reg && tex == gLastTex) return;   // already registered this one
    if (reg && tex != gLastTex) {         // different texture: unregister old
        unregisterTexture();
    }
    gLastTex = tex;

    NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER };
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.resourceToRegister = encodeTex.Get();
    rr.width = W;
    rr.height = H;
    rr.pitch = 0;
    rr.subResourceIndex = 0;
    rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR; // R8G8B8A8_UNORM -> ABGR
    NVENC_DO_VOID(f.nvEncRegisterResource(enc, &rr));
    reg = rr.registeredResource;
}

void NvencD3D11::unregisterTexture() {
    if (!reg) return;
    NVENC_DO_VOID(f.nvEncUnregisterResource(enc, reg));
    reg = nullptr;
}

bool NvencD3D11::encodeTexture(ID3D11Texture2D* srcTex, uint64_t pts100ns, bool& outKeyframe) {
    outKeyframe = false;
    if (!opened || !srcTex) { Log("[NVENC] encodeTexture: not opened or null src"); return false; }

    D3D11_TEXTURE2D_DESC s{}; srcTex->GetDesc(&s);
    if (s.SampleDesc.Count != 1) { Log("[NVENC] encodeTexture: source MSAA not supported"); return false; }
    if ((int)s.Width != W || (int)s.Height != H) { Log("[NVENC] encodeTexture: source size mismatch"); return false; }

    if (!ensureEncodeSurface()) return false;

    context->CopyResource(encodeTex.Get(), srcTex);

    if (!ensureRegisteredSurface()) return false;

    NV_ENC_MAP_INPUT_RESOURCE map{ NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = reg;
    NVENCSTATUS st = f.nvEncMapInputResource(enc, &map);
    if (st != NV_ENC_SUCCESS) { NVENC_LOG("f.nvEncMapInputResource", st); return false; }
    mapped = map.mappedResource;

    if (bitstreams.empty()) { Log("[NVENC] no bitstream buffers"); f.nvEncUnmapInputResource(enc, mapped); mapped = nullptr; return false; }
    NV_ENC_OUTPUT_PTR bs = bitstreams[bsIndex]; bsIndex = (bsIndex + 1) % bitstreams.size();


    NV_ENC_PIC_PARAMS pp{ 0 };
    /*pp.codecPicParams.h264PicParams.displayPOCSyntax = 0;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputTimeStamp = pts100ns;
    pp.inputBuffer = mapped;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;   
    pp.inputWidth = W;
    pp.inputHeight = H;
    pp.outputBitstream = bs;                  
    pp.inputDuration = 10'000'000ull / (FPS ? FPS : 60);*/
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.inputTimeStamp = pts100ns;
    pp.inputBuffer = mapped;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    pp.inputWidth = W;
    pp.inputHeight = H;
    pp.outputBitstream = bs;
    pp.completionEvent = nullptr;
    if (forceIDR) { 
        pp.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR; 
        pp.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        forceIDR = false;
        //outKeyframe = true; 
    }



    st = f.nvEncEncodePicture(enc, &pp);
    if (st != NV_ENC_SUCCESS) { NVENC_LOG("f.nvEncEncodePicture", st); f.nvEncUnmapInputResource(enc, mapped); mapped = nullptr; return false; }

    NV_ENC_LOCK_BITSTREAM lock{ NV_ENC_LOCK_BITSTREAM_VER }; lock.outputBitstream = bs; lock.doNotWait = 0;
    st = f.nvEncLockBitstream(enc, &lock);
    if (st != NV_ENC_SUCCESS) { NVENC_LOG("f.nvEncLockBitstream", st); f.nvEncUnmapInputResource(enc, mapped); mapped = nullptr; return false; }


    ensureOutFile();
    if (s_SaveLocally && !fp) Log("[NVENC] WARNING: file handle is null (no write)");

    if (lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes > 0) {

        // write to file
        if(s_SaveLocally) writeBytes(lock.bitstreamBufferPtr, lock.bitstreamSizeInBytes);
        /*static int logCount = 0;
        if (logCount < 10) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[NVENC] wrote %u bytes", (unsigned)lock.bitstreamSizeInBytes);
            Log(buf);
            logCount++;
        }*/

        // notify WebRTC sink AFTER writing
        if (g_frameSink) {
            /*if (lock.pictureType == NV_ENC_PIC_TYPE_P) {
                Log(std::string("[H264] P-frame at PTS=" + std::to_string(pp.inputTimeStamp)).c_str());
            }*/
            bool isKey = (lock.pictureType == NV_ENC_PIC_TYPE_IDR);
            g_frameSink((const uint8_t*)lock.bitstreamBufferPtr,
                (int)lock.bitstreamSizeInBytes,
                pp.inputTimeStamp, isKey);
        }
    }


    f.nvEncUnlockBitstream(enc, lock.outputBitstream);
    f.nvEncUnmapInputResource(enc, mapped); mapped = nullptr;
    return true;
}


void NvencD3D11::requestIDR() { forceIDR = true; }
void NvencD3D11::setBitrate(int kbps) {
    if (!opened) return; 
    Bitrate = kbps;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = Bitrate * 1000; 
    cfg.rcParams.maxBitRate = Bitrate * 1000;
    cfg.rcParams.vbvBufferSize = cfg.rcParams.averageBitRate / FPS;
    cfg.rcParams.enableAQ = 0;
    NV_ENC_RECONFIGURE_PARAMS rp{ NV_ENC_RECONFIGURE_PARAMS_VER }; 
    rp.reInitEncodeParams = init; 
    rp.reInitEncodeParams.encodeConfig = &cfg;
    NVENC_DO_VOID(f.nvEncReconfigureEncoder(enc, &rp));
}

void NvencD3D11::close() {
    if (reg) { NVENC_DO_VOID(f.nvEncUnregisterResource(enc, reg)); reg = nullptr; }
    encodeTex.Reset();
    for (auto bs : bitstreams) { if (bs) NVENC_DO_VOID(f.nvEncDestroyBitstreamBuffer(enc, bs)); }
    bitstreams.clear();
    if (enc) { NVENC_DO_VOID(f.nvEncDestroyEncoder(enc)); enc = nullptr; }
    if (fp) { std::fclose(fp); fp = nullptr; }
    device.Reset(); context.Reset(); opened = false; Log("[NVENC] Closed");
}

void NvencD3D11::GetVideoDesc(int* width, int* height, int* fps, int* bitratekbps)
{
    *width = W;
    *height = H;
    *fps = FPS;
    *bitratekbps = Bitrate;
}

NVWR_API bool Nvenc_Open(ID3D11Device* dev, int w, int h, int fps, int bitrateKbps) {
    try { return NvencD3D11::I().open(dev, w, h, fps, bitrateKbps); }
    catch (...) { Log("[NVENC] open threw"); return false; }
}
NVWR_API bool Nvenc_EncodeTexture(ID3D11Texture2D* tex, std::uint64_t pts100ns,
    std::vector<std::uint8_t>& outAnnexB, bool& outKeyframe,
    std::vector<std::uint8_t>* outSPS, std::vector<std::uint8_t>* outPPS)
{
    (void)outAnnexB; if (outSPS) outSPS->clear(); if (outPPS) outPPS->clear();
    return NvencD3D11::I().encodeTexture(tex, pts100ns, outKeyframe);
}
NVWR_API void Nvenc_RequestIDR() { NvencD3D11::I().requestIDR(); }
NVWR_API void Nvenc_SetBitrate(int kbps) { NvencD3D11::I().setBitrate(kbps); }
NVWR_API void Nvenc_Close() { NvencD3D11::I().close(); }

NVWR_API void NWR_GetVideoDesc(int* width, int* height, int* fps, int* bitratekbps)
{
    NvencD3D11::I().GetVideoDesc(width, height, fps, bitratekbps);
}


static std::string gOutDir = "EncodedOut";
NVWR_API void NWR_SetOutputDir(const char* path) { if (path && *path) gOutDir = path; NvencD3D11::I().outDir = gOutDir; }


// Render-thread encode event
NVWR_API void UNITY_INTERFACE_API NWR_OnRenderEvent(int eventId, void* data) {
    if (eventId != 1) return;
    auto tex = reinterpret_cast<ID3D11Texture2D*>(data);
    bool key = false; 
    long long pts = gPendingPTS100ns.load(std::memory_order_relaxed);
    if (!NvencD3D11::I().encodeTexture(tex, pts, key))
    { 
        Log("[NVENC] encodeTexture failed (render event)"); 
    }
}
NVWR_API UnityRenderingEventAndData NWR_GetRenderEventFunc() { return &NWR_OnRenderEvent; }


NVWR_API bool NWR_InitVideoWithD3D11Device(void* d3d11Device, int width, int height, int fps, int bitrateKbps, bool saveLocally) {
    s_SaveLocally = saveLocally;
    return Nvenc_Open(reinterpret_cast<ID3D11Device*>(d3d11Device), width, height, fps, bitrateKbps);
}

