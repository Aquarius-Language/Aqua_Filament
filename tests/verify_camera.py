"""Pixel regression for a DPI-unaware Windows host and the headless framebuffer.

Requires Pillow. Run from the package root: python tests/verify_camera.py
The old WGL driver draws only a corner of the viewport at 250% display scaling.
"""
import math
from pathlib import Path
import sys
import tempfile

from PIL import Image
from native_driver import Lib


def verify(library, headless, output):
    root = Path(__file__).resolve().parents[1]
    lib = Lib(str(library))
    try:
        lib.call([
            {"op": "init", "width": 320, "height": 180, "headless": headless,
             "visible": False, "materials": str(root / "materials")},
            {"op": "view", "postProcessing": False},
            {"op": "camera", "eye": [0, 0, 4], "target": [0, 0, 0], "up": [0, 1, 0]},
            {"op": "instance", "id": 1, "material": "unlit",
             "params": {"baseColor": [1, 0, 0, 1], "intensity": 1}},
            {"op": "shape", "id": 2, "shape": "box", "size": [1, 1, 1], "material": 1},
        ])
        for mode, fov in [("none", 45), ("orbit", 55), ("fly", 45)]:
            lib.call([{"op": "lens", "fov": fov}, {"op": "cameraControl", "mode": mode}])
            for _ in range(3):
                lib.call([{"op": "frame", "wait": True}])
            path = output / f"camera_{'headless' if headless else 'window'}_{mode}.png"
            lib.call([{"op": "screenshot", "path": str(path)}])
            image = Image.open(path).convert("RGB")
            mask = image.point(lambda v: v).split()[0].point(lambda r: 255 if r > 200 else 0)
            bounds = mask.getbbox()
            assert bounds, f"No cube in {path} (viewport/backbuffer DPI mismatch)"
            left, top, right, bottom = bounds
            width, height = image.size
            assert abs((left + right) / 2 - width / 2) <= 2, bounds
            assert abs((top + bottom) / 2 - height / 2) <= 2, bounds
            expected = height / (7 * math.tan(math.radians(fov / 2)))
            assert abs((right - left) - expected) <= 3, (bounds, expected)
            assert abs((bottom - top) - expected) <= 3, (bounds, expected)
            print(f"PASS {'headless' if headless else 'window'} {mode}: {width}x{height}, FOV {fov}")
    finally:
        lib.shutdown()


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    library = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "runtimes/windows/aqua_filament.dll"
    with tempfile.TemporaryDirectory(prefix="aqua-camera-", dir=root / "tests") as directory:
        for headless in (False, True):
            verify(library, headless, Path(directory))
