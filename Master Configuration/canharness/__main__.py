"""CLI: python -m canharness {import-legacy,validate,generate} ..."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .generators.dash_json import generate_dash
from .generators.head_c import generate_head
from .generators.hub_c import generate_hub
from .legacy import import_yaml
from .model import Project
from .validate import errors, validate


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="canharness")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("import-legacy", help="import a SensorHubV1 YAML config")
    p.add_argument("yaml", type=Path)
    p.add_argument("project", type=Path)
    p = sub.add_parser("validate")
    p.add_argument("project", type=Path)
    p = sub.add_parser("generate")
    p.add_argument("project", type=Path)
    p.add_argument("--out", type=Path, default=Path("out"))
    a = ap.parse_args(argv)

    if a.cmd == "import-legacy":
        import_yaml(a.yaml).save(a.project)
        return 0
    project = Project.load(a.project)
    issues = validate(project)
    for i in issues:
        print(i)
    if errors(issues):
        return 1
    if a.cmd == "generate":
        for h in project.hubs:
            d = a.out / f"hub_{h.hub_id}"
            d.mkdir(parents=True, exist_ok=True)
            (d / "sensor_config_generated.c").write_text(generate_hub(project, h.hub_id), encoding="utf-8")
        d = a.out / "head"
        d.mkdir(parents=True, exist_ok=True)
        for name, text in generate_head(project).items():
            (d / name).write_text(text, encoding="utf-8")
        d = a.out / "dash"
        d.mkdir(parents=True, exist_ok=True)
        (d / "channels.json").write_text(generate_dash(project), encoding="utf-8")
        print(f"generated into {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
