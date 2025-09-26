const WebSocket = require("ws");

const wss = new WebSocket.Server({ port: 9090 });
let unitySocket = null;
let browserSocket = null;

wss.on("connection", socket => {
  socket.on("message", msg => {
    console.log("[WS] Message received:", msg.toString());
    let data;
    try {
    data = JSON.parse(msg);
    } catch (e) {
      console.log("[WS] Received non-JSON or invalid message, dropping:", msg.toString());
      return;
    }
    
    if (data.type === "register") {
      if (data.role === "unity") unitySocket = socket;
      if (data.role === "browser") browserSocket = socket;
      console.log(`[WS] Registered ${data.role}`);
    }

    if (data.type === "sdp-offer" && browserSocket)
      browserSocket.send(msg);
    else if (data.type === "sdp-answer" && unitySocket)
      unitySocket.send(msg);
    else if (data.type === "ice-candidate") {
      if (socket === unitySocket && browserSocket) {
        browserSocket.send(msg);
      } else if (socket === browserSocket && unitySocket) {
        unitySocket.send(msg);
      }
    } 
    else if (data.type === "command" && unitySocket)
      unitySocket.send(msg);
    else if (data.type === "log" && browserSocket)
      browserSocket.send(msg);
  });

  socket.on("close", () => {
    if (socket === unitySocket) unitySocket = null;
    if (socket === browserSocket) browserSocket = null;
  });
});

console.log("WebSocket signaling server running on ws://localhost:9090");
// raw-tcp-server.js
// const net = require('net');
// const clients = {};

// const server = net.createServer((socket) => {
//   socket.setEncoding('utf-8');

//   console.log("Client connected");

//   let role = null;

//   socket.on('data', (data) => {
//     try {
//       const msg = JSON.parse(data.trim());

//       if (msg.type === 'register') {
//         role = msg.role;
//         clients[role] = socket;
//         console.log(`Registered ${role}`);
//       } else if (msg.type === 'sdp-offer' && clients['browser']) {
//         clients['browser'].write(JSON.stringify(msg) + '\n');
//       } else if (msg.type === 'sdp-answer' && clients['unity']) {
//         clients['unity'].write(JSON.stringify(msg) + '\n');
//       }
//     } catch (e) {
//       console.log("Bad data or parse error:", data);
//     }
//   });

//   socket.on('end', () => {
//     if (role) {
//       delete clients[role];
//       console.log(`${role} disconnected`);
//     }
//   });
// });

// server.listen(9090, () => {
//   console.log("TCP signaling server running on port 9090");
// });
