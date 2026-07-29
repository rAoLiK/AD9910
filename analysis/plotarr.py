#!/usr/bin/env python3
"""Interactive visualizer for gdb array logs with FFT support.

Features:
- Read all files under ../log (filename is array name)
- Parse numbers from brace block in gdb output
- Backward-compatible group syntax: [1,2][3] and save syntax with { ... }
- FFT syntax: f$2(1,2)@1M (window optional, sample rate optional)
- Two-panel collage in one figure: ';' (vertical) or '|' (horizontal)
- Optional per-figure metadata: <name=...,title=...>
"""

from __future__ import annotations

import re
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.axes import Axes
from matplotlib.backend_bases import Event
from matplotlib.backend_bases import MouseEvent
from matplotlib.artist import Artist
from matplotlib.figure import Figure
from matplotlib.lines import Line2D


ROOT_DIR = Path(__file__).resolve().parent
LOG_DIR = (ROOT_DIR / "../log").resolve()
SAVE_DIR = (ROOT_DIR / "output").resolve()
DEFAULT_WINDOW_ID = 1
DEFAULT_SAMPLE_RATE = 1_000_000.0

WindowBuilder = Callable[[int], np.ndarray]
ThemeName = Literal["dark", "bright"]


def build_rectangular(size: int) -> np.ndarray:
    return np.ones(size, dtype=float)


WINDOW_BUILDERS: dict[int, tuple[str, WindowBuilder]] = {
    1: ("Hann", np.hanning),
    2: ("Hamming", np.hamming),
    3: ("Blackman", np.blackman),
    4: ("Bartlett", np.bartlett),
    5: ("Rectangular", build_rectangular),
}

FFT_TOKEN_RE = re.compile(
    r"^(?P<prefix>fz|f)(?:\$(?P<window>\d+))?\((?P<indices>[^()]*)\)(?:@(?P<rate>[0-9]*\.?[0-9]+[kKmMgG]?))?$"
)


@dataclass
class TimeSeriesSpec:
    index: int


@dataclass
class FftSeriesSpec:
    indices: list[int]
    window_id: int
    sample_rate: float
    remove_dc: bool


@dataclass
class PaneSpec:
    series: list[TimeSeriesSpec | FftSeriesSpec]


@dataclass
class PlotGroupSpec:
    panes: list[PaneSpec]
    split: str | None
    save: bool
    fig_name: str | None
    title: str | None
    source: str


def parse_array_from_gdb_text(text: str) -> list[float]:
    """Extract numeric values from the first {...} block in gdb log text."""
    brace_match = re.search(r"\{([^{}]*)\}", text, flags=re.DOTALL)
    if brace_match is None:
        raise ValueError("No brace-enclosed array data found")

    payload = brace_match.group(1)
    if not payload.strip():
        raise ValueError("Array data block is empty")

    values: list[float] = []
    for token in payload.split(","):
        cleaned = token.strip()
        if not cleaned:
            continue
        try:
            values.append(float(cleaned))
        except ValueError as exc:
            raise ValueError(f"Invalid numeric token '{cleaned}'") from exc

    if not values:
        raise ValueError("No numeric values parsed from array block")
    return values


def load_arrays_from_log_dir(log_dir: Path) -> dict[str, list[float]]:
    """Read all files from log dir and parse array data."""
    if not log_dir.exists() or not log_dir.is_dir():
        raise FileNotFoundError(f"Log directory not found: {log_dir}")

    arrays: dict[str, list[float]] = {}
    parse_errors: dict[str, str] = {}

    for path in sorted(log_dir.iterdir()):
        if not path.is_file():
            continue

        name = path.name
        try:
            content = path.read_text(encoding="utf-8", errors="ignore")
            arrays[name] = parse_array_from_gdb_text(content)
        except Exception as exc:  # noqa: BLE001 - keep loop alive and show per-file parsing errors
            parse_errors[name] = str(exc)

    if parse_errors:
        print("\n[Warning] Some files cannot be plotted:")
        for fname, err in parse_errors.items():
            print(f"  - {fname}: {err}")
        print()

    return arrays


def print_array_list(names: Sequence[str]) -> None:
    print("\nAvailable plottable arrays:")
    for idx, name in enumerate(names, start=1):
        print(f"{idx}. {name}")
    print()


