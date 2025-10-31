//let pc = null;
let dataChannel = null;
let candidateQueue = [];
let pendingMouse = null, rafScheduled = false;
const videoEl = document.getElementById("unity");
const wsBadge = document.getElementById("wsStatus");
const pcBadge = document.getElementById("pcStatus");
const btnStart = document.getElementById("btnStart");
const fsBtn  = document.getElementById('fsBtn');
const player = document.getElementById('wrap');

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

videoEl.muted = true;
videoEl.playsInline = true;
videoEl.autoplay = true;
videoEl.controls = false;

videoEl.addEventListener('focus', () => videoEl.blur());

// 2) Prevent click-to-pause on the video surface
['click','dblclick','mousedown','mouseup'].forEach(t =>
  videoEl.addEventListener(t, e => { e.preventDefault(); e.stopPropagation(); }, { passive:false })
);

// 3) Fullscreen toggle on your custom button (not the video)
fsBtn.addEventListener('click', async () => {
  try {
    if (!document.fullscreenElement) {
      // go fullscreen on the container (not the video) so the video never gets focus
      await videoEl.requestFullscreen();
      videoEl.requestPointerLock();
    } else {
      await document.exitFullscreen();
      await document.exitPointerLock();
    }
  } catch (err) {
    console.error('Fullscreen error:', err);
  }
});

document.addEventListener('pointerlockchange', () => {
  if (document.pointerLockElement === videoEl) {
    console.log('Pointer locked');
  } else {
    console.log('Pointer released');
  }
});

// 4) While fullscreen, block media control keys (space, k, arrow keys, etc.)
document.addEventListener('keydown', e => {
  const inFS = !!document.fullscreenElement;
  if (!inFS) return;

  // keys that commonly control media in Chromium
  const key = e.key.toLowerCase();
  const block = (
    key === ' ' || key === 'k' ||
    key === 'mediaPlayPause' || key === 'mediaplaypause' ||
    key === 'j' || key === 'l'
  );

  if (block) {
    e.preventDefault();
    e.stopPropagation();
  }
}, { capture: true });

// 5) Also ensure the video keeps playing if something else tried to pause it
videoEl.addEventListener('pause', () => {
  // If you truly never want pause, immediately resume.
  // (Comment this out if you want to allow pausing via your own UI.)
  videoEl.play().catch(()=>{});
});

function sendPending() {
  rafScheduled = false;
  if (!pendingMouse || dataChannel.readyState !== 'open') return;
  dataChannel.send(JSON.stringify(pendingMouse));
  pendingMouse = null;
}

function onPointerMove(ev) {
  if (document.pointerLockElement !== videoEl) return;
  const r = videoEl.getBoundingClientRect();
  if (r.width <= 0 || r.height <= 0) return;
  // clamp & normalize
  const nx = Math.min(Math.max((ev.clientX - r.left) / r.width, 0), 1);
  const ny = Math.min(Math.max((ev.clientY - r.top)  / r.height, 0), 1);
  pendingMouse = { type: "mouse", action: "move", x: nx, y: ny, ts: performance.now() };
  if (!rafScheduled) { rafScheduled = true; requestAnimationFrame(sendPending); }
  //const dx = ev.movementX || 0;
  //const dy = ev.movementY || 0;
  //// Send deltas (not absolute coords)
  //if (dataChannel.readyState === 'open' && (dx || dy)) {
  //  dataChannel.send(JSON.stringify({ type:'mouse', action:'move',  x: dx,  y: dy, ts: performance.now() }));
  //}
  // const r = videoEl.getBoundingClientRect();
  // if (r.width <= 0 || r.height <= 0) return;

  // // clamp & normalize
  // const nx = Math.min(Math.max((ev.clientX - r.left) / r.width, 0), 1);
  // const ny = Math.min(Math.max((ev.clientY - r.top)  / r.height, 0), 1);

  // pendingMouse = { type: "mouse", action: "move", x: nx, y: ny, ts: performance.now() };
  // if (!rafScheduled) { rafScheduled = true; requestAnimationFrame(sendPending); }
}

document.addEventListener('mousemove', onPointerMove, { passive: true });

function setupInputForwarding() {
      const send = (obj) => {
        if (dataChannel && dataChannel.readyState === "open") {
          dataChannel.send(JSON.stringify(obj));
        }
      };
      window.addEventListener("keydown", e => {
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
    };

//fsBtn.onclick = () => {
//    toggleFull();
//};
function toggleFull() {
  if (!document.fullscreenElement) {
    videoEl.controls = false; 
    videoEl.tabIndex = -1;
    videoEl.requestFullscreen().catch(err => console.error(err));
  } else {
    document.exitFullscreen();
  }
}

pc.ontrack = (e) => {
  console.log("[JS] ontrack:", e.track.kind, e.streams?.[0]);
  videoEl.srcObject = e.streams[0] ?? new MediaStream([e.track]);
  videoEl.autoplay = true;
  videoEl.muted = true;
  videoEl.playsInline = true;
  videoEl.play().catch(()=>{});
  //const aspect = video.videoWidth / video.videoHeight;
  //
  //// Create a quad layer and size it with the same aspect:
  //const layerInit = {
  //  space: xrRefSpace,
  //  transform: new XRRigidTransform(), // position later as needed
  //  width: 1.6,                         // meters in world
  //  height: 1.6 / aspect,               // keep aspect!
  //  layout: "mono"
  //};
  //
  //const mediaBinding = new XRMediaBinding(xrSession);
  //const quadLayer = mediaBinding.createQuadLayer(video, layerInit);
  //xrSession.updateRenderState({ layers: [quadLayer] });
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
//
//     // Force video to play
//    videoEl.play().catch(e => console.log('Play failed:', e));
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
  const payload = {
    type: "ice-candidate",
    candidate: {
      candidate: e.candidate.candidate,
      sdpMid: e.candidate.sdpMid,
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
    rawData = await rawData.text();
  }

  const msg = JSON.parse(rawData);
  console.log("[WS] Msg Received", msg);

  if (msg.type === "sdp-offer") {
    console.log("[Browser] Offer Received");
    console.log("Raw SDP string received:");
    console.log(msg.sdp);
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

    // Perform SDP handshake
    await pc.setRemoteDescription({ type: "offer", sdp: msg.sdp });
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
