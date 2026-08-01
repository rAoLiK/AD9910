"""Host regression tests for the pre-recognition input-signal gate."""

import ast
from pathlib import Path


class FakeBlob:
    def __init__(self, rect, pixels=1000):
        self.rect = rect
        self.pixels = pixels


class FakeMask:
    def __init__(self, blobs):
        self.blobs = blobs

    def rsub(self, other):
        del other

    def binary(self, thresholds):
        del thresholds

    def b_and(self, other):
        del other

    def dilate(self, size):
        del size

    def find_blobs(self, thresholds, **kwargs):
        del thresholds, kwargs
        return self.blobs


class FakeFrame:
    def __init__(self, blobs, width=320, height=240):
        self.mask = FakeMask(blobs)
        self.frame_width = width
        self.frame_height = height

    def to_grayscale(self, **kwargs):
        del kwargs
        return self.mask

    def width(self):
        return self.frame_width

    def height(self):
        return self.frame_height


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
            if any(name.startswith("SIGNAL_") for name in names):
                selected.append(node)
        elif (
            isinstance(node, ast.ClassDef)
            and node.name == "InputSignalDetector"
        ):
            selected.append(node)

    namespace = {}
    module = ast.Module(body=selected, type_ignores=[])
    exec(compile(module, str(source_path), "exec"), namespace)
    return namespace


def main():
    namespace = load_detector()
    detector_type = namespace["InputSignalDetector"]
    confirm_frames = namespace["SIGNAL_CONFIRM_FRAMES"]
    minimum_samples = namespace["SIGNAL_WIDTH_MIN_SAMPLES"]
    history_frames = namespace["SIGNAL_WIDTH_HISTORY_FRAMES"]
    frames_to_confirm = minimum_samples + confirm_frames - 1

    # The source-absent reference is a tall, narrow green line in XY mode.
    # It is deliberately located and measured, but its horizontal span must
    # never accumulate confirmation frames.
    vertical_line = FakeFrame([FakeBlob((210, 30, 8, 180), pixels=900)])
    detector = detector_type()
    for _ in range(frames_to_confirm + 2):
        assert not detector.update(vertical_line)
    assert detector.consecutive_frames == 0
    assert detector.candidate_rect == (210, 30, 8, 180)
    assert detector.last_width_ratio < namespace["SIGNAL_EXIT_WIDTH_RATIO"]

    # Bloom around the centered idle line can make it several times thicker,
    # but it remains below the horizontal-spread enter threshold.
    glowing_vertical_line = FakeFrame(
        [FakeBlob((200, 30, 28, 180), pixels=1800)]
    )
    detector.reset()
    for _ in range(frames_to_confirm + 2):
        assert not detector.update(glowing_vertical_line)
    assert not detector.expanded

    # A valid two-dimensional Lissajous trace is accepted only after the full
    # median window and confirmation interval have elapsed.
    valid_trace = FakeFrame([FakeBlob((155, 40, 120, 150), pixels=2400)])
    detector.reset()
    for _ in range(frames_to_confirm - 1):
        assert not detector.update(valid_trace)
    assert detector.update(valid_trace)
    assert detector.confirmed
    assert detector.candidate_rect == (155, 40, 120, 150)

    # The supplied valid-input reference is viewed with noticeable perspective
    # and produces an approximately 2:1 connected extent after QVGA scaling.
    perspective_trace = FakeFrame(
        [FakeBlob((70, 70, 150, 75), pixels=2600)]
    )
    detector.reset()
    for _ in range(frames_to_confirm - 1):
        assert not detector.update(perspective_trace)
    assert detector.update(perspective_trace)

    # One narrow refresh frame inside a valid run is suppressed by the median
    # filter instead of clearing an otherwise stable confirmation.
    detector.reset()
    for _ in range(minimum_samples + 1):
        assert not detector.update(valid_trace)
    assert not detector.update(vertical_line)
    assert detector.expanded
    assert detector.consecutive_frames == 3

    # A sustained return to the centered line eventually dominates the median
    # and clears the expanded state despite the lower hysteresis threshold.
    while detector.expanded:
        assert not detector.update(vertical_line)
    assert detector.consecutive_frames == 0

    # Isolated wide noise frames among idle-line frames cannot move the median
    # past the enter threshold.
    detector.reset()
    noise_sequence = [
        vertical_line,
        valid_trace,
        vertical_line,
        vertical_line,
        valid_trace,
        vertical_line,
        vertical_line,
    ]
    assert len(noise_sequence) == history_frames
    for frame in noise_sequence:
        assert not detector.update(frame)
    assert not detector.expanded

    # A wide object outside the calibrated XY center region is ignored; the
    # detector continues measuring the central narrow idle line.
    off_center_noise = FakeFrame(
        [
            FakeBlob((260, 40, 50, 150), pixels=4000),
            FakeBlob((210, 30, 8, 180), pixels=900),
        ]
    )
    detector.reset()
    for _ in range(frames_to_confirm):
        assert not detector.update(off_center_noise)
    assert detector.candidate_rect == (210, 30, 8, 180)

    # A broad horizontal artifact is also invalid; both axes must carry a
    # plausible trace extent.
    horizontal_line = FakeFrame([FakeBlob((100, 100, 170, 12), pixels=1100)])
    detector.reset()
    assert not detector.update(horizontal_line)
    assert detector.candidate_rect is None

    print("OpenMV input-signal gate tests passed")


if __name__ == "__main__":
    main()
