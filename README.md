# AudioRuntime

Windows x64 native full-duplex audio service for ChatCaht. It is the only process that opens the communication capture and render endpoints. Rendered TTS is passed through WebRTC APM's reverse stream; microphone capture is processed by AEC3, high-pass filtering, noise suppression, and WebRTC VAD before 16 kHz PCM is published.

The 48 kHz APM output is converted to 16 kHz with WebRTC's stateful sinc resampler. Capture frames and near-end events enter a bounded SPSC queue; a dedicated transport thread performs WebSocket sends so network locks and socket backpressure never run on the MMCSS capture thread.

## Build

Use a Visual Studio 2022 x64 developer prompt. CMake downloads dependencies at the pinned commits in `CMakeLists.txt`.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target chat-audio-runtime --parallel 1
```

Use `--parallel 1` for the pinned WebRTC APM dependency; parallel MSVC compilation has been unstable on the supported build host.

The service refuses to report `ready=true` unless both WASAPI endpoints and WebRTC APM are initialized. Managed ChatCaht runs generate the effective config from ChatCaht's `audio:` section:

```powershell
.\build\Release\chat-audio-runtime.exe --config ..\ChatCaht\artifacts\run\audio-runtime.generated.json
```

`configs/config.json` remains a standalone development example. Do not maintain a second production copy there; ChatCaht validates the effective native settings during the initial WebSocket handshake.

The loopback-only WebSocket endpoint is `ws://127.0.0.1:8810/v1/audio/ws`. Text messages carry control JSON; server-to-client binary messages are AEC-clean 16-bit mono PCM at 16 kHz.

## Protocol

Every connection receives a `status` message first. Voice startup requires both `ready: true` and `aec_ready: true`.

- `status`, `ping`, `shutdown`
- `play_start` with `stream_id`, `sample_rate`, and `channels`
- `play_chunk` with an exact byte count, followed by one binary PCM message
- `play_end` to drain the bounded render queue once after all chunks in the logical response
- `play_cancel` to clear queued render audio immediately

The render queue is bounded to 500 ms by default and applies producer backpressure. Arbitrarily large PCM messages are admitted incrementally instead of requiring the whole message to fit. Canceling or replacing playback advances a generation token, so an older producer blocked on backpressure cannot refill the queue after cancellation. Capture WebSocket buffering and ChatCaht routing queues are bounded to 1 second. A client that remains backpressured beyond the consecutive-drop threshold makes `capture_transport_ready` and overall health false. Status includes the protocol version, effective config, capture-drop counters, and AEC operational metrics for capture frames, reverse frames, processing errors, and near-end events. Server events include `near_end_start`, `near_end_end`, `playback_started`, and `playback_ended`. Disconnecting a connection with an active playback generation cancels that generation so stale audio cannot continue.

The service uses the Windows default communication endpoints unless `input_device` or `output_device` is configured. Failure to initialize WASAPI, AEC3, or VAD leaves health unready; there is no legacy fallback.

While running, AudioRuntime checks selected endpoint state, polls default communication endpoint IDs when no explicit device is configured, monitors capture-frame freshness, and monitors render progress whenever playback audio is queued. Device invalidation, five consecutive WASAPI failures, a default endpoint change, or a capture/render stall longer than `capture_stall_timeout_ms` marks health unready and exits with code 4. ChatCaht Supervisor then restarts the whole process, rebuilding WASAPI, APM, VAD, resampler state, and transport threads from a clean state. A standalone launch exits and should be restarted by its process manager.
