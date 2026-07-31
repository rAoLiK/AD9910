"""
OpenMV oscilloscope XY-boundary monitor.

The camera must be fixed relative to the oscilloscope.  Only the plotting area
should be inside ROI.  The script:
  1. locates the bright, near-square XY boundary;
  2. draws its real quadrilateral and axis-aligned bounding box;
  3. checks edge continuity, squareness, tilt/skew and centre offset;
  4. flashes the on-board red LED after several consecutive bad frames.

Designed for OpenMV firmware 4.x (sensor API) and 5.x (csi API).
"""

import math
import time


SCRIPT_VERSION = "20260730-OPENMV5-FIX1"


# ---------------------------------------------------------------------------
# User calibration (QVGA: 320 x 240)
# ---------------------------------------------------------------------------

FRAME_W = 320
FRAME_H = 240

# IMPORTANT: change this so it contains the oscilloscope plotting area only.
ROI = (35, 25, 250, 190)  # x, y, width, height

# None means the centre of ROI.  For a deliberately off-centre normal pattern,
# replace it with the centre measured from a good frame, e.g. (164, 121).
EXPECTED_CENTER = None

# LAB thresholds for the bright green/white trace:
# (L_min, L_max, A_min, A_max, B_min, B_max)
# Use OpenMV IDE -> Tools -> Machine Vision -> Threshold Editor when tuning.
TRACE_THRESHOLDS = [
    (45, 100, -80, 20, -35, 80),
]

# Pixel-level continuity test.  A pixel is accepted when it is bright enough,
# or when the green channel strongly dominates.
LUMA_MIN = 135
GREEN_MIN = 95
GREEN_OVER_RED = 18
GREEN_OVER_BLUE = 8

# Detector parameters.
RECT_MAG_THRESHOLD = 4000
MIN_TRACE_PIXELS = 55
MIN_BLOB_AREA = 350
BLOB_MERGE_MARGIN = 7
MIN_TARGET_AREA_RATIO = 0.045
MAX_TARGET_AREA_RATIO = 0.92
MIN_TARGET_EDGE = 28
MAX_DETECT_SIDE_RATIO = 2.20

# Completeness thresholds.
EDGE_SAMPLE_COUNT = 44
EDGE_BAND_RADIUS = 4
EDGE_ENDPOINT_SKIP = 0.04
MIN_EDGE_COVERAGE = 0.60
MAX_EDGE_GAP_RATIO = 0.23
ROI_VISIBLE_MARGIN = 2

# "Near square" thresholds.
MAX_SIDE_RATIO = 1.30
MAX_DIAGONAL_RATIO = 1.18
MAX_CORNER_ANGLE_ERROR_DEG = 15.0

# Tilt / perspective skew thresholds.
MAX_TILT_DEG = 6.0
MAX_SKEW_DEG = 8.0

# Deflection means displacement of the boundary centre from EXPECTED_CENTER.
MAX_CENTER_OFFSET_X_RATIO = 0.08
MAX_CENTER_OFFSET_Y_RATIO = 0.08

# Alarm filtering and indication.
ALERT_ON_INCOMPLETE_OR_BAD_SHAPE = True
BAD_FRAMES_TO_ALARM = 4
GOOD_FRAMES_TO_CLEAR = 5
LED_BLINK_MS = 250
PRINT_PERIOD_MS = 500

# Optional image orientation / lens correction.
H_MIRROR = False
V_FLIP = False
ENABLE_LENS_CORR = False
LENS_CORR_STRENGTH = 1.6


COLOR_OK = (0, 255, 0)
COLOR_BAD = (255, 0, 0)
COLOR_ROI = (0, 120, 255)
COLOR_EXPECTED = (255, 255, 0)
COLOR_TEXT = (255, 255, 255)


def _call_or_value(obj, name, tuple_index=None):
    """Read both OpenMV 4.x method fields and OpenMV 5.x attr fields."""
    value = getattr(obj, name, None)
    if value is not None:
        return value() if callable(value) else value
    if tuple_index is not None:
        return obj[tuple_index]
    return None


def _ticks_ms():
    fn = getattr(time, "ticks_ms", None)
    if fn is not None:
        return fn()
    try:
        import pyb
        return pyb.millis()
    except Exception:
        return int(time.time() * 1000)


def _ticks_diff(new_value, old_value):
    fn = getattr(time, "ticks_diff", None)
    if fn is not None:
        return fn(new_value, old_value)
    return new_value - old_value


def _distance(a, b):
    dx = b[0] - a[0]
    dy = b[1] - a[1]
    return math.sqrt((dx * dx) + (dy * dy))


