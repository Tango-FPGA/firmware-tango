# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the Tango authors
# Host unit tests for the Tango firmware modules (no SDK needed).
#   make            build + run everything: confstr tests, the launcher tests (which record the SPI streams
#                   into out/streams/, or into the menu repository's fpga/sim/streams/ when the firmware sits in its
#                   bl616/ folder), check_fwid.sh (the build id System > About shows: mk_fwid.sh), then the model
#                   check (menu-tango tools/osd-model/tango_stream_check.py: skipped outside the menu repository)
CFLAGS = -std=c99 -Wall -Wextra -Werror -O1 -g
# the tests write tango_page_t as a short positional list ({ banner, title, ... detail, detail_val }): the trailing `scroll` (the
# About ticker's position) is 0 there, as C intends
CFLAGS += -Wno-missing-field-initializers
# tango_zip.c pulls in miniz's tinfl (decompressor only, third_party/miniz/UPSTREAM.md): these three disable
# everything in the vendored miniz.h/_tdef.h/_zip.h headers except the tinfl declarations -- clean
# under this exact CFLAGS. MINIZ_NO_ARCHIVE_WRITING_APIS is NOT also needed/allowed here: miniz.h
# defines it itself once MINIZ_NO_ARCHIVE_APIS is set, and redefining it too is an error under -Werror.
CFLAGS += -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_APIS
ROOT    = ../../..
LAYOUT  = out
# The firmware folder is either the menu repository's bl616/ (its gateware sim and OSD model are then at ROOT) or the
# root of the firmware's own repository (ROOT is then outside it: nothing is written or read there).
IN_MENU := $(and $(filter bl616,$(notdir $(abspath ../..))),$(wildcard $(ROOT)/fpga/sim))
ifneq ($(IN_MENU),)
STREAMS = $(ROOT)/fpga/sim/streams
MODEL   = $(ROOT)/tools/osd-model/tango_stream_check.py
else
STREAMS = $(LAYOUT)/streams
MODEL   =
endif
MINIZ_SRC = ../third_party/miniz/miniz_tinfl.c
# tango_coremenu.c's own build_dip()/dip_select() call tango_mra.h's
# tango_mra_switches_dip_at directly (the DIP page's per-row, on-demand parse) -- tango_mra.c (and its own
# tango_md5.c dependency, for the <rom> md5 check its OTHER functions use, even though this one never
# does) must link into every test binary that builds tango_coremenu.c, not just test_mra's own.
TANGO_SRC = ../tango_padlearn.c ../tango_hidparse.c ../tango_paddec.c ../tango_paddb.c ../tango_layout.c ../tango_proto.c ../tango_browser.c ../tango_launcher.c ../tango_font_data.c ../tango_coremenu.c ../confstr.c ../tango_padmap.c ../tango_menubtn.c ../tango_save.c ../tango_kbd.c ../tango_vproc.c ../tango_volume.c ../tango_about.c ../tango_zip.c ../tango_mra.c ../tango_md5.c $(MINIZ_SRC)
TANGO_HDR = ../tango_padlearn.h ../tango_layout.h ../tango_proto.h ../tango_browser.h ../tango_launcher.h ../tango_font.h ../tango_model.h ../tango_coremenu.h ../confstr.h ../tango_padmap.h ../tango_menubtn.h ../tango_save.h ../tango_kbd.h ../tango_vproc.h ../tango_volume.h ../tango_about.h ../tango_zip.h ../tango_mra.h ../tango_md5.h

test: test_confstr test_tango test_zip test_coremenu_zip test_md5 test_mra test_paddb test_padmgr test_padlearn test_padgen test_dragonrise test_arcade test_coremenu_dip
	./test_confstr
	mkdir -p $(STREAMS) $(LAYOUT)
	./test_tango $(STREAMS) $(LAYOUT)
	./test_zip
	./test_coremenu_zip
	./test_md5
	./test_mra
	./check_fwid.sh
	./test_paddb
	./test_padmgr
	./test_padlearn
	./test_padgen 1000 1
	./test_padgen 1000 1 -dialog
	./test_padgen 1000 1 -dialog -stuck
	./test_padgen 20 1 -real
	./test_dragonrise
	./test_arcade
	./test_coremenu_dip
ifneq ($(MODEL),)
	python3 $(MODEL) --streams $(STREAMS) --layout $(LAYOUT) --dump
else
	@echo "model check skipped: tools/osd-model (the OSD model) lives with the menu gateware, not in this repository"
endif
test_confstr: test_confstr.c ../confstr.c ../confstr.h
	$(CC) $(CFLAGS) -o $@ test_confstr.c ../confstr.c
check_confstr: check_confstr.c fake_io.c fake_io.h $(TANGO_SRC) $(TANGO_HDR)
	$(CC) $(CFLAGS) -o $@ check_confstr.c fake_io.c $(TANGO_SRC)
