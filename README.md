📌 Overview

WebRTCStreamer is a prototype system that streams Unity gameplay to remote clients in real-time using a custom C++ plugin, NVIDIA’s NVENC hardware encoder, and WebRTC for transport. It demonstrates how to integrate Unity with native C++ for high-performance cloud streaming pipelines.

This project is independent from VelEngine and serves as a focused SDK-style prototype.
---------------------------------------------------------------------------------------------------------
⚙️ Architecture

🔹 Frame Capture
- Unity captures frames directly from the Direct3D11 render pipeline.

- Captured textures are passed into a native C++ DLL via Unity’s plugin interface.

🔹 Encoding (NVENC)

- The DLL submits textures directly to NVIDIA NVENC for GPU-accelerated encoding.

- Frames are encoded into H.264 with minimal CPU overhead.

🔹 Networking (WebRTC)

- The DLL integrates WebRTC and manages peer connection setup.

- A Node.js signaling server handles offer/answer exchange and ICE candidates.

- Encoded frames are streamed to the client over WebRTC data/media channels.

🔹 Input Round-Trip

- Client input (keyboard, mouse, gamepad) → signaling server → C++ DLL → Unity scripting layer.

- Ensures bidirectional interactivity with low latency.
---------------------------------------------------------------------------------------------------------
🚀 Features

- Unity → C++ plugin integration.

- GPU-based encoding using NVENC.

- WebRTC transport with Node.js signaling.

- Bidirectional input handling for real-time interaction.

- Works with minimal Unity scenes for quick testing.
---------------------------------------------------------------------------------------------------------
🐞 Current Work

- Investigating a stability bug in the frame pipeline.

- Expanding support for multiple clients.

- Adding adaptive bitrate & resolution scaling (future).
---------------------------------------------------------------------------------------------------------
🔧 Build & Run

Requirements

- Unity 202x.x.x with Direct3D11.

- Visual Studio (C++17 or higher) for building the DLL.

- libdatachannel
 (clone and build; link the compiled library + headers to your Visual Studio project).

- NVIDIA NVENC SDK (for GPU video encoding).

- Node.js vXX for the signaling server.

- Windows 10/11 + NVIDIA GPU (for NVENC).

Steps

1. Clone this repo.

2. Build the C++ DLL (Visual Studio → Release x64).
   
3. Copy the DLL into Unity’s Assets/Plugins folder.

4. Start the signaling server:
- cd html
- node signalling-server.js

5. Open index.html

6. Run Unity project and press Play.

7. Connect via browser client → see streamed gameplay + send input.
---------------------------------------------------------------------------------------------------------
📜 License

MIT License © 2025 Krupesh Parmar
(Demo Only)

🧑‍💻 Author

Krupesh Parmar – Indie game developer & engine programmer
LinkedIn: www.linkedin.com/in/krupesh-parmar
