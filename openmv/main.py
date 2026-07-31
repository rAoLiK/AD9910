"""
OpenMV 5.0 vertical-period counter with an SSD1309 aiming preview.

The XY plotting quadrilateral is located and tracked automatically. Perspective
scan lanes follow that quadrilateral, and eight horizontal candidate positions
vote on the band count. This rejects text, local waveform changes, and the
sloping line without requiring a hand-aligned ROI or scan coordinate.

Period definition:
    N detected horizontal bands contain max(0, N - 1) complete vertical
    periods. Both numbers are reported so there is no counting ambiguity.

OpenMV 5.0 notes:
    * Uses the new csi.CSI camera API.
    * Uses tuple-packed coordinates for all Image drawing calls.
    * Uses attribute access on statistics result objects.
"""

import gc
import math
import time
from array import array

import csi
from machine import I2C, Pin, SoftI2C


try:
    import micropython
    _native = getattr(micropython, "native", None)
except ImportError:
    _native = None

if _native is None:
    def _native(function):
        return function


# ---------------------------------------------------------------------------
# Camera and automatic plot localisation (full QVGA frame is never cropped)
# ---------------------------------------------------------------------------

# These are startup defaults only. OpenMV N6 defines csi.QVGA as 320x200,
# whereas H7/H7 Plus cameras normally return 320x240. _init_camera() replaces
# them with the dimensions of the first real frame before any detector object
# allocates its geometry-dependent buffers.
FRAME_W = 320
FRAME_H = 240
CAMERA_FRAMEBUFFERS = 2
CAMERA_SETTLE_MS = 1500

# Optional sensor frame-rate request. Unsupported values are ignored.
TARGET_CAMERA_FPS = 60

# find_rects() is the primary locator. A coarse dark-region component locator
# is used when glare or a broken border prevents quadrilateral detection.
RECT_SEARCH_ROI = (4, 4, FRAME_W - 8, FRAME_H - 8)
RECT_EDGE_THRESHOLD = 2500
PLOT_MIN_WIDTH = 105
PLOT_MIN_HEIGHT = 70
PLOT_MIN_AREA_PERCENT = 12
PLOT_MAX_AREA_PERCENT = 88
PLOT_MIN_ASPECT_X100 = 65
PLOT_MAX_ASPECT_X100 = 250
PLOT_REDETECT_FRAMES = 15
PLOT_FAST_RETRY_FRAMES = 3
PLOT_MISSES_TO_LOST = 4
PLOT_SMOOTH_OLD_WEIGHT = 3

# Ignore the border and axis labels after a plotting quadrilateral is found.
PLOT_INSET_U_PERMILLE = 45
PLOT_INSET_TOP_PERMILLE = 65
PLOT_INSET_BOTTOM_PERMILLE = 70

# Coarse fallback locator. Components touching the full-frame edge are rejected
# so the black area outside the oscilloscope cannot become the plotting area.
FALLBACK_CELL_SIZE = 8
FALLBACK_MIN_DARK_CELLS = 70

# The detector follows the located quadrilateral, so screen translation,
# rotation, and moderate perspective skew require no manual ROI alignment.
PROFILE_SAMPLES = 160
SCAN_LANE_COUNT = 5
SCAN_LANE_U_STEP_PERMILLE = 14
SCAN_CENTER_CANDIDATES = (150, 250, 350, 450, 550, 650, 750, 850)
SCAN_RESELECT_FRAMES = 60
SCAN_BAD_FRAMES_TO_RESELECT = 3


# ---------------------------------------------------------------------------
# Detector tuning
# ---------------------------------------------------------------------------

# A lane is bright when any of its three horizontal pixels reaches the adaptive
# threshold. Threshold statistics are refreshed only occasionally in native
# code, leaving the normal frame path very small.
INITIAL_PIXEL_THRESHOLD = 130
THRESHOLD_MIN = 55
THRESHOLD_MAX = 225
THRESHOLD_STDEV_NUMERATOR = 3
THRESHOLD_STDEV_DENOMINATOR = 4
THRESHOLD_UPDATE_FRAMES = 10

# Row votes are smoothed vertically before contiguous bright groups are turned
# into band centres. With 5 lanes and radius 2, a real thin trace normally
# contributes at least 5 votes; the diagonal usually contributes only 1 or 2.
SMOOTH_RADIUS = 2
MIN_SMOOTH_SCORE = 5
MAX_BRIDGED_DARK_ROWS = 1
MIN_BAND_DISTANCE = 8
MAX_BANDS = 20
MIN_BANDS_FOR_PERIOD = 2

# Reject very irregular groups caused by menus/text. 100 is perfectly regular.
MIN_REGULARITY_QUALITY = 55

# Two consecutive frames give a fast result while suppressing one-frame noise.
COUNT_CONFIRM_FRAMES = 2
COUNT_CLEAR_FRAMES = 3


# ---------------------------------------------------------------------------
# OLED, annotations, and telemetry
# ---------------------------------------------------------------------------

OLED_MAX_FPS = 5
ENABLE_OLED = True
OLED_REQUIRED = False
OLED_ADDRESS = 0x3C
OLED_COLUMN_OFFSET = 0
OLED_CONTRAST_PERCENT = 250

# The OLED is only 1-bit and the QVGA frame is reduced to 128x64.  A gray,
# one-pixel outline can therefore disappear.  Draw every selected boundary as
# a black halo with a pure-white core: one polarity remains visible on a bright
# trace and the other remains visible on a dark plot background.
PLOT_BOX_HALO_THICKNESS = 8
PLOT_BOX_CORE_THICKNESS = 3
CONTENT_BOX_HALO_THICKNESS = 5
CONTENT_BOX_CORE_THICKNESS = 2
SCAN_LINE_HALO_THICKNESS = 5
SCAN_LINE_CORE_THICKNESS = 2

DRAW_IDE_AND_OLED_OVERLAY = True
REPORT_PERIOD_MS = 250


def _draw_inverse_edges(
    frame,
    corners,
    halo_thickness,
    core_thickness,
):
    """Draw a maximum-contrast, OLED-safe inverse outline."""
    frame.draw_edges(
        corners,
        color=0,
        thickness=halo_thickness,
    )
    frame.draw_edges(
        corners,
        color=255,
        thickness=core_thickness,
    )


def _draw_inverse_line(
    frame,
    line,
    halo_thickness,
    core_thickness,
):
    """Draw a black halo and pure-white centre line."""
    frame.draw_line(
        line,
        color=0,
        thickness=halo_thickness,
    )
    frame.draw_line(
        line,
        color=255,
        thickness=core_thickness,
    )


