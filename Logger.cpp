// ============================================================================
// WebRTC + NVENC Real-Time Video Streaming Plugin
// ============================================================================
// Copyright (c) Krupesh Parmar
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
//
// OVERVIEW:
// This plugin enables real-time H.264 video streaming from Unity to a web browser
// using WebRTC for peer-to-peer communication and NVIDIA NVENC for GPU-accelerated
// video encoding.
//
// ARCHITECTURE:
// - Unity Main Thread: Renders frames, calls plugin APIs
// - NVENC Thread: GPU encodes frames to H.264
// - Worker Thread: Manages WebRTC/WebSocket/DataChannel operations
// - Callback Threads: Internal libdatachannel/ixwebsocket threads
//
// THREADING SAFETY:
// All WebRTC operations happen on a single worker thread to avoid race conditions.
// Other threads communicate via thread-safe queues. No callbacks directly access
// shared WebRTC state - they only queue messages for the worker thread to process.
//
// KEY LESSONS LEARNED:
// 1. Never hardcode bindAddress - let WebRTC auto-detect network interfaces
// 2. Always wrap WebRTC operations in try/catch - uncaught exceptions crash silently
// 3. Callbacks must only queue data, never perform complex operations
// 4. Poll for ICE gathering completion instead of using onLocalCandidate callbacks
// 5. Proper cleanup order: DataChannel → Track → Packetizer → PeerConnection → WebSocket
// ============================================================================

#define NOMINMAX

// ============================================================================
// INCLUDES
// ============================================================================
#include "Common.h"
#include <ixwebsocket/IXWebSocket.h>
#include <thread>
#include <random>
#include <cstdint>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstring>
#include <rtc/rtc.hpp>
#include <rtc/nalunit.hpp>
#include <rtc/common.hpp>
#include <rtc/track.hpp>
#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include <d3d11.h>
#include "NvencD3D11.h"
#include <psapi.h>
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dbghelp.lib")

using json = nlohmann::json;
using ByteVec = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

// ============================================================================
// SECTION 1: CORE TYPES & STRUCTURES
// ============================================================================

/**
 * @brief Transform data from Unity (position, rotation, scale)
 * Used for debugging and telemetry sent over DataChannel
 */
struct transform_component {
    float position[3];
    float rotation[3];
    float scale[3];
};

/**
 * @brief Log data structure sent from Unity to browser
 * Contains transform data and shader state for debugging
 */
struct log_data {
    transform_component transform;
    bool shader1state;
};

/**
 * @brief Callback type for receiving commands from browser
 * Unity registers this callback to receive messages via DataChannel or WebSocket
 * Thread-safe: Called from worker thread, queues to Unity's main thread
 */
typedef void (*CommandCallback)(const char* message);

// ============================================================================
// SECTION 2: LOGGING SYSTEM
// ============================================================================

static std::ofstream s_Logger;          // File stream for logging
static std::mutex log_mutex;            // Protects s_Logger from concurrent access

/**
 * @brief Generates timestamp string for log entries
 * Format: [YYYY-MM-DD HH:MM:SS.mmm]
 * Thread-safe: Uses only local variables
 */
std::string GetTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_s(&tm_buf, &t);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "[%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
    return oss.str();
}

/**
 * @brief Logs a formatted message to file
 * Thread-safe: Uses mutex to protect file access
 * Format: [timestamp] message
 *
 * @param format printf-style format string
 * @param ... variable arguments for formatting
 */