def _polygon_area(corners):
    area2 = 0.0
    for i in range(4):
        p = corners[i]
        q = corners[(i + 1) & 3]
        area2 += (p[0] * q[1]) - (q[0] * p[1])
    return abs(area2) * 0.5


def _canonical_corners(corners):
    """Return four points clockwise in image coordinates, starting top-left."""
    if corners is None or len(corners) != 4:
        return None

    cx = sum([p[0] for p in corners]) * 0.25
    cy = sum([p[1] for p in corners]) * 0.25
    ordered = sorted(
        [(int(p[0]), int(p[1])) for p in corners],
        key=lambda p: math.atan2(p[1] - cy, p[0] - cx),
    )

    start = 0
    best = ordered[0][0] + ordered[0][1]
    for i in range(1, 4):
        score = ordered[i][0] + ordered[i][1]
        if score < best:
            best = score
            start = i
    return ordered[start:] + ordered[:start]


def _bbox(corners):
    xs = [p[0] for p in corners]
    ys = [p[1] for p in corners]
    x0 = min(xs)
    y0 = min(ys)
    return (x0, y0, max(xs) - x0 + 1, max(ys) - y0 + 1)


def _axis_error_deg(angle_deg, axis_deg):
    value = angle_deg - axis_deg
    while value > 90.0:
        value -= 180.0
    while value <= -90.0:
        value += 180.0
    return value


def _line_angle_deg(a, b):
    return math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))


def _quad_metrics(corners, roi, expected_center):
    sides = [_distance(corners[i], corners[(i + 1) & 3]) for i in range(4)]
    min_side = min(sides)
    max_side = max(sides)
    side_ratio = max_side / max(1.0, min_side)

    diag0 = _distance(corners[0], corners[2])
    diag1 = _distance(corners[1], corners[3])
    diagonal_ratio = max(diag0, diag1) / max(1.0, min(diag0, diag1))

    angle_errors = []
    for i in range(4):
        p = corners[i]
        p_prev = corners[(i - 1) & 3]
        p_next = corners[(i + 1) & 3]
        ax = p_prev[0] - p[0]
        ay = p_prev[1] - p[1]
        bx = p_next[0] - p[0]
        by = p_next[1] - p[1]
        denom = max(1.0, math.sqrt((ax * ax) + (ay * ay)) *
                    math.sqrt((bx * bx) + (by * by)))
        cos_value = ((ax * bx) + (ay * by)) / denom
        cos_value = max(-1.0, min(1.0, cos_value))
        angle = math.degrees(math.acos(cos_value))
        angle_errors.append(abs(angle - 90.0))

    # p0->p1 and p3->p2 are horizontal in a level square.
    # p1->p2 and p0->p3 are vertical in a level square.
    edge_errors = [
        _axis_error_deg(_line_angle_deg(corners[0], corners[1]), 0.0),
        _axis_error_deg(_line_angle_deg(corners[3], corners[2]), 0.0),
        _axis_error_deg(_line_angle_deg(corners[1], corners[2]), 90.0),
        _axis_error_deg(_line_angle_deg(corners[0], corners[3]), 90.0),
    ]
    tilt_deg = sum(edge_errors) * 0.25
    skew_deg = max([abs(v - tilt_deg) for v in edge_errors])

    cx = sum([p[0] for p in corners]) * 0.25
    cy = sum([p[1] for p in corners]) * 0.25
    offset_x_ratio = (cx - expected_center[0]) / float(roi[2])
    offset_y_ratio = (cy - expected_center[1]) / float(roi[3])

    return {
        "area": _polygon_area(corners),
        "sides": sides,
        "side_ratio": side_ratio,
        "diagonal_ratio": diagonal_ratio,
        "max_angle_error_deg": max(angle_errors),
        "tilt_deg": tilt_deg,
        "skew_deg": skew_deg,
        "center": (cx, cy),
        "offset_x_ratio": offset_x_ratio,
        "offset_y_ratio": offset_y_ratio,
    }


def _is_trace_pixel(img, x, y):
    # OpenMV 5.x requires coordinates packed in one tuple.  OpenMV 4.x also
    # accepts this form, so using it here keeps one code path for both APIs.
    pixel = img.get_pixel((int(x), int(y)))
    if pixel is None:
        return False
    if isinstance(pixel, tuple) or isinstance(pixel, list):
        red, green, blue = pixel[0], pixel[1], pixel[2]
        luma = ((30 * red) + (59 * green) + (11 * blue)) // 100
        green_trace = (
            green >= GREEN_MIN
            and (green - red) >= GREEN_OVER_RED
            and (green - blue) >= GREEN_OVER_BLUE
        )
        return luma >= LUMA_MIN or green_trace
    return pixel >= LUMA_MIN


