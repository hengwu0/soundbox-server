# soundbox-server

`soundbox-server` is a local service for the Xiaoai soundbox realtime path:

```text
Xiaoai WebSocket audio -> frontend -> KWS -> session_start -> llm_start
  -> AEC -> FileRecorder WAV -> session_end -> llm_stop -> KWS
```

Playback PCM can also be written to `frontend_playback.sock`; the frontend packs it
as a `tag=play` WebSocket binary payload and sends it to Xiaoai.

## Build

```sh
cmake -S . -B .
cmake --build .
ctest --output-on-failure
```

The build prepares vendored WebRTC APM, ixwebsocket, nlohmann_json, spdlog, and
sherpa-onnx from `third_party/archives`.

## Run

Edit `apm.yaml` first:

```yaml
soundbox:
  ws_url: "ws://192.168.0.50:4399/"
  ws_token: "listen-code-from-open-xiaoai-client"
```

Then run:

```sh
./soundbox-server --config apm.yaml --output output/aec_processed.wav
```

Production startup does not require `--input`. Missing `soundbox.ws_url` or
`soundbox.ws_token` is a startup error.

## open-xiaoai-client Requirement

Start `open-xiaoai-client` in listen mode and use the listen code as
`soundbox.ws_token`:

```sh
open-xiaoai-client -l
```

## Audio Formats

KWS mode:

```text
1ch / S16_LE / 16000 Hz
```

AEC mode:

```text
2ch / S16_LE / 16000 Hz / interleaved
ch0 = near / mic
ch1 = playback reference
```

AEC output to FileRecorder:

```text
1ch / S16_LE / 16000 Hz WAV
```

Playback socket input:

```text
1ch / S16_LE / 24000 Hz
```

## Local Sockets

Given `socket_dir: "/tmp/soundbox-server"`:

```text
/tmp/soundbox-server/frontend_kws.sock
  frontend -> KWS: 1ch/16k/S16 PCM
  KWS -> frontend: session_start JSON line

/tmp/soundbox-server/frontend_aec.sock
  frontend -> AEC: 2ch/16k/S16 PCM
  AEC -> frontend: session_end JSON line

/tmp/soundbox-server/aec_llm.sock
  AEC -> FileRecorder: 1ch/16k/S16 PCM

/tmp/soundbox-server/frontend_playback.sock
  mock playback/LLM -> frontend: playback PCM
```

All listen sockets are single-client sockets. If the current client disconnects,
the listener returns to `accept()` for the next client.

## Session Control Protocol

KWS hit:

```json
{"type":"session_start","reason":"kws_hit","score":0,"timestamp_ms":123456}
```

AEC session end:

```json
{"type":"session_end","reason":"input_eof","timestamp_ms":123456}
```

The frontend only accepts `session_start` while in KWS mode and only accepts
`session_end` while in AEC mode. Duplicate messages in other states are logged and
ignored.

## AEC MD5 Test

The file-type frontend is only a test tool:

```text
tests/aec/file_audio_stream_frontend.cpp
tests/aec/file_audio_stream_frontend.hpp
```

The AEC regression fixture is:

```text
tests/fixtures/aec_2ch_16k.s16
tests/fixtures/expected_aec_processed.md5
```

Run:

```sh
ctest --output-on-failure
```

If WebRTC APM code or parameters intentionally change, regenerate
`expected_aec_processed.md5` from the new `aec_processed.wav` and record the reason
in the change.

## Mock Soundbox Smoke Test

`audio_processing_module_tests` also starts an in-process mock Xiaoai WebSocket
server and fake single-client KWS/AEC sockets. The smoke path verifies:

```text
frontend websocket connect
  -> start_play
  -> fast_recording
  -> KWS record PCM routed to frontend_kws.sock
  -> session_start
  -> llm_start
  -> AEC record PCM routed to frontend_aec.sock
  -> session_end
  -> llm_stop
  -> playback PCM forwarded as websocket tag=play
```

Run it with the normal test command:

```sh
ctest --output-on-failure
```

## Common Failures

`missing required config: soundbox.ws_url`

Set `soundbox.ws_url` in `apm.yaml`.

`missing required config: soundbox.ws_token`

Run `open-xiaoai-client -l` and copy the listen code into `soundbox.ws_token`.

`missing required KWS asset`

Check that `assets/keywords.txt`, `assets/tokens.txt`, `assets/encoder.onnx`,
`assets/decoder.onnx`, and `assets/joiner.onnx` exist or update the `wakeup`
paths in `apm.yaml`.

`llm_start` or `llm_stop` timeout

Check the Xiaoai WebSocket connection, token, and client logs. During start/stop
transitions the frontend drops incoming record audio to avoid mixing KWS and AEC
formats.

Socket connect failure

Remove stale socket files under `socket_dir` or restart the service. The service
also unlinks its own socket files before binding.