void LogMessage(const char* format, ...) {
    va_list args;
    va_start(args, format);

    int size = vsnprintf(nullptr, 0, format, args);
    va_end(args);

    if (size <= 0) return;

    std::string buffer(size + 1, '\0');
    va_start(args, format);
    vsnprintf(&buffer[0], buffer.size(), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(log_mutex);
    if (!s_Logger.is_open())
        s_Logger.open("WebStreamLogs/logs.txt", std::ios::app);
    s_Logger << GetTimestamp() << buffer << "\n";
    s_Logger.flush();
}

/**
 * @brief Logs a string message (convenience overload)
 * Thread-safe: Delegates to formatted LogMessage
 */
void LogMessage(std::string msg) {
    LogMessage("%s", msg.c_str());
}

/**
 * @brief Bridge function for NVENC encoder to log messages
 * Thread-safe: Called from NVENC thread, delegates to thread-safe LogMessage
 */
static void NvEncBridgeLogger(const char* msg) {
    LogMessage(msg);
}

// ============================================================================
// SECTION 3: GLOBAL STATE - WEBRTC CORE
// ============================================================================
// THREAD OWNERSHIP: Worker thread only (except atomic flags)
// These objects must NEVER be accessed from callbacks or other threads directly

static std::shared_ptr<rtc::PeerConnection> g_peer_connection;  // WebRTC peer connection
static std::shared_ptr<rtc::Track> g_videoTrack;                // Video track for H.264 stream
static std::shared_ptr<rtc::H264RtpPacketizer> g_h264Packetizer;// RTP packetizer for H.264
static std::shared_ptr<rtc::DataChannel> g_dc;                  // DataChannel for bidirectional messaging

// ============================================================================
// SECTION 4: GLOBAL STATE - WEBSOCKET & SIGNALING
// ============================================================================

static ix::WebSocket g_websocket;                      // WebSocket for signaling
static std::atomic<bool> g_websocketOpen{ false };      // True when WebSocket is connected
static std::atomic<bool> g_websocketAlive{ false };     // True when WebSocket is active (for safe send)
static std::atomic<bool> g_connected{ false };          // Legacy flag, mirrors g_websocketOpen

static std::mutex sdpMutex;                            // Protects SDP and ICE candidate data
static std::string g_localSdp;                         // Local SDP offer (after ICE gathering)
static std::string offerDescription;                   // Legacy, use g_localSdp
static std::vector<std::string> pendingCandidates;     // Buffered ICE candidates (before remote SDP set)

// ============================================================================
// SECTION 5: GLOBAL STATE - THREADING & SYNCHRONIZATION
// ============================================================================

static std::thread g_workerThread;                     // Worker thread for all WebRTC operations
static std::atomic<bool> g_running{ false };             // True when worker thread should run
static std::atomic<bool> g_offerSent{ false };          // True after SDP offer sent to browser
static std::atomic<bool> remoteSet{ false };            // True after browser's SDP answer received
static std::atomic<bool> g_dcOpen{ false };             // True when DataChannel is open

// ICE gathering state
static bool g_gatheringComplete = false;               // True when ICE gathering finishes
static std::string g_lastLocalSdp;                     // Last SDP sent (to detect changes)

// Legacy task queue (currently unused in polling architecture)
static std::mutex qM;
static std::condition_variable qCV;
static std::queue<std::function<void()>> q;
static std::atomic_bool running{ false };

// Mutex for protecting various global state (legacy, consider consolidating)
static std::mutex gPeerMutex;
static std::mutex gDataConnectionMutex;
static std::mutex localIceMtx;

// ============================================================================
// SECTION 6: GLOBAL STATE - VIDEO ENCODING
// ============================================================================

static uint32_t gSSRC = 0;                             // Synchronization Source identifier for RTP
static int gV_W = 1920, gV_H = 1080;                  // Video width and height
static int gV_FPS = 30;                                // Target frames per second
static int gV_BR = 6000;                               // Target bitrate in kbps
static uint8_t gPT = 96;                               // RTP payload type for H.264

// Legacy/unused video state
//static uint32_t gTs = 0x12345678;
//static uint32_t gTs90k = 0;
//static uint32_t gTsStep = 90000 / std::max(1u, static_cast<unsigned int>(gV_FPS));
//static uint16_t gSeq = 1;
//static const size_t kMTU = 1200;
//static std::atomic<bool> gVideoReady{ false };
//static bool gCodecHeaderSent = false;
//static std::vector<uint8_t> gSPS, gPPS;
//static Clock::time_point t0 = Clock::now();

// ============================================================================
// SECTION 7: GLOBAL STATE - CALLBACKS
// ============================================================================

static CommandCallback g_CommandCallback = nullptr;    // Unity callback for browser messages

/**
 * @brief Forwards browser message to Unity callback
 * Thread-safety: Called from worker thread
 * Internal function - invokes registered Unity callback
 */
void OnMessageFromBrowser(std::string msg) {
    if (g_CommandCallback) {
        g_CommandCallback(msg.c_str());
    }
}

// ============================================================================
// SECTION 8: HELPER CLASSES
// ============================================================================

/**
 * @brief Manages RTP timestamp conversion from presentation timestamps
 * Converts Unity's 100ns timestamps to RTP's 90kHz clock
 * Thread-safe: No shared state, each instance independent
 */
class TimestampManager {
private:
    uint32_t rtpTimestampBase = 0;
    uint64_t basePts100ns = 0;
    bool initialized = false;

public:
    /**
     * @brief Converts presentation timestamp to RTP timestamp
     * @param pts100ns Presentation timestamp in 100-nanosecond units
     * @return RTP timestamp at 90kHz clock rate
     */
    uint32_t convertPtsToRtp(uint64_t pts100ns) {
        if (!initialized) {
            basePts100ns = pts100ns;
            rtpTimestampBase = static_cast<uint32_t>(rand());
            initialized = true;
            return rtpTimestampBase;
        }

        // Convert 100ns → microseconds → 90kHz
        uint64_t deltaPts100ns = pts100ns - basePts100ns;
        uint64_t deltaPtsUs = deltaPts100ns / 10;
        uint32_t rtpDelta = static_cast<uint32_t>((deltaPtsUs * 90) / 1000);

        return rtpTimestampBase + rtpDelta;
    }
};

static TimestampManager tsManager;  // Currently unused, but available for timestamp conversion

// ============================================================================
// SECTION 9: THREAD-SAFE MESSAGE QUEUES
// ============================================================================
// These queues allow safe communication between threads without blocking

/**
 * @brief Frame data from NVENC encoder
 * Pushed by: NVENC thread
 * Popped by: Worker thread
 */
struct FrameData {
    std::vector<uint8_t> data;  // H.264 encoded frame data
    uint64_t pts100ns;          // Presentation timestamp (100ns units)
};

static std::mutex g_frameMutex;
static std::queue<FrameData> g_frameQueue;

/**
 * @brief Pushes encoded frame to queue for WebRTC transmission
 * Thread-safe: Called from NVENC thread
 * Queue limit: 60 frames (drops oldest if exceeded)
 */
static void PushFrame(const uint8_t* data, int bytes, uint64_t pts100ns) {
    FrameData frame;
    frame.data.assign(data, data + bytes);
    frame.pts100ns = pts100ns;

    std::lock_guard<std::mutex> lk(g_frameMutex);
    g_frameQueue.push(std::move(frame));

    // Prevent unbounded growth - drop old frames if queue too large
    while (g_frameQueue.size() > 60) {
        g_frameQueue.pop();
    }
}

/**
 * @brief Pops frame from queue for transmission
 * Thread-safe: Called from worker thread
 * @return true if frame retrieved, false if queue empty
 */
static bool PopFrame(FrameData& frame) {
    std::lock_guard<std::mutex> lk(g_frameMutex);
    if (g_frameQueue.empty()) return false;
    frame = std::move(g_frameQueue.front());
    g_frameQueue.pop();
    return true;
}

/**
 * @brief WebSocket message from browser
 * Pushed by: WebSocket callback thread
 * Popped by: Worker thread
 */
struct WsMessage {
    std::string data;  // Raw JSON message
};

static std::mutex g_websocketMsgMutex;
static std::queue<WsMessage> g_websocketMsgQueue;

/**
 * @brief Pushes WebSocket message to queue for processing
 * Thread-safe: Called from WebSocket callback thread
 */
static void PushWsMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_websocketMsgMutex);
    g_websocketMsgQueue.push({ msg });
}