def _edge_continuity(img, p0, p1):
    """Return (bright coverage, longest dark gap) for one quadrilateral edge."""
    dx = p1[0] - p0[0]
    dy = p1[1] - p0[1]
    length = math.sqrt((dx * dx) + (dy * dy))
    if length < 1.0:
        return 0.0, 1.0

    # Unit normal lets the test tolerate a few pixels of detector error/bloom.
    nx = -dy / length
    ny = dx / length
    bright_count = 0
    gap = 0
    longest_gap = 0

    usable = 1.0 - (2.0 * EDGE_ENDPOINT_SKIP)
    for i in range(EDGE_SAMPLE_COUNT):
        t = EDGE_ENDPOINT_SKIP
        if EDGE_SAMPLE_COUNT > 1:
            t += usable * i / float(EDGE_SAMPLE_COUNT - 1)
        x = p0[0] + (dx * t)
        y = p0[1] + (dy * t)
        bright = False
        for band in range(-EDGE_BAND_RADIUS, EDGE_BAND_RADIUS + 1):
            sx = int(x + (nx * band) + 0.5)
            sy = int(y + (ny * band) + 0.5)
            if _is_trace_pixel(img, sx, sy):
                bright = True
                break

        if bright:
            bright_count += 1
            gap = 0
        else:
            gap += 1
            if gap > longest_gap:
                longest_gap = gap

    return (
        bright_count / float(EDGE_SAMPLE_COUNT),
        longest_gap / float(EDGE_SAMPLE_COUNT),
    )


def _inside_roi(corners, roi):
    x0, y0, width, height = roi
    x1 = x0 + width - 1
    y1 = y0 + height - 1
    for x, y in corners:
        if (
            x < (x0 + ROI_VISIBLE_MARGIN)
            or x > (x1 - ROI_VISIBLE_MARGIN)
            or y < (y0 + ROI_VISIBLE_MARGIN)
            or y > (y1 - ROI_VISIBLE_MARGIN)
        ):
            return False
    return True


def _candidate_allowed(corners, roi, expected_center):
    box = _bbox(corners)
    metrics = _quad_metrics(corners, roi, expected_center)
    area_ratio = metrics["area"] / float(roi[2] * roi[3])
    if box[2] < MIN_TARGET_EDGE or box[3] < MIN_TARGET_EDGE:
        return False
    if area_ratio < MIN_TARGET_AREA_RATIO or area_ratio > MAX_TARGET_AREA_RATIO:
        return False
    if metrics["side_ratio"] > MAX_DETECT_SIDE_RATIO:
        return False
    return True


def _candidate_score(corners, source, roi, expected_center, strength=0):
    metrics = _quad_metrics(corners, roi, expected_center)
    dx = abs(metrics["offset_x_ratio"])
    dy = abs(metrics["offset_y_ratio"])
    centre_factor = 1.0 / (1.0 + dx + dy)
    shape_factor = 1.0 / (
        metrics["side_ratio"] * metrics["diagonal_ratio"]
    )
    source_factor = 1.18 if source == "rect" else 1.0
    strength_factor = 1.0 + min(float(strength) / 40000.0, 0.35)
    return (
        metrics["area"]
        * centre_factor
        * shape_factor
        * source_factor
        * strength_factor
    )


def _find_candidates(img, roi, expected_center):
    candidates = []

    # A true four-edge detector is preferred when supported by the board.
    try:
        for rect in img.find_rects(roi=roi, threshold=RECT_MAG_THRESHOLD):
            corners = _canonical_corners(_call_or_value(rect, "corners", 5))
            if corners and _candidate_allowed(corners, roi, expected_center):
                magnitude = _call_or_value(rect, "magnitude", 4) or 0
                candidates.append((
                    _candidate_score(
                        corners, "rect", roi, expected_center, magnitude
                    ),
                    "rect",
                    corners,
                ))
    except (AttributeError, OSError):
        # find_rects is unavailable on some low-memory/older boards.
        pass

    # Fallback also localises broken boundaries, which find_rects may reject.
    try:
        blobs = img.find_blobs(
            TRACE_THRESHOLDS,
            roi=roi,
            pixels_threshold=MIN_TRACE_PIXELS,
            area_threshold=MIN_BLOB_AREA,
            merge=True,
            margin=BLOB_MERGE_MARGIN,
        )
        for blob in blobs:
            corners = _canonical_corners(_call_or_value(blob, "corners", 14))
            if corners and _candidate_allowed(corners, roi, expected_center):
                pixels = _call_or_value(blob, "pixels", 6) or 0
                candidates.append((
                    _candidate_score(
                        corners, "blob", roi, expected_center, pixels * 10
                    ),
                    "blob",
                    corners,
                ))
    except (AttributeError, OSError):
        pass

    if not candidates:
        return None
    candidates.sort(key=lambda item: item[0], reverse=True)
    return candidates[0]


