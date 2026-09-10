'use strict';

const { WebSocketServer } = require('ws');
const { io: createDashboardSocket } = require('socket.io-client');

const dashboardPort = Number(process.env.PORT || 3000);
const dashboardUrl = process.env.DASHBOARD_URL || `https://127.0.0.1:${dashboardPort}`;
const dashboardSocket = createDashboardSocket(dashboardUrl, {
  reconnection: true,
  autoConnect: true,
  rejectUnauthorized: process.env.TLS_REJECT_UNAUTHORIZED !== '0'
});
const peers = new Map();

dashboardSocket.on('connect', () => console.log(`[BRIDGE] Dashboard Socket.IO connected on port ${dashboardPort}`));
dashboardSocket.on('connect_error', error => console.error(`[BRIDGE] Dashboard Socket.IO error: ${error.message}`));
dashboardSocket.on('audio_transport', ({ device_id, ip, port, payload_type, clock_rate, ssrc }) => {
  if (!device_id) return;
  const message = {
    type: 'audio_transport',
    device_id,
    ip,
    port,
    payload_type,
    clock_rate,
    ssrc
  };
  sendToPeer(device_id, message);
});
dashboardSocket.on('server_command', message => {
  if (!message?.type) return;
  if (message.device_id) {
    sendToPeer(message.device_id, message);
    return;
  }
  for (const peer of peers.values()) {
    if (peer.socket?.readyState === 1) peer.socket.send(JSON.stringify(message));
  }
});

function sendToPeer(peerId, message) {
  const peer = peers.get(peerId);
  if (peer?.socket && peer.socket.readyState === 1) {
    peer.socket.send(JSON.stringify(message));
  }
}

function resolvePeerId(payload, socket) {
  const basePeerId = String(payload.peerId || 'esp32');
  const peerIp = String(payload.ip || socket.remoteAddress || 'unknown').replace(/^::ffff:/, '');
  const existing = peers.get(basePeerId);

  if (!existing || existing.socket === socket || existing.ip === peerIp) {
    return basePeerId;
  }

  // Keep older firmware builds with a shared PEER_NAME from replacing another board.
  const suffix = peerIp.replace(/[^a-zA-Z0-9]/g, '_');
  return `${basePeerId}_${suffix}`;
}

function resolveAudioSsrc(payload, peerId) {
  const requested = Number(payload.audioSsrc);
  if (Number.isInteger(requested) && requested > 0) {
    return requested >>> 0;
  }

  let hash = 0x53545544;
  for (const character of peerId) {
    hash = Math.imul(hash ^ character.charCodeAt(0), 16777619);
  }
  return (hash >>> 0) || 0x53545545;
}

async function startBridgeServer() {
  const server = new WebSocketServer({ host: '0.0.0.0', port: 8081 });

  server.on('connection', socket => {
    console.log('[BRIDGE] WebSocket client connected');

    let peerId = null;
    let isAlive = true;
    let missedPongs = 0;
    const keepAlive = setInterval(() => {
      if (socket.readyState !== socket.OPEN) return;
      if (!isAlive) {
        missedPongs++;
        console.warn(`[BRIDGE] WebSocket pong missed${peerId ? `: ${peerId}` : ''} count=${missedPongs}/3`);
        if (missedPongs < 3) {
          socket.ping();
          return;
        }
        console.error(`[BRIDGE] WebSocket heartbeat lost${peerId ? `: ${peerId}` : ''}; terminating stale socket`);
        socket.terminate();
        return;
      }
      isAlive = false;
      socket.ping();
    }, 10000);

    socket.on('pong', () => {
      isAlive = true;
      missedPongs = 0;
      console.log(`[BRIDGE] WebSocket pong${peerId ? `: ${peerId}` : ''}`);
      if (peerId) dashboardSocket.emit('device_event', { device_id: peerId, type: 'heartbeat' });
    });

    socket.on('error', error => {
      console.error(`[BRIDGE] WebSocket error${peerId ? ` (${peerId})` : ''}: ${error.message}`);
    });

    socket.on('close', (code, reason) => {
      clearInterval(keepAlive);
      console.warn(`[BRIDGE] WebSocket closed${peerId ? `: ${peerId}` : ''} code=${code} reason=${reason.toString() || 'none'}`);
      if (peerId) {
        const currentPeer = peers.get(peerId);
        if (currentPeer?.socket !== socket) return;
        dashboardSocket.emit('bridge_disconnect', { device_id: peerId });
        peers.delete(peerId);
        console.log(`[BRIDGE] WebSocket client disconnected: ${peerId}`);
      }
    });

    socket.on('message', async (message) => {
      try {
        const text = message.toString();
        const payload = JSON.parse(text);

        if (payload.type === 'offer') {
          const peerIp = payload.ip || socket.remoteAddress || '127.0.0.1';
          peerId = resolvePeerId(payload, socket);
          const audioSsrc = resolveAudioSsrc(payload, peerId);
          const audioReceivePort = Number(payload.audioReceivePort ?? payload.audio_receive_port ?? 5004);
          const previousPeer = peers.get(peerId);
          if (previousPeer?.socket && previousPeer.socket !== socket) {
            previousPeer.socket.close();
          }
          const peerEntry = previousPeer ? { ...previousPeer, socket, ip: peerIp.replace(/^::ffff:/, '') } : { socket, ip: peerIp.replace(/^::ffff:/, '') };
          peerEntry.socket = socket;
          peers.set(peerId, peerEntry);
          dashboardSocket.emit('register', {
            device_id: peerId,
            name: payload.studentName || peerId,
            device_type: 'ESP32-S3',
            ip: peerIp.replace(/^::ffff:/, ''),
            audioSsrc,
            audioReceivePort: Number.isInteger(audioReceivePort) && audioReceivePort > 0 ? audioReceivePort : 5004
          });
          console.log(`[BRIDGE] Registered ${peerId} name=${payload.studentName || peerId} audioSsrc=${audioSsrc}`);
          console.log(`[BRIDGE] Signaling offer accepted for ${peerId}; audio uses Node PlainTransport/RTP`);
          socket.send(JSON.stringify({ type: 'answer', peerId, sdp: '' }));
        } else if (payload.type === 'device_event' && peerId) {
          const eventType = String(payload.event_type || '');
          if (eventType) {
            dashboardSocket.emit('device_event', {
              device_id: peerId,
              type: eventType,
              muted: Boolean(payload.muted)
            });
          }
          socket.send(JSON.stringify({ type: 'ack', ok: true }));
        } else {
          socket.send(JSON.stringify({ type: 'ack', ok: true }));
        }
      } catch (error) {
        console.error('[BRIDGE] error:', error);
        socket.send(JSON.stringify({ type: 'error', message: error.message }));
      }
    });
  });

  server.on('listening', () => {
    console.log('ESP-WebRTC bridge listening on ws://0.0.0.0:8081');
  });
}

(async () => {
  await startBridgeServer();
})().catch((error) => {
  console.error('[BRIDGE] startup failed:', error);
  process.exit(1);
});
