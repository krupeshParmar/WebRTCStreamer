// Copyright (c) Krupesh Parmar
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#define NOMINMAX
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

#pragma comment(lib, "ws2_32.lib")

// Globals
ix::WebSocket g_websocket;
std::atomic_bool g_connected = false;
static std::atomic<bool> g_offerSent{ false };

std::shared_ptr<rtc::PeerConnection> peer_connection;
static std::atomic<bool> remoteSet{ false };
std::shared_ptr<rtc::DataChannel> data_connection;
std::string offerDescription = "";
std::vector<rtc::Candidate> pendingCandidates;

typedef void (*CommandCallback)(const char* message);
CommandCallback g_CommandCallback = nullptr;

using json = nlohmann::json;
void OnMessageFromBrowser(std::string msg);

WEBRTC_STREAMER_API void NWR_SyncVideoSsrcFromOffer();
WEBRTC_STREAMER_API bool NWR_AddH264VideoMLine(int payloadType /* e.g., 96 */);
struct transform_component
{
	float position[3];
	float rotation[3];
	float scale[3];
};

struct log_data
{
	transform_component transform;
	bool shader1state;
};


using Clock = std::chrono::steady_clock;
auto t0 = Clock::now();
using ByteVec = std::vector<uint8_t>;
// ---- Video globals ----
static std::shared_ptr<rtc::Track> gVideoTrack;
static int      gV_W = 1920, gV_H = 1080, gV_FPS = 30, gV_BR = 6000;
static uint8_t  gPT = 96;                 // H.264 PT (dynamic)
static uint32_t gTs = 0x12345678;           // random start
static uint32_t gTs90k = 0;
static uint32_t gTsStep = 90000 / std::max(1u, static_cast<unsigned int>(gV_FPS));	// for 30 fps; set to 90000/fps
static uint16_t gSeq = 1;
static const size_t kMTU = 1200;
std::atomic<bool>     gVideoReady = false;
static bool     gCodecHeaderSent = false; // STAP-A sent
static std::vector<uint8_t> gSPS, gPPS;   // cached latest SPS/PPS from NVENC

static uint32_t gSSRC = 0;

static uint32_t MakeRandomSSRC() {
	static std::mt19937 rng{ std::random_device{}() };
	static std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFFu);
	return dist(rng);
}

static std::ofstream s_Logger;
std::mutex log_mutex;

class TimestampManager {
private:
	uint32_t rtpTimestampBase = 0;
	uint64_t basePts100ns = 0;
	bool initialized = false;

public:
	uint32_t convertPtsToRtp(uint64_t pts100ns) {
		if (!initialized) {
			basePts100ns = pts100ns;
			rtpTimestampBase = static_cast<uint32_t>(rand()); // Random start
			initialized = true;
			return rtpTimestampBase;
		}

		// Convert 100ns units to 90kHz RTP timestamp
		uint64_t deltaPts100ns = pts100ns - basePts100ns;
		uint64_t deltaPtsUs = deltaPts100ns / 10;  // 100ns -> microseconds
		uint32_t rtpDelta = static_cast<uint32_t>((deltaPtsUs * 90) / 1000); // us -> 90kHz

		return rtpTimestampBase + rtpDelta;
	}
};

static TimestampManager tsManager;