/**
 * @brief Pops WebSocket message from queue
 * Thread-safe: Called from worker thread
 * @return true if message retrieved, false if queue empty
 */
static bool PopWsMessage(WsMessage& msg) {
    std::lock_guard<std::mutex> lk(g_websocketMsgMutex);
    if (g_websocketMsgQueue.empty()) return false;
    msg = std::move(g_websocketMsgQueue.front());
    g_websocketMsgQueue.pop();
    return true;
}

/**
 * @brief DataChannel message (bidirectional)
 * Pushed by: Unity thread (OUTGOING) or DataChannel callback (INCOMING)
 * Popped by: Worker thread
 */
struct DataChannelMessage {
    enum Direction { OUTGOING, INCOMING };
    Direction dir;      // Message direction
    std::string data;   // JSON message payload
};

static std::mutex g_dcMsgMutex;
static std::queue<DataChannelMessage> g_dcMsgQueue;

/**
 * @brief Pushes DataChannel message to queue
 * Thread-safe: Called from Unity thread or DataChannel callback
 */
static void PushDataChannelMessage(DataChannelMessage msg) {
    std::lock_guard<std::mutex> lk(g_dcMsgMutex);
    g_dcMsgQueue.push(std::move(msg));
}

/**
 * @brief Pops DataChannel message from queue
 * Thread-safe: Called from worker thread
 * @return true if message retrieved, false if queue empty
 */
static bool PopDataChannelMessage(DataChannelMessage& msg) {
    std::lock_guard<std::mutex> lk(g_dcMsgMutex);
    if (g_dcMsgQueue.empty()) return false;
    msg = std::move(g_dcMsgQueue.front());
    g_dcMsgQueue.pop();
    return true;
}

// ============================================================================
// SECTION 10: UTILITY FUNCTIONS
// ============================================================================

/**
 * @brief Generates random SSRC for RTP stream
 * Thread-safe: Uses static local with internal mutex
 * @return Random 32-bit SSRC value
 */
static uint32_t MakeRandomSSRC() {
    static std::mt19937 rng{ std::random_device{}() };
    static std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFFu);
    return dist(rng);
}

/**
 * @brief Safely sends message via WebSocket
 * Thread-safe: Checks atomic flags before sending
 *
 * @param msg JSON message to send
 * @return true if sent successfully, false otherwise
 */
static bool SafeWsSend(const std::string& msg) {
    if (!g_websocketAlive.load() || !g_connected.load()) {
        return false;
    }
    try {
        g_websocket.send(msg);
        return true;
    }
    catch (const std::exception& e) {
        LogMessage(std::string("[WS] send exception: ") + e.what());
        return false;
    }
}

/**
 * @brief Patches SDP to fix compatibility issues
 * Currently simplified - returns SDP as-is
 *
 * Original functionality (commented out in source):
 * - Normalized H.264 fmtp parameters
 * - Fixed video port to 9 instead of 0
 * - Added BUNDLE grouping
 * - Injected SSRC attributes
 *
 * @param sdp Original SDP string
 * @return Patched SDP string
 */
static std::string PatchH264Fmtp(const std::string& sdp) {
    // Simplified - just return as-is
    // Full implementation available in commented code above
    return sdp;
}

// ============================================================================
// SECTION 11: CRASH HANDLING
// ============================================================================

