# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AD9910 driver library for STM32F407ZGT6 + STM32 HAL. Controls the AD9910 Direct Digital Synthesis (DDS) chip via SPI, supporting single-tone output, frequency/phase/amplitude sweeps, RAM-based arbitrary waveform generation, and DRG/OSK features.

## File Structure

- `ad9910.h` — Header: all compile-time configuration macros, type definitions, register constants, API declarations
- `ad9910.c` — Implementation: SPI communication, register access, waveform tables (Flash-resident), all driver logic
- `README.md` — Full usage manual in Chinese (pin tables, CubeMX setup, API reference, examples)
- `doc/AD9910.md` / `doc/AD9910.pdf` — AD9910 datasheet reference

## Build & Integration

This is a library, not a standalone project. It integrates into an STM32 HAL-based firmware project:

1. Copy `ad9910.h` and `ad9910.c` into your STM32 project's user source directory
2. Ensure `stm32f4xx_hal.h` is available (HAL library enabled)
3. Configure GPIO pins in CubeMX (see pin table in README.md)
4. Set SPI mode and pin macros before including `ad9910.h` (or in project-wide defines)

No Makefile, CMakeLists, or test framework is included — this library is consumed by an enclosing STM32 firmware project.

## Architecture

### Single-file driver pattern
The entire driver is two files. `ad9910.h` contains all configuration macros, data structures, and API declarations. `ad9910.c` contains all implementation including Flash-resident waveform lookup tables.

### Configuration via compile-time macros
All hardware configuration (pin assignments, SPI mode, timeouts) is done through `#ifndef`/`#define` macros in `ad9910.h`. Users override them by defining before include or in project settings. Defaults use GPIOE pins.

### SPI mode selection
Three modes controlled by `AD9910_SPI_MODE`:
- `AD9910_SPI_MODE_SOFTWARE` (default) — bit-banged via GPIO, most portable
- `AD9910_SPI_MODE_HARDWARE` — uses STM32 SPI peripheral
- Hardware + DMA — set `AD9910_SPI_USE_DMA 1U` with hardware mode

### Runtime state
All runtime state lives in `ad9910_t` struct (initialized via `AD9910_Init`). Shadow registers (`cfr1_shadow`, `cfr2_shadow`, `cfr3_shadow`) cache CFR values to avoid read-modify-write cycles.

### API conventions
- All public functions prefixed `AD9910_`
- Return `ad9910_status_t` (OK, ERROR, TIMEOUT, INVALID_PARAM, NOT_INITIALIZED, NOT_SUPPORTED, VERIFY_FAILED)
- Most write functions take `auto_io_update` parameter: non-zero triggers `IO_UPDATE` pulse automatically after write
- GPIO operations use direct `GPIOx->BSRR` register access for speed

### Register write flow
AD9910 registers require an `IO_UPDATE` pulse after write to transfer from buffer to working registers. The driver handles this automatically when `auto_io_update=1`, or callers can batch updates and call `AD9910_IOUpdate()` once.

### RAM waveform system
1024-point waveform tables stored in Flash (`g_ad9910_wave_triangle`, `g_ad9910_wave_square`, `g_ad9910_wave_sinc`). Custom waveforms must also be 1024 points, each 14-bit (0–16383). RAM playback requires: RAM enabled, destination set, data loaded, profile configured, profile selected, and IO_UPDATE — all six conditions.

## Key Pitfalls

- `IO_RESET` only resets the SPI state machine, NOT the chip. Use `RESET` pin for chip reset.
- PROFILE pins must be tied LOW if unused, otherwise AD9910 won't function.
- Readback (`AD9910_ReadRegister`, `AD9910_ReadRam`) requires `AD9910_ENABLE_READBACK=1` and the SDO/MISO pin connected.
- For phase-continuous frequency changes, only update FTW — do not rewrite POW or clear the phase accumulator.

## Language

README.md and in-code documentation are in Chinese (Simplified). API names and code identifiers are in English.
