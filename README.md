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
| 3000 | TCP | Dashboard HTTPS and Socket.IO |
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

## Run on the local network

The project includes `start-local.ps1`, which detects the computer's LAN IPv4 address, uses the existing certificate files in `server/certs`, installs dependencies if needed, and starts both Node processes.

From PowerShell:

```powershell
cd C:\Users\thesi\Desktop\Speechlab_Dashboard
.\start-local.ps1
```

Open the printed URL from another device on the same network, for example:

```powershell
https://192.168.0.18:3000
```

The certificate is self-signed, so accept the browser certificate warning once on each device. The ESP32 bridge remains on port `8081`, and ESP32 firmware should continue using the computer's LAN address as `SIGNALING_HOST`.

### Trust the certificate on Android

The default certificate is suitable for a trusted classroom LAN, but Android Chrome or an Android WebView may show a TLS/SSL warning because it is self-signed. For a more convenient LAN certificate, install `mkcert` on the server computer.

If Chocolatey is unavailable, use Windows Package Manager:

```powershell
winget install FiloSottile.mkcert
mkcert -install
mkcert 192.168.0.18 localhost 127.0.0.1
```

Copy the generated certificate and key to:

```text
server/certs/lan-cert.pem
server/certs/lan-key.pem
```

Find the local certificate authority file with:

```powershell
mkcert -CAROOT
```

Install `rootCA.pem` on the Android tablet as a trusted CA certificate, then restart `start-local.ps1`. A public certificate authority such as Let's Encrypt cannot issue a certificate for a private address like `192.168.0.18`; use a real domain name for a publicly trusted certificate.

### Android web-to-app wrappers

If the dashboard is packaged with a WebView-based tool, configure the app for HTTPS, microphone permission, and private-network access. The tablet and server must remain on the same Wi-Fi network. A self-signed certificate may still need to be installed or explicitly trusted inside the Android app; packaging the page does not automatically make the certificate trusted.

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
| Status WS2812B data | 48 |
| Mute WS2812B data | 47 |
| Mute button | 4 |

Power the INMP441 from 3.3 V and connect its L/R pin to the selected channel. Power the MAX98357A from a suitable 3-5 V supply, connect SD/EN HIGH, and connect the speaker only between its `+` and `-` outputs. Use a common ground and verify GPIOs against the exact ESP32-S3 board before wiring.

The onboard status WS2812B shows connection state: red means no Wi-Fi, orange means Wi-Fi is connected but the signaling server is unavailable, and green means the ESP32 WebSocket is connected to the bridge. The external mute WS2812B is red when the ESP32 microphone is muted and green when it is unmuted. The mute button is active-low and uses the firmware's internal pull-up; pressing it toggles the microphone mute state.

The GPIO values are defined near the top of `main.c` and can be changed if the board wiring differs:

```c
#define STATUS_LED_GPIO 48
#define MUTE_LED_GPIO 47
#define MUTE_BUTTON_GPIO 4
```

The firmware uses a legacy ESP-IDF I2S API for the current audio implementation. The related deprecation message during compilation is a warning, not a build failure.

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

## Teacher access and audio controls

Only one browser dashboard can be the active teacher at a time. The first browser to initialize teacher audio owns the teacher WebRTC transports and producer. A second browser is redirected to `server-busy.html` and cannot replace or close the active teacher's audio producer. When the active browser disconnects, another browser can connect.

The dashboard Push-to-Talk setting supports:

- **Hold to Talk:** hold the PTT button to transmit; release it to mute.
- **Toggle:** press once to transmit and press again to mute.

The PTT button uses pointer events and disables long-press browser menus for Android tablet use.

## Troubleshooting

- **ESP32 connects but the teacher hears no student audio:** verify the firmware logged its dynamic RTP target, the target IP is the server's LAN IP, UDP ports `40000-40100` are allowed through Windows Firewall, and the firmware/server PCMU settings and SSRC match.
- **Teacher audio does not reach the ESP32:** verify the ESP32 is listening on UDP `5004`, the teacher microphone is enabled, and the Node server reports a connected teacher audio consumer.
- **Audio uses the wrong teacher output:** select the output device in the dashboard and check browser autoplay permission.
- **Build reports an old Python environment:** activate ESP-IDF, run `idf.py fullclean`, then run `idf.py build` again.

## Limitations

The system is intended for a trusted LAN. Device and classroom state is stored in memory, RTP audio is unencrypted, and the firmware currently uses the ESP-IDF legacy I2S API. Add authentication and persistence before deploying outside a trusted test network.
