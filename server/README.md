# SpeechLab server

Flask-SocketIO backend for the teacher dashboard and ESP32-S3 classroom devices.

- `GET /api/state` returns the current classroom snapshot.
- `register` creates or updates an ESP32 device.
- `device_event` accepts device status events.
- `teacher_command` broadcasts teacher commands.

Run from the repository root with `python server/app.py`, or activate the virtual environment described in the root README first.
