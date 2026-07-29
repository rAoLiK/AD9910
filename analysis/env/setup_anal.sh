#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${SCRIPT_DIR}/environment.yml"

if ! command -v conda >/dev/null 2>&1; then
  echo "[ERROR] conda not found in PATH. Install Miniconda/Anaconda first."
  exit 1
fi

echo "[INFO] Updating/creating conda environment 'Anal' from ${ENV_FILE}"
conda env update --name Anal --file "${ENV_FILE}" --prune

echo "[INFO] Done. Activate with: conda activate Anal"
