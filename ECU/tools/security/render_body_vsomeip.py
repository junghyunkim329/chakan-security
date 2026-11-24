#!/usr/bin/env python3
"""Render BodyControl vSomeIP local-credential policy for a deployment principal."""

import argparse
import json
import os
from pathlib import Path


CLIENT_IDS = ("0x2002", "0x2FFF")
REQUESTS = [{"service": "0x1100", "instance": "0x0001"}]


def _credential(value: int, name: str) -> str:
    if not 0 < value < 2**32:
        raise ValueError(f"{name} must be a non-root 32-bit Linux identifier")
    return str(value)


def render(source: Path, destination: Path, uid: int, gid: int) -> None:
    credentials = {"uid": _credential(uid, "uid"), "gid": _credential(gid, "gid")}
    document = json.loads(source.read_text())
    document["file-permissions"] = {"permissions-uds": "0600"}
    document["routing-credentials"] = dict(credentials)
    document["security"] = {
        "check_credentials": "true",
        "policies": [
            {
                "client": client_id,
                "credentials": dict(credentials),
                "allow": {"requests": list(REQUESTS)},
            }
            for client_id in CLIENT_IDS
        ],
    }
    destination.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    destination.write_text(json.dumps(document, indent=2) + "\n")
    os.chmod(destination, 0o644)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("uid", type=int)
    parser.add_argument("gid", type=int)
    args = parser.parse_args()
    render(args.source, args.destination, args.uid, args.gid)


if __name__ == "__main__":
    main()