std::string GetTimestamp()
{
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

void LogMessage(std::string msg) {
	LogMessage("%s", msg.c_str());
}

static void NvEncBridgeLogger(const char* msg) { LogMessage(msg); }
static void OnEncodedFrame(const uint8_t* data, int bytes, uint64_t pts90k, bool key);

//static std::unique_ptr<H264RTPSender> rtpSender = nullptr;
using ByteVec = std::vector<uint8_t>;
struct H264_Frame
{
	ByteVec bytevec;
	bool isIDR = false;
	uint32_t ts90k = 0;
};
struct SimplePacer {
	std::atomic<bool> running{ false };
	std::thread th;
	std::mutex m;
	std::condition_variable cv;

	H264_Frame latest;
	bool hasFrame = false;

	int targetFps = 30;
	std::chrono::steady_clock::time_point nextSend;

	std::shared_ptr<rtc::Track> track;
	int payloadType = 96;

	void start(int fps) {
		/*if (rtpSender) {
			rtpSender->reset();
		}
		else {
			rtpSender = std::make_unique<H264RTPSender>();
		}*/
		LogMessage("[Video] RTP sender initialized");
		targetFps = (fps > 0 ? fps : 30);
		running = true;
		nextSend = std::chrono::steady_clock::now();
		th = std::thread([this] { run(); });
		LogMessage("[Pacer] started");
	}

	void stop() {
		running = false;
		cv.notify_all();
		if (th.joinable()) th.join();
		LogMessage("[Pacer] stopped");
	}

	void push(ByteVec&& frame, bool iskey, uint32_t ts90k) {
		{
			std::lock_guard<std::mutex> lk(m);
			latest.bytevec = std::move(frame);
			latest.isIDR = iskey;
			latest.ts90k = ts90k;
			hasFrame = true;
		}
		cv.notify_one();
	}

private:
	void run() {
		using namespace std::chrono;
		const auto frameDur = duration<double>(1.0 / double(targetFps));

		size_t sent = 0;
		auto lastLog = steady_clock::now();

		while (running) {
			std::unique_lock<std::mutex> lk(m);
			cv.wait_until(lk, nextSend, [this] { return !running || hasFrame; });
			if (!running) break;

			auto now = steady_clock::now();
			if (now < nextSend) {
				continue;
			}

			H264_Frame frame;
			if (hasFrame) {
				frame.bytevec.swap(latest.bytevec);
				hasFrame = false;
			}
			lk.unlock();

			if (!frame.bytevec.empty()) {
				//rtpSender->sendAccessUnit(frame.bytevec/*, frame.bytevec.size(), frame.ts90k*/);
				//++sent;
				
				// Send via libdatachannel track; packetizer must be attached once at setup.
				rtc::binary bin(frame.bytevec.size());
				std::transform(frame.bytevec.begin(), frame.bytevec.end(), bin.begin(),
					[](uint8_t c) { return std::byte(c); });

				bool ok = track && track->send(bin);
				if (!ok) {
					LogMessage("[Pacer] track->send() failed");
				}
				else {
					++sent;
				}
			}
			else {
				
			}

			nextSend += duration_cast<steady_clock::duration>(frameDur);

			now = steady_clock::now();
			if (nextSend < now - milliseconds(5)) {
				nextSend = now + duration_cast<steady_clock::duration>(frameDur);
			}

			if (now - lastLog >= seconds(1)) {
				LogMessage("[Pacer] fps=" + std::to_string(sent) +
					" queue=" + std::to_string(hasFrame ? 1 : 0));
				sent = 0;
				lastLog = now;
			}
		}
	}
};
SimplePacer gPacer;


static inline rtc::binary ToBinary(const uint8_t* data, size_t len) {
	rtc::binary b(len);
	std::transform(data, data + len, b.begin(),
		[](uint8_t c) { return static_cast<std::byte>(c); });
	return b;
}

static std::string PatchH264Fmtp(const std::string& sdp)
{
	// 1) Split into lines, collect mids and remember header end
	std::vector<std::string> lines;
	lines.reserve(256);
	{
		std::stringstream ss(sdp);
		std::string ln;
		while (std::getline(ss, ln)) {
			if (!ln.empty() && ln.back() == '\r') ln.pop_back();
			lines.push_back(ln);
		}
	}

	// Find where the session header ends (after the first "t=" line)
	int headerEnd = -1;
	for (int i = 0; i < (int)lines.size(); ++i) {
		if (lines[i].rfind("t=", 0) == 0) { headerEnd = i; break; }
	}
	if (headerEnd < 0) headerEnd = 0; // fallback

	// 2) Remove any existing group:BUNDLE; collect mids; normalize fmtp and m=video port
	std::vector<std::string> mids;
	std::vector<std::string> out;
	out.reserve(lines.size() + 4);

	for (int i = 0; i < (int)lines.size(); ++i) {
		const std::string& L = lines[i];

		// skip existing group lines; we’ll rebuild once
		if (L.rfind("a=group:BUNDLE", 0) == 0) continue;

		// collect mids
		if (L.rfind("a=mid:", 0) == 0) {
			mids.push_back(L.substr(6));
		}

		// force m=video port 9 (not 0)
		if (L.rfind("m=video ", 0) == 0) {
			// m=video <port> ...
			std::string fixed = L;
			// replace the first token after "m=video " with "9"
			size_t pos = std::string("m=video ").size();
			size_t sp = fixed.find(' ', pos);
			if (sp != std::string::npos) {
				fixed.replace(pos, sp - pos, "9");
			}
			out.push_back(fixed);
			continue;
		}

		// normalize H264 fmtp
		if (L.rfind("a=fmtp:96", 0) == 0) {
			out.push_back("a=fmtp:96 packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1");
			continue;
		}
		if (L == "a=framerate") {
			out.push_back("a=framerate:30");
			continue;
		}

		out.push_back(L);
	}

	// 3) Insert a single correct group line AFTER the header block
	if (!mids.empty()) {
		std::string group = "a=group:BUNDLE";
		for (auto& m : mids) group += " " + m;
		out.insert(out.begin() + std::min((size_t)headerEnd + 1, out.size()), group);
	}

	{
		const uint32_t ssrc = gSSRC ? gSSRC : 0xA71DB857; // or whatever you already chose
		const std::string ssrcStr = std::to_string(ssrc);
	
		// Find the video mid line
		for (size_t i = 0; i < out.size(); ++i) {
			if (out[i].rfind("a=mid:video", 0) == 0) {
	
				// Avoid double-injecting if already present
				bool alreadyHas = false;
				for (size_t k = i + 1; k < std::min(out.size(), i + 10); ++k) {
					if (out[k].rfind("a=ssrc:", 0) == 0 || out[k].rfind("a=msid:", 0) == 0) {
						alreadyHas = true; break;
					}
					if (out[k].rfind("m=", 0) == 0) break; // next m= section
				}
				if (alreadyHas) break;
	
				// Insert immediately after a=mid:video
				std::vector<std::string> ins = {
					"a=msid:stream v",
					"a=ssrc:" + ssrcStr + " cname:webrtc",
					"a=ssrc:" + ssrcStr + " msid:stream v",
					"a=ssrc:" + ssrcStr + " mslabel:stream",
					"a=ssrc:" + ssrcStr + " label:v"
				};
				out.insert(out.begin() + i + 1, ins.begin(), ins.end());
				break;
			}
		}
	}

	// 4) Rejoin with CRLF
	std::string sdpOut;
	for (auto& L : out) sdpOut += L + "\r\n";
	return sdpOut;
}

void SendSDPOfferToBrowser() {
	if (offerDescription.empty()) return;
	if (!g_connected) return;
	if (g_offerSent.load()) return;
	json offer = {
		{"type", "sdp-offer"},
		{"sdp", offerDescription}
	};
	g_websocket.send(offer.dump());
	g_offerSent.store(true);
	LogMessage("[WS] SDP Offer sent to browser!");
}

void StartSignalling() {
	if (!peer_connection.get()) {
		LogMessage("[PC] peer_connection is null!");
		return;
	}
	peer_connection->setLocalDescription();
	LogMessage("[PC] Offer sent to Browser");
}

void CreatePeerAndOffer() {
	pendingCandidates.clear();
	remoteSet.store(false);
	g_offerSent.store(false);
	rtc::Configuration config;
	config.bindAddress = "10.0.0.3";
	config.portRangeBegin = 50000;
	config.portRangeEnd = 50100;
	config.iceServers.clear();
	/*config.iceServers.emplace_back("stun:stun1.l.google.com:19302");
	config.iceServers.emplace_back("stun:stun2.l.google.com:19302");*/
	peer_connection = std::make_shared<rtc::PeerConnection>(config);
	NWR_AddH264VideoMLine(96);
	peer_connection->onLocalDescription([](rtc::Description desc) {
		/*if (!offerDescription.empty())
		{
			LogMessage("[WebRTC][DLL] onLocalDescription already called before");
			return;
		}*/
		offerDescription = PatchH264Fmtp(std::string(desc));

		LogMessage("[WebRTC][DLL] onLocalDescription triggered: " + offerDescription);
		if (g_connected) SendSDPOfferToBrowser();
		//NWR_SyncVideoSsrcFromOffer();
		});

	data_connection = peer_connection->createDataChannel("data");

	data_connection->onOpen([]() {
		data_connection->send("Hello from WebStreamer DLL!");
		LogMessage("[WebRTC][DLL] DataChannel opened");
		});

	data_connection->onMessage([](rtc::message_variant message) {
		if (std::holds_alternative<std::string>(message)) {
			OnMessageFromBrowser(std::get<std::string>(message).c_str());
		}
		});
	peer_connection->onStateChange([](rtc::PeerConnection::State state) {
		std::stringstream ss;
		ss << static_cast<int>(state);
		LogMessage("[WebRTC][DLL] PeerConnection state: " + ss.str());
		});

	peer_connection->onGatheringStateChange([](rtc::PeerConnection::GatheringState gs) {
		LogMessage(std::string("[ICE] gathering=") + std::to_string((int)gs));
		/*if (gs == rtc::PeerConnection::GatheringState::Complete && !g_offerSent.exchange(true)) {
			if (!offerDescription.empty() && g_connected) {
				LogMessage("[ICE] Complete -> sending SDP offer with candidates");
				SendSDPOfferToBrowser();
			}
		}*/
	});

	peer_connection->onLocalCandidate([](rtc::Candidate candidate) {
		std::string mid = candidate.mid();            // <-- critical
		if (mid.empty()) mid = "video";          // fallback
		const std::string candStr = candidate.candidate();
		if (candStr.find("typ srflx") != std::string::npos &&
			(candStr.find(" raddr 0.0.0.0") != std::string::npos ||
				candStr.find(" rport 0") != std::string::npos)) {
			LogMessage("[ICE OUT] drop malformed srflx: " + candStr);
			return;
		}
		json cand = {
			{"type", "ice-candidate"},
			{"candidate", {
				{"candidate", candidate.candidate()},
				{"sdpMid", mid},
				{"sdpMLineIndex", 0}
			}}
		};
		g_websocket.send(cand.dump());
		LogMessage("[ICE OUT] mid=" + candStr);
		LogMessage("[WS] ICE candidate sent");
		});

	peer_connection->setLocalDescription();
}

static void FlushPendingCandidates() {

	LogMessage("[WEBRTC] [DLL] Pending Candidates size: " + std::to_string(pendingCandidates.size()));
	if (!peer_connection || !remoteSet)
	{
		LogMessage("[WEBRTC] [DLL] Peer Connection and/or Remote Set is/are false");
		return;
	}
	for (auto &c : pendingCandidates) {
		try { 
			LogMessage("[WEBRTC] [DLL] " + std::string(c.candidate().c_str()));
			peer_connection->addRemoteCandidate(c); 
		}
		catch (const std::exception& e) { LogMessage(std::string("[PC] addRemoteCandidate err: ") + e.what()); }
	}
	pendingCandidates.clear();
}

void InitSocket(const std::string& host = "127.0.0.1", const std::string& port = "9090") {
	LogMessage("[WS] Initializing socket");
	g_websocket.setUrl("ws://localhost:9090");

	g_websocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {
		using ix::WebSocketMessageType;

		std::stringstream ss;
		switch (msg->type) {
		case WebSocketMessageType::Open:
			g_connected = true;

			// Register Unity with the server
			g_websocket.send(R"({"type":"register","role":"unity"})");
			LogMessage("[WS] Connected to signaling server");
			if (offerDescription.empty()) {
				LogMessage("[WS] No cached offer -> generating now");
				CreatePeerAndOffer();
			}
			else {
				LogMessage("[WS] Have cached offer -> sending now");
				SendSDPOfferToBrowser();
			}
			break;

		case WebSocketMessageType::Close:
			ss << "[WS] Disconnected from signaling server" << msg->closeInfo.code
				<< " reason=" << msg->closeInfo.reason;
			LogMessage(ss.str());
			g_connected = false;
			break;

		case WebSocketMessageType::Error:
			LogMessage("[WS] Error: " + msg->errorInfo.reason);
			break;

		case WebSocketMessageType::Message:
			LogMessage("[WS] Received: " + msg->str);

			try {
				json j = json::parse(msg->str);
				const std::string type = j.value("type", "");
				if (type == "sdp-answer") {
					if (!peer_connection) { LogMessage("[WS] No peer_connection for sdp-answer"); return; }
					if (remoteSet.load()) { LogMessage("[WS] Remote already set, ignoring extra answer"); return; }

					const std::string sdp = j.value("sdp", "");
					if (sdp.empty()) { LogMessage("[WS] sdp-answer missing sdp"); return; }
					try {
						auto j = json::parse(msg->str);
						LogMessage("[WS] SDP string: " + sdp);
						rtc::Description desc(sdp, "answer");
						peer_connection->setRemoteDescription(desc);
						remoteSet.store(true);
						LogMessage("[WS] setRemoteDescription success");
						FlushPendingCandidates();
					}
					catch (const std::exception& e) {
						LogMessage(std::string("[WS] Exception in setRemoteDescription: ") + e.what());
					}
					catch (...) {
						LogMessage("[WS] Unknown exception in setRemoteDescription");
					}
				}
				else if (type == "command") {
					if (j.contains("data")) {
						OnMessageFromBrowser(j["data"].get<std::string>());
					}
				}
				else if (type == "ice-candidate") {
					if (!peer_connection) { LogMessage("[WS] No peer_connection for ICE"); return; }
					/*try {
						std::string candidateStr = j["candidate"]["candidate"];
						std::string sdpMid = j["candidate"]["sdpMid"];
						LogMessage("Adding ICE candidate: " + candidateStr + " mid: " + sdpMid);
						rtc::Candidate candidate = rtc::Candidate(
							candidateStr,
							sdpMid);
						if (!peer_connection->remoteDescription()) {
							pendingCandidates.push_back(candidate);
						}
						else {
							peer_connection->addRemoteCandidate(candidate);
						}
						LogMessage("addRemoteCandidate success");
					}
					catch (const std::exception& e) {
						LogMessage(std::string("Exception in addRemoteCandidate: ") + e.what());
					}
					catch (...) {
						LogMessage("Unknown exception in addRemoteCandidate");
					}*/
					if (!j.contains("candidate")) { LogMessage("[WS] ICE missing candidate"); return; }
					const auto& jc = j["candidate"];
					const std::string candStr = jc.value("candidate", "");
					std::string sdpMid = jc.value("sdpMid", "");

					LogMessage("[WS] Adding ICE candidate: " + candStr + " mid: " + sdpMid);

					try {
						rtc::Candidate cand(candStr, sdpMid);
						if (!remoteSet.load()) {
							pendingCandidates.push_back(cand);
							LogMessage("[WS] Queued ICE (remote not set yet). Queue size=" + std::to_string(pendingCandidates.size()));
						}
						else {
							peer_connection->addRemoteCandidate(cand);
							LogMessage("[WS] addRemoteCandidate OK (immediate)");
						}
					}
					catch (const std::exception& e) {
						LogMessage(std::string("[WS] addRemoteCandidate exception: ") + e.what());
					}
					LogMessage("[WS] Received and added ICE candidate");
				}
			}
			catch (...) {
				std::cerr << "[WS] Failed to parse JSON message\n";
			}
			break;
		}
		});

	g_websocket.start();
}