def print_window_list() -> None:
    print("Window functions (for FFT $N):")
    for idx in sorted(WINDOW_BUILDERS):
        win_name = WINDOW_BUILDERS[idx][0]
        suffix = " (default)" if idx == DEFAULT_WINDOW_ID else ""
        print(f"  {idx}. {win_name}{suffix}")
    print()


def split_top_level(text: str, delimiter: str) -> list[str]:
    """Split by delimiter while ignoring delimiters inside parentheses."""
    parts: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif ch == delimiter and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return parts


def split_top_level_quote_aware(text: str, delimiter: str) -> list[str]:
    """Split by delimiter while ignoring delimiters in quotes/parentheses."""
    parts: list[str] = []
    depth = 0
    quote: str | None = None
    start = 0

    for i, ch in enumerate(text):
        if quote is not None:
            if ch == quote:
                quote = None
            continue

        if ch in {'"', "'"}:
            quote = ch
            continue

        if ch == "(":
            depth += 1
            continue
        if ch == ")":
            depth = max(0, depth - 1)
            continue
        if ch == delimiter and depth == 0:
            parts.append(text[start:i])
            start = i + 1

    parts.append(text[start:])
    return parts


def find_first_top_level_separator(text: str, separators: set[str]) -> tuple[int, str] | None:
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
            continue
        if ch == ")":
            depth = max(0, depth - 1)
            continue
        if depth == 0 and ch in separators:
            return i, ch
    return None


def replace_top_level_separators(text: str, separators: set[str], replacement: str) -> str:
    chars = list(text)
    depth = 0
    for i, ch in enumerate(chars):
        if ch == "(":
            depth += 1
            continue
        if ch == ")":
            depth = max(0, depth - 1)
            continue
        if depth == 0 and ch in separators:
            chars[i] = replacement
    return "".join(chars)


def parse_index_csv(content: str) -> list[int]:
    if not content.strip():
        raise ValueError("Empty index list is not allowed")

    out: list[int] = []
    for item in content.split(","):
        token = item.strip()
        if not token:
            continue
        if not token.isdigit():
            raise ValueError(f"Invalid index '{token}'")
        out.append(int(token))

    if not out:
        raise ValueError("No valid index found")
    return out


def parse_sample_rate(rate_text: str | None) -> float:
    if rate_text is None:
        return DEFAULT_SAMPLE_RATE

    cleaned = rate_text.strip()
    match = re.fullmatch(r"([0-9]*\.?[0-9]+)([kKmMgG]?)", cleaned)
    if match is None:
        raise ValueError(f"Invalid sample rate '{rate_text}'")

    value = float(match.group(1))
    unit = match.group(2).upper()
    multiplier = {
        "": 1.0,
        "K": 1_000.0,
        "M": 1_000_000.0,
        "G": 1_000_000_000.0,
    }[unit]
    sample_rate = value * multiplier
    if sample_rate <= 0:
        raise ValueError("Sample rate must be positive")
    return sample_rate


def format_sample_rate(sample_rate: float) -> str:
    if sample_rate >= 1_000_000_000 and sample_rate % 1_000_000_000 == 0:
        return f"{int(sample_rate / 1_000_000_000)}G"
    if sample_rate >= 1_000_000 and sample_rate % 1_000_000 == 0:
        return f"{int(sample_rate / 1_000_000)}M"
    if sample_rate >= 1_000 and sample_rate % 1_000 == 0:
        return f"{int(sample_rate / 1_000)}K"
    return f"{sample_rate:g}"


def parse_fft_token(token: str) -> FftSeriesSpec | None:
    match = FFT_TOKEN_RE.fullmatch(token)
    if match is None:
        return None

    remove_dc = match.group("prefix") == "fz"
    window_text = match.group("window")
    window_id = int(window_text) if window_text is not None else DEFAULT_WINDOW_ID
    if window_id not in WINDOW_BUILDERS:
        raise ValueError(f"Unsupported window id {window_id}. Use one of {sorted(WINDOW_BUILDERS)}")

    indices = parse_index_csv(match.group("indices"))
    sample_rate = parse_sample_rate(match.group("rate"))
    return FftSeriesSpec(indices=indices, window_id=window_id, sample_rate=sample_rate, remove_dc=remove_dc)


