param(
    [string]$Workspace = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"

function Get-Section {
    param(
        [string]$Text,
        [string]$StartMarker,
        [string]$EndMarker
    )

    $start = $Text.IndexOf($StartMarker)
    if ($start -lt 0) {
        throw "Missing start marker: $StartMarker"
    }
    $end = $Text.IndexOf($EndMarker, $start)
    if ($end -lt 0) {
        throw "Missing end marker: $EndMarker"
    }
    return $Text.Substring($start, $end - $start)
}

$ref1Path = Join-Path $Workspace "ref/main_1k_new_dataset.py"
$ref10Path = Join-Path $Workspace "ref/main_10k_new_dataset.py"
$targetPath = Join-Path $Workspace "openmv/OpenMV_main_task5_uart.py"

$ref1 = [System.IO.File]::ReadAllText($ref1Path)
$ref10 = [System.IO.File]::ReadAllText($ref10Path)
$target = [System.IO.File]::ReadAllText($targetPath)

$modelStart = "# =========================== MODEL DATA ==========================="
$classifierStart = "# ======================== PCA/KNN CLASSIFIER ======================"
$targetStart = "# ========================== 1 KHZ MODEL ==========================="

$model1 = Get-Section $ref1 $modelStart $classifierStart
$model10 = Get-Section $ref10 $modelStart $classifierStart
$model1 = $model1.Replace($modelStart, "# ========================== 1 KHZ MODEL ===========================")
$model10 = $model10.Replace($modelStart, "# ========================== 10 KHZ MODEL ==========================")
$model1 = [regex]::Replace($model1, "\bMODEL_", "M1_")
$model10 = [regex]::Replace($model10, "\bMODEL_", "M10_")

$modelMap = @'
# Bias-correction rules are ordered from the highest raw-frequency band
# downward. Each rule is (inclusive_low_hz, inclusive_high_hz_or_None,
# correction_hz), followed by the model's valid output range.
M1_FREQUENCY_CORRECTION = (
    (
        (7400, None, 800),
        (5600, None, 500),
        (3800, None, 300),
        (1900, None, 200),
    ),
    1000,
    10000,
)

M10_FREQUENCY_CORRECTION = (
    (
        (51000, None, 2000),
        (20000, 50000, 1000),
    ),
    10000,
    100000,
)

# =========================== MODEL MAP ============================

MODEL_1K_SPEC = (
    M1_FEATURE_SIDE,
    M1_FEATURE_COUNT,
    M1_COMPONENTS,
    M1_EXEMPLARS,
    M1_PROJECTION_QUANTIZATION,
    M1_GREEN_MINUS_RED_MIN,
    M1_GREEN_MINUS_BLUE_MIN,
    M1_MASK_DILATION,
    M1_WEIGHT_SCALES,
    M1_BIASES,
    M1_WEIGHT_B64_PARTS,
    M1_EXEMPLAR_B64_PARTS,
    M1_LABEL_B64_PARTS,
    M1_FREQUENCY_CORRECTION,
)

MODEL_10K_SPEC = (
    M10_FEATURE_SIDE,
    M10_FEATURE_COUNT,
    M10_COMPONENTS,
    M10_EXEMPLARS,
    M10_PROJECTION_QUANTIZATION,
    M10_GREEN_MINUS_RED_MIN,
    M10_GREEN_MINUS_BLUE_MIN,
    M10_MASK_DILATION,
    M10_WEIGHT_SCALES,
    M10_BIASES,
    M10_WEIGHT_B64,
    M10_EXEMPLAR_B64,
    M10_LABEL_B64,
    M10_FREQUENCY_CORRECTION,
)

'@

$replacement = $model1 + $model10 + $modelMap
$start = $target.IndexOf($targetStart)
$end = $target.IndexOf($classifierStart, $start)
if ($start -lt 0 -or $end -lt 0) {
    throw "Target model markers are missing"
}
$updated = $target.Substring(0, $start) + $replacement + $target.Substring($end)

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($targetPath, $updated, $utf8NoBom)
Write-Output "Synchronized embedded models into $targetPath"