static inline bool IsVideoReady()
{
	if (!peer_connection) 
	{
		LogMessage("[WEBRTC] [DLL] Peer Connection is null");
		return false;
	}
	auto st = peer_connection->state();
	if (st != rtc::PeerConnection::State::Connected)
	{
		LogMessage("[WEBRTC] [DLL] Peer Connection is not connected");
		return false;
	}
	if (!gVideoTrack)
	{
		LogMessage("[WEBRTC] [DLL] Video Track is null");
		return false;
	}
	if (!gVideoTrack->isOpen())
	{
		LogMessage("[WEBRTC] [DLL]  Video Track is not open");
		return false;
	}
	return true;
}

// Parse SSRC for the video m= section from the local SDP offer (so our RTP SSRC matches)
static uint32_t ParseVideoSsrcFromOffer(const std::string& sdp)
{
	size_t mpos = sdp.find("\nm=video ");
	if (mpos == std::string::npos) mpos = sdp.find("\r\nm=video ");
	if (mpos == std::string::npos) return 0;

	size_t nextM = sdp.find("\nm=", mpos + 1);
	if (nextM == std::string::npos) nextM = sdp.find("\r\nm=", mpos + 1);
	size_t sectionEnd = (nextM == std::string::npos) ? sdp.size() : nextM;

	size_t a = sdp.find("a=ssrc:", mpos);
	if (a == std::string::npos || a > sectionEnd) return 0;

	size_t p = a + 7;
	while (p < sdp.size() && sdp[p] == ' ') ++p;
	uint32_t ssrc = 0;
	while (p < sdp.size()) {
		char c = sdp[p++];
		if (c < '0' || c > '9') break;
		ssrc = ssrc * 10 + (c - '0');
	}
	return ssrc;
}

