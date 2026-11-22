# Bench spike

Throwaway: answers spikes S1 to S3 of [the design](../../docs/remote-flashing-design.md) §9,
and is deleted in step 5 of [the plan](../../docs/remote-flashing-plan.md).

- `./build-firmware.sh` builds `hello_world` and the abort variant into `build/`.
- `npm install && node server.mjs` serves the page on `:3520`, which is
  `https://bench.<env-id>.home`, and a control API on `127.0.0.1:3521`:
  `GET /status`, `GET /events?n=`, `GET /log?n=`, `POST /job` with
  `{"kind":"flash","fw":"hello_world"}` (or `connect`, `reset`), `POST /s2`.
- What the page reports lands in `run/events.jsonl`, `run/serial.log` and `run/findings.json`.
