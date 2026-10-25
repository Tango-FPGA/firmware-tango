/* miniz_export.h -- NOT part of upstream miniz. Upstream generates this file from CMake's
 * GenerateExportHeader module (shared-library visibility attributes); it ships no static copy of it in
 * the repo (this exact path 404s against raw.githubusercontent.com). Tango builds
 * miniz_tinfl.c as a plain object file in a single static firmware image, so no export/import attribute
 * is needed; this substitute just defines the one macro every vendored miniz header expects. */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
#define MINIZ_EXPORT
#endif