inline bool containsIDR(const uint8_t* data, size_t size,
	bool* outHasSPS = nullptr,
	bool* outHasPPS = nullptr)
{
	if (!data || size < 1) return false;
	if (outHasSPS) *outHasSPS = false;
	if (outHasPPS) *outHasPPS = false;

	auto read_u32_be = [](const uint8_t* p) -> uint32_t {
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
			(uint32_t(p[2]) << 8) | uint32_t(p[3]);
		};

	size_t i = 0;
	bool isAnnexB = false;

	for (size_t j = 0; j + 3 < size && j < 64; ++j) {
		if (data[j] == 0x00 && data[j + 1] == 0x00 &&
			((data[j + 2] == 0x01) || (data[j + 2] == 0x00 && data[j + 3] == 0x01))) {
			isAnnexB = true;
			break;
		}
	}

	if (isAnnexB) {
		while (i + 3 < size) {
			size_t z = i;
			int zeros = 0;
			while (z < size && data[z] == 0x00) { ++z; ++zeros; }
			if (z < size && data[z] == 0x01 && zeros >= 2) {
				size_t nal_start = z + 1;
				size_t k = nal_start;
				while (k + 3 < size) {
					if (data[k] == 0x00 && data[k + 1] == 0x00 &&
						((data[k + 2] == 0x01) || (data[k + 2] == 0x00 && data[k + 3] == 0x01)))
						break;
					++k;
				}
				if (nal_start < size) {
					uint8_t nal_hdr = data[nal_start];
					uint8_t nal_type = nal_hdr & 0x1F; // H.264
					if (nal_type == 7 && outHasSPS) *outHasSPS = true;	// SPS
					if (nal_type == 8 && outHasPPS) *outHasPPS = true;	// PPS
					if (nal_type == 5) return true;						// IDR
				}
				i = k;
			}
			else {
				++i;
			}
		}
		return false;
	}
	else {
		size_t pos = 0;
		while (pos + 4 <= size) {
			uint32_t nal_len = read_u32_be(data + pos);
			pos += 4;
			if (nal_len == 0 || pos + nal_len > size) break;
			uint8_t nal_hdr = data[pos];
			uint8_t nal_type = nal_hdr & 0x1F;
			if (nal_type == 7 && outHasSPS) *outHasSPS = true;
			if (nal_type == 8 && outHasPPS) *outHasPPS = true;
			if (nal_type == 5) return true;
			pos += nal_len;
		}
		return false;
	}
}

