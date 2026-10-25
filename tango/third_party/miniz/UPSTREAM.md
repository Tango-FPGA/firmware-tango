# Upstream: richgel999/miniz (decompressor only)

- **Upstream:** https://github.com/richgel999/miniz
- **License:** MIT (`LICENSE`, copied unmodified from upstream).
- **Imported commit:** `4987e1a4d7ffd4d11b8d69f1e4c28d2b1e4ce8b8`.
- **Vendored unmodified:** `miniz.h`, `miniz_common.h`, `miniz_tinfl.h`, `miniz_tinfl.c`, `miniz_tdef.h`,
  `miniz_zip.h`. Only `miniz_tinfl.c` is compiled; the others are header-only and, with the build defines
  below, declare nothing (`miniz_tdef.h` is entirely `#ifndef MINIZ_NO_DEFLATE_APIS`, `miniz_zip.h` entirely
  `#ifndef MINIZ_NO_ARCHIVE_APIS`) -- they exist only because `miniz_tinfl.c`'s own first line is
  `#include "miniz.h"`, and `miniz.h` unconditionally includes all four at its end. Keeping that line
  untouched (rather than redirecting it to a smaller umbrella of our own) is what "vendored unmodified" means
  here: every file in this folder except `miniz_export.h` is byte-for-byte what upstream ships.
- **Not upstream:** `miniz_export.h`. Upstream generates it from CMake's `GenerateExportHeader` module and
  does not check in a static copy (this path 404s against `raw.githubusercontent.com`).
  It is a one-line substitute (`#define MINIZ_EXPORT` to nothing -- Tango links this into one static firmware
  image, so no shared-library import/export attribute is needed), not miniz's own code.
- **Deliberately not vendored:** `miniz_tdef.c` (the compressor) and `miniz_zip.c` (the zip reader/writer, ~4,900
  lines). Tango only ever decompresses, and has its own, much smaller container parser written against the
  public .ZIP format spec instead of miniz's general-purpose reader (`../../tango_zip.c`, `../../tango_zip.h`'s
  header comment cites the exact spec sections): MiSTer's zip behaviour is matched feature for feature by an
  implementation of Tango's own, not a port of MiSTer's code.
- **Build:** compile only `miniz_tinfl.c`, with `-DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS
  -DMINIZ_NO_ZLIB_APIS` (clean under `-std=c99 -Wall -Wextra -Werror`, host gcc: see the
  host test Makefile and `build.sh`). `MINIZ_NO_ARCHIVE_WRITING_APIS` is implied by `MINIZ_NO_ARCHIVE_APIS`
  and must not also be passed on the command line (`miniz.h` redefines it itself; passing it too is a
  redefinition error under `-Werror`).
