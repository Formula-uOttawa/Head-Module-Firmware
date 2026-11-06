# CAN-harness Master Configuration

Single source of truth for hub, head and dash configuration.

    python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
    .venv/bin/python -m canharness.web system.json      # web UI at http://127.0.0.1:8765 (creates system.json if missing)
    .venv/bin/python -m canharness generate system.json --out out
    .venv/bin/python -m pytest tests

CAN IDs follow `../FSAE Sensor Hub CAN Tables.xlsx`. Output: `out/hub_<id>/{hub_<id>.blob,hub_default_blob.c}` (hub config blob and its
compiled-in default), `out/head/{head_default.blob,head_default_blob.c}`, `out/dash/channels.json`.

`generate system.json --deploy` copies the head default into `../Common/Src`, `--deploy-hub <id>` copies that hub's default into
`../../SensorHubV1/Core/Src`. Both boards load their config at runtime (head: blob over UART; hub: blob over the head's CAN relay or the hub's
UART, stored in the hub's flash); the compiled-in defaults only matter for a fresh board. Details: `docs/protocol.md` and `docs/firmware.md`.