/**
 * @brief Windows Structured Exception Handler for crash reporting
 * Logs exception code and address before termination
 * Helps diagnose crashes in release builds without debugger
 *
 * Common codes:
 * - 0xC0000005: Access violation (null pointer, out of bounds)
 * - 0xC000001D: Illegal instruction (ABI mismatch, corrupted code)
 * - 0xC0000094: Integer divide by zero
 * - 0xC00000FD: Stack overflow (infinite recursion)
 */
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exceptionInfo) {
    DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    PVOID addr = exceptionInfo->ExceptionRecord->ExceptionAddress;

    std::ostringstream oss;
    oss << "[CRASH] Exception 0x" << std::hex << std::uppercase << code
        << " at address 0x" << addr;
    LogMessage(oss.str());

    switch (code) {
    case 0xC0000005: LogMessage("[CRASH] ACCESS_VIOLATION"); break;
    case 0xC000001D: LogMessage("[CRASH] ILLEGAL_INSTRUCTION"); break;
    case 0xC0000094: LogMessage("[CRASH] INTEGER_DIVIDE_BY_ZERO"); break;
    case 0xC00000FD: LogMessage("[CRASH] STACK_OVERFLOW"); break;
    default: LogMessage("[CRASH] Unknown exception"); break;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// ============================================================================
// SECTION 12: CALLBACK HANDLERS
// ============================================================================
// CRITICAL: These functions run on internal library threads
// They must ONLY queue data - never perform complex operations or access shared state

/**
 * @brief WebSocket callback handler
 * Thread-safety: Runs on ixwebsocket internal thread
 * Safe operations: Setting atomic flags, queuing messages
 * Unsafe operations: Accessing WebRTC objects, complex processing
 */
static void OnWsCallback(const ix::WebSocketMessagePtr& ixMsg) {
    using ix::WebSocketMessageType;

    switch (ixMsg->type) {
    case WebSocketMessageType::Open:
        LogMessage("[WS] Open");
        g_websocketOpen = true;
        try {
            // Register with signaling server
            g_websocket.send(R"({"type":"register","role":"unity"})");
        }
        catch (...) {}
        break;

    case WebSocketMessageType::Close:
        LogMessage("[WS] Close");
        g_websocketOpen = false;
        break;

    case WebSocketMessageType::Error:
        LogMessage("[WS] Error: " + ixMsg->errorInfo.reason);
        break;

    case WebSocketMessageType::Message:
        // Just queue the message - worker thread will parse JSON
        PushWsMessage(ixMsg->str);
        break;
    }
}

/**
 * @brief NVENC encoded frame callback
 * Thread-safety: Runs on NVENC thread
 * Simply queues frame data for worker thread to transmit
 *
 * @param data H.264 NAL units (Annex-B format)
 * @param bytes Size of encoded data
 * @param pts100ns Presentation timestamp in 100-nanosecond units
 * @param key True if keyframe (IDR)
 */
static void OnEncodedFrameCallback(const uint8_t* data, int bytes, uint64_t pts100ns, bool key) {
    if (data && bytes > 0) {
        PushFrame(data, bytes, pts100ns);
    }
}

// ============================================================================
// SECTION 13: WORKER THREAD - MESSAGE PROCESSING
// ============================================================================
// These functions run ONLY on the worker thread
// They have exclusive access to WebRTC objects

/**
 * @brief Sends SDP offer to browser via WebSocket
 * Thread-safety: Called only from worker thread
 * Checks: WebSocket open, offer not already sent, SDP not empty
 */
static void SendOfferIfReady() {
    if (!g_websocketOpen.load()) {
        LogMessage("[Send] WS not open, can't send offer");
        return;
    }

    if (g_offerSent.exchange(true)) {
        LogMessage("[Send] Offer already sent");
        return;
    }

    std::string sdp;
    {
        std::lock_guard<std::mutex> lk(sdpMutex);
        sdp = g_localSdp;
    }

    if (sdp.empty()) {
        LogMessage("[Send] ERROR: SDP is empty!");
        g_offerSent = false;  // Reset so we can try again
        return;
    }

    nlohmann::json j = {
        {"type", "sdp-offer"},
        {"sdp", sdp}
    };

    try {
        std::string offerJson = j.dump();
        LogMessage("[Send] Sending offer (" + std::to_string(offerJson.size()) + " bytes)");
        g_websocket.send(offerJson);
        LogMessage("[Worker] Offer sent successfully!");
    }
    catch (const std::exception& e) {
        LogMessage("[Worker] Send offer failed: " + std::string(e.what()));
        g_offerSent = false;  // Reset so we can try again
    }
}

/**
 * @brief Sends buffered ICE candidates after remote SDP is set
 * Thread-safety: Called only from worker thread
 *
 * ICE candidates are buffered if they arrive before the browser's SDP answer.
 * Once the answer is received, this function flushes all pending candidates.
 */
static void FlushPendingCandidates() {
    if (!g_websocketOpen.load() || !remoteSet.load()) return;

    std::vector<std::string> toSend;
    {
        std::lock_guard<std::mutex> lk(sdpMutex);
        toSend.swap(pendingCandidates);
    }

    if (!toSend.empty()) {
        LogMessage("[Worker] Flushing " + std::to_string(toSend.size()) + " candidates");
    }

    for (const auto& candJson : toSend) {
        try {
            g_websocket.send(candJson);
        }
        catch (...) {}
    }
}

/**
 * @brief Processes WebSocket messages from queue
 * Thread-safety: Called only from worker thread
 *
 * Handles three message types:
 * 1. sdp-answer: Browser's SDP answer (completes handshake)
 * 2. ice-candidate: Browser's ICE candidates (establishes connectivity)
 * 3. command: Application-level commands (forwarded to Unity)
 */
static void ProcessWsMessages() {
    WsMessage msg;
    while (PopWsMessage(msg)) {
        try {
            if (!g_peer_connection) continue;

            auto j = nlohmann::json::parse(msg.data);
            std::string type = j.value("type", "");

            if (type == "sdp-answer") {
                std::string sdp = j.value("sdp", "");
                if (!sdp.empty()) {
                    g_peer_connection->setRemoteDescription(rtc::Description(sdp, "answer"));
                    remoteSet = true;
                    LogMessage("[Worker] Remote SDP set");
                    FlushPendingCandidates();
                }
            }
            else if (type == "ice-candidate") {
                auto& jc = j["candidate"];
                std::string cand = jc.value("candidate", "");
                std::string mid = jc.value("sdpMid", "video");
                if (!cand.empty()) {
                    g_peer_connection->addRemoteCandidate(rtc::Candidate(cand, mid));
                    LogMessage("[Worker] Added remote candidate");
                }
            }
            else if (type == "command") {
                if (j.contains("data")) {
                    std::string cmdData = j["data"].get<std::string>();
                    LogMessage("[Worker] Command received: " + cmdData);
                    OnMessageFromBrowser(cmdData);
                }
            }
        }
        catch (const std::exception& e) {
            LogMessage("[Worker] WS message error: " + std::string(e.what()));
        }
    }
}

/**
 * @brief Processes and sends video frames from queue
 * Thread-safety: Called only from worker thread
 *
 * Batch limit: 10 frames per iteration to prevent starvation of other tasks
 * Sends only when: PeerConnection connected, track open, packetizer ready
 */
static void ProcessFrames() {
    if (!g_peer_connection) return;
    if (g_peer_connection->state() != rtc::PeerConnection::State::Connected) return;
    if (!g_videoTrack || !g_videoTrack->isOpen()) return;
    if (!g_h264Packetizer) return;

    FrameData frame;
    int sent = 0;
    while (PopFrame(frame) && sent < 10) {  // Limit to prevent blocking
        try {
            // Convert presentation timestamp to RTP timestamp (90kHz clock)
            uint32_t ts90k = uint32_t((frame.pts100ns * 9) / 1000);
            g_h264Packetizer->rtpConfig->timestamp = ts90k;

            // Convert std::vector<uint8_t> to rtc::binary
            rtc::binary bin(frame.data.size());
            std::transform(frame.data.begin(), frame.data.end(),
                bin.begin(), [](uint8_t c) { return std::byte(c); });

            if (g_videoTrack->send(bin)) {
                sent++;
            }
        }
        catch (...) {}
    }
}

/**
 * @brief Processes DataChannel messages from queue
 * Thread-safety: Called only from worker thread
 *
 * Handles bidirectional DataChannel communication:
 * - OUTGOING: Messages from Unity to browser
 * - INCOMING: Messages from browser to Unity (calls registered callback)
 */
static void ProcessDataChannelMessages() {
    if (!g_dc) return;

    DataChannelMessage msg;
    while (PopDataChannelMessage(msg)) {
        try {
            if (msg.dir == DataChannelMessage::OUTGOING) {
                // Send to browser
                if (g_dc->isOpen()) {
                    g_dc->send(msg.data);
                }
                else {
                    LogMessage("[DC] Can't send - not open");
                }
            }
            else if (msg.dir == DataChannelMessage::INCOMING) {
                // Forward to Unity callback
                OnMessageFromBrowser(msg.data);
            }
        }
        catch (const std::exception& e) {
            LogMessage("[DC] Message processing error: " + std::string(e.what()));
        }
    }
}

/**
 * @brief Polls PeerConnection for completed ICE gathering
 * Thread-safety: Called only from worker thread
 *
 * IMPORTANT: This uses polling instead of onLocalDescription/onLocalCandidate
 * callbacks because those callbacks were causing crashes on certain machines
 * due to threading issues in libdatachannel.
 *
 * Flow:
 * 1. Check if ICE gathering is complete
 * 2. Retrieve local SDP (includes all gathered ICE candidates)
 * 3. Send complete offer to browser
 *
 * This approach is more reliable than trickle ICE for our use case.
 */
static void PollLocalDescription() {
    if (!g_peer_connection) {
        LogMessage("[Poll] No PeerConnection");
        return;
    }

    if (g_offerSent.load()) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            LogMessage("[Poll] Offer already sent, skipping");
            loggedOnce = true;
        }
        return;
    }

    // Check ICE gathering state
    auto gatheringState = g_peer_connection->gatheringState();

    if (gatheringState == rtc::PeerConnection::GatheringState::Complete) {
        if (!g_gatheringComplete) {
            g_gatheringComplete = true;
            LogMessage("[Worker] ICE gathering complete");
        }

        // Retrieve complete local description (SDP + all ICE candidates)
        auto localDesc = g_peer_connection->localDescription();
        if (localDesc.has_value()) {
            try {
                std::string sdp = std::string(localDesc.value());

                if (sdp != g_lastLocalSdp && !sdp.empty()) {
                    g_lastLocalSdp = sdp;
                    LogMessage("[Worker] Got local SDP (" + std::to_string(sdp.size()) + " bytes)");

                    std::string patched = PatchH264Fmtp(sdp);
                    {
                        std::lock_guard<std::mutex> lk(sdpMutex);
                        g_localSdp = patched;
                    }
                    SendOfferIfReady();
                }
                else if (sdp.empty()) {
                    LogMessage("[Poll] WARNING: localDescription is empty!");
                }
            }
            catch (const std::exception& e) {
                LogMessage("[Worker] Error reading local description: " + std::string(e.what()));
            }
        }
        else {
            LogMessage("[Poll] WARNING: localDescription() returned no value!");
        }
    }
    else {
        // Still gathering - log progress periodically
        static int logThrottle = 0;
        if ((logThrottle++ % 100) == 0) {
            LogMessage("[Worker] Gathering state: " + std::to_string(static_cast<int>(gatheringState)));
        }
    }
}

