"""Copy and verify the canonical Orange Pi UART golden-vector artifact."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def normalized_vectors(path: Path) -> list[dict]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return [
        {
            "name": item["name"],
            "sequence": item["sequence"],
            "command": item["command"],
            "command_name": item["command_name"],
            "raw_hex": item["raw_hex"],
        }
        for item in data["golden_vectors"]
    ]


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=root.parent / "nightshift-opi" / "contracts" / "uart" / "golden_vectors.json",
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    target = root / "contracts" / "uart" / "golden_vectors.json"
    source_vectors = normalized_vectors(args.source)
    target_vectors = normalized_vectors(target)
    if args.check:
        if source_vectors != target_vectors:
            raise SystemExit("golden vectors differ from Orange Pi contract")
        print(f"golden vectors match {args.source}")
        return 0
    target.write_text(
        json.dumps({"golden_vectors": source_vectors}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"updated {target} from {args.source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