def parse_metadata(meta_text: str) -> tuple[str | None, str | None]:
    """Parse metadata inside <...>, e.g. name=Win,title=My Figure."""
    fig_name: str | None = None
    title: str | None = None

    for raw in split_top_level_quote_aware(meta_text, ","):
        token = raw.strip()
        if not token:
            continue

        if "=" not in token:
            if fig_name is None:
                fig_name = strip_quotes(token)
                continue
            if title is None:
                title = strip_quotes(token)
                continue
            raise ValueError(
                f"Invalid metadata token '{token}'. Positional form supports at most two values: <figure_name,title>"
            )

        key, value = token.split("=", 1)
        key_clean = key.strip().lower()
        value_clean = strip_quotes(value.strip())
        if not value_clean:
            raise ValueError(f"Metadata value for '{key_clean}' is empty")

        if key_clean in {"name", "n", "fig", "figure"}:
            fig_name = value_clean
        elif key_clean in {"title", "t"}:
            title = value_clean
        else:
            raise ValueError(f"Unknown metadata key '{key_clean}'")

    return fig_name, title


def strip_quotes(text: str) -> str:
    if len(text) >= 2 and text[0] == text[-1] and text[0] in {'"', "'"}:
        return text[1:-1]
    return text


def parse_group_content(content: str) -> tuple[list[str], str | None]:
    cleaned = content.strip()
    if not cleaned:
        raise ValueError("Group content is empty")

    has_vertical = find_first_top_level_separator(cleaned, {";"}) is not None
    has_horizontal = find_first_top_level_separator(cleaned, {"|"}) is not None

    if not has_vertical and not has_horizontal:
        return [cleaned], None

    if has_vertical and has_horizontal:
        raise ValueError("Mixed top-level separators ';' and '|' in one group are ambiguous; use only one type")

    split_char = ";" if has_vertical else "|"
    panes = [part.strip() for part in split_top_level(cleaned, split_char)]
    if not panes or any(not pane for pane in panes):
        raise ValueError("Subplot split requires non-empty pane expressions")
    return panes, split_char


def parse_pane(content: str) -> PaneSpec:
    series: list[TimeSeriesSpec | FftSeriesSpec] = []
    for raw in split_top_level(content, ","):
        token = raw.strip()
        if not token:
            continue

        if token.startswith("f") and "@" in token and parse_fft_token(token) is None:
            raise ValueError(
                f"Invalid FFT sample rate in token '{token}'. Use numeric with optional K/M/G, e.g. @500K or @2M"
            )

        fft_spec = parse_fft_token(token)
        if fft_spec is not None:
            series.append(fft_spec)
            continue

        if token.isdigit():
            series.append(TimeSeriesSpec(index=int(token)))
            continue

        raise ValueError(f"Unsupported token '{token}'. Use integer index or FFT token like f$2(1,2)@1M")

    if not series:
        raise ValueError("Pane contains no valid series")
    return PaneSpec(series=series)


def parse_group_expression(expr: str) -> list[PlotGroupSpec]:
    """Parse command expression with backward compatibility and Add1 extensions.

    Examples:
      [1,2][3]
      [1;2,3][3,4|5,6]
      [1,2]{f$2(1,2)@1M|f(3)@2M,f(4)@1M}[6]
      [1,2]<name=Raw,title=Raw Signal>{f(1)@1M}<name=FFT,title=FFT Result>
    """
    if not expr.strip():
        raise ValueError("Input is empty")

    groups: list[PlotGroupSpec] = []
    i = 0
    n = len(expr)

    while i < n:
        while i < n and expr[i].isspace():
            i += 1
        if i >= n:
            break

        open_char = expr[i]
        if open_char not in "[{":
            raise ValueError(f"Unexpected character '{open_char}' at position {i}")
        close_char = "]" if open_char == "[" else "}"

        end = expr.find(close_char, i + 1)
        if end == -1:
            raise ValueError(f"Missing closing '{close_char}'")
        content = expr[i + 1 : end]
        i = end + 1

        while i < n and expr[i].isspace():
            i += 1

        fig_name: str | None = None
        title: str | None = None
        if i < n and expr[i] == "<":
            meta_end = expr.find(">", i + 1)
            if meta_end == -1:
                raise ValueError("Missing closing '>' for metadata")
            meta_text = expr[i + 1 : meta_end]
            fig_name, title = parse_metadata(meta_text)
            i = meta_end + 1

        pane_texts, split_char = parse_group_content(content)
        panes = [parse_pane(pane_text) for pane_text in pane_texts]
        groups.append(
            PlotGroupSpec(
                panes=panes,
                split=split_char,
                save=(open_char == "{"),
                fig_name=fig_name,
                title=title,
                source=content.strip(),
            )
        )

    if not groups:
        raise ValueError("No groups parsed")
    return groups


