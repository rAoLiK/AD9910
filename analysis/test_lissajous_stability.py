"""Host regression tests for the OpenMV Lissajous decision window.

The deployed script cannot be imported on CPython because it imports OpenMV
hardware modules and starts its camera loop at module scope. This test loads
only the detector/constants from its AST and supplies a tiny ulab-compatible
vector stub; image extraction itself remains an N6 hardware test.
"""

import ast
from pathlib import Path


class Vector(list):
    def __mul__(self, other):
        if isinstance(other, (list, tuple, Vector)):
            return Vector(left * right for left, right in zip(self, other))
        return Vector(value * other for value in self)

    def copy(self):
        return Vector(self)


class FakeNumpy:
    float = float

    @staticmethod
    def array(values, dtype=None):
        del dtype
        return Vector(values)

    @staticmethod
    def sum(values, axis=None):
        assert axis is None
        return sum(values)


class FakeTime:
    @staticmethod
    def ticks_add(value, delta):
        return value + delta

    @staticmethod
    def ticks_diff(new, old):
        return new - old


class FakeFrame:
    def __init__(self, width, height):
        self._width = width
        self._height = height

    def width(self):
        return self._width

    def height(self):
        return self._height


def load_detector():
    source_path = (
        Path(__file__).resolve().parents[1]
        / "openmv"
        / "OpenMV_main_task5_uart.py"
    )
    tree = ast.parse(source_path.read_text(encoding="utf-8"))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            names = [
                target.id
                for target in node.targets
                if isinstance(target, ast.Name)
            ]
            if any(name.startswith("DDS_") for name in names):
                selected.append(node)
        elif (
            isinstance(node, ast.FunctionDef)
            and node.name in ("_clamp", "_ticks_diff")
        ):
            selected.append(node)
        elif (
            isinstance(node, ast.ClassDef)
            and node.name == "LissajousStabilityDetector"
        ):
            selected.append(node)

    namespace = {
        "np": FakeNumpy,
        "time": FakeTime,
        "omv_image": type("ImageModule", (), {"BILINEAR": 0}),
    }
    module = ast.Module(body=selected, type_ignores=[])
    exec(compile(module, str(source_path), "exec"), namespace)
    return namespace


def make_trace(side, first, last, center):
    raw = Vector([0.0] * (side * side))
    for y in range(first, last):
        for x in range(first, last):
            raw[(y * side) + x] = 1.0
    return raw, raw.copy(), float(sum(raw)), center, center


def run(detector_type, samples):
    detector = detector_type()
    detector.reset_test(0)
    decisions = []
    for now_ms, sample in samples:
        decisions.append(detector.update(sample, now_ms))
    return decisions


def main():
    namespace = load_detector()
    base_type = namespace["LissajousStabilityDetector"]
    target = namespace["DDS_RESULT_TARGET_REACHED"]
    not_matched = namespace["DDS_RESULT_NOT_MATCHED"]
    image_error = namespace["DDS_RESULT_IMAGE_ERROR"]
    side = namespace["DDS_FEATURE_SIDE"]

    class SyntheticDetector(base_type):
        def _extract_trace(self, frame):
            return frame

    # The coarse-stage green-trace rectangle must provide a usable fixed
    # square ROI without relying on the connected dark background.
    geometry_detector = base_type()
    geometry_detector.set_trace_seed((100, 70, 90, 65))
    assert geometry_detector._locate_seeded_screen(FakeFrame(320, 240))
    assert geometry_detector.locator_source == 1
    outline = geometry_detector.screen_outline_rect
    assert outline[2] == outline[3]
    assert outline[0] <= 100
    assert outline[1] <= 70
    assert outline[0] + outline[2] >= 190
    assert outline[1] + outline[3] >= 135
    assert geometry_detector.screen_patch_rect is not None

    stable = make_trace(side, 28, 36, 31.5)
    unstable = make_trace(side, 4, 12, 7.5)

    decisions = run(
        SyntheticDetector,
        [(index * 100, stable) for index in range(8)],
    )
    assert decisions[-1][0] == target

    # Eight frames concentrated into 70 ms are not 600 ms of evidence.
    decisions = run(
        SyntheticDetector,
        [(index * 10, stable) for index in range(8)]
        + [(1400, None)],
    )
    assert decisions[-1][0] == image_error

    # Historical votes cannot hide an unstable latest frame.
    decisions = run(
        SyntheticDetector,
        [(index * 100, stable) for index in range(7)]
        + [(700, unstable), (1400, None)],
    )
    assert decisions[7] is None
    assert decisions[-1][0] == not_matched

    # A missing frame breaks continuity; three new adjacent comparisons are
    # required before the detector may recover to TARGET.
    decisions = run(
        SyntheticDetector,
        [
            (0, stable),
            (100, stable),
            (200, stable),
            (300, None),
            (400, stable),
            (500, stable),
            (600, stable),
            (700, stable),
            (800, stable),
        ],
    )
    assert decisions[7] is None
    assert decisions[-1][0] == target
    print("Lissajous stability decision tests passed")


if __name__ == "__main__":
    main()
