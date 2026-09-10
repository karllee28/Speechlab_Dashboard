const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const express = require('express');
const https = require('node:https');
const { Server } = require('socket.io');
const mediasoup = require('mediasoup');

const app = express();
const certDirectory = process.env.CERT_DIR || path.join(__dirname, 'certs');
const server = https.createServer({
  key: fs.readFileSync(process.env.TLS_KEY || path.join(certDirectory, 'lan-key.pem')),
  cert: fs.readFileSync(process.env.TLS_CERT || path.join(certDirectory, 'lan-cert.pem'))
}, app);
const io = new Server(server, { cors: { origin: '*' } });
const devices = new Map();
let mediaWorker;
let mediaRouter;
let socketTeacherProducer;
let teacherProducerSocketId = null;
let activeTeacherSocketId = null;
const audioMulticastIp = process.env.AUDIO_MULTICAST_IP || '';
const audioMulticastPort = Number(process.env.AUDIO_MULTICAST_PORT || 40001);
let multicastAudioTransport;
let multicastAudioConsumer;
let question = { active: false, id: null, text: '', options: {}, correct: null };
const now = () => new Date().toISOString();
const networkInterfaces = os.networkInterfaces();
const wifiAddress = Object.entries(networkInterfaces)
  .filter(([name]) => /wi-?fi|wireless/i.test(name))
  .flatMap(([, addresses]) => addresses)
  .find(address => address.family === 'IPv4' && !address.internal)?.address;
const announcedIp = process.env.ANNOUNCED_IP || wifiAddress || Object.values(networkInterfaces).flat().find(address => address.family === 'IPv4' && !address.internal)?.address || '127.0.0.1';

app.use(express.json());
app.use(express.static(path.join(__dirname, '..')));
app.get('/api/state', (_req, res) => res.json(snapshot()));
app.get('/api/server-info', (_req, res) => res.json({ host: announcedIp, port, url: `https://${announcedIp}:${port}` }));
app.get('/', (_req, res) => res.sendFile(path.join(__dirname, '..', 'speechlab.html')));