static bool AU_HasIDR(const uint8_t* p, size_t n) {
	auto isStart = [&](size_t k)->size_t {
		if (k + 3 < n && !p[k] && !p[k + 1] && !p[k + 2] && p[k + 3] == 1) return 4;
		if (k + 2 < n && !p[k] && !p[k + 1] && p[k + 2] == 1) return 3;
		return 0;
		};
	size_t i = 0, start = SIZE_MAX;
	while (i < n) {
		size_t sc = isStart(i);
		if (sc) {
			if (start != SIZE_MAX) {
				size_t len = i - start; while (len && p[start + len - 1] == 0) --len;
				if (len) { uint8_t h = p[start]; uint8_t t = h & 0x1F; if (t == 5) return true; }
			}
			start = i + sc; i += sc;
		}
		else ++i;
	}
	if (start != SIZE_MAX && start < n) {
		size_t len = n - start; while (len && p[start + len - 1] == 0) --len;
		if (len) { uint8_t h = p[start]; uint8_t t = h & 0x1F; if (t == 5) return true; }
	}
	return false;
}

static void OnEncodedFrame(const uint8_t* data, int bytes, uint64_t pts100ns, bool key)
{
	if (!IsVideoReady()) {
		static int throttle = 0;
		if ((throttle++ % 120) == 0)
			LogMessage("[RTP] Drop: PC/Track not ready");
		return;
	}
	if (!data || bytes <= 0) return;
	//-----1----------
	ByteVec au(bytes);
	std::memcpy(au.data(), data, bytes);
	bool idrInAu = AU_HasIDR(au.data(), au.size());
	LogMessage(std::string("[H264] AU key=") + (idrInAu ? "1" : "0") +
		" bytes=" + std::to_string(bytes));
	const uint32_t ts90k = static_cast<uint32_t>((pts100ns * 9) / 1000);

	gPacer.push(std::move(au), key, ts90k);
	//-----1----------
}

