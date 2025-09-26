//let pc = null;
let dataChannel = null;
let candidateQueue = [];
const videoEl = document.getElementById("unity");
const wsBadge = document.getElementById("wsStatus");
const pcBadge = document.getElementById("pcStatus");
const btnStart = document.getElementById("btnStart");
const ws = new WebSocket("ws://localhost:9090");

ws.onopen = () => {
  ws.send(JSON.stringify({ type: "register", role: "browser" }));
};

const pc = new RTCPeerConnection({
  iceServers: [
    // { urls: "stun:stun1.l.google.com:19302" },
    // { urls: "stun:stun2.l.google.com:19302" },
  ],
  iceTransportPolicy: "all"
});

videoEl.muted = true;       // autoplay policy
videoEl.playsInline = true; // iOS
videoEl.autoplay = true;
videoEl.controls = true;

pc.ontrack = (e) => {
  console.log("[JS] ontrack:", e.track.kind, e.streams?.[0]);
  videoEl.srcObject = e.streams[0] ?? new MediaStream([e.track]);
  videoEl.autoplay = true;
  videoEl.muted = true;
  videoEl.playsInline = true;
  videoEl.play().catch(()=>{});
};

//setInterval(() => {
//        console.log('Video state:', {
//            readyState: videoEl.readyState,
//            currentTime: videoEl.currentTime,
//            buffered: videoEl.buffered.length,
//            videoWidth: videoEl.videoWidth,
//            videoHeight: videoEl.videoHeight,
//            paused: videoEl.paused,
//            ended: videoEl.ended
//        });
//    }, 2000);

//     // Force video to play
//    videoEl.play().catch(e => console.log('Play failed:', e));
// Check WebRTC receiver stats
//setInterval(async () => {
//    const stats = await pc.getStats();
//    stats.forEach(report => {
//        if (report.type === 'inbound-rtp' && report.mediaType === 'video') {
//            console.log('WebRTC Video Stats:', {
//                framesReceived: report.framesReceived,
//                framesDecoded: report.framesDecoded,
//                framesDropped: report.framesDropped || 0,
//                keyFramesDecoded: report.keyFramesDecoded || 0,
//                totalDecodeTime: report.totalDecodeTime || 0
//            });
//        }
//    });
//  });

pc.onconnectionstatechange = () => console.log("[JS] pc.connectionState:", pc.connectionState);
pc.oniceconnectionstatechange = () => console.log("[JS] pc.iceConnectionState:", pc.iceConnectionState);
pc.onicegatheringstatechange  = () => console.log("[JS] pc.iceGatheringState:", pc.iceGatheringState);
pc.onsignalingstatechange     = () => console.log("[JS] pc.signalingState:", pc.signalingState);
pc.ondatachannel = (e) => console.log("[JS] ondatachannel:", e.channel.label);

pc.onicecandidate = (e) => {
  if (!e.candidate) return;
  console.log("[ICE OUT]", e.candidate.candidate, "mid=", e.candidate.sdpMid);
  // Send *exactly* { type:"ice-candidate", candidate:{ candidate, sdpMid } }
  const payload = {
    type: "ice-candidate",
    candidate: {
      candidate: e.candidate.candidate,
      sdpMid: e.candidate.sdpMid,      // IMPORTANT: don’t convert to index; keep the string mid
    },
  };
  console.log("[JS] sending ICE ->", payload);
  ws.send(JSON.stringify(payload));
};

ws.onopen = () => {
  console.log("[JS] WS open");
  ws.send(JSON.stringify({ type: "register", role: "browser" }));
};

ws.onmessage = async (event) => {
  let rawData = event.data;

  if (rawData instanceof Blob) {
    rawData = await rawData.text(); // Fix for blob transport
  }

  const msg = JSON.parse(rawData);
  console.log("[WS] Msg Received", msg);

  if (msg.type === "sdp-offer") {
    console.log("[Browser] Offer Received");
    console.log("Raw SDP string received:");
    console.log(msg.sdp);

    // ✅ Set up DataChannel handling before setting remote description
    pc.ondatachannel = (event) => {
      dataChannel = event.channel;

      dataChannel.onopen = () => {
        console.log("[Browser] DataChannel open");
        document.getElementById("sendCommand").onclick = () => {
          ws.send(JSON.stringify({ type: "command", data: "toggleShader" }));
        };
      };

      dataChannel.onmessage = (e) => {
        document.getElementById("logOutput").textContent += e.data + "\n";
      };

      dataChannel.onerror = (e) => {
        console.error("[Browser] DataChannel error", e);
      };
    };

    // 🧠 Perform SDP handshake
    await pc.setRemoteDescription({ type: "offer", sdp: msg.sdp });
    // Add any queued candidates now that remote description is set
    for (const candidate of candidateQueue) {
      try {
        await pc.addIceCandidate(candidate);
        console.log("[Browser] Added queued ICE candidate");
      } catch (e) {
        console.error("[Browser]23 Failed to add queued ICE candidate", e);
      }
    }
    candidateQueue = [];
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);

    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "sdp-answer",
        sdp: pc.localDescription.sdp
      }));
      console.log("[Browser] Sent SDP answer");
    } else {
      console.error("[Browser] WebSocket not open, can't send answer");
    }
  }

  else if (msg.type === "ice-candidate") {
    const candidate =  new RTCIceCandidate({
      candidate: msg.candidate.candidate,
      sdpMid: msg.candidate.sdpMid,
      sdpMLineIndex: 0
    });

    if (!pc || !pc.remoteDescription) {
      console.log("[Browser] Remote description not ready, queueing ICE candidate");
      candidateQueue.push(candidate);
    } else {
      try {
        await pc.addIceCandidate(candidate);
        console.log("[Browser] Added ICE candidate");
      } catch (e) {
        console.error("[Browser]56 Failed to add ICE candidate", e, candidate);
      }
    }
  }

  else if (msg.type === "log") {
    document.getElementById("logOutput").textContent += msg.data + "\n";
  }
};