function deviceFor(id) {
  if (!devices.has(id)) devices.set(id, {
    device_id: id, name: id, device_type: 'ESP32-S3', online: true,
    muted: false, remote_muted: false, hand_raised: false, answer: null,
    last_seen: now(), wifi_rssi: null, audio_active: false, mic_level: null,
    socketId: null, audioReceivePort: null, audioSsrc: 0x53545544, ip: null
  });
  return devices.get(id);
}
function snapshot() {
  const publicDevices = [...devices.values()].map(({ audioTransport, audioProducer, teacherAudioTransport, teacherAudioConsumer, ...device }) => device);
  return { devices: publicDevices, question };
}
function publish() { io.emit('classroom_state', snapshot()); }
function attachTransportDiagnostics(label, transport) {
  transport.observer.on('close', () => console.log(`[MEDIA] ${label} transport closed`));
  transport.on('close', () => console.log(`[MEDIA] ${label} transport close event`));
  transport.on('connectionstatechange', state => console.log(`[MEDIA] ${label} connectionstate=${state}`));
  transport.on('icestatechange', state => console.log(`[MEDIA] ${label} ice=${state}`));
  transport.on('dtlsstatechange', state => console.log(`[MEDIA] ${label} dtls=${state}`));
  transport.on('sctpstatechange', state => console.log(`[MEDIA] ${label} sctp=${state}`));
  transport.on('tuple', tuple => console.log(`[MEDIA] ${label} RTP tuple=${tuple.localIp}:${tuple.localPort} <- ${tuple.remoteIp || 'unknown'}:${tuple.remotePort || 'unknown'}`));
  transport.on('rtcptuple', tuple => console.log(`[MEDIA] ${label} RTCP tuple=${tuple.localIp}:${tuple.localPort}`));
  transport.on('trace', trace => console.log(`[MEDIA] ${label} trace=${JSON.stringify(trace)}`));
}
function attachProducerDiagnostics(label, producer) {
  producer.observer.on('close', () => console.log(`[MEDIA] ${label} producer closed`));
  producer.on('transportclose', () => console.log(`[MEDIA] ${label} producer transport closed`));
  producer.on('score', score => console.log(`[MEDIA] ${label} producer score=${JSON.stringify(score)}`));
  producer.on('trace', trace => console.log(`[MEDIA] ${label} producer trace=${JSON.stringify(trace)}`));
}
function attachConsumerDiagnostics(label, consumer) {
  consumer.observer.on('close', () => console.log(`[MEDIA] ${label} consumer closed`));
  consumer.on('transportclose', () => console.log(`[MEDIA] ${label} consumer transport closed`));
  consumer.on('producerclose', () => console.log(`[MEDIA] ${label} consumer producer closed`));
  consumer.on('score', score => console.log(`[MEDIA] ${label} consumer score=${JSON.stringify(score)}`));
  consumer.on('trace', trace => console.log(`[MEDIA] ${label} consumer trace=${JSON.stringify(trace)}`));
}
async function createAudioTransport(device) {
  if (!mediaRouter || device.audioTransport) return device.audioRtp;
  const transport = await mediaRouter.createPlainTransport({ listenIp: { ip: '0.0.0.0', announcedIp }, rtcpMux: true, comedia: true });
  attachTransportDiagnostics(`device=${device.device_id} student-mic`, transport);
  const producer = await transport.produce({
    kind: 'audio',
    rtpParameters: {
      codecs: [{ mimeType: 'audio/PCMU', payloadType: 0, clockRate: 8000, channels: 1 }],
      encodings: [{ ssrc: device.audioSsrc }]
    }
  });
  device.audioTransport = transport;
  device.audioProducer = producer;
  device.audioRtp = { ip: announcedIp, port: transport.tuple.localPort, payload_type: 0, clock_rate: 8000, ssrc: device.audioSsrc };
  attachProducerDiagnostics(`device=${device.device_id} student-mic`, producer);
  producer.on('transportclose', () => closeAudioTransport(device));
  return device.audioRtp;
}
async function sendAudioTransport(socket, device) {
  const audioRtp = await createAudioTransport(device);
  if (!audioRtp) throw new Error('Audio transport is not initialized');
  socket.emit('audio_transport', { device_id: device.device_id, ...audioRtp });
}
async function connectTeacherAudio(device) {
  if (audioMulticastIp) {
    device.socketId && io.sockets.sockets.get(device.socketId)?.emit('teacher_audio_multicast', { ip: audioMulticastIp, port: audioMulticastPort, payload_type: 0 });
    return;
  }
  if (!device.audioReceivePort || !socketTeacherProducer) return;
  device.teacherAudioConsumer?.close();
  device.teacherAudioTransport?.close();
  const transport = await mediaRouter.createPlainTransport({ listenIp: { ip: '0.0.0.0', announcedIp }, rtcpMux: true, comedia: false });
  attachTransportDiagnostics(`device=${device.device_id} teacher-audio`, transport);
  await transport.connect({ ip: device.ip, port: device.audioReceivePort });
  const consumer = await transport.consume({
    producerId: socketTeacherProducer.id,
    rtpCapabilities: { codecs: [{ mimeType: 'audio/PCMU', payloadType: 0, clockRate: 8000, channels: 1 }], headerExtensions: [] },
    paused: false
  });
  device.teacherAudioTransport = transport;
  device.teacherAudioConsumer = consumer;
  attachConsumerDiagnostics(`device=${device.device_id} teacher-audio`, consumer);
  consumer.on('transportclose', () => { device.teacherAudioConsumer = null; device.teacherAudioTransport = null; });
}
async function connectTeacherMulticast() {
  if (!audioMulticastIp || !socketTeacherProducer || multicastAudioConsumer) return;
  multicastAudioTransport = await mediaRouter.createPlainTransport({ listenIp: { ip: '0.0.0.0', announcedIp }, rtcpMux: true, comedia: false });
  await multicastAudioTransport.connect({ ip: audioMulticastIp, port: audioMulticastPort });
  multicastAudioConsumer = await multicastAudioTransport.consume({
    producerId: socketTeacherProducer.id,
    rtpCapabilities: { codecs: [{ mimeType: 'audio/PCMU', payloadType: 0, clockRate: 8000, channels: 1 }], headerExtensions: [] },
    paused: false
  });
  multicastAudioConsumer.on('transportclose', () => { multicastAudioConsumer = null; multicastAudioTransport = null; });
  for (const device of devices.values()) if (device.online) connectTeacherAudio(device);
}
function closeTeacherMulticast() {
  multicastAudioConsumer?.close();
  multicastAudioTransport?.close();
  multicastAudioConsumer = null;
  multicastAudioTransport = null;
}
function closeAudioTransport(device) {
  device.audioProducer?.close();
  device.audioTransport?.close();
  device.audioProducer = null;
  device.audioTransport = null;
  device.audioRtp = null;
}
async function createTeacherTransport(socket) {
  const transport = await mediaRouter.createWebRtcTransport({
    listenIps: [{ ip: '0.0.0.0', announcedIp }],
    enableUdp: true,
    enableTcp: true,
    preferUdp: true
  });
  attachTransportDiagnostics(`teacher=${socket.id} recv`, transport);
  socket.data.teacherTransport = transport;
  return transport;
}
async function createTeacherSendTransport(socket) {
  const transport = await mediaRouter.createWebRtcTransport({
    listenIps: [{ ip: '0.0.0.0', announcedIp }],
    enableUdp: true,
    enableTcp: true,
    preferUdp: true
  });
  attachTransportDiagnostics(`teacher=${socket.id} send`, transport);
  socket.data.teacherSendTransport = transport;
  return transport;
}
function emitCommand(type, deviceId, payload = {}) {
  io.emit('server_command', { type, ...(deviceId ? { device_id: deviceId } : {}), ...payload });
  for (const device of devices.values()) {
    if (deviceId && device.device_id !== deviceId) continue;
    if (type === 'remote_mute') device.remote_muted = Boolean(payload.enabled);
    if (type === 'set_hand') device.hand_raised = Boolean(payload.raised);
    if (type === 'clear_answers') device.answer = null;
  }
  publish();
}

