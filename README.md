# firmware-tango -- the Tango BL616 firmware

**Version 2026-10-25** (date code `20261025`)

firmware-tango is the firmware of the **BL616 microcontroller** on the **Sipeed Tang Console 138K**, the board the
Tango FPGA system runs on. It is a fork of [FPGA Companion](https://github.com/MiSTle-Dev/FPGA-Companion)
(MiSTle-Dev), the BL616 firmware of the MiSTle cores, with the Tango launcher and on-screen menu added. At power-on it
loads the launcher from the SD card into the FPGA; from then on it runs the launcher's pages, loads the core you pick,
runs that core's on-screen menu, feeds the core its files, keeps its settings, and turns USB keyboards and game
controllers into input for the menu and the core.

The FPGA side is not in this repository: the framework every core is built on is
[sys-tango](https://github.com/Tango-FPGA/sys-tango), the launcher core (`menu.bin`) is
[menu-tango](https://github.com/Tango-FPGA/menu-tango) (released separately), and
[console-template-tango](https://github.com/Tango-FPGA/console-template-tango) is the example core.

## Contents

- [What it does](#what-it-does)
- [Supported board](#supported-board)
- [Installing](#installing)
- [The SD card](#the-sd-card)
- [Which firmware is running](#which-firmware-is-running)
- [Building from source](#building-from-source)
- [Tests](#tests)
- [Layout](#layout)
- [Attributions](#attributions)
- [Licence](#licence)

## What it does

- **Loads the launcher at power-on.** The BL616 loads `/menu.fs`, else `/menu.bin`, from the SD card into the FPGA over
  JTAG, as MiSTer loads `menu.rbf`. Without either file the FPGA keeps the image it booted from its own flash. If an
  upload breaks off, the FPGA is told to reconfigure from that flash image.
- **The launcher** (`tango/tango_launcher.c`, `tango_browser.c`): MiSTer's menu core in the Tango look, page for page:
  the Cores browser (folders whose names start with `_`, and core files `.bin`, `.fs` and `.mra`), System Settings
  and Misc. Options (Information, Core Volume, Global Volume), with MiSTer's keys. A core file named
  `<Name>_<YYYYMMDD>.bin` is listed as `<Name>` with its date code. Picking a core loads it into the FPGA; after a
  load that fails, the BL616 restarts, loads the launcher again, and the launcher says what went wrong.
- **Arcade games** (`tango/tango_mra.c`, `tango_zip.c`): an `.mra` file is listed like a core. Picking one finds its
  core in `_Arcade/cores/` by MiSTer's matching rule, loads it, assembles the ROM from the zip files the `.mra` names
  (in `games/mame/`, else `_Arcade/mame/`; parts, patches and md5 checks as MiSTer's loader), and sends it to the
  core. The DIP switches of the `.mra` appear in the core's menu. An arcade game starts directly, without its menu
  opening first.
- **The in-core menu** (`tango/tango_coremenu.c`): MiSTer's OSD for a running core, built from the core's `CONF_STR`.
  Open and close it with **Start+Select** on a pad, or **F12** on a keyboard. Option pages, triggers, file loading
  (`F` entries, with a file browser that starts in `games/<core name>/` and opens `.zip` files as folders), backup-RAM
  saves (`FS` entries: `saves/<core>/<rom>.sav`), the System page (Core, Define buttons, Video processing, Audio
  filter, Reset settings, Save settings, About, Reboot) and Misc. Options. A core the launcher started opens with its
  menu shown (an arcade game from an `.mra` excepted).
- **Settings in MiSTer's formats** (`config/` on the card): the core's option word (`<CORE>.CFG`), its video
  processing choices, volume and button maps, written as MiSTer writes them.
- **Video and audio processing** (`tango/tango_vproc.c`): MiSTer's scaler filters, gamma tables, shadow masks, audio
  filters and video presets, read from MiSTer's own files on the card and sent to the framework, with `Tango.ini`
  taking MiSTer.ini's processing keys.
- **USB input.** Keyboards and game controllers plug into the Console's two USB-A ports. The FPGA's USB hosts
  (sys-tango) run both **full-speed and low-speed** devices; the firmware reads their reports over the SPI link:
  - **Keyboards** (HID boot protocol) drive the menus with MiSTer's keys and reach the core as PS/2 key codes.
  - **Game controllers** get a decode table from the **controller database** (`tango/tango_padres.h`), first match
    wins: a decode you defined for that pad (Define buttons), your own line in
    `linux/gamecontrollerdb/gamecontrollerdb_user.txt`, the built-in DualShock 4 decode, the fixed Xbox 360 (XInput)
    layout, a line in `linux/gamecontrollerdb/gamecontrollerdb.txt` (SDL_GameControllerDB format, as MiSTer reads
    it), the built-in table compiled from SDL_GameControllerDB (only when the card file cannot be opened), and last
    the pad's own HID report descriptor. The firmware programs the FPGA's decoder with the result. **Define buttons**
    maps any pad button to any core button and can save the pad's line to `gamecontrollerdb_user.txt`.
- **What it talks to on the FPGA:** the link of the FPGA Companion (SPI, with the targets SYS, HID and OSD), extended
  by Tango with the menu overlay's drawing commands, the pad decoder's commands, file downloads and uploads, and the
  video processing commands. The launcher and every Tango core speak it through sys-tango.
- **No networking.** The Tango build starts no Wi-Fi, FTP server, Telnet server or AT modem (`TANGO_NO_NET` in
  `companion/src/bl616/CMakeLists.txt`). Upstream's networking guide (`companion/NETWORKING.md`) describes upstream's
  build.

## Supported board

**Sipeed Tang Console 138K.** The release image is built for it (`./build.sh console138k`). `build.sh` also accepts
upstream's `console60k` and `primer25k` board names, but the Tango gateware exists for the Console 138K only.

## Installing

The BL616's flash holds two images:

| Address | Image | From |
|---|---|---|
| `0x0` | the **FPGA Partner** firmware (Sipeed's). It keeps the board's USB debugger (JTAG and UART for a PC) working and starts the firmware at `0x40000`. | not in this repository: [FPGA Companion](https://github.com/MiSTle-Dev/FPGA-Companion), `src/bl616/bl616_fpga_partner/bl616_fpga_partner_Console.bin` (110,992 bytes, sha256 `38e45f62965548c3e0efbf1048f48d28f45bb06b746a589ccba0ce7c801d6190`; FPGA Companion release v1.4.29's `bl616_fpga_partner_console60k.bin` is the same file) |
| `0x40000` | **firmware-tango** | the release's `firmware-tango_console138k.bin`, or your own build |

If `0x0` already holds this FPGA Partner image (the sha256 above), only `0x40000` needs writing.

**Put the BL616 in update (boot) mode:**

1. Unplug every cable that can power the board, including the **FPGA** USB-C and any power supply (HDMI can stay).
   While anything powers the board, the BL616 is already running and ignores the BOOT button.
2. Hold the **BOOT** button. On the current Console board it is the small button labelled BOOT just above the
   **MCU** USB-C port: of the two buttons stacked there, the shorter, flat one (not S2). On older Consoles it sits on
   the module, next to its USB-C port.
3. Plug a USB cable from the PC into the **MCU** USB-C port, then release BOOT.
4. The PC should now show a USB device **`Bouffalo CDC DEMO`** (`349b:6160`) with a serial port (`/dev/ttyACM*` on
   Linux). If it shows `SIPEED USB Debugger`, the board is not in update mode: start again at step 1.

**Write the images** with Bouffalo Lab's `BLFlashCommand` (in the SDK: `tools/bflb_tools/bouffalo_flash_cube/`,
builds for Linux, macOS and Windows). Put the images beside a configuration file such as this `flash_console138k.ini`:

    [cfg]
    # 0: no erase, 1: programmed section erase, 2: chip erase
    erase = 1
    skip_mode = 0x0, 0x0
    boot2_isp_mode = 0

    [official]
    filedir = ./bl616_fpga_partner_Console.bin
    address = 0x0

    [custom]
    filedir = ./firmware-tango_console138k.bin
    address = 0x40000

(To write only the firmware, leave out the `[official]` section.) Then, with the port the PC gave the board:

    BLFlashCommand-ubuntu --interface=uart --baudrate=2000000 --port=/dev/ttyACM0 --chipname=bl616 --cpu_id= \
        --config=flash_console138k.ini

Power-cycle the board. Connected to a PC, it comes back as `SIPEED USB Debugger`, and JTAG works as before.

**Starting the firmware:** the FPGA Partner starts firmware-tango only when **no PC is connected to the MCU USB-C
port**; with a PC there it stays the USB debugger for the PC, and the SD card is not read. Power the board from its
other USB-C port (or a power supply) to boot from the SD card.

## The SD card

A FAT32 card (the firmware's FatFs is built without exFAT). MiSTer's layout, with Tango's file names for the FPGA
images:

| Path | What |
|---|---|
| `/menu.fs` or `/menu.bin` | the launcher core ([menu-tango](https://github.com/Tango-FPGA/menu-tango)), loaded at power-on; `menu.fs` is tried first |
| `/_Console/<Name>_<YYYYMMDD>.bin` | a core (`.bin` = raw bitstream, `.fs` = Gowin's text format); the launcher lists any folder whose name starts with `_` |
| `/_Arcade/<game>.mra`, `/_Arcade/cores/` | arcade games (MiSTer's `.mra` files) and the cores they name |
| `/games/mame/`, `/_Arcade/mame/` | the ROM zip files the `.mra` files name |
| `/games/<core name>/` | a core's files; its file browser starts here (else at the card's root) |
| `/saves/<core name>/` | backup-RAM saves (`<rom>.sav`) |
| `/config/` | written by the firmware: `<CORE>.CFG` (the option word, on Save settings), `<core>_scaler.cfg`, `_gamma.cfg`, `_shmask.cfg`, `_afilter.cfg`, `_volume.cfg`, `inputs/` (button maps, `*_input_<vid>_<pid>_v3.map`) |
| `/linux/gamecontrollerdb/` | `gamecontrollerdb.txt` (SDL_GameControllerDB) and `gamecontrollerdb_user.txt` (your own lines, written by Define buttons) |
| `/Filters/`, `/Gamma/`, `/Shadow_Masks/`, `/Filters_Audio/`, `/Presets/` | MiSTer's video and audio processing files |
| `/Tango.ini` | optional: MiSTer.ini's processing keys (`VFILTER_DEFAULT=` ...), in `[MiSTer]` or a section named after the core |

The firmware itself is not on the card: it lives in the BL616's flash. While it restarts into a core, it keeps the
request in a small file, `/menu.req`.

## Which firmware is running

Open a core's menu, **System > About**: under `TangoFPGA.com` it shows `Tango FPGA v<yymmdd> (g<hash>)` (the
firmware's build date and the commit it was built from) and the core's name and version. The launcher's System
Settings shows the whole id, `Firmware g<hash> <date> <time>`. `-dirty` after the hash means the image was built from
a tree that differed from that commit; `Tango FPGA (build unknown)` means it was built without `build.sh`. The release
image's id is `g<the release commit> 2026-10-25 12:00`.

## Building from source

On Linux (x86-64), with `git`, `make`, `rsync` and Python 3. Everything the build needs goes into one folder, named by
`BL616_SDK` (`build.sh` has a default; setting it is clearer):

    export BL616_SDK=/path/to/bl616-sdk
    mkdir -p "$BL616_SDK" && cd "$BL616_SDK"
    # MiSTle-Dev's Bouffalo SDK, at the commit this release was built with
    git clone https://github.com/MiSTle-Dev/bouffalo_sdk.git && git -C bouffalo_sdk checkout 63784aa0
    # Bouffalo Lab's RISC-V toolchain (Xuantie GCC 10.2.0)
    git clone https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git && git -C toolchain_gcc_t-head_linux checkout c4afe91
    # FPGA Companion at the commit this fork is based on, for the submodules this repository leaves out
    git clone https://github.com/MiSTle-Dev/FPGA-Companion.git && git -C FPGA-Companion checkout ccb06d2
    git -C FPGA-Companion submodule update --init src/u8g2 src/FreeRTOS-Kernel src/tusb_xinput

`companion/UPSTREAM.md` lists the submodule commits (`git submodule update` checks out exactly those). Then, in this
repository:

    ./build.sh console138k clean

| Variable | Default | What |
|---|---|---|
| `BL616_SDK` | (see `build.sh`) | the folder with `bouffalo_sdk`, `toolchain_gcc_t-head_linux` and `FPGA-Companion` |
| `FPGA_COMPANION` | `$BL616_SDK/FPGA-Companion` | the FPGA Companion clone |
| `TANGO_BUILD_TREE` | `$BL616_SDK/tango-build` | the scratch tree the build runs in (two builds at once need two trees) |
| `TANGO_HEAP_MIN_KB` | 75 | the RAM check's floor (below) |
| `SOURCE_DATE_EPOCH` | unset | pins every build date and time (below) |

The build never writes into this repository. It copies `companion/` (the fork) and `tango/` into the scratch tree, adds
the three submodules from the FPGA Companion clone, writes the firmware id (`mk_fwid.sh`: `g<commit>[-dirty] <date>
<time>`), and runs the SDK's recipe. The image is
`$TANGO_BUILD_TREE/src/bl616/build/build_out/fpga_companion_bl616.bin` -- the file to write at `0x40000` (the release
names it `firmware-tango_console138k.bin`). Two checks fail the build:

- **Firmware id:** the image must contain the id just generated (a stale object cannot ship an old id).
- **RAM:** every FreeRTOS task stack comes from the heap; the OCRAM heap that static RAM leaves must be at least
  `TANGO_HEAP_MIN_KB` KB, so the boot's task stacks (about 66 KB) always fit.

**Reproducible builds.** Build from a clean checkout, with `clean`, and with `SOURCE_DATE_EPOCH` set: GCC pins every
`__DATE__` / `__TIME__` to it (in UTC), `mk_fwid.sh` pins the id's date, and every compile maps the build machine's
directories away (`-ffile-prefix-map`, `companion/src/bl616/CMakeLists.txt`). Built this way from two checkouts of this
commit in different folders, with different build trees, the image came out byte-identical. The release image was
built so, from this repository's commit, with the SDK, toolchain and FPGA Companion commits above:

    SOURCE_DATE_EPOCH=$(date -u -d '2026-10-25 12:00' +%s) ./build.sh console138k clean

Prebuilt libraries inside the SDK keep their own version strings.

## Tests

Host unit tests of the Tango modules (no SDK, no board; a C compiler, `make`, Python 3 and `git`):

    make -C tango/test

This builds and runs the CONF_STR parser tests, the launcher and in-core menu tests (`test_tango`: every page, key and
SPI transfer, recorded to `tango/test/out/`), the zip, md5 and `.mra` tests, the firmware id check (`check_fwid.sh`),
the controller database, pad manager and button-learning tests (with real HID descriptors and generated pads), and the
arcade DIP and upload tests. Not part of `make`, or not in this repository:

- **The OSD model check** (`tango_stream_check.py`), which compares the recorded pages with the menu's Python model,
  lives with the menu gateware in [menu-tango](https://github.com/Tango-FPGA/menu-tango); `make` here skips it.
- **The co-simulation** of the firmware against the menu gateware (Verilator) needs the gateware and is not included.
- `make vproc FILTERS=<folder> SYS=<sys-tango checkout>`: the video-processing transfers for every file of a folder of
  MiSTer's filter files, checked against sys-tango's Python implementation (`build_tools/vproc_preset.py`).
- `test_mra_real <MiSTer pack folder>` (or `MISTER_PACK`): the `.mra` loader against real `.mra` and ROM zip files;
  it skips when they are absent.
- `pd_vectors` and `pad_rtl_vectors` write the test vectors of the pad-decoder benches in sys-tango and menu-tango.

## Layout

| Path | What |
|---|---|
| `build.sh`, `mk_fwid.sh` | the build and the firmware id |
| `companion/` | the FPGA Companion fork: upstream's tree at `ccb06d2` with Tango's changes (marked `Tango` / `TANGO` in the code and `Modified for Tango` in each changed file's header), upstream's documentation (`README.md`, `SPI.md`, `DEBUGGING.md`, `NETWORKING.md`, `src/bl616/README.md`) and `UPSTREAM.md` |
| `companion/src/bl616/` | the BL616 port (`mcu_hw.c`, `CMakeLists.txt`); upstream's other ports (`src/rp2040`, `src/esp32`) are kept unchanged |
| `tango/` | the Tango launcher, in-core menu, controller database, saves, zip / `.mra` loading and video processing (portable C, also built on a PC by the tests) |
| `tango/test/` | the host unit tests |
| `tango/third_party/` | miniz's decompressor (`miniz/`, see its `UPSTREAM.md`) and the menu font's licence (`saira/OFL.txt`) |

Not included: Sipeed's closed binaries that upstream carries (`src/bl616/bl616_fpga_partner/*.bin`,
`src/bl616/friend_20k/*.bin`; get them from upstream), FatFs's HTML documentation bundle (at elm-chan.org), and
upstream's top-level `.gitmodules` (kept as `companion/gitmodules.upstream`).

## Attributions

| Component | Where | Author | Licence |
|---|---|---|---|
| **FPGA Companion** | `companion/` (the fork base, upstream `ccb06d2`) | MiSTle-Dev (Till Harbaum and contributors) | Apache-2.0 (`companion/LICENSE`) |
| **Bouffalo SDK** (MiSTle-Dev's `bouffalo_sdk`) | build dependency, compiled in (drivers, FreeRTOS port, CherryUSB, lwIP, FatFs, mbedTLS, ...) | Bouffalo Lab, MiSTle-Dev | Apache-2.0 (its `LICENSE`); its bundled components keep their own licences |
| **FreeRTOS** | in the SDK | Amazon.com, Inc. or its affiliates | MIT |
| **CherryUSB** (the BL616's USB stack) | in the SDK; `companion/src/bl616/usb_config.h`, `hid_keyboard_template.c` | sakumisu | Apache-2.0 |
| **lwIP** | in the SDK; `companion/src/bl616/lwipopts_user.h` | Swedish Institute of Computer Science and contributors | BSD-3-Clause |
| **mbedTLS** | in the SDK; `companion/src/bl616/mbedtls_sample_config.h` | The Mbed TLS Contributors | Apache-2.0 |
| **FatFs** | in the SDK (R0.15); a copy in `companion/src/fatfs/` for upstream's other ports | ChaN | FatFs licence (BSD-style, `companion/src/fatfs/LICENSE.txt`) |
| **u8g2** | FPGA Companion submodule `src/u8g2`, compiled in; `companion/src/font_helvR08_te.c` | Oliver Kraus; the font: Adobe Systems, Digital Equipment Corporation | BSD-2-Clause (its `LICENSE`); the font's copyright notice is in the file |
| **tusb_xinput** | FPGA Companion submodule `src/tusb_xinput`, copied into the build tree | Ryan Wendland | MIT |
| **puff** (inflate) | `companion/src/puff.c`, `puff.h` | Mark Adler | zlib licence (`puff.h`) |
| **SDL_GameControllerDB** | `companion/src/usb_controller_maps.h` (generated from it by `generate_usb_controller_maps.py`), the built-in pad table | Sam Lantinga and contributors | zlib |
| **miniz** (decompressor only) | `tango/third_party/miniz/` (upstream `4987e1a4`) | Rich Geldreich, RAD Game Tools and Valve Software | MIT (`tango/third_party/miniz/LICENSE`) |
| **Saira Condensed** SemiBold (the menu font) | rasterised glyphs in `tango/tango_font_data.c` | The Saira Project Authors | SIL Open Font License 1.1 (`tango/third_party/saira/OFL.txt`); reserved font name "Saira" |
| **Xuantie GCC** runtime and newlib | linked into the image | T-Head, the GNU project, the newlib authors | GCC runtime library exception; newlib's BSD-style licences |
| **FPGA Partner** firmware | not included (see [Installing](#installing)) | Sipeed | closed binary, no licence stated |

**Behaviour read and reimplemented (no code copied):** the launcher, the in-core menu, the saves, the settings files,
the controller database lookup, the button maps, `.mra` loading and the video and audio processing follow MiSTer's
behaviour ([MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer): pages, keys, file names and
formats); each Tango file cites the MiSTer source lines it was written from. The only MiSTer text in this repository is
test data: `tango/test/test_confstr.c` checks the CONF_STR parser against the menu strings of Template_MiSTer
(`Template.sv`) and Menu_MiSTer (`menu.sv`), copied as strings. The `.zip` reader follows PKWARE's
APPNOTE, the HID parser the USB HID 1.11 specification, the md5 RFC 1321.

## Licence

**Apache License 2.0** (`companion/LICENSE`). FPGA Companion is Apache-2.0; the code written for Tango (`tango/`
outside `third_party/`, `build.sh`, `mk_fwid.sh`) is Apache-2.0 too, `Copyright (c) 2026 the Tango authors`, as each
file's `SPDX-License-Identifier: Apache-2.0` header says; the FPGA Companion files Tango changed say so in their
headers ("Modified for Tango, 2026 ... Apache-2.0 as the original").

Except these third-party parts, which keep their own licences (their notices are in the files):

| Files | Licence |
|---|---|
| `companion/src/fatfs/` (FatFs, ChaN) | FatFs licence (BSD-style; `companion/src/fatfs/LICENSE.txt`) |
| `companion/src/puff.c`, `puff.h` (Mark Adler) | zlib licence (`puff.h`) |
| `companion/src/font_helvR08_te.c` (a u8g2 font from Adobe / Digital Equipment Corporation's Helvetica bitmap font) | the copyright and permission notice in the file |
| `companion/src/usb_controller_maps.h` (generated from SDL_GameControllerDB) | zlib |
| `companion/src/bl616/FreeRTOSConfig.h`, `companion/src/rp2040/FreeRTOSConfig.h` (Amazon.com, Inc. or its affiliates) | MIT |
| `companion/src/bl616/lwipopts_user.h` (Swedish Institute of Computer Science) | BSD-3-Clause |
| `companion/src/bl616/usb_config.h`, `hid_keyboard_template.c` (sakumisu, CherryUSB) | Apache-2.0 |
| `companion/src/bl616/mbedtls_sample_config.h` (The Mbed TLS Contributors) | Apache-2.0 |
| `companion/src/rp2040/tusb_config.h` (Ha Thach, TinyUSB) | MIT |
| `companion/src/rp2040/pico_sdk_import.cmake` (Raspberry Pi (Trading) Ltd.) | BSD-3-Clause |
| `tango/third_party/miniz/` (Rich Geldreich, RAD Game Tools and Valve Software) | MIT (`tango/third_party/miniz/LICENSE`) |
| the Saira Condensed glyphs rasterised into `tango/tango_font_data.c` (The Saira Project Authors) | SIL Open Font License 1.1 (`tango/third_party/saira/OFL.txt`) |

The SDK, the toolchain runtime and the FPGA Companion submodules compiled into the image keep their own licences (see
[Attributions](#attributions)).