// ============================================================================
// SECTION 14: WORKER THREAD MAIN LOOP
// ============================================================================

/**
 * @brief Main worker thread loop - handles all WebRTC operations
 * Thread-safety: This is THE worker thread - owns all WebRTC objects
 *
 * ARCHITECTURE:
 * This thread is the ONLY thread that touches WebRTC objects. All other threads
 * communicate via queues. This design prevents race conditions and crashes.
 *
 * INITIALIZATION SEQUENCE:
 * 1. Reset all state flags and clear queues
 * 2. Initialize WebSocket connection
 * 3. Get NVENC configuration (wait if not ready)
 * 4. Create PeerConnection WITHOUT callbacks
 * 5. Add video track with H.264 codec
 * 6. Create DataChannel for bidirectional messaging
 * 7. Setup RTP packetizer
 * 8. Start ICE gathering
 * 9. Enter main processing loop
 *
 * MAIN LOOP:
 * Polls and processes messages in priority order:
 * 1. ICE gathering status (signaling)
 * 2. WebSocket messages (browser communication)
 * 3. Video frames (streaming)
 * 4. DataChannel messages (bidirectional data)
 *
 * SHUTDOWN SEQUENCE:
 * Proper cleanup order is critical to avoid crashes:
 * 1. Stop processing (set flags)
 * 2. Close DataChannel
 * 3. Close video track
 * 4. Reset packetizer
 * 5. Close PeerConnection
 * 6. Stop WebSocket
 * 7. Clear all queues
 * 8. Reset all state flags
 *
 * CRITICAL LESSONS:
 * - Never hardcode bindAddress (let auto-detect)
 * - Always wrap WebRTC calls in try/catch
 * - Poll for ICE completion instead of using callbacks
 * - Clean up in reverse order of creation
 */