function enforceTeacherRole(socket) {
  if (activeTeacherSocketId === null) {
    activeTeacherSocketId = socket.id;
    console.log(`[TEACHER] ${socket.id} is now the active teacher`);
    return { isTeacher: true, newTeacher: true };
  }
  if (activeTeacherSocketId === socket.id) {
    return { isTeacher: true, newTeacher: false };
  }
  return { isTeacher: false, newTeacher: false };
}

function clearTeacherRole(socketId) {
  if (activeTeacherSocketId === socketId) {
    console.log(`[TEACHER] ${socketId} teacher role removed`);
    activeTeacherSocketId = null;
  }
}

io.on('connection', socket => {
  const clientAddr = (socket.handshake.address || '').replace(/^::ffff:/, '');
  console.log(`[SOCKET] client connected ${socket.id} from ${clientAddr}, handshake.url=${socket.handshake.url}, referer=${socket.handshake.headers.referer}`);
  socket.on('error', error => console.error(`[SOCKET] ${socket.id} error: ${error.message}`));
  socket.emit('classroom_state', snapshot());
  socket.on('register', message => {
    const id = message && message.device_id;
    if (!id) return socket.emit('error', { message: 'device_id is required' });
    const device = deviceFor(id);
    const remoteIp = (message.ip || message.device_ip || socket.handshake.address || '127.0.0.1').replace(/^::ffff:/, '');
    const requestedPort = Number(message.audioReceivePort ?? message.audio_receive_port ?? message.audioPort ?? device.audioReceivePort ?? 5004);
    const audioReceivePort = Number.isInteger(requestedPort) && requestedPort > 0 ? requestedPort : null;
    const requestedSsrc = Number(message.audioSsrc);
    const audioSsrc = Number.isInteger(requestedSsrc) && requestedSsrc > 0 ? requestedSsrc >>> 0 : device.audioSsrc;
    const sameLiveRegistration = device.socketId === socket.id &&
      device.online &&
      device.ip === remoteIp &&
      device.audioReceivePort === audioReceivePort &&
      device.audioSsrc === audioSsrc;

    if (!sameLiveRegistration) {
      closeAudioTransport(device);
      device.teacherAudioConsumer?.close();
      device.teacherAudioTransport?.close();
      device.teacherAudioConsumer = null;
      device.teacherAudioTransport = null;
    }

    if (device.socketId && device.socketId !== socket.id) {
      const previousSocket = io.sockets.sockets.get(device.socketId);
      previousSocket?.disconnect(true);
    }
    if (remoteIp === '127.0.0.1' || remoteIp === '::1') {
      console.warn(`[DEVICE] ${id} reported loopback IP; teacher RTP cannot reach the device`);
    }
    device.audioReceivePort = audioReceivePort;
    device.audioSsrc = audioSsrc;
    console.log(`[DEVICE] ${sameLiveRegistration ? 'heartbeat registration' : 'registered'} ${id} from ${remoteIp}:${device.audioReceivePort ?? 'n/a'}`);
    Object.assign(device, { name: message.name || device.name, device_type: message.device_type || 'ESP32-S3', online: true, last_seen: now(), ip: remoteIp, socketId: socket.id });
    socket.data.deviceIds = socket.data.deviceIds || new Set();
    socket.data.deviceIds.add(id);
    socket.data.deviceId = id;
    socket.emit('sync_state', { device_id: id, question, remote_muted: device.remote_muted, hand_raised: device.hand_raised, answer: device.answer });
    sendAudioTransport(socket, device).then(() => {
      if (socketTeacherProducer && device.audioReceivePort && (!sameLiveRegistration || !device.teacherAudioConsumer)) {
        console.log(`[AUDIO] Connecting teacher audio to ${device.device_id} after device registration`);
        connectTeacherAudio(device).catch(err => console.error(`[AUDIO] connectTeacherAudio failed on register: ${err.message}`));
      }
      if (!sameLiveRegistration) {
        console.log(`[AUDIO] New device registration detected - notifying teachers about producer ${device.audioProducer.id} for device ${device.device_id}`);
        let notificationCount = 0;
        for (const teacher of io.sockets.sockets.values()) {
          if (teacher.data.teacherTransport && teacher.id !== socket.id) {
            teacher.emit('teacher_audio_new_producer', { producerId: device.audioProducer.id, name: device.name });
            notificationCount++;
            console.log(`[AUDIO] Notified teacher ${teacher.id} about new producer ${device.audioProducer.id}`);
          }
        }
        console.log(`[AUDIO] Total teachers notified: ${notificationCount}, total teacher sockets: ${[...io.sockets.sockets.values()].filter(s => s.data.teacherTransport).length}`);
      }
    })
      .catch(error => socket.emit('error', { message: `audio transport unavailable: ${error.message}` }));
    publish();
  });
  socket.on('bridge_disconnect', ({ device_id: id } = {}) => {
    const device = id && devices.get(id);
    if (!device || device.socketId !== socket.id) return;
    closeAudioTransport(device);
    device.teacherAudioConsumer?.close();
    device.teacherAudioTransport?.close();
    device.teacherAudioConsumer = null;
    device.teacherAudioTransport = null;
    Object.assign(device, { online: false, socketId: null, last_seen: now() });
    publish();
  });
  socket.on('audio_transport_request', async (_message, callback = () => {}) => {
    const device = socket.data.deviceId && devices.get(socket.data.deviceId);
    if (!device || device.socketId !== socket.id) return callback({ error: 'Device is not registered' });
    try {
      await sendAudioTransport(socket, device);
      callback({ ok: true });
    } catch (error) {
      callback({ error: error.message });
    }
  });
  socket.on('audio_receive_ready', async ({ port }, callback = () => {}) => {
    const device = socket.data.deviceId && devices.get(socket.data.deviceId);
    if (!device || device.socketId !== socket.id || !Number.isInteger(port) || port < 1 || port > 65535) return callback({ error: 'Invalid audio receive port' });
    device.audioReceivePort = port;
    console.log(`[AUDIO] Device ${device.device_id} receive port ${port}`);
    try {
      if (socketTeacherProducer) {
        await connectTeacherAudio(device);
        console.log(`[AUDIO] Connected teacher audio to ${device.device_id}:${port}`);
      }
      callback({ ok: true });
    } catch (error) {
      console.error(`[AUDIO] Failed to connect teacher audio to ${device.device_id}: ${error.message}`);
      callback({ error: error.message });
    }
  });
  socket.on('teacher_audio_start', async (_message, callback) => {
    const teacherRole = enforceTeacherRole(socket);
    if (!teacherRole.isTeacher) {
      console.log(`[TEACHER] ${socket.id} rejected: another teacher (${activeTeacherSocketId}) is already active`);
      socket.emit('dashboard_busy', { noticeUrl: '/server-busy.html' });
      setTimeout(() => socket.disconnect(true), 100);
      return callback({ error: 'TEACHER_SESSION_BUSY', message: 'Another teacher is already active. Only one teacher can control the classroom at a time.' });
    }
    try {
      const initialProducers = [...devices.values()].filter(device => device.audioProducer && device.online).map(device => ({ id: device.audioProducer.id, name: device.name }));
      console.log(`[AUDIO] teacher_audio_start - Initial producers available: ${initialProducers.map(p => `${p.name}=${p.id}`).join(', ') || 'none'}, active devices: ${[...devices.values()].filter(d => d.online).map(d => d.device_id).join(', ') || 'none'}`);
      const transport = socket.data.teacherTransport || await createTeacherTransport(socket);
      const sendTransport = socket.data.teacherSendTransport || await createTeacherSendTransport(socket);
      callback({
        routerRtpCapabilities: mediaRouter.rtpCapabilities,
        transportOptions: {
          id: transport.id,
          iceParameters: transport.iceParameters,
          iceCandidates: transport.iceCandidates,
          dtlsParameters: transport.dtlsParameters
        },
        sendTransportOptions: {
          id: sendTransport.id,
          iceParameters: sendTransport.iceParameters,
          iceCandidates: sendTransport.iceCandidates,
          dtlsParameters: sendTransport.dtlsParameters
        },
        producers: initialProducers
      });
    } catch (error) {
      callback({ error: error.message });
    }
  });
  socket.on('teacher_audio_connect', async ({ dtlsParameters, direction }, callback) => {
    try {
      const transport = direction === 'send' ? socket.data.teacherSendTransport : socket.data.teacherTransport;
      await transport.connect({ dtlsParameters });
      callback({ ok: true });
    } catch (error) {
      callback({ error: error.message });
    }
  });
  socket.on('teacher_audio_produce', async ({ kind, rtpParameters }, callback) => {
    if (activeTeacherSocketId !== socket.id) {
      console.log(`[TEACHER] ${socket.id} rejected producer: not the active teacher`);
      return callback({ error: 'NOT_ACTIVE_TEACHER', message: 'You are not the active teacher. Only the first connected teacher can produce audio.' });
    }
    try {
      if (!socket.data.teacherSendTransport) throw new Error('Teacher send transport is not ready');
      console.log(`[AUDIO] Teacher producer starting (kind=${kind}), existing consumers: ${[...(socket.data.teacherConsumers || [])].map(c => c.id).join(', ') || 'none'}`);
      socketTeacherProducer?.close();
      socketTeacherProducer = await socket.data.teacherSendTransport.produce({ kind, rtpParameters });
      teacherProducerSocketId = socket.id;
      console.log(`[MEDIA] Teacher producer created id=${socketTeacherProducer.id} kind=${kind}`);
      attachProducerDiagnostics(`teacher=${socket.id}`, socketTeacherProducer);
      socketTeacherProducer.on('transportclose', () => { socketTeacherProducer = null; closeTeacherMulticast(); });
      await connectTeacherMulticast();
      const connected = [];
      const pending = [];
      for (const device of devices.values()) {
        if (!device.online) continue;
        console.log(`[AUDIO] Connecting teacher audio to device ${device.device_id} (ip=${device.ip}, port=${device.audioReceivePort}, audioProducerId=${device.audioProducer?.id})`);
        try {
          await connectTeacherAudio(device);
          device.audioReceivePort ? connected.push(device.device_id) : pending.push(device.device_id);
        } catch (err) {
          console.error(`[AUDIO] Failed to connect ${device.device_id}: ${err.message}`);
        }
      }
      console.log(`[AUDIO] Teacher audio connected to: ${connected.join(', ') || 'none'}, pending: ${pending.join(', ') || 'none'}`);
      console.log(`[AUDIO] Active consumers after teacher produce: ${[...(socket.data.teacherConsumers || [])].map(c => c.id).join(', ') || 'none'}`);
      callback({ id: socketTeacherProducer.id });
    } catch (error) {
      console.error(`[AUDIO] Teacher audio produce failed: ${error.message}`);
      callback({ error: error.message });
    }
  });
  socket.on('teacher_audio_consume', async ({ producerIds, rtpCapabilities }, callback) => {
    try {
      if (!socket.data.teacherTransport) throw new Error('Teacher audio transport is not ready');
      console.log(`[AUDIO] teacher_audio_consume request: producerIds=${JSON.stringify(producerIds)}, isActive=${activeTeacherSocketId === socket.id}`);
      console.log(`[AUDIO] Browser rtpCapabilities: ${JSON.stringify(rtpCapabilities)}`);
      const consumers = [];
      for (const producerId of producerIds || []) {
        const producer = [...devices.values()].find(d => d.audioProducer?.id === producerId);
        if (!producer) {
          console.warn(`[AUDIO] Producer ${producerId} not found in devices`);
          continue;
        }
        const canConsume = mediaRouter.canConsume({ producerId, rtpCapabilities });
        console.log(`[AUDIO] canConsume check - Producer ${producerId} (device=${producer.device_id}, ssrc=${producer.audioSsrc}): canConsume=${canConsume}`);
        if (!canConsume) {
          console.warn(`[AUDIO] ⚠️  Cannot consume producer ${producerId} - checking compatibility...`);
          // Try to get more info about why canConsume failed
          const producerObj = mediaRouter._producers?.get(producerId);
          if (producerObj) {
            console.warn(`[AUDIO]   Producer details: rtpParameters=${JSON.stringify(producerObj.rtpParameters)}`);
          }
          continue;
        }
        const consumer = await socket.data.teacherTransport.consume({ producerId, rtpCapabilities, paused: false });
        attachConsumerDiagnostics(`teacher=${socket.id} producer=${producerId}`, consumer);
        consumers.push({ id: consumer.id, producerId, kind: consumer.kind, rtpParameters: consumer.rtpParameters });
        socket.data.teacherConsumers = [...(socket.data.teacherConsumers || []), consumer];
        console.log(`[AUDIO] ✅ Consumer created for producer ${producerId}: consumer.id=${consumer.id}`);
      }
      console.log(`[AUDIO] teacher_audio_consume result: ${consumers.length} consumers created`);
      callback({ consumers });
    } catch (error) {
      console.error(`[AUDIO] teacher_audio_consume error: ${error.message}`);
      callback({ error: error.message });
    }
  });
  socket.on('device_event', message => {
    const id = message && message.device_id;
    if (!id || !message.type) return socket.emit('error', { message: 'type and device_id are required' });
    const device = deviceFor(id);
    Object.assign(device, { online: true, last_seen: now() });
    if (message.type === 'answer' && /^[ABCD]$/.test(message.answer) && question.active) device.answer = message.answer;
    if (message.type === 'hand_raise') device.hand_raised = Boolean(message.raised);
    if (message.type === 'mute_state') device.muted = Boolean(message.muted);
    if (message.type === 'heartbeat') device.wifi_rssi = message.wifi_rssi ?? null;
    if (message.type === 'audio_state') {
      device.audio_active = Boolean(message.active);
      if (Number.isFinite(Number(message.mic_level))) device.mic_level = Math.max(0, Math.min(100, Number(message.mic_level)));
    }
    publish();
  });
  socket.on('teacher_command', message => {
    if (activeTeacherSocketId !== socket.id) {
      console.log(`[TEACHER] ${socket.id} rejected command: not the active teacher`);
      return;
    }
    if (!message || !message.type) return;
    if (message.type === 'question_start') {
      question = { active: true, id: message.question_id || 'q1', text: message.text || '', options: message.options || {}, correct: null };
      for (const device of devices.values()) device.answer = null;
      io.emit('server_command', message); publish();
    } else if (message.type === 'question_end') {
      question.active = false; io.emit('server_command', message); publish();
    } else if (['clear_answers', 'set_hand', 'remote_mute'].includes(message.type)) {
      const { type, device_id: id, ...payload } = message; emitCommand(type, id, payload);
    }
  });
  socket.on('disconnect', () => {
    socket.data.teacherConsumers?.forEach(consumer => consumer.close());
    socket.data.teacherTransport?.close();
    socket.data.teacherSendTransport?.close();
    if (socket.id === teacherProducerSocketId) {
      socketTeacherProducer?.close();
      socketTeacherProducer = null;
      teacherProducerSocketId = null;
      closeTeacherMulticast();
    }
    clearTeacherRole(socket.id);
    const deviceIds = socket.data.deviceIds || (socket.data.deviceId ? new Set([socket.data.deviceId]) : []);
    for (const deviceId of deviceIds) {
      if (!devices.has(deviceId)) continue;
      const device = devices.get(deviceId);
      if (device.socketId !== socket.id) continue;
      device.online = false;
      device.last_seen = now();
      device.socketId = null;
      closeAudioTransport(device);
      device.teacherAudioConsumer?.close();
      device.teacherAudioTransport?.close();
      device.teacherAudioConsumer = null;
      device.teacherAudioTransport = null;
    }
    publish();
  });
});