WEBRTC_STREAMER_API bool NWR_AddH264VideoMLine(int payloadType /* e.g., 96 */)
{
	try {
		gPT = uint8_t(payloadType);
		rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
		NWR_GetVideoDesc(&gV_W, &gV_H, &gV_FPS, &gV_BR);
		std::stringstream os;
		os << "[Video] Description set: Width=" << gV_W << "Height=" << gV_H << "FPS=" << gV_FPS << "Bitrate=" << gV_BR;
		LogMessage(os.str());
		video.setBitrate(3000000);
		video.addH264Codec(payloadType);
		video.addAttribute("framerate");
		gVideoTrack = peer_connection->addTrack(video);
		gSSRC = MakeRandomSSRC();
		const rtc::SSRC ssrc = gSSRC;
		auto rtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(
			ssrc,                          
			"webrtc",                      
			/*payloadType*/ payloadType,   
			/*clockRate*/ 90000,           
			/*videoOrientationId*/ 0       
		);
		auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
			rtc::NalUnit::Separator::StartSequence,
			rtpCfg,
			/*maxFragmentSize*/ 1200
		);
		gVideoTrack->setMediaHandler(packetizer);
		gPacer.track = gVideoTrack;
		gPacer.start(gV_FPS);
		//rtpSender->setSSRC(gSSRC);
		LogMessage("[WebRTC][DLL] Using SSRC 0x" + /* print hex */ [](uint32_t s) {
			std::ostringstream o; o << std::hex << std::uppercase << s; return o.str(); }(gSSRC));
		if (gVideoTrack) {
			gVideoTrack->onOpen([]() 
				{
					gVideoReady = true;
					//StartPacer();
					LogMessage("[NVENC] Video track OPEN"); 
				});
			gVideoTrack->onError([](std::string e) 
				{ 
					gVideoReady = false;
					LogMessage(std::string("[WebRTC][DLL] Video track ERROR: ") + e); 
				});
		}
		LogMessage("[NVENC] Added H264 video m= line (PT=" + std::to_string(payloadType) + ")");
		return true;
	}
	catch (const std::exception& e) {
		LogMessage(std::string("[NVENC] NWR_AddH264VideoMLine exception: ") + e.what());
		return false;
	}
}