def analyse_frame(img, roi=ROI, expected_center=EXPECTED_CENTER):
    """Analyse one image.  Kept separate so geometry can be host-tested."""
    if expected_center is None:
        expected_center = (
            roi[0] + (roi[2] * 0.5),
            roi[1] + (roi[3] * 0.5),
        )

    candidate = _find_candidates(img, roi, expected_center)
    if candidate is None:
        return {
            "detected": False,
            "complete": False,
            "shape_ok": False,
            "level_ok": False,
            "centered_ok": False,
            "valid": False,
            "source": "none",
            "corners": None,
            "reason": ["MISS"],
        }

    _, source, corners = candidate
    metrics = _quad_metrics(corners, roi, expected_center)

    coverages = []
    gap_ratios = []
    for i in range(4):
        coverage, gap = _edge_continuity(
            img, corners[i], corners[(i + 1) & 3]
        )
        coverages.append(coverage)
        gap_ratios.append(gap)

    visible = _inside_roi(corners, roi)
    complete = (
        visible
        and min(coverages) >= MIN_EDGE_COVERAGE
        and max(gap_ratios) <= MAX_EDGE_GAP_RATIO
    )
    shape_ok = (
        metrics["side_ratio"] <= MAX_SIDE_RATIO
        and metrics["diagonal_ratio"] <= MAX_DIAGONAL_RATIO
        and metrics["max_angle_error_deg"] <= MAX_CORNER_ANGLE_ERROR_DEG
    )
    level_ok = (
        abs(metrics["tilt_deg"]) <= MAX_TILT_DEG
        and metrics["skew_deg"] <= MAX_SKEW_DEG
    )
    centered_ok = (
        abs(metrics["offset_x_ratio"]) <= MAX_CENTER_OFFSET_X_RATIO
        and abs(metrics["offset_y_ratio"]) <= MAX_CENTER_OFFSET_Y_RATIO
    )
    valid = complete and shape_ok and level_ok and centered_ok

    reason = []
    if not complete:
        reason.append("OPEN")
    if not shape_ok:
        reason.append("SHAPE")
    if not level_ok:
        reason.append("TILT")
    if not centered_ok:
        reason.append("OFFSET")
    if not reason:
        reason.append("OK")

    result = {
        "detected": True,
        "complete": complete,
        "shape_ok": shape_ok,
        "level_ok": level_ok,
        "centered_ok": centered_ok,
        "valid": valid,
        "source": source,
        "corners": corners,
        "bbox": _bbox(corners),
        "coverage": coverages,
        "gap_ratio": gap_ratios,
        "visible": visible,
        "reason": reason,
    }
    result.update(metrics)
    return result


class DebouncedAlarm:
    def __init__(self):
        self.bad_frames = 0
        self.good_frames = 0
        self.active = False

    def update(self, bad):
        if bad:
            self.good_frames = 0
            self.bad_frames = min(
                BAD_FRAMES_TO_ALARM, self.bad_frames + 1
            )
            if self.bad_frames >= BAD_FRAMES_TO_ALARM:
                self.active = True
        else:
            self.bad_frames = 0
            self.good_frames = min(
                GOOD_FRAMES_TO_CLEAR, self.good_frames + 1
            )
            if self.good_frames >= GOOD_FRAMES_TO_CLEAR:
                self.active = False
        return self.active


class RedBlinker:
    def __init__(self):
        self.led = self._make_led()
        self.state = False
        self.last_change = _ticks_ms()
        self._write(False)

    @staticmethod
    def _make_led():
        try:
            from machine import LED
            return LED("LED_RED")
        except Exception:
            import pyb
            return pyb.LED(1)

    def _write(self, enabled):
        self.state = enabled
        if enabled:
            self.led.on()
        else:
            self.led.off()

    def update(self, alarm_active, now_ms):
        if not alarm_active:
            if self.state:
                self._write(False)
            self.last_change = now_ms
            return

        if _ticks_diff(now_ms, self.last_change) >= LED_BLINK_MS:
            self._write(not self.state)
            self.last_change = now_ms