/*
ws.onopen = () => {
  ws.send(JSON.stringify({ type: "register", role: "browser" }));
};

ws.onmessage = async (event) => {
  let rawData = event.data;

  if (rawData instanceof Blob) {
    rawData = await rawData.text(); // Fix for blob transport
  }

  const msg = JSON.parse(rawData);
  console.log("[WS] Msg Received", msg);

  if (msg.type === "sdp-offer") {
    console.log("[Browser] Offer Received");
    console.log("Raw SDP string received:");
    console.log(msg.sdp);
    // ✅ Create PeerConnection AFTER receiving offer
    pc = new RTCPeerConnection();

    // 📦 Set up ICE candidate sending
    pc.onicecandidate = (event) => {
      if (event.candidate && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({
          type: "ice-candidate",
          candidate: {
            candidate: event.candidate.candidate,
            sdpMid: event.candidate.sdpMid || "video" // Unity’s code sometimes uses "data" – both ok
          }
          
        }));
        console.log("[Browser] Sent ICE candidate");
      }
    };

    pc.oniceconnectionstatechange = () => {
      console.log("[Browser] ICE connection state:", pc.iceConnectionState);
    };

    pc.onconnectionstatechange = () => {
      console.log("[Browser] PeerConnection state:", pc.connectionState);
    };

    pc.ontrack = (e) => {
      if(e.streams && e.streams[0])
      {
        videoEl.srcObject = e.streams[0];
        } else {
          // Fallback: build a stream from the track
          const stream = new MediaStream([e.track]);
          videoEl.srcObject = stream;
        }
        videoEl.play().catch(()=>{ 
           });
      };

    // ✅ Set up DataChannel handling before setting remote description
    pc.ondatachannel = (event) => {
      dataChannel = event.channel;

      dataChannel.onopen = () => {
        console.log("[Browser] DataChannel open");
        document.getElementById("sendCommand").onclick = () => {
          ws.send(JSON.stringify({ type: "command", data: "toggleShader" }));
        };
      };

      dataChannel.onmessage = (e) => {
        document.getElementById("logOutput").textContent += e.data + "\n";
      };

      dataChannel.onerror = (e) => {
        console.error("[Browser] DataChannel error", e);
      };
    };

    // 🧠 Perform SDP handshake
    await pc.setRemoteDescription({ type: "offer", sdp: msg.sdp });
    // Add any queued candidates now that remote description is set
    for (const candidate of candidateQueue) {
      try {
        await pc.addIceCandidate(candidate);
        console.log("[Browser] Added queued ICE candidate");
      } catch (e) {
        console.error("[Browser]23 Failed to add queued ICE candidate", e);
      }
    }
    candidateQueue = [];
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);

    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: "sdp-answer",
        sdp: pc.localDescription.sdp
      }));
      console.log("[Browser] Sent SDP answer");
    } else {
      console.error("[Browser] WebSocket not open, can't send answer");
    }
  }

  else if (msg.type === "ice-candidate") {
    const candidate =  new RTCIceCandidate({
      candidate: msg.candidate.candidate,
      sdpMid: msg.candidate.sdpMid,
      sdpMLineIndex: 0
    });

    if (!pc || !pc.remoteDescription) {
      console.log("[Browser] Remote description not ready, queueing ICE candidate");
      candidateQueue.push(candidate);
    } else {
      try {
        await pc.addIceCandidate(candidate);
        console.log("[Browser] Added ICE candidate");
      } catch (e) {
        console.error("[Browser]56 Failed to add ICE candidate", e, candidate);
      }
    }
  }

  else if (msg.type === "log") {
    document.getElementById("logOutput").textContent += msg.data + "\n";
  }
};
*/

function setupInputForwarding() {
      const send = (obj) => {
        if (dataChannel && dataChannel.readyState === "open") {
          dataChannel.send(JSON.stringify(obj));
        }
      };
      window.addEventListener("keydown", e => {
        // keep it tiny
        send({ type: "key", action: "down", code: e.code, key: e.key });
      });
      window.addEventListener("keyup", e => {
        send({ type: "key", action: "up", code: e.code, key: e.key });
      });
    }

btnStart.onclick = () => {
      btnStart.disabled = true;
      if (!pc) makePeer();
      setupInputForwarding();
      // Nothing else to do: Unity sends the offer automatically after it connects.
    };