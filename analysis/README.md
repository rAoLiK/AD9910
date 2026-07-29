# GDB Array Visualizer + FFT (English)

This folder provides an interactive Python script to visualize array data printed by GDB logs, including FFT analysis.

## Features

- Reads all files from `../log` (file name = array name)
- Extracts numeric array data from the first `{...}` block in each file
- Lists all plottable files with numeric indexes
- Supports multi-figure grouped plotting using command-line syntax
- Supports selective save for chosen figures
- Supports FFT analysis with selectable window functions (default: Hann)
- Supports mixed command grammar with two-panel collage in one figure
- Supports custom figure window name and figure title
- Supports hover snap to nearest data point and coordinate tooltip (works after zoom/pan)
- Right click to pin/unpin snapped points; when multiple points are pinned, adjacent pinned pairs show Δx/Δy
- Loop workflow: plot → close all figures → next round
- Supports `refresh` to re-read `../log`
- Dark-background chart style, clean English labels and titles

## File

- `plotarr.py`

## Environment Setup

Environment name: `Anal`

Use scripts under `../env`:

### Linux/macOS (bash)

```bash
bash ../env/setup_anal.sh
conda activate Anal
```

### Windows (cmd)

```bat
..\env\setup_anal.bat
conda activate Anal
```

## Run

```bash
python visualize_gdb_arrays.py
```

or:

```bash
python plotarr.py
```

## Input Grammar

The script prints available arrays as:

```text
1.a
2.b
3.c
```

You can input grouped expressions:

- `[1,2][3,4,5][6]` → 3 figures, no save
- `[1,2]{3,4,5}[6]` → 3 figures, save the second figure (`{...}` group)
- `[1;2,3]` → one figure with two vertical panels
- `[3,4|5,6]` → one figure with two horizontal panels
- `[1;2;3]` → one figure with three vertical panels
- `[1|2|3]` → one figure with three horizontal panels
- `{f$2(1,2)@1M|f(3)@2M,f(4)@1M}` → saved figure, FFT in two horizontal panels
- `{fz$2(1,2)@1M}` → saved figure, remove DC first, then FFT
- `[1,2]<Raw,Raw Signal>` → positional metadata: `<figure_name,title>`
- `[1,2]<name=Raw,title=Raw Signal>` → keyed metadata (still supported)

### FFT Token

`f$<window_id>(index_list)@<sample_rate>`
`fz$<window_id>(index_list)@<sample_rate>`

- `$` and `@` are optional
- default window: `1` (Hann)
- default sample rate: `1M`
- sample rate unit supports `K`, `M`, `G`
- `fz` means removing DC component (subtract mean) before FFT; other logic is the same as `f`

Separator rule:

- In one group, use only one top-level split type (`;` or `|`)
- Mixed top-level separators in one group are rejected as ambiguous

Window IDs in script:

1. Hann (default)
2. Hamming
3. Blackman
4. Bartlett
5. Rectangular

Commands:

- `refresh` - reload files from `../log`
- `windows` - show FFT window list
- `help` - print help
- `q` - quit

Saved figures are written to:

- `./output/<figure_name>.png`
- If multiple saved groups resolve to the same sanitized file name, the script auto-suffixes (`_2`, `_3`, ...)

## Expected GDB Log Pattern

Example:

```text
=cmd-param-changed,param="logging enabled",value="on"
~"$4 = {0, 1, 2, 3, ...}\n"
```

The script extracts numeric values inside `{}`.
