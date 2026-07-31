"""Host regression tests for the OpenMV line/ellipse-family decision.

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


def make_trace(detector_type, side, density, axis_ratio, radial_cv, runs):
    family, score = detector_type._classify_geometry(
        axis_ratio,
        radial_cv,
        runs,
    )
    if density > 0.30 or density < 0.015:
        family = False
    return (
        float(side * side) * density,
        axis_ratio,
        radial_cv,
        runs,
        score,
        family,
    )


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

    # A thin trace passes the line branch even though its ellipse-normalized
    # radial spread is high. A circle passes the ellipse branch. Dense
    # multi-strand and non-conic shapes fail independently of bright area.
    line_trace = make_trace(
        base_type, side, 0.09, 0.08, 1.50, 0.10
    )
    ellipse_trace = make_trace(
        base_type, side, 0.09, 0.80, 0.20, 0.10
    )
    multi_trace = make_trace(
        base_type, side, 0.20, 0.50, 0.40, 0.85
    )
    nonconic_trace = make_trace(
        base_type, side, 0.10, 0.50, 0.90, 0.10
    )
    assert line_trace[-1]
    assert ellipse_trace[-1]
    assert not multi_trace[-1]
    assert not nonconic_trace[-1]

    # Boundary samples from the 68-frame real positive set remain accepted,
    # while the two recorded multi-strand captures remain rejected.
    for measured in (
        (0.1463, 0.6587, 0.1697),
        (0.1653, 0.5706, 0.1697),
        (0.9635, 0.0940, 0.10),
    ):
        assert base_type._classify_geometry(*measured)[0]
    for measured in (
        (0.5089, 0.6496, 2.3535),
        (0.5895, 0.4733, 0.8361),
    ):
        assert not base_type._classify_geometry(*measured)[0]

    decisions = run(
        SyntheticDetector,
        [(index * 10, ellipse_trace) for index in range(12)],
    )
    assert decisions[-1][0] == target

    # Ten valid frames concentrated into 45 ms are not 100 ms of evidence.
    decisions = run(
        SyntheticDetector,
        [(index * 5, line_trace) for index in range(10)]
        + [(320, None)],
    )
    assert decisions[-1][0] == image_error

    # A persistent multi-curve is rejected in roughly 60 ms instead of
    # consuming the complete candidate deadline.
    decisions = run(
        SyntheticDetector,
        [(index * 10, multi_trace) for index in range(8)],
    )
    assert decisions[-1][0] == not_matched

    # One accidental conic-looking frame inside a non-conic run cannot stop
    # the scan; six subsequent non-family frames trigger early rejection.
    decisions = run(
        SyntheticDetector,
        [
            (0, multi_trace),
            (10, multi_trace),
            (20, multi_trace),
            (30, ellipse_trace),
            (40, multi_trace),
            (50, multi_trace),
            (60, multi_trace),
            (70, multi_trace),
            (80, multi_trace),
            (90, multi_trace),
        ],
    )
    assert decisions[-1][0] == not_matched

    # Two initial refresh outliers are tolerated once at least 80% of the
    # window belongs to the family and its average geometry score recovers.
    samples = [(0, multi_trace), (10, multi_trace)] + [
        (20 + index * 10, ellipse_trace) for index in range(10)
    ]
    decisions = run(SyntheticDetector, samples)
    assert decisions[-1][0] == target

    # No visible green trace remains an image acquisition error, not a
    # business-level NOT_MATCHED result that would advance the DDS scan.
    no_trace = make_trace(
        base_type, side, 0.005, 0.01, 0.10, 0.0
    )
    decisions = run(
        SyntheticDetector,
        [(index * 10, no_trace) for index in range(10)]
        + [(320, None)],
    )
    assert decisions[-1][0] == image_error
    print("Lissajous line/ellipse-family decision tests passed")


if __name__ == "__main__":
    main()
