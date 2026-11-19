<p align="center">
  <img src="Documentation/assets/ValeciumOS.png" alt="ValeciumOS Element Block" width="150"/>
</p>

<h1 align="center">ValeciumOS</h1>

Valecium OS is a Unix-like operating system that is focused on low-level systems, modular kernel architecture, and study level code.

This repository contains:

- A kernel with architecture-specific and architecture-agnostic layers
- Filesystem, memory, process, and syscall subsystems
- Small userspace components (`libmath`, `sh`)
- Build tooling around SCons, QEMU, and a managed cross-toolchain

## Documentation

Primary source docs live in [`Documents/`](Documents/).

- Project index: [`Documents/index.ad`](Documents/index.ad)
- Build guide: [`Documents/building.ad`](Documents/building.ad)
- Development guide and coding conventions: [`Documents/devel.ad`](Documents/devel.ad)
- Implementation roadmap: [`Documents/roadmap.ad`](Documents/roadmap.ad)

Published docs are available at [docs.vyang.org/valecium](https://docs.vyang.org/valecium/).

## Building

### Native Build

```bash
# Basic dependencies
sudo apt install python3 scons

# Install extra deps and build toolchain
scons deps && scons toolchain

# Build
scons
```

### Docker Build

```bash
# Prebuilt docker image with toolchain and deps
docker run --rm -v $(pwd):/build vincent4486/valeciumos-builder:i686 scons
```

Notes:

- Build settings are persisted in `.config` at repository root (auto-created on first run).
- The output iso is at `build/<arch>_<type>/`
- On OS X and Docker, disk image building via `scripts/disk_builder.py` is not supported because mounting is required.

## Build Configuration Reference

Common SCons variables:

- `BuildConfig`: `debug` (default) or `release`
- `BuildArch`: `i686` (default), `x86_64`, `aarch64`
- `BuildType`: `full` (default), `kernel`, `usr`, `image`
- `BootType`: `bios` (default) or `efi`

For full build and platform notes, read [`Documents/building.ad`](Documents/building.ad).

## Development and Contributing

Start with [`Documents/devel.ad`](Documents/devel.ad), which covers:

- Build/development workflow
- Git and patch submission expectations
- Coding style, naming, and commenting conventions
- Architecture separation and HAL strategy (no platform-specific assembly in common code)

If you plan a larger feature, check [`Documents/roadmap.ad`](Documents/roadmap.ad) to align with current priorities.

## Project Status

Valecium OS is actively developed. Roadmap highlights include:

- Core kernel infrastructure improvements
- Expanded syscall coverage
- Filesystem and device capability growth
- Longer-term advanced features (networking, extended process model)

Track current progress in [`Documents/roadmap.ad`](Documents/roadmap.ad).

## Licensing

Project licensing summary is in [`COPYING`](COPYING). Individual license texts are in [`LICENCES/`](LICENCES/).

## Support

If you find a bug or want to request a feature, open an issue on the GitHub repository and include:

- Build command used
- Relevant logs or reproduction steps
