// NvencServerConfig.h
#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct NvencServerConfig
{
    // --- Frame / timing ---
    uint32_t width;                    // e.g., 1920
    uint32_t height;                   // e.g., 1080
    uint32_t fpsNum;                   // e.g., 60
    uint32_t fpsDen;                   // e.g., 1  (frame rate = fpsNum / fpsDen)

    // --- Bitrate / RC ---
    uint32_t bitrateKbps;              // target bitrate (e.g., 10000 = 10 Mbps)
    uint32_t maxBitrateKbps;           // peak (e.g., 12000)
    uint32_t vbvBufferSizeKb;          // 0 = auto (~bitrate/50 ≈ 20ms); else explicit

    // --- GOP / frames ---
    uint32_t gopLength;                // in frames; 60 = 1s @60fps
    uint32_t idrPeriod;                // in frames; 60..120 typical
    uint32_t bFrames;                  // 0 for low latency
    uint32_t refFrames;                // 1 for low latency

    // --- NVENC knobs (use ints for interop; map to NVENC enums inside) ---
    uint32_t preset;                   // map to NV_ENC_PRESET (e.g., LOW_LATENCY_HQ)
    uint32_t tuningInfo;               // map to NV_ENC_TUNING_INFO (LOW_LATENCY)
    uint32_t rcMode;                   // map to NV_ENC_PARAMS_RC_MODE (CBR, CBR_LOWDELAY_HQ, etc.)
    uint32_t profile;                  // H.264 profile (Automatic=0, Baseline, Main, High...)
    uint32_t level;                    // 0 = auto; else H.264 level (e.g., 42 for 4.2)

    uint8_t  enableAQ;                 // 0/1 Adaptive Quantization (start 0)
    uint8_t  enableLookahead;          // 0/1 (start 0)
    uint8_t  enableIntraRefresh;       // 0/1 (start 0)
    uint8_t  repeatSpsPps;             // 0/1 include SPS/PPS on every IDR (1 recommended)
    uint8_t  enableAnnexB;             // 1 = AnnexB NAL output from encoder (common)

    // Input format (map to NV_ENC_BUFFER_FORMAT)
    uint32_t inputFormat;              // e.g., NV12 (recommended) or ARGB if you must

    // --- RTP / packetizer / WebRTC ---
    uint32_t mtuBytes;                 // e.g., 1200 for ICE/UDP
    uint32_t payloadType;              // e.g., 96
    uint32_t clockRateHz;              // 90000 for H.264
    uint32_t ssrc;                     // 0 = choose random
    uint32_t startTimestamp;           // 0 = random; else explicit 90kHz ts start
    uint8_t  packetizationMode;        // 1 (non-interleaved)
    uint8_t  enableNack;               // 1
    uint8_t  enablePli;                // 1
    uint8_t  enableRemb;               // 1

    // --- Misc / logging ---
    uint32_t logVerbosity;             // 0=quiet … 3=verbose
};
#pragma pack(pop)

// Optional: convenience factory with solid defaults for 1080p60 low-latency.
inline NvencServerConfig MakeDefault1080p60()
{
    NvencServerConfig c{};
    c.width = 1920; c.height = 1080;
    c.fpsNum = 60;  c.fpsDen = 1;

    c.bitrateKbps = 10000;
    c.maxBitrateKbps = 12000;
    c.vbvBufferSizeKb = 0;            // auto ≈ 200 kb (~20ms @10Mbps)

    c.gopLength = 60;
    c.idrPeriod = 60;
    c.bFrames = 0;
    c.refFrames = 1;

    // Map these inside your NVENC setup:
    // preset -> NV_ENC_PRESET_LOW_LATENCY_HQ_GUID (or HP if needed)
    // tuning -> NV_ENC_TUNING_INFO_LOW_LATENCY
    // rcMode -> NV_ENC_PARAMS_RC_CBR
    c.preset = 1;  // your own mapping
    c.tuningInfo = 1;  // your own mapping
    c.rcMode = 1;  // your own mapping

    // Profile/Level: 0 = auto; many browsers accept High@4.2 for 1080p60
    c.profile = 0;
    c.level = 0;

    c.enableAQ = 0;
    c.enableLookahead = 0;
    c.enableIntraRefresh = 0;
    c.repeatSpsPps = 1;
    c.enableAnnexB = 1;

    // Input format: prefer NV12 zero-copy path
    // Define in your code: 0=NV12, 1=ARGB, etc.
    c.inputFormat = 0;

    c.mtuBytes = 1200;
    c.payloadType = 96;
    c.clockRateHz = 90000;
    c.ssrc = 0;   // random
    c.startTimestamp = 0;   // random
    c.packetizationMode = 1;
    c.enableNack = 1;
    c.enablePli = 1;
    c.enableRemb = 1;

    c.logVerbosity = 1;
    return c;
}