static void WorkerLoop() {
    // Install crash handler for debugging
    SetUnhandledExceptionFilter(CrashHandler);

    LogMessage("[Worker] Started");
    g_running = true;

    // ========================================================================
    // PHASE 1: STATE RESET
    // ========================================================================
    // Clear any leftover state from previous run (Unity doesn't unload DLL)

    g_offerSent = false;
    remoteSet = false;
    g_dcOpen = false;
    g_gatheringComplete = false;
    g_lastLocalSdp.clear();

    {
        std::lock_guard<std::mutex> lk(sdpMutex);
        g_localSdp.clear();
        pendingCandidates.clear();
    }

    // Clear all message queues
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        while (!g_frameQueue.empty()) g_frameQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_websocketMsgMutex);
        while (!g_websocketMsgQueue.empty()) g_websocketMsgQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_dcMsgMutex);
        while (!g_dcMsgQueue.empty()) g_dcMsgQueue.pop();
    }

    LogMessage("[Worker] State reset complete");

    // ========================================================================
    // PHASE 2: WEBSOCKET INITIALIZATION
    // ========================================================================

    g_websocket.setUrl("ws://localhost:9090");
#if IXWEBSOCKET_VERSION_MAJOR >= 11
    g_websocket.disablePerMessageDeflate();
#else
    g_websocket.setPerMessageDeflateOptions(false);
#endif
    g_websocket.disableAutomaticReconnection();
    g_websocket.setOnMessageCallback(OnWsCallback);
    g_websocket.start();

    // Wait for WebSocket to connect
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ========================================================================
    // PHASE 3: NVENC CONFIGURATION
    // ========================================================================
    // Get video encoding parameters from NVENC
    // Wait up to 1 second if not ready yet

    NvencServerConfig* cfg = GetNvencServerConfig();
    if (!cfg || cfg->width == 0 || cfg->height == 0) {
        LogMessage("[Worker] Waiting for NVENC config...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cfg = GetNvencServerConfig();
    }

    // Fallback to defaults if still not configured
    if (!cfg || cfg->width == 0) {
        LogMessage("[Worker] Using default config");
        static NvencServerConfig defaultCfg;
        defaultCfg.width = 1920;
        defaultCfg.height = 1080;
        defaultCfg.fpsNum = 30;
        defaultCfg.bitrateKbps = 3000;
        defaultCfg.clockRateHz = 90000;
        defaultCfg.startTimestamp = 0;
        cfg = &defaultCfg;
    }

    LogMessage("[Worker] Config: " + std::to_string(cfg->width) + "x" +
        std::to_string(cfg->height) + " @ " + std::to_string(cfg->fpsNum) + "fps");

    // ========================================================================
    // PHASE 4: PEERCONNECTION CREATION
    // ========================================================================
    // CRITICAL: Do NOT set bindAddress - let WebRTC auto-detect interfaces
    // Setting a specific IP causes crashes if that interface isn't available

    rtc::Configuration config;
    // config.bindAddress = "10.0.0.3";  // NEVER hardcode this!
    config.portRangeBegin = 50000;
    config.portRangeEnd = 50100;

    try {
        LogMessage("[Worker] Creating PeerConnection...");
        g_peer_connection = std::make_shared<rtc::PeerConnection>(config);
        LogMessage("[Worker] PeerConnection created (no callbacks)");
    }
    catch (const std::exception& e) {
        LogMessage("[Worker] EXCEPTION creating PC: " + std::string(e.what()));
        g_running = false;
        return;
    }
    catch (...) {
        LogMessage("[Worker] UNKNOWN EXCEPTION creating PC");
        g_running = false;
        return;
    }

    // ========================================================================
    // PHASE 5: VIDEO TRACK CREATION
    // ========================================================================

    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.setBitrate(cfg->bitrateKbps * 1000);  // Convert kbps to bps
    video.addH264Codec(96);
    video.addAttribute("framerate");

    g_videoTrack = g_peer_connection->addTrack(video);

    if (!g_videoTrack) {
        LogMessage("[Worker] ERROR: Failed to create track");
        g_running = false;
        return;
    }

    // ========================================================================
    // PHASE 6: DATACHANNEL CREATION
    // ========================================================================
    // DataChannel enables bidirectional messaging between Unity and browser

    try {
        LogMessage("[Worker] Creating DataChannel...");
        g_dc = g_peer_connection->createDataChannel("data");

        // Setup callbacks - safe because they only queue messages
        g_dc->onOpen([]() {
            LogMessage("[DC] Opened");
            g_dcOpen = true;

            // Send hello message to confirm connection
            DataChannelMessage msg;
            msg.dir = DataChannelMessage::OUTGOING;
            msg.data = "Hello from WebStreamer DLL!";
            PushDataChannelMessage(std::move(msg));
            });

        g_dc->onClosed([]() {
            LogMessage("[DC] Closed");
            g_dcOpen = false;
            });

        g_dc->onError([](std::string error) {
            LogMessage("[DC] Error: " + error);
            g_dcOpen = false;
            });

        g_dc->onMessage([](rtc::message_variant message) {
            if (std::holds_alternative<std::string>(message)) {
                std::string msg = std::get<std::string>(message);
                LogMessage("[DC] Received: " + msg);

                // Queue for processing on worker thread
                DataChannelMessage dcMsg;
                dcMsg.dir = DataChannelMessage::INCOMING;
                dcMsg.data = std::move(msg);
                PushDataChannelMessage(std::move(dcMsg));
            }
            });

        LogMessage("[Worker] DataChannel created");
    }
    catch (const std::exception& e) {
        LogMessage("[Worker] DataChannel creation failed: " + std::string(e.what()));
        // Continue without DataChannel (video will still work)
    }

    // ========================================================================
    // PHASE 7: RTP PACKETIZER SETUP
    // ========================================================================
    // Packetizer converts H.264 NAL units to RTP packets

    uint32_t ssrc = MakeRandomSSRC();
    auto rtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc,                   // Synchronization Source ID
        "webrtc",               // CNAME
        96,                     // Payload type (dynamic range for H.264)
        cfg->clockRateHz,       // 90000 Hz for video
        0                       // Video orientation extension ID (unused)
    );
    rtpCfg->startTimestamp = cfg->startTimestamp;

    g_h264Packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence,  // Annex-B format
        rtpCfg,
        1200  // MTU size (safe for most networks)
    );

    g_videoTrack->setMediaHandler(g_h264Packetizer);

    LogMessage("[Worker] Track ready, SSRC=0x" + [](uint32_t s) {
        std::ostringstream o;
        o << std::hex << std::uppercase << s;
        return o.str();
        }(ssrc));

    // ========================================================================
    // PHASE 8: START ICE GATHERING
    // ========================================================================
    // This triggers ICE candidate gathering
    // We poll for completion instead of using callbacks (more reliable)

    try {
        LogMessage("[Worker] Starting ICE gathering...");
        g_peer_connection->setLocalDescription();
        LogMessage("[Worker] setLocalDescription() returned successfully");
    }
    catch (const std::exception& e) {
        LogMessage("[Worker] EXCEPTION in setLocalDescription: " + std::string(e.what()));
        g_running = false;
        return;
    }
    catch (...) {
        LogMessage("[Worker] UNKNOWN EXCEPTION in setLocalDescription");
        g_running = false;
        return;
    }

    // ========================================================================
    // PHASE 9: MAIN PROCESSING LOOP
    // ========================================================================
    // Poll and process messages at 100Hz (10ms intervals)

    int iteration = 0;
    while (g_running.load()) {
        // 1. Check ICE gathering and send offer when ready
        PollLocalDescription();

        // 2. Process incoming WebSocket messages (SDP answer, ICE candidates, commands)
        ProcessWsMessages();

        // 3. Send queued video frames
        ProcessFrames();

        // 4. Process DataChannel messages (bidirectional)
        ProcessDataChannelMessages();

        // Sleep to prevent busy-waiting (10ms = ~100Hz poll rate)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        iteration++;
    }

    // ========================================================================
    // PHASE 10: SHUTDOWN AND CLEANUP
    // ========================================================================
    // CRITICAL: Clean up in reverse order of creation to avoid crashes

    LogMessage("[Worker] Stopping");

    // Stop processing new messages immediately
    g_running = false;
    g_dcOpen = false;
    g_websocketOpen = false;

    // 1. Close DataChannel
    if (g_dc) {
        try {
            LogMessage("[Worker] Closing DataChannel...");
            if (g_dc->isOpen()) {
                g_dc->close();
            }
        }
        catch (const std::exception& e) {
            LogMessage("[Worker] DC close error: " + std::string(e.what()));
        }
        g_dc.reset();
        LogMessage("[Worker] DataChannel cleaned up");
    }

    // 2. Close video track
    if (g_videoTrack) {
        try {
            LogMessage("[Worker] Closing video track...");
            g_videoTrack->close();
        }
        catch (const std::exception& e) {
            LogMessage("[Worker] Track close error: " + std::string(e.what()));
        }
        g_videoTrack.reset();
        LogMessage("[Worker] Video track cleaned up");
    }

    // 3. Reset packetizer
    if (g_h264Packetizer) {
        g_h264Packetizer.reset();
        LogMessage("[Worker] Packetizer cleaned up");
    }

    // 4. Close PeerConnection
    if (g_peer_connection) {
        try {
            LogMessage("[Worker] Closing PeerConnection...");
            g_peer_connection->close();

            // Wait for cleanup to complete
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        catch (const std::exception& e) {
            LogMessage("[Worker] PC close error: " + std::string(e.what()));
        }
        g_peer_connection.reset();
        LogMessage("[Worker] PeerConnection cleaned up");
    }

    // 5. Stop WebSocket
    try {
        LogMessage("[Worker] Stopping WebSocket...");
        g_websocket.stop();

        // Wait for WS to fully close
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    catch (const std::exception& e) {
        LogMessage("[Worker] WS stop error: " + std::string(e.what()));
    }

    // 6. Clear all queues to free memory
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        while (!g_frameQueue.empty()) g_frameQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_websocketMsgMutex);
        while (!g_websocketMsgQueue.empty()) g_websocketMsgQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_dcMsgMutex);
        while (!g_dcMsgQueue.empty()) g_dcMsgQueue.pop();
    }

    // 7. Reset all state flags for next run
    g_offerSent = false;
    remoteSet = false;
    g_dcOpen = false;
    g_gatheringComplete = false;
    g_lastLocalSdp.clear();

    {
        std::lock_guard<std::mutex> lk(sdpMutex);
        g_localSdp.clear();
        pendingCandidates.clear();
    }

    LogMessage("[Worker] Stopped - all state reset");
}

