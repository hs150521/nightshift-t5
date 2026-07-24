"""Verify both endpoint copies of the independent frozen UART contract."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def normalized_vectors(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return data["golden_vectors"]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-root",
        type=Path,
        default=root.parent / "nightshift-opi",
    )
    args = parser.parse_args()
    target_dir = root / "contracts" / "uart"
    source_dir = args.source_root / "contracts" / "uart"
    target_schema = (target_dir / "commands.yaml").read_text(encoding="utf-8")
    source_schema = (source_dir / "commands.yaml").read_text(encoding="utf-8")
    if target_schema.replace("\r\n", "\n") != source_schema.replace("\r\n", "\n"):
        raise SystemExit("shared commands.yaml copies differ")
    if normalized_vectors(target_dir / "golden_vectors.json") != \
            normalized_vectors(source_dir / "golden_vectors.json"):
        raise SystemExit("canonical golden vectors differ")
    print(f"schema and golden vectors match {args.source_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
