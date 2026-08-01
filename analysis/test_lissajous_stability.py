"""Host regression tests for the OpenMV line/ellipse-family decision.

The deployed script cannot be imported on CPython because it imports OpenMV
hardware modules and starts its camera loop at module scope. This test loads
only the detector/constants from its AST and supplies a tiny ulab-compatible
vector stub; image extraction itself remains an N6 hardware test.
"""

import ast
import math
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
        self.drawn_strings = []

    def width(self):
        return self._width

    def height(self):
        return self._height

    def draw_rectangle(self, *args, **kwargs):
        del args, kwargs

    def draw_string(self, position, value, **kwargs):
        del position, kwargs
        self.drawn_strings.append(value)


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
            if any(
                name.startswith("DDS_")
                or name.startswith("VISUAL_")
                or name.startswith("STATE_")
                for name in names
            ):
                selected.append(node)
        elif (
            isinstance(node, ast.FunctionDef)
            and node.name in (
                "_clamp",
                "_ticks_diff",
                "clamp_int",
                "annotate",
            )
        ):
            selected.append(node)
        elif (
            isinstance(node, ast.ClassDef)
            and node.name in (
                "FoldedPhaseSpeedTracker",
                "LissajousStabilityDetector",
            )
        ):
            selected.append(node)

    namespace = {
        "np": FakeNumpy,
        "time": FakeTime,
        "math": math,
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
    if (
        density > 0.30
        or density < 0.015
        or density > detector_type._max_family_density(axis_ratio)
    ):
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
    tracker_type = namespace["FoldedPhaseSpeedTracker"]

    assert math.isclose(
        base_type._visual_phase_proxy(0, 0.5, 0.5, 0.5, 0.0),
        math.acos(0.5),
    )
    generalized_positive = base_type._visual_phase_proxy(
        2, 0.0, 0.5, 0.5, -0.125
    )
    generalized_negative = base_type._visual_phase_proxy(
        2, 0.9, 0.5, 0.5, 0.125
    )
    assert math.isclose(generalized_positive, math.radians(120.0))
    assert math.isclose(generalized_negative, math.radians(60.0))

    class VisualController:
        state = namespace["STATE_VISUAL_LOCK"]
        visual_phase_mdeg = 123400
        visual_speed_millihz = 567
        last_rect = None

    visual_frame = FakeFrame(320, 240)
    namespace["annotate"](visual_frame, VisualController(), 24.5)
    assert "VISUAL" in visual_frame.drawn_strings[0]
    assert visual_frame.drawn_strings[1] == "VISUAL 1X LINE  STREAMING"
    assert "P:123.4" in visual_frame.drawn_strings[2]
    assert "D:0.57Hz" in visual_frame.drawn_strings[2]

    class SignalDetectorStatus:
        last_width_ratio = 0.123
        consecutive_frames = 4
        confirm_frames = 6

    class WaitSignalController:
        state = namespace["STATE_WAIT_SIGNAL"]
        signal_detector = SignalDetectorStatus()
        last_rect = None

    wait_frame = FakeFrame(320, 240)
    namespace["annotate"](wait_frame, WaitSignalController(), 24.5)
    assert "WAIT SIGNAL" in wait_frame.drawn_strings[0]
    assert wait_frame.drawn_strings[1] == "CONNECT INPUT SIGNAL"
    assert "W:12%" in wait_frame.drawn_strings[2]
    assert "VALID:4/6" in wait_frame.drawn_strings[2]

    class HoldController:
        state = namespace["STATE_LOCK_HOLD"]
        lock_input_frequency_hz = 5200
        lock_output_frequency_hz = 10400
        visual_speed_millihz = 25
        last_rect = None

    hold_frame = FakeFrame(320, 240)
    namespace["annotate"](hold_frame, HoldController(), 24.5)
    assert "LOCK HOLD" in hold_frame.drawn_strings[0]
    assert hold_frame.drawn_strings[1] == (
        "VISUAL HOLD  +/-5Hz  MANUAL EXIT"
    )
    assert "OUT: 10400" in hold_frame.drawn_strings[2]

    tracker = tracker_type()
    speeds = [
        tracker.update(0.1 * index, 100 * index)
        for index in range(6)
    ]
    assert speeds[-1] == 159

    # One interval straddles the folded phase reflection and under-reports
    # motion. The short median preserves the true surrounding speed.
    tracker.reset()
    reflected = [2.7, 2.9, 3.1, 3.0, 2.8, 2.6]
    speeds = [
        tracker.update(phase, 100 * index)
        for index, phase in enumerate(reflected)
    ]
    assert speeds[-1] == 318

    class SyntheticDetector(base_type):
        def _extract_trace(self, frame):
            return frame

    deadline_detector = base_type()
    deadline_detector.reset_test(100)
    assert deadline_detector.deadline_ms == 420
    deadline_detector.reset_test(100, 180)
    assert deadline_detector.deadline_ms == 280

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
    # radial spread is high. A circle passes the ellipse branch. Multi-strand
    # and non-conic shapes fail independently of bright area.
    line_trace = make_trace(
        base_type, side, 0.09, 0.08, 1.50, 0.10
    )
    ellipse_trace = make_trace(
        base_type, side, 0.09, 0.80, 0.20, 0.10
    )
    multi_trace = make_trace(
        base_type, side, 0.10, 0.50, 0.40, 0.85
    )
    nonconic_trace = make_trace(
        base_type, side, 0.10, 0.50, 0.90, 0.10
    )
    assert line_trace[-1]
    assert ellipse_trace[-1]
    assert not multi_trace[-1]
    assert not nonconic_trace[-1]

    # Fast phase sweep can fill a broad band whose outer envelope is still a
    # good line or ellipse. Area is only a rejection gate: calibrated thin
    # traces pass, while the two observed 20%/28% bands cannot vote family.
    calibrated_line = make_trace(
        base_type, side, 0.14, 0.05, 1.50, 0.10
    )
    calibrated_ellipse = make_trace(
        base_type, side, 0.11, 0.60, 0.20, 0.10
    )
    wide_line = make_trace(
        base_type, side, 0.16, 0.05, 1.50, 0.10
    )
    wide_ellipse_20 = make_trace(
        base_type, side, 0.20, 0.75, 0.21, 0.12
    )
    wide_ellipse_28 = make_trace(
        base_type, side, 0.28, 0.83, 0.22, 0.16
    )
    assert calibrated_line[-1]
    assert calibrated_ellipse[-1]
    assert not wide_line[-1]
    assert not wide_ellipse_20[-1]
    assert not wide_ellipse_28[-1]

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

    # High-frequency ranking keeps the best three non-consecutive family
    # frames instead of averaging them away among rapidly jittering frames.
    # A single density/topology-rejected conic envelope remains capped below
    # the family range and therefore cannot win by accident.
    peak_trace = ellipse_trace[:4] + (900, True)
    weak_trace = multi_trace[:4] + (300, False)
    jitter_samples = [
        peak_trace,
        weak_trace,
        weak_trace,
        peak_trace,
        weak_trace,
        weak_trace,
        peak_trace,
        weak_trace,
        weak_trace,
        weak_trace,
    ]
    average_detector = SyntheticDetector()
    average_detector.reset_test(0, 320, False)
    high_detector = SyntheticDetector()
    high_detector.reset_test(0, 320, True)
    for index, sample in enumerate(jitter_samples):
        average_detector.update(sample, index * 10)
        high_detector.update(sample, index * 10)
    assert average_detector._quality()[0] == 480
    assert high_detector._quality()[0] == 900

    rejected_envelope = multi_trace[:4] + (900, False)
    rejected_detector = SyntheticDetector()
    rejected_detector.reset_test(0, 320, True)
    for index in range(3):
        rejected_detector.update(rejected_envelope, index * 10)
    assert rejected_detector._quality()[0] == 499

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