// This reads the SSRC from our own offer so RTP SSRC matches.
WEBRTC_STREAMER_API void NWR_SyncVideoSsrcFromOffer()
{
	if (offerDescription.empty()) return;
	gSSRC = ParseVideoSsrcFromOffer(offerDescription);
	if (gSSRC == 0) LogMessage("[NVENC] Could not parse video SSRC from offer; using fallback 0x11223344");
	else LogMessage("[NVENC] Parsed video SSRC from offer: " + std::to_string(gSSRC));
}

WEBRTC_STREAMER_API bool LogData(log_data* log)
{
	if (!log) return false;
	json log_json = {
		{"type", "log"},
		{"position", {log->transform.position[0], log->transform.position[1], log->transform.position[2]}},
		{"rotation", {log->transform.rotation[0], log->transform.rotation[1], log->transform.rotation[2]}},
		{"scale", {log->transform.scale[0], log->transform.scale[1], log->transform.scale[2]}},
		{"shader1state", log->shader1state}
	};
	std::string jsonStr = log_json.dump();
	/*if(!s_Logger.is_open())
		s_Logger = std::ofstream{ "WebStreamLogs/logs.txt", std::ios::app };
	s_Logger << jsonStr << "\n";
	s_Logger.flush();*/

	// Send to browser over WebRTC if connected
	if (data_connection && data_connection->isOpen()) {
		data_connection->send(jsonStr);
		//LogMessage("[WebRTC][DLL] Message sent " + jsonStr);
	}

	return true;
}

