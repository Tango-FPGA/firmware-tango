# Upstream: MiSTle-Dev/FPGA-Companion

This folder is the BL616 firmware Tango forks. It is Apache-2.0; see `LICENSE`, which is upstream's own.
Its files keep Apache-2.0, Tango's changes to them included. The firmware built from this folder together with
`../tango/` is GPL-3.0-or-later as a whole, because `../tango/` contains code from MiSTer's Main_MiSTer (GPL-3.0); see
`../NOTICE` and `../LICENSE-GPL-3.0`.

- **Upstream:** https://github.com/MiSTle-Dev/FPGA-Companion
- **Imported commit:** `ccb06d2`, imported unchanged, so every Tango change shows as a diff after it.
- **Not imported** (submodules), pinned to these commits; `build.sh` copies the first three from a clone of upstream at `ccb06d2` with its submodules (`$BL616_SDK/FPGA-Companion`, or `FPGA_COMPANION`):
  - `src/u8g2` @ `dc9fc73ddfc4fc8690afea370780ba322bd0cd79` (and `sys/arm-linux/c-periphery` @ `23795679…`)
  - `src/FreeRTOS-Kernel` @ `31419bfceec515e19f77b88ff4b71be108afe015`
  - `src/tusb_xinput` @ `ae4f3a9fd7c57a6c8c36fdba7f7b1abc9b6a1a56`
  - `src/rp2040/FreeRTOS-Kernel` @ `4f7299d6ea746b27a9dd19e87af568e34bd65b15`
  - The original `.gitmodules` is kept as `gitmodules.upstream`.
- **SDK:** MiSTle-Dev/bouffalo_sdk @ `63784aa0`. **Toolchain:** bouffalolab/toolchain_gcc_t-head_linux @ `c4afe91` (Xuantie GCC 10.2.0). Both live in `$BL616_SDK`.
- **Closed binaries:** `src/bl616/bl616_fpga_partner/*.bin` and `src/bl616/friend_20k/*.bin` are Sipeed's images as shipped upstream; this repository does not include them (get them from upstream).