def validate_groups(groups: Sequence[PlotGroupSpec], max_index: int) -> None:
    for gi, group in enumerate(groups, start=1):
        for pane in group.panes:
            for item in pane.series:
                if isinstance(item, TimeSeriesSpec):
                    if item.index < 1 or item.index > max_index:
                        raise ValueError(
                            f"Group {gi} has out-of-range index {item.index}; valid range: 1..{max_index}"
                        )
                else:
                    for idx in item.indices:
                        if idx < 1 or idx > max_index:
                            raise ValueError(
                                f"Group {gi} has out-of-range FFT index {idx}; valid range: 1..{max_index}"
                            )


def next_power_of_two(value: int) -> int:
    if value <= 1:
        return 1
    return 1 << (value - 1).bit_length()


def compute_fft(data: np.ndarray, window_id: int, sample_rate: float, n_fft: int) -> tuple[np.ndarray, np.ndarray]:
    window_builder = WINDOW_BUILDERS[window_id][1]
    window = window_builder(data.size)
    windowed = data * window

    padded = np.zeros(n_fft, dtype=float)
    padded[: data.size] = windowed

    spectrum = np.fft.rfft(padded)
    frequencies = np.fft.rfftfreq(n_fft, d=1.0 / sample_rate)
    magnitude = np.abs(spectrum)
    return frequencies, magnitude


def compute_fft_remove_dc(data: np.ndarray, window_id: int, sample_rate: float, n_fft: int) -> tuple[np.ndarray, np.ndarray]:
    dc_removed = data - np.mean(data)
    return compute_fft(dc_removed, window_id, sample_rate, n_fft)


def apply_plot_style(theme: ThemeName) -> None:
    if theme == "dark":
        plt.style.use("dark_background")
    else:
        plt.style.use("default")
    plt.rcParams["axes.grid"] = True
    plt.rcParams["grid.alpha"] = 0.35
    plt.rcParams["grid.linestyle"] = "--"
    # plt.rcParams["figure.dpi"] = 100  # 100是默认，120=放大，150=更大


def set_window_title(fig: Figure, title: str) -> None:
    manager = getattr(fig.canvas, "manager", None)
    if manager is None:
        return
    setter = getattr(manager, "set_window_title", None)
    if callable(setter):
        _ = setter(title)


