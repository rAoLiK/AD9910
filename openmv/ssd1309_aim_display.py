"""
Low-overhead SSD1309 128x64 aiming preview for OpenMV.

The adapter never owns or modifies the camera. Pass the same full Image used by
the main vision pipeline to update_if_due().
"""

import time
import image as omv_image
from array import array
from machine import I2C, Pin, SoftI2C


try:
    import micropython
    _native = getattr(micropython, "native", None)
except ImportError:
    _native = None

if _native is None:
    def _native(function):
        return function


OLED_WIDTH = 128
OLED_HEIGHT = 64


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


def _adjust_threshold(value, brightness, contrast_percent):
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
def _pack_grayscale(
    src,
    dst,
    source_width,
    x_map,
    y_map,
    threshold_x0,
    threshold_x1,
    threshold_x2,
    threshold_x3,
):
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
            dst[output] = (
                (1 if src[row0 + source_x] >= threshold_x0[x] else 0)
                | (2 if src[row1 + source_x] >= threshold_x1[x] else 0)
                | (4 if src[row2 + source_x] >= threshold_x2[x] else 0)
                | (8 if src[row3 + source_x] >= threshold_x3[x] else 0)
                | (16 if src[row4 + source_x] >= threshold_x0[x] else 0)
                | (32 if src[row5 + source_x] >= threshold_x1[x] else 0)
                | (64 if src[row6 + source_x] >= threshold_x2[x] else 0)
                | (128 if src[row7 + source_x] >= threshold_x3[x] else 0)
            )
            output += 1


class AimDisplay:
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
        self.gray_scratch = None

        self.address_requested = address
        self.column_offset = column_offset
        self.oled_contrast = oled_contrast
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
            adjusted.append(tuple(
                _adjust_threshold(v, brightness, contrast_percent)
                for v in row
            ))
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
        first_col = self.column_offset
        self.window = bytes((
            0x00,
            0x21, first_col, first_col + OLED_WIDTH - 1,
            0x22, 0, 7,
        ))
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
                    bus = SoftI2C(scl=scl, sda=sda, freq=frequency)
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
        self._commands(bytes((
            0xAE,
            0xD5, 0x80,
            0xA8, 0x3F,
            0xD3, 0x00,
            0x40,
            0x20, 0x00,
            0xA1,
            0xC8,
            0xDA, 0x12,
            0x81, self.oled_contrast,
            0xD9, 0xF1,
            0xDB, 0x40,
            0xA4,
            0xA6,
            0xAF,
        )))
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
        # Source coordinates can exceed 255 at QVGA and above.
        self.x_map = array(
            "H",
            [(x * (width - 1)) // (OLED_WIDTH - 1)
             for x in range(OLED_WIDTH)],
        )
        self.y_map = array(
            "H",
            [(y * (height - 1)) // (OLED_HEIGHT - 1)
             for y in range(OLED_HEIGHT)],
        )
        self.gray_scratch = None

    def _grayscale_buffer(self, frame):
        if frame.format() == omv_image.GRAYSCALE:
            return frame.bytearray()

        if self.gray_scratch is None:
            self.gray_scratch = omv_image.Image(
                self.source_width,
                self.source_height,
                omv_image.GRAYSCALE,
            )
        self.gray_scratch.draw_image(frame, (0, 0))
        return self.gray_scratch.bytearray()

    def set_max_fps(self, max_fps):
        if max_fps <= 0:
            raise ValueError("max_fps must be positive")
        self.period_ms = max(1, 1000 // max_fps)

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
        src = self._grayscale_buffer(frame)
        _pack_grayscale(
            src,
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
