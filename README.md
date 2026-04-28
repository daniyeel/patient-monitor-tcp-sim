# patient-monitor-sim

A small C++17 exercise simulating a bedside patient monitor streaming vitals over TCP to a central listener — built to explore POSIX socket programming and NDJSON message framing in a healthcare-connectivity context.

## What it does

`device_sim` connects to a TCP listener and sends one newline-delimited JSON (NDJSON) message per second containing simulated patient vitals: heart rate, SpO2, and systolic/diastolic blood pressure. Values evolve via a Gaussian random-walk model seeded from the hardware RNG, so the stream looks plausible rather than static. The process exits cleanly on Ctrl-C or when the listener closes the connection.

## Build and run

```bash
# Build
./compile.sh

# Terminal 1 — listen for incoming data
nc -l 9000

# Terminal 2 — run the simulator
./device_sim --id bed-3
```

Sample output in the `nc` terminal:

```
{"device_id":"bed-3","ts":1730000001,"hr":72,"spo2":98,"sys":120,"dia":80}
{"device_id":"bed-3","ts":1730000002,"hr":73,"spo2":98,"sys":121,"dia":79}
{"device_id":"bed-3","ts":1730000003,"hr":72,"spo2":97,"sys":120,"dia":80}
{"device_id":"bed-3","ts":1730000004,"hr":71,"spo2":98,"sys":119,"dia":81}
```

### CLI options

| Flag | Default | Description |
|---|---|---|
| `--host` | `127.0.0.1` | Listener hostname or IP |
| `--port` | `9000` | Listener TCP port |
| `--id` | `bed-1` | Value for the `device_id` field |
| `--interval` | `1000` | Milliseconds between messages |

## Protocol

Each message is a single JSON object followed by a newline (`\n`), i.e. NDJSON:

```
{"device_id":"bed-1","ts":1730000000,"hr":72,"spo2":98,"sys":120,"dia":80}
```

| Field | Type | Description |
|---|---|---|
| `device_id` | string | Identifier set via `--id` |
| `ts` | integer | Unix timestamp (seconds) |
| `hr` | integer | Heart rate, bpm (50–110). 50: athletic bradycardia; 110: mild tachycardia |
| `spo2` | integer | Oxygen saturation, % (90–100). Below 90% is clinical hypoxemia |
| `sys` | integer | Systolic blood pressure, mmHg (90–160). 90: hypotension; 160: stage 2 hypertension |
| `dia` | integer | Diastolic blood pressure, mmHg (60–100). 60: low diastolic; 100: hypertension threshold |

NDJSON was chosen over length-prefixed binary because each message is self-describing and human-readable with no tooling. `nc`, `grep`, and `jq` all work out of the box. For a production system carrying high-frequency waveforms (ECG, PPG) the framing overhead would matter; for 1 Hz vitals it is negligible.

## Limitations

- **Linux/WSL only** — uses POSIX sockets with `MSG_NOSIGNAL`; no Windows/Winsock path.
- **No TLS or authentication** — data is sent in plaintext; suitable only for local development.
- **No reconnect** — if the listener drops, the process exits.
- **Single listener** — one connection per process; no multi-client fanout.
- **No test suite** — this is a learning exercise, not production code.

## Environment

Developed and tested on WSL2 (Ubuntu) on Windows 11.
