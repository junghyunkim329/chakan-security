#!/usr/bin/env python3
"""Install the complete pinned vSomeIP runtime used by the BodyControl client."""

import argparse
import shutil
from pathlib import Path


REQUIRED_MODULES = (
    "libvsomeip3.so.3",
    "libvsomeip3-cfg.so.3",
    "libvsomeip3-sd.so.3",
    "libvsomeip3-e2e.so.3",
)
BOOST_FILESYSTEM = "libboost_filesystem.so.1.90.0"


def stage(source: Path, boost_source: Path, destination: Path) -> None:
    resolved = []
    for name in REQUIRED_MODULES:
        library = (source / name).resolve(strict=True)
        if not library.is_file():
            raise FileNotFoundError(f"vSomeIP runtime module is not a file: {name}")
        resolved.append((name, library))
    boost_library = (boost_source / BOOST_FILESYSTEM).resolve(strict=True)
    if not boost_library.is_file():
        raise FileNotFoundError(f"Boost runtime library is not a file: {BOOST_FILESYSTEM}")
    resolved.append((BOOST_FILESYSTEM, boost_library))

    destination.mkdir(mode=0o755, parents=True, exist_ok=True)
    for name, library in resolved:
        shutil.copy2(library, destination / name)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("boost_source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    stage(args.source, args.boost_source, args.destination)


if __name__ == "__main__":
    main()