test_tango: test_tango.c fake_io.c fake_io.h $(TANGO_SRC) $(TANGO_HDR)
	$(CC) $(CFLAGS) -o $@ test_tango.c fake_io.c $(TANGO_SRC)
# tango_zip.c's own container/streaming tests: real, standards-compliant .zip fixtures (fake_zip_fixtures.h,
# generated -- see its own header comment), an in-memory fake (fake_zip.c), no coremenu involved.
test_zip: test_zip.c fake_zip.c fake_zip.h fake_zip_fixtures.h ../tango_zip.c ../tango_zip.h ../tango_browser.c ../tango_browser.h $(MINIZ_SRC)
	$(CC) $(CFLAGS) -o $@ test_zip.c fake_zip.c ../tango_zip.c ../tango_browser.c ../tango_layout.c ../tango_font_data.c $(MINIZ_SRC)
# the browser-level enter/exit state machine (tango_coremenu.c's enter_zip/leave_zip), driven through the
# public tango_core_event API exactly as test_tango.c drives everything else.
test_coremenu_zip: test_coremenu_zip.c fake_io.c fake_io.h fake_zip_fixtures.h $(TANGO_SRC) $(TANGO_HDR)
	$(CC) $(CFLAGS) -o $@ test_coremenu_zip.c fake_io.c $(TANGO_SRC)
# RFC 1321's own test vectors, needed before trusting tango_md5.c on any real .mra md5="" check.
test_md5: test_md5.c ../tango_md5.c ../tango_md5.h
	$(CC) $(CFLAGS) -o $@ test_md5.c ../tango_md5.c
# tango_mra.c: <rbf> extraction, rom/part assembly, patch/md5-mismatch (fake_zip.c's
# in-memory tango_zipfs_t for both the .mra and the ROM zip) and the core-matching rule (fake_io.c's
# fake_card_t, a tango_fs_t); needs tango_zip.c (+ miniz) for the zip-member reads, tango_md5.c for
# the mismatch rule, tango_browser.c only for tango_is_core_file.
test_mra: test_mra.c fake_zip.c fake_zip.h fake_io.c fake_io.h ../tango_mra.c ../tango_mra.h ../tango_browser.c ../tango_browser.h ../tango_zip.c ../tango_zip.h ../tango_md5.c ../tango_md5.h $(MINIZ_SRC)
	$(CC) $(CFLAGS) -o $@ test_mra.c fake_zip.c fake_io.c ../tango_mra.c ../tango_browser.c ../tango_layout.c ../tango_font_data.c ../tango_zip.c ../tango_md5.c $(MINIZ_SRC)
# Controller database: HID descriptor parser, SDL lines, lookup ladder, decode tables, and the
# whole built-in pad table (companion/src/usb_controller_maps.h, included by the test itself: -DCONTROLLERDB_ATTR= keeps it out of sections)
PADDB_SRC = ../tango_hidparse.c ../tango_paddb.c ../tango_paddec.c ../tango_padres.c ../tango_paddb_builtin.c
PADDB_HDR = ../tango_hidparse.h ../tango_paddb.h ../tango_paddec.h ../tango_padres.h ../tango_paddb_builtin.h pad_fixtures.h
test_paddb: test_paddb.c $(PADDB_SRC) $(PADDB_HDR) ../../companion/src/usb_controller_maps.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ test_paddb.c $(PADDB_SRC)
# the pad manager (tango_padmgr.c) over the same libraries, with the link recorder of fake_io.c
PADMGR_SRC = ../tango_padlearn.c fake_pad_fpga.c $(PADDB_SRC) ../tango_padmgr.c ../tango_proto.c ../tango_layout.c ../tango_font_data.c ../tango_browser.c fake_io.c
test_padmgr: real_descriptors.h learn_gen.h test_padmgr.c $(PADMGR_SRC) $(PADDB_HDR) ../tango_padmgr.h fake_io.h fake_pad_fpga.h ../../companion/src/usb_controller_maps.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ test_padmgr.c $(PADMGR_SRC)
# the generated-pad campaign (padgen.h): N seeded pads through the real manager, capture and Define session
test_padgen: padgen.h test_padgen.c test_padmgr.c real_descriptors.h learn_gen.h $(PADMGR_SRC) $(PADDB_HDR) ../tango_padmgr.h fake_io.h fake_pad_fpga.h ../../companion/src/usb_controller_maps.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Wno-unused-function -o $@ test_padgen.c $(PADMGR_SRC) ../tango_padmap.c ../confstr.c
# a low-speed pad (DragonRise 0079:0006, real descriptors, real_pads.h) through the manager and the Define session
test_dragonrise: test_dragonrise.c real_pads.h test_padmgr.c real_descriptors.h learn_gen.h $(PADMGR_SRC) $(PADDB_HDR) ../tango_padmgr.h fake_io.h fake_pad_fpga.h ../../companion/src/usb_controller_maps.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Wno-unused-function -o $@ test_dragonrise.c $(PADMGR_SRC) ../tango_padmap.c ../confstr.c
# the learn engine (tango_padlearn.c) against every fixture
test_padlearn: learn_gen.h test_padlearn.c ../tango_padlearn.c ../tango_padlearn.h $(PADDB_SRC) $(PADDB_HDR)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ test_padlearn.c ../tango_padlearn.c $(PADDB_SRC)
# the vectors the RTL unit bench replays (sys-tango sim/pads/): C reference decode -> text, see pd_vectors.c
# the vectors of the full-chain pad bench (menu-tango fpga/sim/tb_pads.inc): ./pad_rtl_vectors <profile> > <menu-tango>/fpga/sim/pads/vec_<profile>.txt
pad_rtl_vectors: pad_rtl_vectors.c real_pads.h $(PADDB_SRC) $(PADDB_HDR)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ pad_rtl_vectors.c $(PADDB_SRC)
pd_vectors: pd_vectors.c $(PADDB_SRC) $(PADDB_HDR)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ pd_vectors.c $(PADDB_SRC)
# tango_dl_send/tango_ul_recv (tango_coremenu.c) against fake_io.c's own
# stand-in for the upload ports (UL_SELECT/UL_STATUS/READ) alongside the download stand-in.
test_arcade: test_arcade.c fake_io.c fake_io.h $(TANGO_SRC) $(TANGO_HDR)
	$(CC) $(CFLAGS) -o $@ test_arcade.c fake_io.c $(TANGO_SRC)