def _ticks_ms():
    function = getattr(time, "ticks_ms", None)
    if function:
        return function()
    try:
        import pyb
        return pyb.millis()
    except ImportError:
        return int(time.time() * 1000)


def _ticks_diff(new, old):
    function = getattr(time, "ticks_diff", None)
    if function:
        return function(new, old)
    return new - old


def _configure_frame_geometry(frame):
    """Adopt the camera's real grayscale dimensions before detector setup."""
    global FRAME_W, FRAME_H, RECT_SEARCH_ROI

    width = int(frame.width())
    height = int(frame.height())
    if width < 32 or height < 32:
        raise RuntimeError(
            "camera frame too small: %dx%d" % (width, height)
        )

    # OpenMV grayscale images are contiguous and use one byte per pixel.
    # Checking this once gives a useful error instead of a later raw-index
    # exception if the camera ever returns a different pixel format.
    raw_bytes = len(frame.bytearray())
    expected_bytes = width * height
    if raw_bytes != expected_bytes:
        raise RuntimeError(
            "camera frame is not contiguous GRAYSCALE: "
            "frame=%dx%d raw_bytes=%d expected=%d"
            % (width, height, raw_bytes, expected_bytes)
        )

    FRAME_W = width
    FRAME_H = height
    RECT_SEARCH_ROI = (4, 4, width - 8, height - 8)
    return raw_bytes


# ---------------------------------------------------------------------------
# Embedded SSD1309 driver (kept in this file for one-file deployment)
# ---------------------------------------------------------------------------

OLED_WIDTH = 128
OLED_HEIGHT = 64


