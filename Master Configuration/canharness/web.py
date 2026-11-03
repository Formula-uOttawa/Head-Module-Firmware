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
from .model import Hub, Project
from .validate import errors, validate

STATIC = Path(__file__).parent / "static"


def create_app(project_path: Path, out_dir: Path) -> FastAPI:
    app = FastAPI(title="CAN-harness Master Configuration")

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
        for name, text in generate_head(project).items():
            (d / name).write_text(text, encoding="utf-8")
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