setInterval(() => {
  const timeout = Date.now() - 30000;
  for (const device of devices.values()) {
    if (!device.socketId && Date.parse(device.last_seen) < timeout) device.online = false;
  }
  publish();
}, 10000);

const port = Number(process.env.PORT || 3000);
server.on('error', error => console.error(`[SERVER] HTTP error: ${error.message}`));
process.on('unhandledRejection', error => console.error('[PROCESS] Unhandled rejection:', error));
process.on('uncaughtException', error => {
  console.error('[PROCESS] Uncaught exception:', error);
  process.exitCode = 1;
});
server.listen(port, '0.0.0.0', () => {
  console.log(`SpeechLab server listening on https://localhost:${port}`);
  console.log(`SpeechLab LAN URL: https://${announcedIp}:${port}`);
});

(async () => {
  mediaWorker = await mediasoup.createWorker();
  mediaWorker.on('died', () => { console.error('MediaSoup worker died'); process.exit(1); });
  mediaRouter = await mediaWorker.createRouter({ mediaCodecs: [{ kind: 'audio', mimeType: 'audio/PCMU', clockRate: 8000, channels: 1, preferredPayloadType: 0 }] });
  console.log('MediaSoup audio router ready');
})().catch(error => console.error('MediaSoup startup failed; client connections remain available:', error));