def _adjust_oled_threshold(value, brightness, contrast_percent):
    delta = value - 128
    if delta < 0:
        value = 128 - ((-delta * 100) // contrast_percent)
    else:
        value = 128 + ((delta * 100) // contrast_percent)
    value -= brightness
    if value < 0:
        return 0
    if value > 255:
        return 255
    return value


@_native
def _pack_oled_grayscale(
    source,
    destination,
    source_width,
    x_map,
    y_map,
    threshold_x0,
    threshold_x1,
    threshold_x2,
    threshold_x3,
):
    """Scale QVGA grayscale into the SSD1309 page buffer with dithering."""
    output = 1
    for page in range(8):
        y = page * 8
        row0 = y_map[y] * source_width
        row1 = y_map[y + 1] * source_width
        row2 = y_map[y + 2] * source_width
        row3 = y_map[y + 3] * source_width
        row4 = y_map[y + 4] * source_width
        row5 = y_map[y + 5] * source_width
        row6 = y_map[y + 6] * source_width
        row7 = y_map[y + 7] * source_width

        for x in range(OLED_WIDTH):
            source_x = x_map[x]
            destination[output] = (
                (
                    1
                    if source[row0 + source_x] >= threshold_x0[x]
                    else 0
                )
                | (
                    2
                    if source[row1 + source_x] >= threshold_x1[x]
                    else 0
                )
                | (
                    4
                    if source[row2 + source_x] >= threshold_x2[x]
                    else 0
                )
                | (
                    8
                    if source[row3 + source_x] >= threshold_x3[x]
                    else 0
                )
                | (
                    16
                    if source[row4 + source_x] >= threshold_x0[x]
                    else 0
                )
                | (
                    32
                    if source[row5 + source_x] >= threshold_x1[x]
                    else 0
                )
                | (
                    64
                    if source[row6 + source_x] >= threshold_x2[x]
                    else 0
                )
                | (
                    128
                    if source[row7 + source_x] >= threshold_x3[x]
                    else 0
                )
            )
            output += 1


class AimDisplay:
    """Read-only, rate-limited SSD1309 preview for the current grayscale frame."""

    def __init__(
        self,
        max_fps=5,
        brightness=0,
        contrast_percent=200,
        oled_contrast=0xFF,
        address=0x3C,
        column_offset=0,
        hardware_bus=4,
        scl_pin="P7",
        sda_pin="P8",
        allow_soft_fallback=True,
    ):
        if max_fps <= 0:
            raise ValueError("max_fps must be positive")
        if contrast_percent <= 0:
            raise ValueError("contrast_percent must be positive")

        self.enabled = True
        self.period_ms = max(1, 1000 // max_fps)
        self.last_update = None
        self.last_duration_ms = 0
        self.update_count = 0
        self.source_width = 0
        self.source_height = 0
        self.x_map = None
        self.y_map = None
        self.address_requested = address
        self.column_offset = column_offset
        self.oled_contrast = oled_contrast

        # I2C data-control byte plus 128 x 64 / 8 display bytes.
        self.tx = bytearray(1 + (OLED_WIDTH * OLED_HEIGHT // 8))
        self.tx[0] = 0x40

        rows = (
            (8, 136, 40, 168),
            (200, 72, 232, 104),
            (56, 184, 24, 152),
            (248, 120, 216, 88),
        )
        adjusted = []
        for row in rows:
            adjusted.append(
                tuple(
                    _adjust_oled_threshold(
                        value,
                        brightness,
                        contrast_percent,
                    )
                    for value in row
                )
            )
        self.threshold_x0 = bytes(
            adjusted[0][x & 3] for x in range(OLED_WIDTH)
        )
        self.threshold_x1 = bytes(
            adjusted[1][x & 3] for x in range(OLED_WIDTH)
        )
        self.threshold_x2 = bytes(
            adjusted[2][x & 3] for x in range(OLED_WIDTH)
        )
        self.threshold_x3 = bytes(
            adjusted[3][x & 3] for x in range(OLED_WIDTH)
        )

        self.i2c, self.address, self.bus_mode, self.bus_frequency = (
            self._open_bus(
                hardware_bus,
                scl_pin,
                sda_pin,
                allow_soft_fallback,
            )
        )
        first_column = self.column_offset
        self.window = bytes(
            (
                0x00,
                0x21,
                first_column,
                first_column + OLED_WIDTH - 1,
                0x22,
                0,
                7,
            )
        )
        self._init_display()

    def _select_address(self, found):
        for candidate in (self.address_requested, 0x3C, 0x3D):
            if candidate in found:
                return candidate
        return None

    def _open_bus(
        self,
        hardware_bus,
        scl_pin,
        sda_pin,
        allow_soft_fallback,
    ):
        for frequency in (400_000, 100_000):
            bus = None
            try:
                bus = I2C(hardware_bus, freq=frequency)
                address = self._select_address(bus.scan())
                if address is not None:
                    return bus, address, "hardware", frequency
            except (OSError, ValueError):
                pass
            if bus is not None:
                try:
                    bus.deinit()
                except (AttributeError, OSError):
                    pass

        if allow_soft_fallback:
            for frequency in (400_000, 100_000):
                scl = Pin(scl_pin)
                sda = Pin(sda_pin)
                try:
                    bus = SoftI2C(
                        scl=scl,
                        sda=sda,
                        freq=frequency,
                    )
                    address = self._select_address(bus.scan())
                    if address is not None:
                        return bus, address, "software", frequency
                except (OSError, ValueError):
                    pass

        raise RuntimeError(
            "SSD1309 not found; check P7=SCL, P8=SDA, 3V3, GND and I2C mode"
        )

    def _commands(self, values):
        packet = bytearray(len(values) + 1)
        packet[0] = 0x00
        packet[1:] = values
        self.i2c.writeto(self.address, packet)

    def _init_display(self):
        self._commands(
            bytes(
                (
                    0xAE,
                    0xD5,
                    0x80,
                    0xA8,
                    0x3F,
                    0xD3,
                    0x00,
                    0x40,
                    0x20,
                    0x00,
                    0xA1,
                    0xC8,
                    0xDA,
                    0x12,
                    0x81,
                    self.oled_contrast,
                    0xD9,
                    0xF1,
                    0xDB,
                    0x40,
                    0xA4,
                    0xA6,
                    0xAF,
                )
            )
        )
        for index in range(1, len(self.tx)):
            self.tx[index] = 0
        self._show()

    def _show(self):
        self.i2c.writeto(self.address, self.window)
        self.i2c.writeto(self.address, self.tx)

    def _ensure_maps(self, width, height):
        if width < 2 or height < 2:
            raise ValueError("source image must be at least 2x2")
        if width == self.source_width and height == self.source_height:
            return
        self.source_width = width
        self.source_height = height
        self.x_map = array(
            "H",
            [
                (x * (width - 1)) // (OLED_WIDTH - 1)
                for x in range(OLED_WIDTH)
            ],
        )
        self.y_map = array(
            "H",
            [
                (y * (height - 1)) // (OLED_HEIGHT - 1)
                for y in range(OLED_HEIGHT)
            ],
        )

    def update_if_due(self, frame, now_ms=None, force=False):
        if not self.enabled:
            return False
        if now_ms is None:
            now_ms = _ticks_ms()
        if (
            not force
            and self.last_update is not None
            and _ticks_diff(now_ms, self.last_update) < self.period_ms
        ):
            return False

        start = _ticks_ms()
        width = frame.width()
        height = frame.height()
        self._ensure_maps(width, height)
        _pack_oled_grayscale(
            frame.bytearray(),
            self.tx,
            self.source_width,
            self.x_map,
            self.y_map,
            self.threshold_x0,
            self.threshold_x1,
            self.threshold_x2,
            self.threshold_x3,
        )
        self._show()
        done = _ticks_ms()
        self.last_duration_ms = _ticks_diff(done, start)
        self.last_update = now_ms
        self.update_count += 1
        return True


def _clamp(value, minimum, maximum):
    if value < minimum:
        return minimum
    if value > maximum:
        return maximum
    return value


@_native
def _fill_row_votes(
    source,
    frame_width,
    sample_indices,
    sample_count,
    lane_count,
    threshold,
    votes,
):
    """Vote along perspective-correct scan lanes using precomputed indices."""
    index = 0
    for sample_y in range(sample_count):
        vote = 0
        for _lane in range(lane_count):
            position = sample_indices[index]
            index += 1
            if (
                source[position - 1] >= threshold
                or source[position] >= threshold
                or source[position + 1] >= threshold
                or source[position - frame_width] >= threshold
                or source[position + frame_width] >= threshold
            ):
                vote += 1
        votes[sample_y] = vote


@_native
def _extract_band_centres(
    votes,
    smooth,
    height,
    radius,
    minimum_score,
    maximum_gap,
    minimum_distance,
    centres,
):
    """Smooth row votes and return vertically ordered bright-band centres."""
    for y in range(height):
        first = y - radius
        if first < 0:
            first = 0
        last = y + radius
        if last >= height:
            last = height - 1

        score = 0
        for sample_y in range(first, last + 1):
            score += votes[sample_y]
        smooth[y] = score

    count = 0
    active = False
    gap = 0
    weighted_y = 0
    weight = 0

    for y in range(height):
        score = smooth[y]
        if score >= minimum_score:
            if not active:
                active = True
                gap = 0
                weighted_y = 0
                weight = 0
            weighted_y += y * score
            weight += score
            gap = 0
        elif active:
            gap += 1
            if gap > maximum_gap:
                if weight > 0:
                    centre = (weighted_y + (weight // 2)) // weight
                    if (
                        count == 0
                        or (centre - centres[count - 1]) >= minimum_distance
                    ):
                        if count < len(centres):
                            centres[count] = centre
                            count += 1
                active = False

    if active and weight > 0:
        centre = (weighted_y + (weight // 2)) // weight
        if count == 0 or (centre - centres[count - 1]) >= minimum_distance:
            if count < len(centres):
                centres[count] = centre
                count += 1

    return count


def _spacing_and_quality(centres, count):
    if count < 2:
        return 0.0, 0

    spacing = (centres[count - 1] - centres[0]) / float(count - 1)
    if spacing <= 0:
        return 0.0, 0

    maximum_error = 0.0
    for index in range(1, count):
        gap = centres[index] - centres[index - 1]
        error = abs(gap - spacing)
        if error > maximum_error:
            maximum_error = error

    quality = 100 - int((maximum_error * 100.0) / spacing)
    return spacing, _clamp(quality, 0, 100)


def _bbox_from_quad(corners):
    minimum_x = FRAME_W - 1
    minimum_y = FRAME_H - 1
    maximum_x = 0
    maximum_y = 0
    for point in corners:
        x = int(point[0])
        y = int(point[1])
        if x < minimum_x:
            minimum_x = x
        if x > maximum_x:
            maximum_x = x
        if y < minimum_y:
            minimum_y = y
        if y > maximum_y:
            maximum_y = y
    return (
        minimum_x,
        minimum_y,
        maximum_x - minimum_x + 1,
        maximum_y - minimum_y + 1,
    )


def _quad_point(corners, u_permille, v_permille):
    """Map normalized plot coordinates into a perspective quadrilateral."""
    inverse_u = 1000 - u_permille
    inverse_v = 1000 - v_permille

    top_x = (
        (corners[0][0] * inverse_u)
        + (corners[1][0] * u_permille)
    )
    top_y = (
        (corners[0][1] * inverse_u)
        + (corners[1][1] * u_permille)
    )
    bottom_x = (
        (corners[3][0] * inverse_u)
        + (corners[2][0] * u_permille)
    )
    bottom_y = (
        (corners[3][1] * inverse_u)
        + (corners[2][1] * u_permille)
    )

    denominator = 1_000_000
    x = (
        (top_x * inverse_v)
        + (bottom_x * v_permille)
        + (denominator // 2)
    ) // denominator
    y = (
        (top_y * inverse_v)
        + (bottom_y * v_permille)
        + (denominator // 2)
    ) // denominator
    return (int(x), int(y))


def _inset_plot_quad(corners):
    left = PLOT_INSET_U_PERMILLE
    right = 1000 - PLOT_INSET_U_PERMILLE
    top = PLOT_INSET_TOP_PERMILLE
    bottom = 1000 - PLOT_INSET_BOTTOM_PERMILLE
    return (
        _quad_point(corners, left, top),
        _quad_point(corners, right, top),
        _quad_point(corners, right, bottom),
        _quad_point(corners, left, bottom),
    )


def _safe_statistics_roi(corners):
    """Return an axis-aligned rectangle guaranteed to be well inside the quad."""
    x0 = max(corners[0][0], corners[3][0]) + 2
    x1 = min(corners[1][0], corners[2][0]) - 2
    y0 = max(corners[0][1], corners[1][1]) + 2
    y1 = min(corners[2][1], corners[3][1]) - 2

    if x1 - x0 < 8 or y1 - y0 < 8:
        box = _bbox_from_quad(corners)
        x0 = box[0] + max(2, box[2] // 8)
        y0 = box[1] + max(2, box[3] // 8)
        x1 = box[0] + box[2] - max(3, box[2] // 8)
        y1 = box[1] + box[3] - max(3, box[3] // 8)

    x0 = _clamp(int(x0), 1, FRAME_W - 10)
    y0 = _clamp(int(y0), 1, FRAME_H - 10)
    x1 = _clamp(int(x1), x0 + 8, FRAME_W - 2)
    y1 = _clamp(int(y1), y0 + 8, FRAME_H - 2)
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1)


def _quad_distance(first, second):
    distance = 0
    for index in range(4):
        distance += abs(first[index][0] - second[index][0])
        distance += abs(first[index][1] - second[index][1])
    return distance // 8


def _quad_dark_count(source, corners, dark_limit):
    count = 0
    for v in (150, 325, 500, 675, 850):
        for u in (150, 325, 500, 675, 850):
            x, y = _quad_point(corners, u, v)
            x = _clamp(x, 0, FRAME_W - 1)
            y = _clamp(y, 0, FRAME_H - 1)
            if source[(y * FRAME_W) + x] <= dark_limit:
                count += 1
    return count


def _rect_plot_candidate(frame, rect, global_mean, previous_quad):
    width = int(rect.w)
    height = int(rect.h)
    if width < PLOT_MIN_WIDTH or height < PLOT_MIN_HEIGHT:
        return None

    area = width * height
    frame_area = FRAME_W * FRAME_H
    area_percent = (area * 100) // frame_area
    if (
        area_percent < PLOT_MIN_AREA_PERCENT
        or area_percent > PLOT_MAX_AREA_PERCENT
    ):
        return None

    aspect_x100 = (width * 100) // max(1, height)
    if (
        aspect_x100 < PLOT_MIN_ASPECT_X100
        or aspect_x100 > PLOT_MAX_ASPECT_X100
    ):
        return None

    corners = tuple(
        (int(point[0]), int(point[1]))
        for point in rect.corners
    )
    if len(corners) != 4:
        return None

    # Reject a full-frame border; it is usually the monitor or camera frame,
    # not the inner black XY plotting area.
    box = _bbox_from_quad(corners)
    if (
        box[0] <= 1
        or box[1] <= 1
        or box[0] + box[2] >= FRAME_W - 1
        or box[1] + box[3] >= FRAME_H - 1
    ):
        return None

    dark_limit = _clamp(int(global_mean), 45, 190)
    dark_count = _quad_dark_count(frame.bytearray(), corners, dark_limit)
    if dark_count < 12:
        return None

    content_quad = _inset_plot_quad(corners)
    stats = frame.get_statistics(roi=_safe_statistics_roi(content_quad))
    darkness = 255 - int(stats.mean)
    magnitude = int(rect.magnitude)

    # Dark-area density is squared: this favors the black plotting region over
    # a larger outer monitor/UI rectangle.
    score = (
        (area * dark_count * dark_count)
        + (area * max(0, darkness - 80))
        + (magnitude * 8)
    )
    if previous_quad is not None:
        distance = _quad_distance(corners, previous_quad)
        if distance < max(width, height) // 8:
            score += area * 180
    return (score, corners, "rect")


def _locate_with_rects(frame, global_mean, previous_quad):
    best = None
    try:
        rectangles = frame.find_rects(
            roi=RECT_SEARCH_ROI,
            threshold=RECT_EDGE_THRESHOLD,
        )
    except (AttributeError, OSError, MemoryError):
        rectangles = ()

    for rect in rectangles:
        candidate = _rect_plot_candidate(
            frame,
            rect,
            global_mean,
            previous_quad,
        )
        if candidate is not None and (
            best is None or candidate[0] > best[0]
        ):
            best = candidate
    return best


class CoarseDarkLocator:
    """Fallback largest-dark-component locator on an 8-pixel grid."""

    def __init__(self):
        self.grid_width = FRAME_W // FALLBACK_CELL_SIZE
        self.grid_height = FRAME_H // FALLBACK_CELL_SIZE
        self.cell_count = self.grid_width * self.grid_height
        self.mask = bytearray(self.cell_count)
        self.visited = bytearray(self.cell_count)
        self.stack = array("H", [0] * self.cell_count)
        self.integral_width = self.grid_width + 1
        self.integral = array(
            "H",
            [0] * ((self.grid_width + 1) * (self.grid_height + 1)),
        )

    def _classify(self, source, threshold):
        cell_size = FALLBACK_CELL_SIZE
        index = 0
        for grid_y in range(self.grid_height):
            y = (grid_y * cell_size) + (cell_size // 2)
            row = y * FRAME_W
            row_up = (y - 2) * FRAME_W
            row_down = (y + 2) * FRAME_W
            for grid_x in range(self.grid_width):
                x = (grid_x * cell_size) + (cell_size // 2)
                total = (
                    source[row + x]
                    + source[row + x - 2]
                    + source[row + x + 2]
                    + source[row_up + x]
                    + source[row_down + x]
                )
                self.mask[index] = 1 if total <= threshold * 5 else 0
                self.visited[index] = 0
                index += 1

    def _build_integral(self):
        width = self.grid_width
        integral_width = self.integral_width
        for index in range(len(self.integral)):
            self.integral[index] = 0
        for y in range(self.grid_height):
            row_sum = 0
            source_row = y * width
            integral_row = (y + 1) * integral_width
            previous_row = y * integral_width
            for x in range(width):
                row_sum += self.mask[source_row + x]
                self.integral[integral_row + x + 1] = (
                    self.integral[previous_row + x + 1] + row_sum
                )

    def _rectangle_sum(self, x, y, width, height):
        stride = self.integral_width
        bottom = y + height
        right = x + width
        return (
            self.integral[(bottom * stride) + right]
            - self.integral[(y * stride) + right]
            - self.integral[(bottom * stride) + x]
            + self.integral[(y * stride) + x]
        )

    def _locate_dense_window(self, previous_quad):
        """Find a large dense dark window even if it joins edge background."""
        self._build_integral()
        best = None
        for width_cells in (14, 17, 20, 23, 26, 29, 32):
            for height_cells in (9, 12, 15, 18, 21, 24):
                aspect_x100 = (
                    width_cells * 100
                ) // height_cells
                if (
                    aspect_x100 < PLOT_MIN_ASPECT_X100
                    or aspect_x100 > PLOT_MAX_ASPECT_X100
                ):
                    continue
                area = width_cells * height_cells
                ring_area = (
                    (width_cells + 2) * (height_cells + 2)
                ) - area

                for y in range(
                    2,
                    self.grid_height - height_cells - 1,
                ):
                    for x in range(
                        2,
                        self.grid_width - width_cells - 1,
                    ):
                        dark = self._rectangle_sum(
                            x,
                            y,
                            width_cells,
                            height_cells,
                        )
                        density = (dark * 100) // area
                        if density < 55:
                            continue
                        ring_dark = (
                            self._rectangle_sum(
                                x - 1,
                                y - 1,
                                width_cells + 2,
                                height_cells + 2,
                            )
                            - dark
                        )
                        ring_density = (
                            ring_dark * 100
                        ) // ring_area
                        score = (
                            (dark * 100)
                            + ((density * area) // 2)
                            + ((density - ring_density) * area)
                        )

                        x0 = x * FALLBACK_CELL_SIZE
                        y0 = y * FALLBACK_CELL_SIZE
                        x1 = (
                            (x + width_cells) * FALLBACK_CELL_SIZE
                        ) - 1
                        y1 = (
                            (y + height_cells) * FALLBACK_CELL_SIZE
                        ) - 1
                        corners = (
                            (x0, y0),
                            (x1, y0),
                            (x1, y1),
                            (x0, y1),
                        )
                        if previous_quad is not None:
                            distance = _quad_distance(
                                corners,
                                previous_quad,
                            )
                            if distance < max(
                                width_cells,
                                height_cells,
                            ) * FALLBACK_CELL_SIZE // 5:
                                score += area * 40
                        candidate = (score, corners, "dark")
                        if best is None or score > best[0]:
                            best = candidate
        return best

    def locate(self, frame, global_mean, global_stdev, previous_quad):
        source = frame.bytearray()
        threshold = _clamp(
            int(global_mean) - (int(global_stdev) // 6),
            35,
            175,
        )
        self._classify(source, threshold)

        grid_width = self.grid_width
        grid_height = self.grid_height
        best = None

        for start in range(self.cell_count):
            if not self.mask[start] or self.visited[start]:
                continue

            stack_size = 1
            self.stack[0] = start
            self.visited[start] = 1
            component_size = 0
            minimum_x = grid_width
            maximum_x = 0
            minimum_y = grid_height
            maximum_y = 0
            touches_edge = False

            while stack_size:
                stack_size -= 1
                cell = self.stack[stack_size]
                y = cell // grid_width
                x = cell - (y * grid_width)
                component_size += 1
                if x < minimum_x:
                    minimum_x = x
                if x > maximum_x:
                    maximum_x = x
                if y < minimum_y:
                    minimum_y = y
                if y > maximum_y:
                    maximum_y = y
                if (
                    x == 0
                    or y == 0
                    or x == grid_width - 1
                    or y == grid_height - 1
                ):
                    touches_edge = True

                if x > 0:
                    neighbor = cell - 1
                    if self.mask[neighbor] and not self.visited[neighbor]:
                        self.visited[neighbor] = 1
                        self.stack[stack_size] = neighbor
                        stack_size += 1
                if x + 1 < grid_width:
                    neighbor = cell + 1
                    if self.mask[neighbor] and not self.visited[neighbor]:
                        self.visited[neighbor] = 1
                        self.stack[stack_size] = neighbor
                        stack_size += 1
                if y > 0:
                    neighbor = cell - grid_width
                    if self.mask[neighbor] and not self.visited[neighbor]:
                        self.visited[neighbor] = 1
                        self.stack[stack_size] = neighbor
                        stack_size += 1
                if y + 1 < grid_height:
                    neighbor = cell + grid_width
                    if self.mask[neighbor] and not self.visited[neighbor]:
                        self.visited[neighbor] = 1
                        self.stack[stack_size] = neighbor
                        stack_size += 1

            if touches_edge or component_size < FALLBACK_MIN_DARK_CELLS:
                continue

            width_cells = maximum_x - minimum_x + 1
            height_cells = maximum_y - minimum_y + 1
            width = width_cells * FALLBACK_CELL_SIZE
            height = height_cells * FALLBACK_CELL_SIZE
            if width < PLOT_MIN_WIDTH or height < PLOT_MIN_HEIGHT:
                continue

            aspect_x100 = (width * 100) // max(1, height)
            if (
                aspect_x100 < PLOT_MIN_ASPECT_X100
                or aspect_x100 > PLOT_MAX_ASPECT_X100
            ):
                continue

            x0 = minimum_x * FALLBACK_CELL_SIZE
            y0 = minimum_y * FALLBACK_CELL_SIZE
            x1 = min(
                FRAME_W - 2,
                ((maximum_x + 1) * FALLBACK_CELL_SIZE) - 1,
            )
            y1 = min(
                FRAME_H - 2,
                ((maximum_y + 1) * FALLBACK_CELL_SIZE) - 1,
            )
            corners = (
                (x0, y0),
                (x1, y0),
                (x1, y1),
                (x0, y1),
            )
            score = component_size * 1000
            if previous_quad is not None:
                distance = _quad_distance(corners, previous_quad)
                if distance < max(width, height) // 5:
                    score += component_size * 500
            candidate = (score, corners, "dark")
            if best is None or candidate[0] > best[0]:
                best = candidate

        dense = self._locate_dense_window(previous_quad)
        if dense is not None and (best is None or dense[0] > best[0]):
            best = dense
        return best


class AutoPlotLocator:
    def __init__(self):
        self.quad = None
        self.content_quad = None
        self.stats_roi = None
        self.valid = False
        self.source = "none"
        self.confidence = 0
        self.misses = 0
        self.frames_until_locate = 0
        self.revision = 0
        self.pending_quad = None
        self.pending_hits = 0
        self.dark_locator = CoarseDarkLocator()

    def _accept(self, corners, source, confidence):
        corners = tuple(
            (int(point[0]), int(point[1]))
            for point in corners
        )
        previous_quad = self.quad
        previous_source = self.source

        if self.valid:
            old_box = _bbox_from_quad(self.quad)
            far_threshold = max(old_box[2], old_box[3]) // 4
            if _quad_distance(corners, self.quad) > far_threshold:
                if (
                    self.pending_quad is not None
                    and _quad_distance(corners, self.pending_quad) < 12
                ):
                    self.pending_hits += 1
                else:
                    self.pending_quad = corners
                    self.pending_hits = 1
                if self.pending_hits < 2:
                    return (False, False)
            else:
                self.pending_quad = None
                self.pending_hits = 0

            weight = PLOT_SMOOTH_OLD_WEIGHT
            smoothed = []
            for index in range(4):
                smoothed.append(
                    (
                        (
                            (self.quad[index][0] * weight)
                            + corners[index][0]
                        )
                        // (weight + 1),
                        (
                            (self.quad[index][1] * weight)
                            + corners[index][1]
                        )
                        // (weight + 1),
                    )
                )
            corners = tuple(smoothed)

        geometry_changed = (
            previous_quad is None
            or _quad_distance(corners, previous_quad) >= 1
        )
        source_changed = source != previous_source
        self.quad = corners
        self.content_quad = _inset_plot_quad(corners)
        self.stats_roi = _safe_statistics_roi(self.content_quad)
        self.valid = True
        self.source = source
        self.confidence = int(confidence)
        self.misses = 0
        if geometry_changed:
            self.revision += 1
        return (True, geometry_changed or source_changed)

    def update(self, frame):
        if self.frames_until_locate > 0:
            self.frames_until_locate -= 1
            return False

        global_stats = frame.get_statistics()
        candidate = _locate_with_rects(
            frame,
            global_stats.mean,
            self.quad if self.valid else None,
        )
        if candidate is None:
            candidate = self.dark_locator.locate(
                frame,
                global_stats.mean,
                global_stats.stdev,
                self.quad if self.valid else None,
            )

        if candidate is not None:
            accepted, changed = self._accept(
                candidate[1],
                candidate[2],
                candidate[0],
            )
            self.frames_until_locate = (
                PLOT_REDETECT_FRAMES
                if accepted
                else PLOT_FAST_RETRY_FRAMES
            )
            return changed

        self.misses += 1
        self.frames_until_locate = PLOT_FAST_RETRY_FRAMES
        if self.misses >= PLOT_MISSES_TO_LOST:
            changed = self.valid
            self.valid = False
            self.source = "none"
            self.confidence = 0
            self.revision += 1
            return changed
        return False


def _build_scan_indices(corners, center_u, destination):
    """Precompute five perspective scan lanes into a reusable index array."""
    output = 0
    last_sample = PROFILE_SAMPLES - 1
    half_lanes = SCAN_LANE_COUNT // 2
    for sample in range(PROFILE_SAMPLES):
        v = (sample * 1000) // last_sample
        for lane in range(SCAN_LANE_COUNT):
            u = center_u + (
                (lane - half_lanes) * SCAN_LANE_U_STEP_PERMILLE
            )
            x, y = _quad_point(corners, u, v)
            x = _clamp(x, 1, FRAME_W - 2)
            y = _clamp(y, 1, FRAME_H - 2)
            destination[output] = (y * FRAME_W) + x
            output += 1


def _scan_line_from_indices(indices):
    first = indices[SCAN_LANE_COUNT // 2]
    last = indices[
        ((PROFILE_SAMPLES - 1) * SCAN_LANE_COUNT)
        + (SCAN_LANE_COUNT // 2)
    ]
    return (
        first % FRAME_W,
        first // FRAME_W,
        last % FRAME_W,
        last // FRAME_W,
    )


class StableCount:
    """Two-frame confirmation without adding a long time-window delay."""

    def __init__(self):
        self.candidate_bands = -1
        self.candidate_frames = 0
        self.missing_frames = 0
        self.bands = 0
        self.spacing = 0.0
        self.quality = 0

    def update(self, bands, spacing, quality, valid):
        changed = False

        if not valid:
            self.missing_frames += 1
            self.candidate_frames = 0
            self.candidate_bands = -1
            if self.missing_frames >= COUNT_CLEAR_FRAMES and self.bands != 0:
                self.bands = 0
                self.spacing = 0.0
                self.quality = 0
                changed = True
            return changed

        self.missing_frames = 0
        if bands == self.candidate_bands:
            self.candidate_frames += 1
        else:
            self.candidate_bands = bands
            self.candidate_frames = 1

        if self.candidate_frames >= COUNT_CONFIRM_FRAMES:
            if bands != self.bands:
                self.bands = bands
                changed = True
            self.spacing = spacing
            self.quality = quality

        return changed

    def periods(self):
        return max(0, self.bands - 1)


class VerticalPeriodCounter:
    def __init__(self):
        self.votes = bytearray(PROFILE_SAMPLES)
        self.smooth = bytearray(PROFILE_SAMPLES)
        self.centres = [0] * MAX_BANDS
        self.indices = array(
            "I",
            [0] * (PROFILE_SAMPLES * SCAN_LANE_COUNT),
        )
        candidate_count = len(SCAN_CENTER_CANDIDATES)
        self.candidate_bands = bytearray(candidate_count)
        self.candidate_quality = bytearray(candidate_count)
        self.count_weights = [0] * (MAX_BANDS + 1)
        self.threshold = INITIAL_PIXEL_THRESHOLD
        self.threshold_valid = False
        self.frames_until_threshold = 0
        self.frames_until_reselect = 0
        self.bad_frames = 0
        self.plot_revision = -1
        self.selected_center_u = SCAN_CENTER_CANDIDATES[0]
        self.scan_line = (0, 0, 0, 0)
        self.raw_bands = 0
        self.raw_spacing = 0.0
        self.raw_quality = 0
        self.raw_valid = False
        self.stable = StableCount()

    def _update_threshold_if_due(self, frame, statistics_roi, force=False):
        if statistics_roi is None:
            return
        if self.frames_until_threshold > 0:
            if force:
                self.frames_until_threshold = 0
            else:
                self.frames_until_threshold -= 1
                return

        stats = frame.get_statistics(roi=statistics_roi)
        target = int(
            stats.mean
            + (
                (stats.stdev * THRESHOLD_STDEV_NUMERATOR)
                // THRESHOLD_STDEV_DENOMINATOR
            )
        )
        target = _clamp(target, THRESHOLD_MIN, THRESHOLD_MAX)

        if self.threshold_valid and not force:
            self.threshold = ((self.threshold * 3) + target) // 4
        else:
            self.threshold = target
            self.threshold_valid = True

        self.frames_until_threshold = THRESHOLD_UPDATE_FRAMES - 1

    def _detect_current_indices(self, source):
        _fill_row_votes(
            source,
            FRAME_W,
            self.indices,
            PROFILE_SAMPLES,
            SCAN_LANE_COUNT,
            self.threshold,
            self.votes,
        )
        bands = _extract_band_centres(
            self.votes,
            self.smooth,
            PROFILE_SAMPLES,
            SMOOTH_RADIUS,
            MIN_SMOOTH_SCORE,
            MAX_BRIDGED_DARK_ROWS,
            MIN_BAND_DISTANCE,
            self.centres,
        )
        spacing, quality = _spacing_and_quality(self.centres, bands)
        valid = (
            bands >= MIN_BANDS_FOR_PERIOD
            and bands <= MAX_BANDS
            and quality >= MIN_REGULARITY_QUALITY
        )
        return bands, spacing, quality, valid

    def _select_best_scan_band(self, source, content_quad):
        for count in range(MAX_BANDS + 1):
            self.count_weights[count] = 0

        best_fallback_index = 0
        best_fallback_score = -1
        for index, center_u in enumerate(SCAN_CENTER_CANDIDATES):
            _build_scan_indices(content_quad, center_u, self.indices)
            bands, _spacing, quality, valid = self._detect_current_indices(
                source
            )
            self.candidate_bands[index] = min(255, bands)
            self.candidate_quality[index] = min(255, quality)

            fallback_score = (bands * 12) + quality
            if fallback_score > best_fallback_score:
                best_fallback_score = fallback_score
                best_fallback_index = index

            if valid:
                # Weighted mode across horizontal positions rejects a diagonal
                # or a local waveform discontinuity at any single position.
                self.count_weights[bands] += quality + 50

        consensus_bands = 0
        consensus_weight = 0
        for bands in range(MIN_BANDS_FOR_PERIOD, MAX_BANDS + 1):
            if self.count_weights[bands] > consensus_weight:
                consensus_weight = self.count_weights[bands]
                consensus_bands = bands

        selected_index = best_fallback_index
        if consensus_bands:
            best_quality = -1
            for index in range(len(SCAN_CENTER_CANDIDATES)):
                if (
                    self.candidate_bands[index] == consensus_bands
                    and self.candidate_quality[index] > best_quality
                ):
                    best_quality = self.candidate_quality[index]
                    selected_index = index

        self.selected_center_u = SCAN_CENTER_CANDIDATES[selected_index]
        _build_scan_indices(
            content_quad,
            self.selected_center_u,
            self.indices,
        )
        self.scan_line = _scan_line_from_indices(self.indices)
        self.frames_until_reselect = SCAN_RESELECT_FRAMES

    def _spacing_in_image_pixels(self, spacing_samples):
        dx = self.scan_line[2] - self.scan_line[0]
        dy = self.scan_line[3] - self.scan_line[1]
        scan_length = math.sqrt((dx * dx) + (dy * dy))
        return (
            spacing_samples * scan_length
        ) / float(PROFILE_SAMPLES - 1)

    def process(self, frame, plot_locator):
        if not plot_locator.valid:
            self.raw_bands = 0
            self.raw_spacing = 0.0
            self.raw_quality = 0
            self.raw_valid = False
            self.bad_frames += 1
            return self.stable.update(0, 0.0, 0, False)

        geometry_changed = self.plot_revision != plot_locator.revision
        if geometry_changed:
            first_geometry = self.plot_revision < 0
            self.plot_revision = plot_locator.revision
            _build_scan_indices(
                plot_locator.content_quad,
                self.selected_center_u,
                self.indices,
            )
            self.scan_line = _scan_line_from_indices(self.indices)
            if first_geometry:
                self.frames_until_reselect = 0
            self._update_threshold_if_due(
                frame,
                plot_locator.stats_roi,
                force=True,
            )
        else:
            self._update_threshold_if_due(
                frame,
                plot_locator.stats_roi,
            )

        if self.frames_until_reselect > 0:
            self.frames_until_reselect -= 1

        if (
            geometry_changed
            or self.frames_until_reselect <= 0
            or self.bad_frames >= SCAN_BAD_FRAMES_TO_RESELECT
        ):
            self._select_best_scan_band(
                frame.bytearray(),
                plot_locator.content_quad,
            )

        (
            self.raw_bands,
            spacing_samples,
            self.raw_quality,
            self.raw_valid,
        ) = self._detect_current_indices(frame.bytearray())
        self.raw_spacing = self._spacing_in_image_pixels(spacing_samples)

        if self.raw_valid:
            self.bad_frames = 0
        else:
            self.bad_frames += 1

        return self.stable.update(
            self.raw_bands,
            self.raw_spacing,
            self.raw_quality,
            self.raw_valid,
        )

    def annotate(self, frame, plot_locator):
        # Draw only after detection, so annotations cannot affect counting.
        if plot_locator.valid:
            _draw_inverse_edges(
                frame,
                plot_locator.quad,
                PLOT_BOX_HALO_THICKNESS,
                PLOT_BOX_CORE_THICKNESS,
            )
            _draw_inverse_edges(
                frame,
                plot_locator.content_quad,
                CONTENT_BOX_HALO_THICKNESS,
                CONTENT_BOX_CORE_THICKNESS,
            )
            _draw_inverse_line(
                frame,
                self.scan_line,
                SCAN_LINE_HALO_THICKNESS,
                SCAN_LINE_CORE_THICKNESS,
            )

            for index in range(self.raw_bands):
                sample = self.centres[index]
                first_position = self.indices[
                    sample * SCAN_LANE_COUNT
                ]
                last_position = self.indices[
                    (sample * SCAN_LANE_COUNT)
                    + SCAN_LANE_COUNT
                    - 1
                ]
                frame.draw_line(
                    (
                        first_position % FRAME_W,
                        first_position // FRAME_W,
                        last_position % FRAME_W,
                        last_position // FRAME_W,
                    ),
                    color=255,
                    thickness=1,
                )

        frame.draw_rectangle((0, 0, 238, 23), color=0, fill=True)
        if not plot_locator.valid:
            label = "SEARCH PLOT"
        elif self.stable.bands > 0:
            label = "P:%02d B:%02d Q:%02d" % (
                self.stable.periods(),
                self.stable.bands,
                self.stable.quality,
            )
        else:
            label = "P:-- B:%02d Q:%02d" % (
                self.raw_bands,
                self.raw_quality,
            )
        frame.draw_string((2, 2), label, color=255, scale=2)


def _validate_configuration():
    if PROFILE_SAMPLES < 16:
        raise ValueError("PROFILE_SAMPLES is too small")
    if SCAN_LANE_COUNT < 3 or not (SCAN_LANE_COUNT & 1):
        raise ValueError("SCAN_LANE_COUNT must be an odd value >= 3")
    for center in SCAN_CENTER_CANDIDATES:
        half_span = (
            (SCAN_LANE_COUNT // 2) * SCAN_LANE_U_STEP_PERMILLE
        )
        if center - half_span <= 0 or center + half_span >= 1000:
            raise ValueError("scan candidate leaves the plotting quad")

def _init_camera():
    camera = csi.CSI()
    camera.reset()
    camera.pixformat(csi.GRAYSCALE)
    camera.framesize(csi.QVGA)
    camera.framebuffers(CAMERA_FRAMEBUFFERS)
    if TARGET_CAMERA_FPS:
        try:
            camera.framerate(TARGET_CAMERA_FPS)
        except (AttributeError, OSError, ValueError):
            pass
    # In the OpenMV 5 csi API, snapshot(time=...) skips/settles frames and
    # deliberately returns None. Take one normal startup frame afterward to
    # discover the board-specific QVGA geometry (N6=320x200, H7=320x240).
    camera.snapshot(time=CAMERA_SETTLE_MS)
    settled_frame = camera.snapshot()
    if settled_frame is None:
        raise RuntimeError("camera returned no startup frame")
    raw_bytes = _configure_frame_geometry(settled_frame)
    print(
        "camera api=csi format=GRAYSCALE frame=%dx%d raw_bytes=%d "
        "uncropped=1 buffers=%d"
        % (
            FRAME_W,
            FRAME_H,
            raw_bytes,
            CAMERA_FRAMEBUFFERS,
        )
    )
    return camera


def _init_display():
    if not ENABLE_OLED:
        print("oled_disabled config=ENABLE_OLED")
        return None

    try:
        display = AimDisplay(
            max_fps=OLED_MAX_FPS,
            contrast_percent=OLED_CONTRAST_PERCENT,
            address=OLED_ADDRESS,
            column_offset=OLED_COLUMN_OFFSET,
            hardware_bus=4,
            scl_pin="P7",
            sda_pin="P8",
            allow_soft_fallback=True,
        )
        print(
            "oled controller=SSD1309 mode=%s bus=4 scl=P7 sda=P8 "
            "freq=%d address=0x%02X max_fps=%d"
            % (
                display.bus_mode,
                display.bus_frequency,
                display.address,
                OLED_MAX_FPS,
            )
        )
        return display
    except Exception as error:
        print("oled_disabled error=%s" % error)
        if OLED_REQUIRED:
            raise
        return None


def run():
    _validate_configuration()
    camera = _init_camera()
    plot_locator = AutoPlotLocator()
    counter = VerticalPeriodCounter()
    display = _init_display()
    gc.collect()

    clock = time.clock()
    last_report = _ticks_ms()
    worst_primary_ms = 0
    last_oled_ms = 0

    while True:
        clock.tick()
        frame_start = _ticks_ms()

        # Exactly one capture per main-loop iteration.
        frame = camera.snapshot()
        plot_changed = plot_locator.update(frame)
        count_changed = counter.process(frame, plot_locator)

        result_ready = _ticks_ms()
        primary_ms = _ticks_diff(result_ready, frame_start)
        if primary_ms > worst_primary_ms:
            worst_primary_ms = primary_ms

        # Publish the latency-sensitive result before any OLED transfer.
        if (
            count_changed
            or plot_changed
            or _ticks_diff(result_ready, last_report) >= REPORT_PERIOD_MS
        ):
            print(
                "plot=%d locate=%s periods=%d bands=%d "
                "raw_bands=%d valid=%d scan_u=%d "
                "spacing_px=%.1f quality=%d threshold=%d "
                "fps=%.1f primary_ms=%d worst_primary_ms=%d oled_ms=%d"
                % (
                    1 if plot_locator.valid else 0,
                    plot_locator.source,
                    counter.stable.periods(),
                    counter.stable.bands,
                    counter.raw_bands,
                    1 if counter.raw_valid else 0,
                    counter.selected_center_u,
                    counter.stable.spacing,
                    counter.stable.quality,
                    counter.threshold,
                    clock.fps(),
                    primary_ms,
                    worst_primary_ms,
                    last_oled_ms,
                )
            )
            last_report = result_ready
            worst_primary_ms = 0

        if DRAW_IDE_AND_OLED_OVERLAY:
            counter.annotate(frame, plot_locator)

        # Same complete frame as the algorithm; no second snapshot or crop.
        if display is not None:
            try:
                if display.update_if_due(frame):
                    last_oled_ms = display.last_duration_ms
            except OSError as error:
                print("oled_runtime_disabled error=%s" % error)
                display.enabled = False
                display = None


if __name__ == "__main__":
    run()