WEBRTC_STREAMER_API
void RegisterCommandCallback(CommandCallback cb)
{
	g_CommandCallback = cb;
}

void OnMessageFromBrowser(std::string msg)
{
	if (g_CommandCallback)
		g_CommandCallback(msg.c_str());
	//LogMessage("[WebRTC][Browser]: " + msg);
}

void NWR_DestroyPeer()
{
	try {
		// stop NVENC first (flushes & releases)
		Nvenc_Close();
		gPacer.stop();
		//StopPacer();
		// drop video track
		if (gVideoTrack) {
			gVideoTrack->close();
			gVideoTrack.reset();
		}

		// close data channel
		if (data_connection) {
			data_connection->close();
			data_connection.reset();
		}

		// close PC
		if (peer_connection) {
			peer_connection->close();
			peer_connection.reset();
		}

		// stop websocket
		if (g_connected) {
			g_websocket.stop();
			g_connected = false;
		}

		// reset flags/buffers
		offerDescription.clear();
		pendingCandidates.clear();
		remoteSet = false;
		gSeq = 1;
		gCodecHeaderSent = false;
		gSPS.clear(); gPPS.clear();
	}
	catch (...) {
		LogMessage("[CLEANUP] exception during destroy");
	}
}

WEBRTC_STREAMER_API
void StopSignaling() {
	NWR_DestroyPeer();
}


WEBRTC_STREAMER_API void Init()
{
	NWR_DestroyPeer();
	std::filesystem::create_directories("WebStreamLogs");
	Nvenc_SetLogger(&NvEncBridgeLogger);
	Nvenc_SetEncodedFrameSink(&OnEncodedFrame);
	if (!s_Logger.is_open())
		s_Logger = std::ofstream{ "WebStreamLogs/logs.txt", std::ios::app };
	rtc::InitLogger(rtc::LogLevel::Info);
	CreatePeerAndOffer();
	InitSocket();
}

