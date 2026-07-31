"""Host regression tests for the OpenMV Lissajous area-ratio decision.

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


def pixels_for_density(side, density):
    return float(side * side) * density


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

    target_density = 0.10
    mismatched_density = 0.20
    target_trace = pixels_for_density(side, target_density)
    mismatched_trace = pixels_for_density(side, mismatched_density)

    # The 13% threshold covers all 68 same-frequency calibration captures;
    # their measured maximum was 12.207%.
    assert namespace["DDS_TARGET_MAX_TRACE_DENSITY"] >= 0.1221
    assert namespace["DDS_TARGET_MAX_TRACE_DENSITY"] < 0.15

    decisions = run(
        SyntheticDetector,
        [(index * 100, target_trace) for index in range(8)],
    )
    assert decisions[-1][0] == target

    # Eight frames concentrated into 70 ms are not 600 ms of evidence.
    decisions = run(
        SyntheticDetector,
        [(index * 10, target_trace) for index in range(8)]
        + [(1400, None)],
    )
    assert decisions[-1][0] == image_error

    # A persistent multi-curve area does not match even when it is temporally
    # stationary; this detector intentionally uses occupied area, not shape.
    decisions = run(
        SyntheticDetector,
        [(index * 100, mismatched_trace) for index in range(8)]
        + [(1400, None)],
    )
    assert decisions[-1][0] == not_matched

    # One briefly dark frame cannot cause a false stop when most observations
    # remain above the target-area threshold.
    decisions = run(
        SyntheticDetector,
        [(index * 100, mismatched_trace) for index in range(7)]
        + [(700, target_trace), (1400, None)],
    )
    assert decisions[-1][0] == not_matched

    # Some refresh/exposure outliers are tolerated, but the average area,
    # low-density vote ratio, and final consecutive run must all agree.
    samples = [
        (0, mismatched_trace),
        (100, mismatched_trace),
        (200, target_trace),
        (300, target_trace),
        (400, target_trace),
        (500, target_trace),
        (600, target_trace),
        (700, target_trace),
    ]
    decisions = run(SyntheticDetector, samples)
    assert decisions[-1][0] == target

    # No visible green trace remains an image acquisition error, not a
    # business-level NOT_MATCHED result that would advance the DDS scan.
    no_trace = pixels_for_density(side, 0.005)
    decisions = run(
        SyntheticDetector,
        [(index * 100, no_trace) for index in range(8)]
        + [(1400, None)],
    )
    assert decisions[-1][0] == image_error
    print("Lissajous area-ratio decision tests passed")


if __name__ == "__main__":
    main()
