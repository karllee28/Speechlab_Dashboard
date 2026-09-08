# SpeechLab ESP32-S3 Classroom System

SpeechLab is a LAN classroom communication system. A teacher uses `speechlab.html` to manage devices, classroom commands, and two-way audio. The Node.js server provides the dashboard and mediasoup media routing; an ESP32-S3 connects through the WebSocket bridge and exchanges audio over RTP/UDP.

The two audio paths are independent:

```text
Teacher microphone -> browser WebRTC -> mediasoup -> RTP -> ESP32 speaker
ESP32 microphone  -> RTP -> mediasoup -> browser WebRTC -> teacher speaker
```

ESP32 microphone audio is played on the teacher computer, not through the same ESP32 speaker.

## Components

- `speechlab.html`: teacher dashboard and mediasoup-client WebRTC application.
- `server/server.js`: Express, Socket.IO, and mediasoup classroom server.
- `server/esp_webrtc_bridge.js`: WebSocket signaling bridge for ESP32 devices.
- `esp32_client/esp_webrtc_s3/main/main.c`: ESP-IDF firmware for the ESP32-S3.

## Network ports

| Port | Protocol | Purpose |
|---:|---|---|
| 3000 | TCP | Dashboard HTTP and Socket.IO |
| 8081 | TCP | ESP32-to-bridge WebSocket signaling |
| 5004 | UDP | Teacher audio RTP received by the ESP32 speaker |
| Dynamic | UDP | ESP32 microphone RTP received by mediasoup |
| 40000-40100 | UDP/TCP | mediasoup WebRTC media transports |

The dynamic microphone destination is announced to each ESP32 after registration. The firmware must receive that announcement before it sends microphone RTP.

All media uses mono G.711 PCMU:

```text
Payload type: 0
Clock rate:   8000 Hz
Channels:     1
```

WebSocket and Socket.IO carry signaling, registration, status, and classroom commands. Audio does not travel through JSON or WebSocket messages.

## Run the server

Install Node.js dependencies and start the classroom server:

```powershell
cd C:\Users\thesi\Desktop\Speechlab_Dashboard\server
npm install
npm start
```

In a second terminal, start the ESP32 signaling bridge:

```powershell
cd C:\Users\thesi\Desktop\Speechlab_Dashboard\server
node esp_webrtc_bridge.js
```

Open the dashboard at `http://localhost:3000`. The dashboard can be opened directly for a local demo, but live devices require the Node server.

## Configure and flash the ESP32

The firmware uses ESP-IDF, not Arduino. Activate an ESP-IDF PowerShell and edit the configuration constants near the top of `esp32_client/esp_webrtc_s3/main/main.c`:

```c
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"
#define SIGNALING_HOST "192.168.0.18"
#define SIGNALING_PORT 8081
#define PEER_NAME "esp32_s3_student"
#define STUDENT_NAME "Student 1"
```

`SIGNALING_HOST` must be the LAN address of the computer running the bridge. The ESP32 and computer must be able to reach one another on the same network.
`PEER_NAME` is used as an ID prefix; the firmware appends each board's Wi-Fi MAC address, so multiple ESP32 boards can use the same firmware without replacing one another in the dashboard. Change `STUDENT_NAME` on each board to the name shown to the teacher.

Build, flash, and monitor from the firmware project directory:

```powershell
cd C:\Users\thesi\Desktop\Speechlab_Dashboard\esp32_client\esp_webrtc_s3
idf.py build
idf.py -p COMx flash monitor
```

The ESP32 starts its speaker receiver on UDP port `5004`, connects to `ws://SIGNALING_HOST:8081`, sends an offer containing its identity and audio receive port, and waits for the server's dynamic microphone RTP destination.

## Wiring

| Function | Default GPIO |
|---|---:|
| INMP441 BCLK | 5 |
| INMP441 WS/LRCLK | 6 |
| INMP441 data | 7 |
| MAX98357A BCLK | 15 |
| MAX98357A LRC | 16 |
| MAX98357A data | 17 |
| Status LED | 48 |

Power the INMP441 from 3.3 V and connect its L/R pin to the selected channel. Power the MAX98357A from a suitable 3-5 V supply, connect SD/EN HIGH, and connect the speaker only between its `+` and `-` outputs. Use a common ground and verify GPIOs against the exact ESP32-S3 board before wiring.

## Configuration

Optional server environment variables:

- `PORT`: dashboard port; default `3000`.
- `ANNOUNCED_IP`: LAN address advertised by mediasoup when automatic detection selects the wrong interface.
- `AUDIO_MULTICAST_IP`: optional multicast address for teacher-to-ESP32 audio.
- `AUDIO_MULTICAST_PORT`: multicast port; default `40001`.

Without multicast settings, teacher audio uses one unicast mediasoup PlainTransport per ESP32. Multicast requires an access point that supports efficient LAN multicast.

## Signaling and classroom events

The ESP32 sends an offer like:

```json
{
  "type": "offer",
  "peerId": "esp32_s3_student_A1B2C3D4E5F6",
  "studentName": "Student 1",
  "ip": "192.168.0.50",
  "audioReceivePort": 5004,
  "sdp": "..."
}
```

After registration, the bridge forwards an `audio_transport` announcement containing the dynamic RTP destination, payload type, clock rate, and SSRC. Device activity is reported through the bridge, including heartbeats, answers, hand state, mute state, and audio state. The Node server broadcasts `classroom_state` and `server_command` messages to the dashboard and devices.

## Troubleshooting

- **ESP32 connects but the teacher hears no student audio:** verify the firmware logged its dynamic RTP target, the target IP is the server's LAN IP, UDP ports `40000-40100` are allowed through Windows Firewall, and the firmware/server PCMU settings and SSRC match.
- **Teacher audio does not reach the ESP32:** verify the ESP32 is listening on UDP `5004`, the teacher microphone is enabled, and the Node server reports a connected teacher audio consumer.
- **Audio uses the wrong teacher output:** select the output device in the dashboard and check browser autoplay permission.
- **Build reports an old Python environment:** activate ESP-IDF, run `idf.py fullclean`, then run `idf.py build` again.

## Limitations

The system is intended for a trusted LAN. Device and classroom state is stored in memory, RTP audio is unencrypted, and the firmware currently uses the ESP-IDF legacy I2S API. Add authentication and persistence before deploying outside a trusted test network.
