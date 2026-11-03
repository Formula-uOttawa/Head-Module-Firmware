# CAN-harness Master Configuration

Single source of truth for hub, head and dash configuration.

    python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
    .venv/bin/python -m canharness.web system.json      # web UI at http://127.0.0.1:8765 (creates system.json if missing)
    .venv/bin/python -m canharness generate system.json --out out
    .venv/bin/python -m pytest tests

CAN IDs follow `../FSAE Sensor Hub CAN Tables.xlsx`. Output: `out/hub_<id>/sensor_config_generated.c`, `out/head/{head_config.h,head_config_generated.c,head_decode.c}`, `out/dash/channels.json`.

Head integration: call `Head_Init()` once, then `Head_ProcessFrame(bus, id, extended, data, len, now_ms)` for every received frame (bus = `HEAD_BUS_<ID>`). Latest values are in `g_head_values[slot]` (`HEAD_CHANNELS[slot].value_type`). Set each FDCAN's `StdFiltersNbr`/`ExtFiltersNbr` to the `HEAD_BUS_<ID>_STD/EXT_FILTERS` values and load `HEAD_BUSES[].std_filters/ext_filters`.