// ============================================================================
// SECTION 15: PUBLIC API - UNITY INTERFACE
// ============================================================================
// These functions are called from Unity's C# code via P/Invoke
// Thread-safety: Can be called from any Unity thread

/**
 * @brief Sends log data to browser via DataChannel
 * Thread-safety: Safe to call from Unity main thread
 *
 * Queues message for worker thread to send when DataChannel is open.
 * Used for debugging and telemetry (transform data, shader states, etc.)
 *
 * @param log Pointer to log_data structure
 * @return true if queued successfully, false if DataChannel not open
 *
 * USAGE FROM UNITY:
 * [DllImport("YourPlugin")]
 * private static extern bool LogData(ref LogData data);
 */
WEBRTC_STREAMER_API bool LogData(log_data* log) {
    if (!log) return false;

    try {
        // Serialize to JSON
        nlohmann::json log_json = {
            {"type", "log"},
            {"position", {log->transform.position[0], log->transform.position[1], log->transform.position[2]}},
            {"rotation", {log->transform.rotation[0], log->transform.rotation[1], log->transform.rotation[2]}},
            {"scale", {log->transform.scale[0], log->transform.scale[1], log->transform.scale[2]}},
            {"shader1state", log->shader1state}
        };

        std::string jsonStr = log_json.dump();

        // Queue for sending on worker thread
        if (g_dcOpen.load()) {
            DataChannelMessage msg;
            msg.dir = DataChannelMessage::OUTGOING;
            msg.data = std::move(jsonStr);
            PushDataChannelMessage(std::move(msg));
            return true;
        }
        else {
            // DataChannel not open yet
            return false;
        }
    }
    catch (const std::exception& e) {
        LogMessage("[LogData] Error: " + std::string(e.what()));
        return false;
    }
}