def sanitize_filename(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_")
    return cleaned or "figure"


def plot_pane(
    ax: Axes,
    pane: PaneSpec,
    names: Sequence[str],
    arrays: dict[str, list[float]],
) -> None:
    has_time = False
    has_fft = False
    max_sample_rate = 0.0

    for spec in pane.series:
        if isinstance(spec, TimeSeriesSpec):
            has_time = True
            series_name = names[spec.index - 1]
            data = arrays[series_name]
            x_values = list(range(len(data)))
            _ = ax.plot(x_values, data, linewidth=1.6, label=series_name)
            continue

        has_fft = True
        max_sample_rate = max(max_sample_rate, spec.sample_rate)
        selected_names = [names[idx - 1] for idx in spec.indices]
        selected_arrays = [np.asarray(arrays[item_name], dtype=float) for item_name in selected_names]
        max_len = max(len(arr) for arr in selected_arrays)
        n_fft = next_power_of_two(max_len)
        window_name = WINDOW_BUILDERS[spec.window_id][0]

        for item_name, item_array in zip(selected_names, selected_arrays):
            if spec.remove_dc:
                frequencies, magnitude = compute_fft_remove_dc(item_array, spec.window_id, spec.sample_rate, n_fft)
                fft_tag = "FZ"
            else:
                frequencies, magnitude = compute_fft(item_array, spec.window_id, spec.sample_rate, n_fft)
                fft_tag = "FFT"
            label = f"{fft_tag}({item_name}) [{window_name}, fs={format_sample_rate(spec.sample_rate)}]"
            _ = ax.plot(frequencies, magnitude, linewidth=1.5, label=label)

    if has_fft and not has_time:
        _ = ax.set_xlabel("Frequency (Hz)")
        _ = ax.set_ylabel("Magnitude")
        _ = ax.set_xlim(0.0, max_sample_rate / 2.0)
    elif has_time and not has_fft:
        _ = ax.set_xlabel("Index")
        _ = ax.set_ylabel("Value")
    else:
        _ = ax.set_xlabel("Index / Frequency (Hz)")
        _ = ax.set_ylabel("Value / Magnitude")

    _ = ax.legend(loc="best", fontsize=8)


def build_default_title(fig_number: int, group: PlotGroupSpec) -> str:
    return f"Figure {fig_number}: {group.source}"


def attach_snap_tooltip(fig: Figure, axes: Sequence[Axes], theme: ThemeName) -> None:
    """Attach hover snap tooltip that remains effective after zoom/pan."""
    if theme == "dark":
        annotation_fc = "#111111"
        annotation_ec = "#bbbbbb"
        annotation_text_color = "#f5f5f5"
        delta_text_color = "#ffe08a"
        delta_box_fc = "#222222"
        delta_box_ec = "#666666"
        pin_face_color = "#ffd166"
        pin_edge_color = "#111111"
        pin_text_color = "#fff6d6"
        pin_box_fc = "#2f2f2f"
        pin_box_ec = "#999999"
    else:
        annotation_fc = "#f3f3f3"
        annotation_ec = "#666666"
        annotation_text_color = "#111111"
        delta_text_color = "#7a5a00"
        delta_box_fc = "#f7f7f7"
        delta_box_ec = "#888888"
        pin_face_color = "#cc8400"
        pin_edge_color = "#111111"
        pin_text_color = "#111111"
        pin_box_fc = "#ffffff"
        pin_box_ec = "#777777"

    annotation = fig.text(
        0.0,
        0.0,
        "",
        transform=fig.transFigure,
        bbox={"boxstyle": "round,pad=0.3", "fc": annotation_fc, "ec": annotation_ec, "alpha": 0.95},
        color=annotation_text_color,
        fontsize=8,
        zorder=30,
        visible=False,
        ha="left",
        va="bottom",
    )

    marker_line: Line2D = axes[0].plot([], [], marker="o", markersize=6, markerfacecolor="none", markeredgewidth=1.4, markeredgecolor="#ffd166", linestyle="None", zorder=31)[0]
    marker_line.set_visible(False)

    all_lines: list[Line2D] = []
    for ax in axes:
        for line in ax.get_lines():
            x_data = np.asarray(line.get_xdata(orig=False), dtype=float)
            if x_data.size == 0:
                continue
            all_lines.append(line)

    if not all_lines:
        return

    pinned_points: list[tuple[Line2D, int, Line2D, Artist]] = []
    delta_texts: list[Artist] = []

    def remove_artist(artist: Artist) -> None:
        remove_fn = getattr(artist, "remove", None)
        if callable(remove_fn):
            remove_fn()

    def clear_delta_texts() -> None:
        for delta_text in delta_texts:
            remove_artist(delta_text)
        delta_texts.clear()

    def recompute_delta_labels() -> None:
        clear_delta_texts()
        if len(pinned_points) < 2:
            return

        # Sort by x value to match "adjacent along horizontal axis" semantics.
        ordered = sorted(
            pinned_points,
            key=lambda item: float(np.asarray(item[0].get_xdata(orig=False), dtype=float)[item[1]]),
        )

        for left, right in zip(ordered, ordered[1:]):
            left_line, left_idx, _, _ = left
            right_line, right_idx, _, _ = right
            left_x = float(np.asarray(left_line.get_xdata(orig=False), dtype=float)[left_idx])
            left_y = float(np.asarray(left_line.get_ydata(orig=False), dtype=float)[left_idx])
            right_x = float(np.asarray(right_line.get_xdata(orig=False), dtype=float)[right_idx])
            right_y = float(np.asarray(right_line.get_ydata(orig=False), dtype=float)[right_idx])

            dx = right_x - left_x
            dy = right_y - left_y
            mid_x = (left_x + right_x) / 2.0
            mid_y = (left_y + right_y) / 2.0

            raw_axis = left_line.axes if left_line.axes is not None else axes[0]
            axis = raw_axis if isinstance(raw_axis, Axes) else axes[0]
            delta_text = axis.text(
                mid_x,
                mid_y,
                f"Δx={dx:.6g}\nΔy={dy:.6g}",
                color=delta_text_color,
                fontsize=8,
                zorder=36,
                ha="center",
                va="bottom",
                bbox={"boxstyle": "round,pad=0.2", "fc": delta_box_fc, "ec": delta_box_ec, "alpha": 0.9},
            )
            delta_texts.append(delta_text)

    def point_key(line: Line2D, idx: int) -> tuple[int, int]:
        return id(line), idx

    def find_pinned(line: Line2D, idx: int) -> tuple[int, tuple[Line2D, int, Line2D, Artist]] | None:
        key = point_key(line, idx)
        for i, item in enumerate(pinned_points):
            if point_key(item[0], item[1]) == key:
                return i, item
        return None

    def toggle_pin(line: Line2D, idx: int) -> None:
        found = find_pinned(line, idx)
        if found is not None:
            remove_artist(found[1][2])
            remove_artist(found[1][3])
            pinned_points.pop(found[0])
            recompute_delta_labels()
            fig.canvas.draw_idle()
            return

        x_arr = np.asarray(line.get_xdata(orig=False), dtype=float)
        y_arr = np.asarray(line.get_ydata(orig=False), dtype=float)
        x_val = float(x_arr[idx])
        y_val = float(y_arr[idx])

        raw_axis = line.axes if line.axes is not None else axes[0]
        axis = raw_axis if isinstance(raw_axis, Axes) else axes[0]
        pin_marker = axis.plot(
            [x_val],
            [y_val],
            marker="o",
            markersize=7,
            markerfacecolor=pin_face_color,
            markeredgecolor=pin_edge_color,
            markeredgewidth=1.0,
            linestyle="None",
            zorder=35,
        )[0]
        pin_text = axis.text(
            x_val,
            y_val,
            f"x={x_val:.6g}\ny={y_val:.6g}",
            color=pin_text_color,
            fontsize=8,
            zorder=35,
            ha="left",
            va="bottom",
            bbox={"boxstyle": "round,pad=0.2", "fc": pin_box_fc, "ec": pin_box_ec, "alpha": 0.92},
        )

        pinned_points.append((line, idx, pin_marker, pin_text))
        recompute_delta_labels()
        fig.canvas.draw_idle()

    def hide_overlay() -> None:
        if annotation.get_visible() or marker_line.get_visible():
            annotation.set_visible(False)
            marker_line.set_visible(False)
            fig.canvas.draw_idle()

    def update_annotation_position(mouse_x: float, mouse_y: float) -> None:
        pad_px = 12.0
        gap_px = 14.0
        annotation.set_position((0.0, 0.0))

        text_bbox = annotation.get_window_extent()
        fig_bbox = fig.bbox

        text_w = text_bbox.width
        text_h = text_bbox.height
        fig_left = fig_bbox.x0
        fig_bottom = fig_bbox.y0
        fig_right = fig_bbox.x1
        fig_top = fig_bbox.y1

        place_right = mouse_x + gap_px + text_w + pad_px <= fig_right
        place_up = mouse_y + gap_px + text_h + pad_px <= fig_top

        x_px = mouse_x + gap_px if place_right else mouse_x - gap_px - text_w
        y_px = mouse_y + gap_px if place_up else mouse_y - gap_px - text_h

        x_px = min(max(x_px, fig_left + pad_px), max(fig_left + pad_px, fig_right - text_w - pad_px))
        y_px = min(max(y_px, fig_bottom + pad_px), max(fig_bottom + pad_px, fig_top - text_h - pad_px))

        fig_x, fig_y = fig.transFigure.inverted().transform((x_px, y_px))
        annotation.set_position((fig_x, fig_y))

    def on_move(event: Event) -> None:
        if not isinstance(event, MouseEvent):
            return

        if event.inaxes is None or event.x is None or event.y is None:
            hide_overlay()
            return

        best: tuple[float, Line2D, int] | None = None
        for line in all_lines:
            x_data = np.asarray(line.get_xdata(orig=False), dtype=float)
            y_data = np.asarray(line.get_ydata(orig=False), dtype=float)
            if x_data.size == 0:
                continue

            valid = np.isfinite(x_data) & np.isfinite(y_data)
            if not np.any(valid):
                continue

            x_valid = x_data[valid]
            y_valid = y_data[valid]
            axis = line.axes
            if axis is None:
                continue
            xy_pixels = axis.transData.transform(np.column_stack((x_valid, y_valid)))
            dx = xy_pixels[:, 0] - float(event.x)
            dy = xy_pixels[:, 1] - float(event.y)
            dist2 = dx * dx + dy * dy
            local_idx = int(np.argmin(dist2))
            local_d2 = float(dist2[local_idx])

            valid_indices = np.flatnonzero(valid)
            original_idx = int(valid_indices[local_idx])
            if best is None or local_d2 < best[0]:
                best = (local_d2, line, original_idx)

        if best is None:
            hide_overlay()
            return

        pixel_threshold2 = 20.0 * 20.0
        if best[0] > pixel_threshold2:
            hide_overlay()
            return

        _, best_line, idx = best
        x_arr = np.asarray(best_line.get_xdata(orig=False), dtype=float)
        y_arr = np.asarray(best_line.get_ydata(orig=False), dtype=float)
        x_val = float(x_arr[idx])
        y_val = float(y_arr[idx])

        target_axis = best_line.axes
        if target_axis is None:
            hide_overlay()
            return

        marker_line.set_data([x_val], [y_val])
        marker_line.set_color(best_line.get_color())
        marker_line.set_visible(True)
        marker_line.set_transform(target_axis.transData)

        line_label = best_line.get_label()
        annotation.set_text(f"{line_label}\nx={x_val:.6g}\ny={y_val:.6g}")
        update_annotation_position(float(event.x), float(event.y))
        annotation.set_visible(True)

        fig.canvas.draw_idle()

    def find_best_point(event: MouseEvent) -> tuple[Line2D, int] | None:
        if event.inaxes is None or event.x is None or event.y is None:
            return None

        best: tuple[float, Line2D, int] | None = None
        for line in all_lines:
            x_data = np.asarray(line.get_xdata(orig=False), dtype=float)
            y_data = np.asarray(line.get_ydata(orig=False), dtype=float)
            if x_data.size == 0:
                continue

            valid = np.isfinite(x_data) & np.isfinite(y_data)
            if not np.any(valid):
                continue

            axis = line.axes
            if axis is None:
                continue
            xy_pixels = axis.transData.transform(np.column_stack((x_data[valid], y_data[valid])))
            dx = xy_pixels[:, 0] - float(event.x)
            dy = xy_pixels[:, 1] - float(event.y)
            dist2 = dx * dx + dy * dy
            local_idx = int(np.argmin(dist2))
            local_d2 = float(dist2[local_idx])
            valid_indices = np.flatnonzero(valid)
            original_idx = int(valid_indices[local_idx])
            if best is None or local_d2 < best[0]:
                best = (local_d2, line, original_idx)

        if best is None or best[0] > 20.0 * 20.0:
            return None
        return best[1], best[2]

    def on_right_click(event: Event) -> None:
        if not isinstance(event, MouseEvent):
            return
        if event.button != 3:
            return

        best = find_best_point(event)
        if best is None:
            return
        toggle_pin(best[0], best[1])

    _ = fig.canvas.mpl_connect("motion_notify_event", on_move)
    _ = fig.canvas.mpl_connect("figure_leave_event", lambda _: hide_overlay())
    _ = fig.canvas.mpl_connect("axes_leave_event", lambda _: hide_overlay())
    _ = fig.canvas.mpl_connect("button_press_event", on_right_click)


def plot_groups(
    groups: Sequence[PlotGroupSpec],
    names: Sequence[str],
    arrays: dict[str, list[float]],
    theme: ThemeName,
) -> None:
    apply_plot_style(theme)
    SAVE_DIR.mkdir(parents=True, exist_ok=True)

    created_figures: list[tuple[Figure, bool, str]] = []

    for fig_number, group in enumerate(groups, start=1):
        pane_count = len(group.panes)
        if len(group.panes) == 1:
            fig, ax = plt.subplots(num=fig_number, figsize=(10, 5.8), constrained_layout=True)
            axes = [ax]
        elif group.split == "|":
            fig_width = max(10.5, 5.0 * pane_count)
            fig, axis_grid = plt.subplots(
                1,
                pane_count,
                num=fig_number,
                squeeze=False,
                figsize=(fig_width, 5.0),
                constrained_layout=True,
            )
            axes = [axis_grid[0, idx] for idx in range(pane_count)]
        else:
            fig_height = max(6.2, 3.2 * pane_count)
            fig, axis_grid = plt.subplots(
                pane_count,
                1,
                num=fig_number,
                squeeze=False,
                figsize=(9.8, fig_height),
                constrained_layout=True,
            )
            axes = [axis_grid[idx, 0] for idx in range(pane_count)]

        fig_name = group.fig_name or f"figure_{fig_number}"
        fig_title = group.title or build_default_title(fig_number, group)

        set_window_title(fig, fig_name)
        _ = fig.suptitle(fig_title, fontsize=10)

        for ax, pane in zip(axes, group.panes):
            plot_pane(ax, pane, names, arrays)

        attach_snap_tooltip(fig, axes, theme)
        created_figures.append((fig, group.save, fig_name))

    plt.show(block=False)

    used_save_names: set[str] = set()
    for fig, should_save, fig_name in created_figures:
        if not should_save:
            continue

        base_name = sanitize_filename(fig_name)
        candidate = base_name
        suffix = 2
        while candidate in used_save_names:
            candidate = f"{base_name}_{suffix}"
            suffix += 1
        used_save_names.add(candidate)

        filename = SAVE_DIR / f"{candidate}.png"
        fig.savefig(filename, dpi=160, bbox_inches="tight")
        print(f"Saved figure: {filename}")

    _ = input("\nPress Enter to close all figures and continue...")
    plt.close("all")


def print_help() -> None:
    print(
        """
Commands:
  [1,2][3,4]                          Plot two independent figures (backward compatible)
  [1,2]{3,4}                          Save the second figure
  [1;2;3]                             Multi-panel vertical collage in one figure
  [3|4|5]                             Multi-panel horizontal collage in one figure
  {f$2(1,2)@1M|f(3)@2M,f(4)@1M}       FFT analysis with optional window/sample-rate, saved figure
  {fz$2(1,2)@1M}                       FFT after DC removal (same options as FFT)
  [1,2]<Raw,Raw Signal>               Positional metadata: <figure_name,title>
  [1,2]<name=Raw,title=Raw Signal>    Metadata with explicit keys (still supported)

FFT token format:
  f$<window_id>(index_list)@<sample_rate>
  fz$<window_id>(index_list)@<sample_rate>  # remove DC before FFT
  - $ and @ are optional: default window is 1 (Hann), default sample rate is 1M
  - sample rate unit: K/M/G (e.g. 500K, 2M)

Separator rule:
  - In one group, use only one top-level split type (';' or '|').
  - Mixed top-level separators are rejected as ambiguous.

Other commands:
  bright        Switch to bright theme (default is dark)
  dark          Switch back to dark theme
  refresh       Reload ../log files
  windows       Show available FFT window functions
  help          Show this help
  q             Quit
"""
    )


def main() -> None:
    current_theme: ThemeName = "dark"

    print("GDB Array Visualizer + FFT")
    print(f"Log directory: {LOG_DIR}")
    print("Default theme: dark")
    print_help()
    print_window_list()

    while True:
        try:
            arrays = load_arrays_from_log_dir(LOG_DIR)
        except Exception as exc:  # noqa: BLE001
            print(f"[Error] Failed to load log files: {exc}")
            cmd = input("Type 'refresh' to retry or 'q' to quit: ").strip().lower()
            if cmd == "q":
                return
            continue

        names = sorted(arrays.keys())
        if not names:
            cmd = input("No plottable files found. Type 'refresh' to retry or 'q' to quit: ").strip().lower()
            if cmd == "q":
                return
            continue
        
        # print_window_list()
        print_array_list(names)
        user_input = input(
            f"Select arrays/analysis command (theme={current_theme}; bright/dark/refresh/windows/help/q): "
        ).strip()
        lowered = user_input.lower()

        if lowered == "q":
            break
        if lowered == "bright":
            current_theme = "bright"
            print("[Theme] Switched to bright theme.")
            continue
        if lowered == "dark":
            current_theme = "dark"
            print("[Theme] Switched to dark theme.")
            continue
        if lowered == "help":
            print_help()
            continue
        if lowered == "windows":
            print_window_list()
            continue
        if lowered == "refresh":
            continue

        try:
            groups = parse_group_expression(user_input)
            validate_groups(groups, len(names))
            plot_groups(groups, names, arrays, current_theme)
        except Exception as exc:  # noqa: BLE001
            print(f"[Input Error] {exc}")


if __name__ == "__main__":
    main()
