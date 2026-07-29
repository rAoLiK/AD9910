@echo off
setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set ENV_FILE=%SCRIPT_DIR%environment.yml

where conda >nul 2>nul
if errorlevel 1 (
    echo [ERROR] conda not found in PATH. Install Miniconda/Anaconda first.
    exit /b 1
)

echo [INFO] Updating/creating conda environment "Anal" from %ENV_FILE%
call conda env update --name Anal --file "%ENV_FILE%" --prune
if errorlevel 1 (
    echo [ERROR] Failed to create/update environment.
    exit /b 1
)

echo [INFO] Done. Activate with: conda activate Anal
exit /b 0