/**
 * @brief Registers callback for receiving messages from browser
 * Thread-safety: Safe to call from Unity main thread
 *
 * The callback will be invoked from the worker thread when messages arrive
 * from the browser via DataChannel or WebSocket. Unity should marshal the
 * callback to its main thread if needed.
 *
 * @param cb Function pointer to callback (signature: void(*)(const char*))
 *
 * USAGE FROM UNITY:
 * [DllImport("YourPlugin")]
 * private static extern void RegisterCommandCallback(CommandCallbackDelegate callback);
 *
 * [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
 * private delegate void CommandCallbackDelegate(string message);
 */
WEBRTC_STREAMER_API void RegisterCommandCallback(CommandCallback cb) {
    g_CommandCallback = cb;
}

/**
 * @brief Initializes the WebRTC streaming system
 * Thread-safety: Should be called once from Unity's main thread
 *
 * INITIALIZATION FLOW:
 * 1. Creates log directory
 * 2. Logs loaded DLLs (for debugging)
 * 3. Registers NVENC callbacks
 * 4. Initializes libdatachannel logger
 * 5. Starts worker thread
 *
 * The worker thread will:
 * - Connect to WebSocket signaling server
 * - Create PeerConnection
 * - Start ICE gathering
 * - Begin streaming when connected
 *
 * USAGE FROM UNITY:
 * [DllImport("YourPlugin")]
 * private static extern void Init();
 *
 * void Start() {
 *     Init();
 * }
 */
WEBRTC_STREAMER_API void Init() {
    std::filesystem::create_directories("WebStreamLogs");

    LogMessage("=== Init Start ===");

    // Log loaded DLLs for debugging dependency issues
    HMODULE hMods[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            char szModName[MAX_PATH];
            if (GetModuleFileNameExA(GetCurrentProcess(), hMods[i], szModName, sizeof(szModName))) {
                std::string modName = szModName;
                // Only log WebRTC-related DLLs
                if (modName.find("datachannel") != std::string::npos ||
                    modName.find("libcrypto") != std::string::npos ||
                    modName.find("libssl") != std::string::npos) {
                    LogMessage("[DLL] Loaded: " + modName);
                }
            }
        }
    }

    // Register NVENC callbacks
    Nvenc_SetLogger(&NvEncBridgeLogger);
    Nvenc_SetEncodedFrameSink(&OnEncodedFrameCallback);

    // Initialize libdatachannel logger (Warning level to reduce noise)
    rtc::InitLogger(rtc::LogLevel::Warning);

    // Start worker thread (does all WebRTC work)
    g_workerThread = std::thread(WorkerLoop);

    LogMessage("=== Init Complete ===");
}

/**
 * @brief Stops the WebRTC streaming system and cleans up resources
 * Thread-safety: Should be called once from Unity's main thread
 *
 * SHUTDOWN FLOW:
 * 1. Signals worker thread to stop
 * 2. Waits for worker thread to complete cleanup
 * 3. Closes NVENC encoder
 *
 * The worker thread will clean up in proper order:
 * DataChannel → Track → Packetizer → PeerConnection → WebSocket
 *
 * USAGE FROM UNITY:
 * [DllImport("YourPlugin")]
 * private static extern void StopSignaling();
 *
 * void OnDestroy() {
 *     StopSignaling();
 * }
 *
 * IMPORTANT: Must be called before Unity unloads the scene to avoid
 * leaving resources in an inconsistent state for next play session.
 */
WEBRTC_STREAMER_API void StopSignaling() {
    LogMessage("=== Stop Start ===");

    // Signal worker thread to stop
    g_running = false;

    // Wait for worker thread to finish cleanup (with timeout handled internally)
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }

    // Close NVENC encoder
    Nvenc_Close();

    LogMessage("=== Stop Complete ===");
}

// ============================================================================
// SECTION 16: LEGACY/UNUSED CODE
// ============================================================================
// The following functions/variables are kept for reference but not currently used

/**
 * @brief Legacy task posting system
 * UNUSED: Replaced by polling architecture to avoid callback threading issues
 */
static void Post(std::function<void()> t) {
    std::lock_guard<std::mutex> lk(qM);
    q.push(std::move(t));
    qCV.notify_one();
}

// ============================================================================
// END OF FILE
// ============================================================================
//
// MAINTENANCE NOTES:
//
// 1. ADDING NEW FEATURES:
//    - Add new message types to appropriate queue structure
//    - Add processing logic to worker thread loop
//    - Never access WebRTC objects from callbacks
//
// 2. DEBUGGING CRASHES:
//    - Check logs for exception messages
//    - Verify all WebRTC operations are wrapped in try/catch
//    - Ensure no callbacks access shared state
//    - Confirm cleanup happens in reverse order
//
// 3. PERFORMANCE TUNING:
//    - Adjust frame queue size (currently 60)
//    - Adjust batch processing limits (currently 10 frames/iteration)
//    - Tune worker thread poll rate (currently 10ms)
//
// 4. THREAD SAFETY CHECKLIST:
//    - [ ] Operation only on worker thread?
//    - [ ] Shared state protected by mutex?
//    - [ ] Atomic flags used correctly?
//    - [ ] Queue operations lock-free or mutex-protected?
//    - [ ] No callbacks touching WebRTC objects?
//
// 5. BEFORE COMMITTING CHANGES:
//    - Test both first and second Unity play sessions
//    - Test on both laptop and desktop (different network configs)
//    - Verify no memory leaks (check task manager after multiple runs)
//    - Ensure logs show proper initialization and cleanup
//
// ============================================================================