def _init_camera():
    """Return a snapshot callable, using the API available in this firmware."""
    legacy_error = None
    try:
        import sensor
        sensor.reset()
        sensor.set_pixformat(sensor.RGB565)
        sensor.set_framesize(sensor.QVGA)
        sensor.set_hmirror(H_MIRROR)
        sensor.set_vflip(V_FLIP)
        sensor.skip_frames(time=1500)
        # Lock after convergence so thresholds do not drift frame-to-frame.
        try:
            sensor.set_auto_gain(False)
            sensor.set_auto_whitebal(False)
            sensor.set_auto_exposure(False)
        except (AttributeError, OSError):
            pass
        print("camera_api=sensor")
        return sensor.snapshot
    except Exception as exc:
        legacy_error = exc

    try:
        import csi
        camera = csi.CSI()
        camera.reset()
        camera.pixformat(csi.RGB565)
        camera.framesize(csi.QVGA)
        camera.hmirror(H_MIRROR)
        camera.vflip(V_FLIP)
        camera.snapshot(time=1500)
        try:
            camera.auto_gain(False)
            camera.auto_whitebal(False)
            camera.auto_exposure(False)
        except (AttributeError, OSError):
            pass
        print("camera_api=csi")
        # The bound method keeps the camera object alive.
        return camera.snapshot
    except Exception as csi_error:
        print("sensor_init_error:", legacy_error)
        print("csi_init_error:", csi_error)
        raise


def _draw_result(img, result, roi, expected_center):
    if expected_center is None:
        expected_center = (
            int(roi[0] + (roi[2] * 0.5)),
            int(roi[1] + (roi[3] * 0.5)),
        )
    else:
        expected_center = (
            int(expected_center[0]),
            int(expected_center[1]),
        )

    img.draw_rectangle(roi, color=COLOR_ROI)
    img.draw_cross(
        (expected_center[0], expected_center[1]),
        color=COLOR_EXPECTED,
        size=7,
    )

    color = COLOR_OK if result["valid"] else COLOR_BAD
    if result["detected"]:
        img.draw_edges(result["corners"], color=color)
        img.draw_rectangle(result["bbox"], color=color)
        center = result["center"]
        img.draw_cross(
            (int(center[0]), int(center[1])), color=color, size=6
        )

    title = "OK" if result["valid"] else "NG"
    reason = "+".join(result["reason"])
    img.draw_string((2, 2), title, color=color, scale=2)
    img.draw_string((2, 20), reason, color=COLOR_TEXT)


def _print_result(result):
    if not result["detected"]:
        print("det=0 complete=0 shape=0 level=0 center=0 reason=MISS")
        return
    coverage_text = ",".join(
        ["%.2f" % value for value in result["coverage"]]
    )
    print(
        "det=1 src=%s complete=%d shape=%d level=%d center=%d "
        "tilt=%.1f skew=%.1f off=(%.3f,%.3f) side=%.2f "
        "diag=%.2f angle=%.1f cov=[%s] reason=%s"
        % (
            result["source"],
            result["complete"],
            result["shape_ok"],
            result["level_ok"],
            result["centered_ok"],
            result["tilt_deg"],
            result["skew_deg"],
            result["offset_x_ratio"],
            result["offset_y_ratio"],
            result["side_ratio"],
            result["diagonal_ratio"],
            result["max_angle_error_deg"],
            coverage_text,
            "+".join(result["reason"]),
        )
    )


def run():
    snapshot = _init_camera()
    print("script_version=%s" % SCRIPT_VERSION)
    alarm_filter = DebouncedAlarm()
    red_blinker = RedBlinker()
    last_print = _ticks_ms()

    while True:
        img = snapshot()
        if ENABLE_LENS_CORR:
            img.lens_corr(LENS_CORR_STRENGTH)

        result = analyse_frame(img)

        alignment_bad = (
            (not result["detected"])
            or (not result["level_ok"])
            or (not result["centered_ok"])
        )
        if ALERT_ON_INCOMPLETE_OR_BAD_SHAPE:
            alignment_bad = (
                alignment_bad
                or (not result["complete"])
                or (not result["shape_ok"])
            )

        alarm_active = alarm_filter.update(alignment_bad)
        now_ms = _ticks_ms()
        red_blinker.update(alarm_active, now_ms)
        _draw_result(img, result, ROI, EXPECTED_CENTER)

        if _ticks_diff(now_ms, last_print) >= PRINT_PERIOD_MS:
            _print_result(result)
            last_print = now_ms


if __name__ == "__main__":
    run()
