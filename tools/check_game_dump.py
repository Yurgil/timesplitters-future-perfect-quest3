"""Check the executable revision and layout of an extracted US Xbox dump."""

import argparse
import hashlib
import sys
from pathlib import Path


EXPECTED_XBE_SHA256 = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path, help="directory containing default.xbe, pak/ and xmv/")
    game_dir = parser.parse_args().game_dir

    if not game_dir.is_dir():
        print(f"Not a directory: {game_dir}", file=sys.stderr)
        return 1

    xbe = game_dir / "default.xbe"
    if not xbe.is_file():
        print(f"Missing executable: {xbe}", file=sys.stderr)
        return 1

    for folder in ("pak", "xmv"):
        if not (game_dir / folder).is_dir():
            print(f"Missing directory: {game_dir / folder}", file=sys.stderr)
            return 1

    try:
        digest = hashlib.sha256()
        with xbe.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        print(f"Cannot read default.xbe: {error}", file=sys.stderr)
        return 1

    actual = digest.hexdigest()
    if actual != EXPECTED_XBE_SHA256:
        print(f"Unsupported default.xbe (SHA-256: {actual}).", file=sys.stderr)
        print(f"Expected US Xbox executable: {EXPECTED_XBE_SHA256}", file=sys.stderr)
        return 1

    print("Supported US Xbox executable and extracted directory layout.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