# the TANGO_CM_DIP page (tango_coremenu.c's build_dip/dip_select) against
# Galaga's real 11-switch <switches> block (fake_zip.c's own in-memory tango_zipfs_t, the same fake test_mra.c
# already uses for a .mra file).
test_coremenu_dip: test_coremenu_dip.c fake_io.c fake_io.h fake_zip.c fake_zip.h $(TANGO_SRC) $(TANGO_HDR)
	$(CC) $(CFLAGS) -o $@ test_coremenu_dip.c fake_io.c fake_zip.c $(TANGO_SRC)
# real .mra files against a MAME ROM library -- opt-in, like vproc's
# $FILTERS/$SYS below: never part of `make test`, never needs the library to build, skips cleanly
# (exit 0) at runtime if it's missing. No ROM bytes are ever embedded (see the file's own header).
test_mra_real: test_mra_real.c ../tango_mra.c ../tango_mra.h ../tango_browser.c ../tango_browser.h ../tango_zip.c ../tango_zip.h ../tango_md5.c ../tango_md5.h $(MINIZ_SRC)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Wno-format-truncation -o $@ test_mra_real.c ../tango_mra.c ../tango_browser.c ../tango_layout.c ../tango_font_data.c ../tango_zip.c ../tango_md5.c $(MINIZ_SRC)
# MiSTer's video/audio processing: the firmware's transfers vs sys-tango build_tools/vproc_preset.py, every file of
# a folder of MiSTer's filter files (FILTERS, required: Filters_MiSTer, Gamma, Shadow_Masks, Filters_Audio,
# Presets; SYS = a sys-tango checkout, default ../../../sys)
vproc_stream: vproc_stream.c fake_io.c fake_io.h ../tango_vproc.c ../tango_vproc.h ../tango_proto.h
	$(CC) $(CFLAGS) -o $@ vproc_stream.c fake_io.c ../tango_vproc.c
test_vproc: test_vproc.c fake_io.c fake_io.h $(TANGO_SRC) $(TANGO_HDR)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -DVP_MALLOC=t_vp_malloc -DVP_FREE=t_vp_free -o $@ test_vproc.c fake_io.c $(TANGO_SRC)
# the in-core OSD's Audio filter sequences as link streams for console-template-tango sim/tb_vproc.v (afilter_seq.c header)
afilter_seq: afilter_seq.c fake_io.c fake_io.h ../tango_vproc.c ../tango_vproc.h ../tango_proto.h
	$(CC) $(CFLAGS) -o $@ afilter_seq.c fake_io.c ../tango_vproc.c
vproc: vproc_stream test_vproc
	./test_vproc $${FILTERS:?set FILTERS to the folder of the MiSTer filter files}
	python3 vproc_crosscheck.py --sys $${SYS:-$(ROOT)/sys} --filters $${FILTERS:?set FILTERS to the folder of the MiSTer filter files}
# the font/constants are generated from the OSD model (menu-tango tools/osd-model); regenerate after changing it
gen:
	python3 $(ROOT)/tools/osd-model/gen_font_c.py
clean:
	rm -rf test_paddb test_padmgr test_padlearn pd_vectors test_confstr test_tango check_confstr vproc_stream test_vproc afilter_seq test_zip test_coremenu_zip test_md5 test_mra test_mra_real test_arcade test_coremenu_dip test_padgen test_dragonrise $(LAYOUT)
