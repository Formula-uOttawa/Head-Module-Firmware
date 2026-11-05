"""Local web UI: python -m canharness.web [project.json] [--port N]"""

from __future__ import annotations

import argparse
import threading
import webbrowser
from pathlib import Path

import uvicorn
import yaml
from fastapi import Body, FastAPI, HTTPException
from fastapi.responses import FileResponse, JSONResponse

from .analysis import analyze, hw_info
from .generators.dash_json import generate_dash
from .generators.head_c import generate_head
from .generators.hub_c import generate_hub
from .legacy import from_data
from .blob import build as build_blob, parse as parse_blob
from .device import messages as devmsg
from .device.session import DeviceSession, list_ports
from .model import Hub, Project
from .validate import errors, validate

STATIC = Path(__file__).parent / "static"


def create_app(project_path: Path, out_dir: Path, transport_factory=None) -> FastAPI:
    app = FastAPI(title="CAN-harness Master Configuration")
    dev = DeviceSession(transport_factory)

    def device_call(fn, *a):
        """Run a device operation, mapping device/link failures to HTTP errors the UI can show."""
        try:
            return fn(*a)
        except devmsg.DeviceError as e:
            raise HTTPException(409, detail=str(e))
        except (TimeoutError, ConnectionError) as e:
            raise HTTPException(504, detail=str(e) or "device did not answer")
        except OSError as e:
            raise HTTPException(502, detail=f"serial error: {e}")
        except HTTPException:
            raise
        except Exception as e:  # a bug or an unexpected reply: say what, instead of a bare "Internal Server Error"
            raise HTTPException(500, detail=f"{type(e).__name__}: {e}")

    if not project_path.exists():
        Project(hubs=[Hub(hub_id=0, name="Hub 0")]).save(project_path)

    @app.get("/")
    def index():
        return FileResponse(STATIC / "index.html")

    @app.get("/api/project")
    def get_project():
        return JSONResponse({"path": str(project_path), "project": Project.load(project_path).model_dump(mode="json")})

    @app.get("/api/hw")
    def get_hw():
        return hw_info()

    @app.post("/api/analyze")
    def post_analyze(body: dict = Body(...)):
        return analyze(Project.model_validate(body))

    @app.put("/api/project")
    def put_project(body: dict = Body(...)):
        project = Project.model_validate(body)
        project.save(project_path)
        return {"saved": True, "config_hash": project.config_hash()}

    @app.post("/api/generate")
    def post_generate(body: dict = Body(...)):
        project = Project.model_validate(body)
        issues = validate(project)
        if errors(issues):
            raise HTTPException(422, detail=[str(i) for i in errors(issues)])
        files = []
        for h in project.hubs:
            d = out_dir / f"hub_{h.hub_id}"
            d.mkdir(parents=True, exist_ok=True)
            f = d / "sensor_config_generated.c"
            f.write_text(generate_hub(project, h.hub_id), encoding="utf-8")
            files.append(str(f))
        d = out_dir / "head"
        d.mkdir(parents=True, exist_ok=True)
        for name, data in generate_head(project).items():
            (d / name).write_bytes(data if isinstance(data, bytes) else data.encode())
            files.append(str(d / name))
        d = out_dir / "dash"
        d.mkdir(parents=True, exist_ok=True)
        f = d / "channels.json"
        f.write_text(generate_dash(project), encoding="utf-8")
        files.append(str(f))
        return {"files": files, "warnings": [str(i) for i in issues]}

    @app.post("/api/import-legacy")
    def post_import(body: dict = Body(...)):
        try:
            project = from_data(yaml.safe_load(body["yaml"]), body.get("name", "legacy"))
        except Exception as e:  # malformed YAML / missing keys
            raise HTTPException(400, detail=f"could not import: {e}")
        return project.model_dump(mode="json")

    # ---- device ------------------------------------------------------------------------------------------------
    @app.get("/api/device/ports")
    def dev_ports():
        return {"ports": list_ports(), "connected": dev.port}

    @app.post("/api/device/connect")
    def dev_connect(body: dict = Body(...)):
        info = device_call(dev.connect, body["port"], int(body.get("baud", 115200)))
        return {"port": dev.port, "info": info}

    @app.post("/api/device/disconnect")
    def dev_disconnect():
        dev.disconnect()
        return {"connected": None}

    @app.get("/api/device/status")
    def dev_status(since: int = 0):
        return device_call(dev.status, since)

    @app.post("/api/device/ascii")
    def dev_ascii(body: dict = Body(...)):
        return device_call(dev.ascii, str(body["line"]))

    @app.get("/api/device/info")
    def dev_info():
        return device_call(dev.info)

    @app.post("/api/device/upload")
    def dev_upload(body: dict = Body(...)):
        project = Project.model_validate(body["project"])
        issues = validate(project)
        if errors(issues):
            raise HTTPException(422, detail=[str(i) for i in errors(issues)])
        try:
            blob = build_blob(project)
        except ValueError as e:
            raise HTTPException(422, detail=[str(e)])
        res = device_call(dev.upload, blob, bool(body.get("apply", True)), bool(body.get("store", False)))
        return dict(res, bytes=len(blob), project_hash=project.config_hash())

    @app.get("/api/device/config")
    def dev_config(source: str = "active"):
        blob = device_call(dev.download, source)
        parsed = parse_blob(blob)
        return {"bytes": len(blob), "hash": f"{parsed['config_hash']:016x}",
                "counts": {k: len(v) for k, v in parsed.items() if isinstance(v, list)}}

    @app.post("/api/device/clear")
    def dev_clear(body: dict = Body(...)):
        device_call(dev.clear, bool(body.get("revert", True)))
        return {"ok": True}

    @app.post("/api/device/reboot")
    def dev_reboot():
        device_call(dev.reboot)
        return {"ok": True}

    @app.get("/api/device/live")
    def dev_live():
        return device_call(dev.live)

    @app.get("/api/device/stats")
    def dev_stats():
        return device_call(dev.stats)

    @app.post("/api/device/sniff")
    def dev_sniff(body: dict = Body(...)):
        device_call(dev.sniff, int(body["bus"]), bool(body["enable"]))
        return {"ok": True}

    @app.get("/api/device/sniff")
    def dev_sniff_poll(since: int = 0):
        return device_call(dev.sniff_poll, since)

    return app


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("project", nargs="?", type=Path, default=Path("system.json"))
    ap.add_argument("--out", type=Path, default=Path("out"))
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    app = create_app(a.project.resolve(), a.out.resolve())
    if not a.no_browser:
        threading.Timer(1.0, lambda: webbrowser.open(f"http://127.0.0.1:{a.port}")).start()
    uvicorn.run(app, host="127.0.0.1", port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
