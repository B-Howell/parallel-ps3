# Tests of the libretro-common pieces that replaced third-party libraries,
# against data the old libraries wrote. Run by the "Library replacement
# tests" workflow (.github/workflows/library-tests.yml):
#   make -C rpcs3/libretro/tests check

CC       ?= cc
LRC      := ../libretro-common
CFLAGS   ?= -O2
CFLAGS   += -Wall -I$(LRC)/include
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer
LDLIBS   += -lm

# The tests are C89; libretro-common builds as RetroArch's samples build it
TEST_STD := -std=c89 -Wdeclaration-after-statement
LRC_STD  := -std=gnu99
# as libretro_common.cmake builds it: net_http over its own TLS client
LRC_DEFS := -DHAVE_DYLIB -DHAVE_SSL -DHAVE_RETROSSL -DHAVE_CRYPTO \
	-DHAVE_RWAV -DHAVE_RFLAC -DHAVE_RVORBIS -DHAVE_ROPUS -DHAVE_RMP3 -DHAVE_RAAC \
	-DHAVE_RMP4 -DHAVE_RAC3 -DHAVE_RWMA -DHAVE_RWEBM -DHAVE_RATRAC3 -DHAVE_RATRAC3P

TESTS := test_zstd test_deflate test_rxml test_png_rows test_images test_jpg_rows test_http test_bcn test_tls_duplex test_yaml test_font test_np2 test_rsx_scale test_vpost test_atracx test_vdec test_media test_record test_config_database test_vblank_list test_video_sink test_video_sink_tsan test_vk_images test_vk_images_tsan test_sw_frames test_sw_frames_tsan test_gl_frames test_gl_fallback test_pad_values test_input_poll test_audio_handover test_audio_ring test_audio_ring_tsan test_audio_mix test_object_pack test_object_pack_tsan test_eeprom_table test_eeprom_table_tsan test_lf_u64_map test_lf_u64_map_tsan test_lf_item_pool test_lf_item_pool_tsan test_lf_sig_table test_lf_sig_table_tsan test_jit_host_symbols test_jit_host_symbols_tsan test_cpu_slots test_cpu_slots_tsan test_owner_calls test_owner_calls_tsan test_hle_service test_hle_service_tsan test_lf_hazard test_lf_hazard_tsan test_dma_views test_dma_views_tsan test_guest_page_locks test_guest_page_locks_tsan test_guest_faults test_guest_faults_tsan test_vk_interp_uploads test_vk_interp_dynamic test_crypto_hashes test_vk_heap test_vk_heap_tsan test_np_pool test_np_pool_tsan test_lf_post test_timer_ctrl test_lv2_waitset test_lv2_countq test_lv2_ownq test_lv2_flagq test_lv2_rwq test_lv2_lwq test_gcm_io_pages test_lv2_evq test_lv2_sched test_jit_cores test_rpcn_signal test_rsx_wake test_rsx_wake_tsan test_rsx_hold test_rsx_hold_tsan test_rsx_timestamp test_rpcn_reply_slots test_rpcn_reply_slots_tsan test_osk_text test_osk_text_tsan test_net_wake test_vk_loader

test_zstd_LRC := \
	$(LRC)/encodings/encoding_rzstd.c \
	$(LRC)/encodings/encoding_xxh64.c

test_deflate_LRC := \
	$(LRC)/encodings/encoding_deflate.c \
	$(LRC)/encodings/encoding_crc32.c \
	$(LRC)/features/features_cpu.c \
	$(LRC)/compat/compat_strl.c \
	$(LRC)/time/rtime.c

test_rxml_LRC := \
	$(LRC)/formats/xml/rxml.c

# libpng is the oracle here, never linked by the core: 1.6.59, the
# version RPCS3 built with, fetched and built from source, as distros
# ship older ones whose colour handling differs
test_png_rows_LRC := \
	$(LRC)/formats/png/rpng_rows.c \
	$(test_deflate_LRC)
LIBPNG     := libpng-1.6.59
LIBPNG_SRC := png pngerror pngget pngmem pngpread pngread pngrio pngrtran \
	pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil
LIBPNG_OBJ := $(patsubst %,obj/libpng/%.o,$(LIBPNG_SRC))
# plain C, no hardware filter code: the same arithmetic on every host
LIBPNG_DEF := -DPNG_ARM_NEON_OPT=0 -DPNG_MIPS_MSA_OPT=0 -DPNG_MIPS_MMI_OPT=0 \
	-DPNG_POWERPC_VSX_OPT=0 -DPNG_INTEL_SSE_OPT=0 -DPNG_LOONGARCH_LSX_OPT=0 \
	-DPNG_RISCV_RVV_OPT=0
PNG_CFLAGS := -I$(LIBPNG)

$(LIBPNG)/png.c:
	git clone -q --depth 1 -b v1.6.59 https://github.com/pnggroup/libpng.git $(LIBPNG)

$(LIBPNG)/pnglibconf.h: $(LIBPNG)/png.c
	cp $(LIBPNG)/scripts/pnglibconf.h.prebuilt $@

obj/libpng/%.o: $(LIBPNG)/pnglibconf.h
	@mkdir -p $(@D)
	$(CC) -O2 -w $(LIBPNG_DEF) -I$(LIBPNG) -c -o $@ $(LIBPNG)/$*.c

# stb is the oracle for rpng, rjpeg and rgif (stb_image, whose writer
# also makes the PNGs and JPEGs) and for rtt (stb_truetype): the commit
# RPCS3 pinned (3rdparty/stblib/stb, gone)
STB        := stb-013ac3b
STB_COMMIT := 013ac3beddff3dbffafd5177e7972067cd2b5083

$(STB)/stb_image.h:
	git clone -q https://github.com/nothings/stb.git $(STB)
	git -C $(STB) checkout -q $(STB_COMMIT)

test_images_LRC := \
	$(LRC)/formats/png/rpng.c \
	$(LRC)/formats/jpeg/rjpeg.c \
	$(LRC)/formats/gif/rgif.c \
	$(LRC)/streams/trans_stream.c \
	$(LRC)/streams/trans_stream_deflate.c \
	$(LRC)/streams/trans_stream_pipe.c \
	$(test_deflate_LRC)

# bcdec is the oracle for rdds's block decoders: the commit RPCS3's copy
# (3rdparty/bcdec/bcdec.hpp, gone) was based on
BCDEC        := bcdec-963c5e5
BCDEC_COMMIT := 963c5e56b7a335e066cff7d16a3de75f4e8ad366

$(BCDEC)/bcdec.h:
	git clone -q https://github.com/iOrange/bcdec.git $(BCDEC)
	git -C $(BCDEC) checkout -q $(BCDEC_COMMIT)

test_bcn_LRC := $(LRC)/formats/dds/rdds.c

# ryaml and ryaml_writer; rinflate for the gzipped fixtures
test_yaml_LRC := \
	$(LRC)/formats/yaml/ryaml.c \
	$(LRC)/formats/yaml/ryaml_writer.c \
	$(test_deflate_LRC)

# net_http and the TLS client, with what they need
test_http_LRC := \
	$(LRC)/net/net_http.c \
	$(LRC)/net/net_socket.c \
	$(LRC)/net/net_compat.c \
	$(LRC)/net/net_socket_ssl_retro.c \
	$(LRC)/crypto/crypto.c \
	$(LRC)/crypto/kdf.c \
	$(LRC)/crypto/pk.c \
	$(LRC)/crypto/x509.c \
	$(LRC)/hash/lrc_hash.c \
	$(LRC)/utils/md5.c \
	$(LRC)/encodings/encoding_base64.c \
	$(LRC)/encodings/encoding_utf.c \
	$(LRC)/streams/file_stream.c \
	$(LRC)/vfs/vfs_implementation.c \
	$(LRC)/file/file_path.c \
	$(LRC)/file/file_path_io.c \
	$(LRC)/file/retro_dirent.c \
	$(LRC)/string/rstrtod.c \
	$(LRC)/string/stdstring.c \
	$(LRC)/lists/string_list.c \
	$(LRC)/compat/fopen_utf8.c \
	$(LRC)/compat/compat_strl.c \
	$(LRC)/compat/compat_strcasestr.c \
	$(LRC)/compat/compat_posix_string.c \
	$(LRC)/features/features_cpu.c \
	$(LRC)/time/rtime.c

all: $(TESTS)

obj/%.o: $(LRC)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LRC_STD) $(LRC_DEFS) $(SANITIZE) -c -o $@ $<

obj/%.o: %.c test_common.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

lrc_objs = $(patsubst $(LRC)/%.c,obj/%.o,$(1))

test_zstd: obj/test_zstd.o $(call lrc_objs,$(test_zstd_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_deflate: obj/test_deflate.o $(call lrc_objs,$(test_deflate_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_rxml: obj/test_rxml.o $(call lrc_objs,$(test_rxml_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

obj/cellPngDec_rows.o: ../../Emu/Cell/Modules/cellPngDec_rows.c ../../Emu/Cell/Modules/cellPngDec_rows.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_png_rows.o: test_png_rows.c test_common.h ../../Emu/Cell/Modules/cellPngDec_rows.h $(LIBPNG)/pnglibconf.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LRC_STD) $(PNG_CFLAGS) $(SANITIZE) -c -o $@ $<

test_png_rows: obj/test_png_rows.o obj/cellPngDec_rows.o $(call lrc_objs,$(test_png_rows_LRC)) $(LIBPNG_OBJ)
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) -lz $(LDLIBS)

obj/test_images.o: test_images.c $(STB)/stb_image.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LRC_STD) -I$(STB) $(SANITIZE) -c -o $@ $<

obj/stb_impl.o: stb_impl.c $(STB)/stb_image.h
	@mkdir -p $(@D)
	$(CC) -O2 -w $(LRC_STD) -I$(STB) -c -o $@ $<

test_images: obj/test_images.o obj/stb_impl.o $(call lrc_objs,$(test_images_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# cellJpgDec's decode; its allocations counted by wrapping malloc
test_jpg_rows_LRC := \
	$(LRC)/formats/jpeg/rjpeg.c \
	$(LRC)/features/features_cpu.c \
	$(LRC)/compat/compat_strl.c

obj/cellJpgDec_rows.o: ../../Emu/Cell/Modules/cellJpgDec_rows.c ../../Emu/Cell/Modules/cellJpgDec_rows.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_jpg_rows.o: test_jpg_rows.c ../../Emu/Cell/Modules/cellJpgDec_rows.h $(STB)/stb_image.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LRC_STD) -I$(STB) $(SANITIZE) -c -o $@ $<

test_jpg_rows: obj/test_jpg_rows.o obj/cellJpgDec_rows.o obj/stb_impl.o $(call lrc_objs,$(sort $(test_jpg_rows_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) -Wl,--wrap=malloc,--wrap=calloc $(LDLIBS)

obj/test_http.o: test_http.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LRC_STD) $(SANITIZE) -c -o $@ $<

test_http: obj/test_http.o $(call lrc_objs,$(sort $(test_http_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS) -lpthread

# rpcs3/Crypto's hashes on libretro-common's, with the TLS test's set
test_crypto_hashes: obj/test_crypto_hashes.o $(call lrc_objs,$(sort $(test_http_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS) -lpthread

obj/bcdec_impl.o: bcdec_impl.c $(BCDEC)/bcdec.h
	@mkdir -p $(@D)
	$(CC) -O2 -w $(LRC_STD) -I$(BCDEC) -c -o $@ $<

obj/test_tls_duplex.o: test_tls_duplex.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) -D_GNU_SOURCE $(SANITIZE) -c -o $@ $<

test_tls_duplex: obj/test_tls_duplex.o $(call lrc_objs,$(sort $(test_http_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS) -lpthread

test_yaml: obj/test_yaml.o $(call lrc_objs,$(test_yaml_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# the overlays' packer is the core's own header, built here as C (gnu99,
# as stb_truetype is not C89)
test_font_LRC := \
	$(LRC)/gfx/font/rtt.c \
	$(test_deflate_LRC)

obj/test_font.o: test_font.c ../../Emu/RSX/Overlays/overlay_font_pack.h $(STB)/stb_image.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LRC_STD) -Wdeclaration-after-statement -isystem $(STB) $(SANITIZE) -c -o $@ $<

test_font: obj/test_font.o obj/stb_impl.o $(call lrc_objs,$(sort $(test_font_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# RPCN's messages: the code the core builds is made again from the .proto
# here, checked against the one in the tree, and read and written against
# what the protobuf library made of the fixtures
CXX     ?= c++
NP2     := ../../Emu/NP/generated
NP2_GEN := obj/np2/np2_structs.h obj/np2/np2_structs.cpp obj/np2/np2_dump.h

$(NP2_GEN): $(NP2)/np2_structs.proto $(NP2)/np2_structs_gen.py
	@mkdir -p obj/np2
	python3 $(NP2)/np2_structs_gen.py --out obj/np2 --dump obj/np2/np2_dump.h
	cmp obj/np2/np2_structs.h $(NP2)/np2_structs.h
	cmp obj/np2/np2_structs.cpp $(NP2)/np2_structs.cpp

obj/np2/%.o: obj/np2/%.cpp $(NP2_GEN) ../../Emu/NP/pb_wire.h
	$(CXX) -O2 -std=c++17 -Wall -Wextra -I../.. $(SANITIZE) -c -o $@ $<

obj/test_np2.o: test_np2.cpp $(NP2_GEN) ../../Emu/NP/pb_wire.h
	$(CXX) -O2 -std=c++17 -Wall -Wextra -Iobj/np2 -I../.. -I$(LRC)/include $(SANITIZE) -c -o $@ $<

test_np2: obj/test_np2.o obj/np2/np2_structs.o $(call lrc_objs,$(test_deflate_LRC))
	$(CXX) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

test_bcn: obj/test_bcn.o obj/bcdec_impl.o $(call lrc_objs,$(test_bcn_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# the RSX blit engine's scaled copy, on libretro-common's scaler; its
# allocations counted by wrapping malloc
test_rsx_scale_LRC := \
	$(LRC)/gfx/scaler/pixconv.c \
	$(LRC)/gfx/scaler/scaler.c \
	$(LRC)/gfx/scaler/scaler_filter.c \
	$(LRC)/gfx/scaler/scaler_int.c \
	$(LRC)/features/features_cpu.c

obj/rsx_scale.o: ../../Emu/RSX/Utils/rsx_scale.c ../../Emu/RSX/Utils/rsx_scale.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_rsx_scale.o: test_rsx_scale.c test_common.h ../../Emu/RSX/Utils/rsx_scale.h

test_rsx_scale: obj/test_rsx_scale.o obj/rsx_scale.o $(call lrc_objs,$(sort $(test_rsx_scale_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc $(LDLIBS)

# cellVpost's YUV to RGBA, on image_yuv_blit and the scaler
test_vpost_LRC := \
	$(LRC)/formats/image/image_yuv_blit.c \
	$(test_rsx_scale_LRC)

obj/cellVpost_conv.o: ../../Emu/Cell/Modules/cellVpost_conv.c ../../Emu/Cell/Modules/cellVpost_conv.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_vpost.o: test_vpost.c test_common.h ../../Emu/Cell/Modules/cellVpost_conv.h

test_vpost: obj/test_vpost.o obj/cellVpost_conv.o $(call lrc_objs,$(sort $(test_vpost_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc $(LDLIBS)

# cellAtracXdec's decode: ratrac3p, and the module's output conversion
test_atracx_LRC := $(LRC)/formats/atrac3p/ratrac3p.c

obj/cellAtracXdec_pcm.o: ../../Emu/Cell/Modules/cellAtracXdec_pcm.c ../../Emu/Cell/Modules/cellAtracXdec_pcm.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/formats/atrac3p/ratrac3p.o: $(LRC)/formats/atrac3p/ratrac3p_tables.h
obj/test_atracx.o: test_atracx.c test_common.h ../../Emu/Cell/Modules/cellAtracXdec_pcm.h

test_atracx: obj/test_atracx.o obj/cellAtracXdec_pcm.o $(call lrc_objs,$(test_atracx_LRC))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# cellVdec's decoders and conversions; rinflate for the gzipped references
test_vdec_LRC := \
	$(LRC)/formats/h264/rh264.c \
	$(LRC)/formats/mpeg1/rmpeg1_video.c \
	$(LRC)/formats/mpeg4/rmpeg4_video.c \
	$(LRC)/formats/image/image_yuv_blit.c \
	$(test_deflate_LRC)

obj/cellVdec_codec.o: ../../Emu/Cell/Modules/cellVdec_codec.c ../../Emu/Cell/Modules/cellVdec_codec.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_vdec.o: test_vdec.c test_common.h ../../Emu/Cell/Modules/cellVdec_codec.h

test_vdec: obj/test_vdec.o obj/cellVdec_codec.o $(call lrc_objs,$(sort $(test_vdec_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS) -lpthread

# cellSearch's listings and cellMusicDecode's playback: audio_transfer and
# its decoders, rtags, the containers, the sinc resampler, filestream
test_media_LRC := \
	$(LRC)/formats/audio_transfer.c \
	$(LRC)/formats/wav/rwav.c \
	$(LRC)/formats/flac/rflac.c \
	$(LRC)/formats/vorbis/rvorbis.c \
	$(LRC)/formats/opus/ropus.c \
	$(LRC)/formats/mp3/rmp3.c \
	$(LRC)/formats/aac/raac.c \
	$(LRC)/formats/mp4/rmp4.c \
	$(LRC)/formats/ac3/rac3_frame.c \
	$(LRC)/formats/ac3/rac3_decode.c \
	$(LRC)/formats/wma/rwma_standard.c \
	$(LRC)/formats/wma/rwma_pro.c \
	$(LRC)/formats/wma/rwma_stream.c \
	$(LRC)/formats/atrac3/ratrac3.c \
	$(LRC)/formats/atrac3p/ratrac3p.c \
	$(LRC)/formats/tags/rtags.c \
	$(LRC)/formats/webm/rwebm.c \
	$(LRC)/formats/mpeg1/rmpeg1_ps.c \
	$(LRC)/formats/container/rcontainer.c \
	$(LRC)/audio/resampler/drivers/sinc_resampler.c \
	$(LRC)/memmap/memalign.c \
	$(LRC)/streams/file_stream.c \
	$(LRC)/vfs/vfs_implementation.c \
	$(LRC)/file/file_path.c \
	$(LRC)/compat/compat_strcasestr.c \
	$(LRC)/compat/fopen_utf8.c \
	$(LRC)/encodings/encoding_utf.c \
	$(LRC)/string/stdstring.c \
	$(LRC)/string/rstrtod.c \
	$(LRC)/file/file_path_io.c \
	$(test_deflate_LRC)

obj/media_decode.o: ../../util/media_decode.c ../../util/media_decode.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_media.o: test_media.c test_common.h ../../util/media_decode.h

test_media: obj/test_media.o obj/media_decode.o $(call lrc_objs,$(sort $(test_media_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS) -lpthread

# cellRec's recordings: the encoders, the MP4 muxer, the scaler and the
# RGB to 4:2:0 conversion, and media_decode to read them back
test_record_LRC := \
	$(LRC)/formats/mpeg4/rmpeg4_encode.c \
	$(LRC)/formats/h264/rh264_encode.c \
	$(LRC)/formats/jpeg/rjpeg_encode.c \
	$(LRC)/formats/aac/raac_encode.c \
	$(LRC)/formats/mp4/rmp4_mux.c \
	$(LRC)/formats/image/image_yuv_blit.c \
	$(LRC)/audio/conversion/float_to_ulaw.c \
	$(LRC)/gfx/scaler/pixconv.c \
	$(LRC)/gfx/scaler/scaler.c \
	$(LRC)/gfx/scaler/scaler_filter.c \
	$(LRC)/gfx/scaler/scaler_int.c \
	$(test_media_LRC)

obj/media_encode.o: ../../util/media_encode.c ../../util/media_encode.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_record.o: test_record.c test_common.h ../../util/media_encode.h ../../util/media_decode.h

test_record: obj/test_record.o obj/media_encode.o obj/media_decode.o $(call lrc_objs,$(sort $(test_record_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS) -lpthread

# the per-game settings database's lookup, against every entry of the copy
# the core carries
test_config_database_LRC := \
	$(LRC)/formats/json/rjson.c \
	$(LRC)/string/rstrtod.c \
	$(LRC)/compat/compat_strl.c

obj/config_database.o: ../config_database.c ../config_database.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_config_database.o: test_config_database.c test_common.h ../config_database.h

test_config_database: obj/test_config_database.o obj/config_database.o $(call lrc_objs,$(sort $(test_config_database_LRC)))
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)
# VBlank Frequency "From List": the block of vblank_unlock.txt a game is in
obj/vblank_list.o: ../vblank_list.c ../vblank_list.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o $@ $<

obj/test_vblank_list.o: test_vblank_list.c test_common.h ../vblank_list.h

test_vblank_list: obj/test_vblank_list.o obj/vblank_list.o
	$(CC) $(SANITIZE) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# cellRec's lock-free queues (util/video_sink.h) on retro_spsc; again
# under ThreadSanitizer, which needs the ring built with it too
VIDEO_SINK_DEPS := test_video_sink.cpp ../../util/video_sink.h $(LRC)/queues/retro_spsc.c $(LRC)/include/retro_spsc.h $(LRC)/include/retro_atomic.h
VIDEO_SINK_FLAGS := -std=c++20 -O1 -g -Wall -Wextra -I../.. -I$(LRC)/include

test_video_sink: $(VIDEO_SINK_DEPS)
	$(CXX) $(VIDEO_SINK_FLAGS) $(SANITIZE) -o $@ test_video_sink.cpp -x c -std=gnu99 -DHAVE_THREADS $(LRC)/queues/retro_spsc.c $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c -x none -lpthread

test_video_sink_tsan: $(VIDEO_SINK_DEPS)
	$(CXX) $(VIDEO_SINK_FLAGS) -fsanitize=thread -o $@ test_video_sink.cpp -x c -std=gnu99 -DHAVE_THREADS $(LRC)/queues/retro_spsc.c $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c -x none -lpthread

# The libretro swapchain's image bookkeeping, also under ThreadSanitizer
VK_IMAGES_DEPS := test_vk_images.c test_common.h ../../Emu/RSX/VK/vkutils/libretro_images.h $(LRC)/include/retro_atomic.h

test_vk_images: $(VK_IMAGES_DEPS)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -o $@ test_vk_images.c $(LDFLAGS) $(LDLIBS) -lpthread

test_vk_images_tsan: $(VK_IMAGES_DEPS)
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -o $@ test_vk_images.c $(LDFLAGS) $(LDLIBS) -lpthread

# The software frames' triple buffer, also under ThreadSanitizer
SW_FRAMES_DEPS := test_sw_frames.c test_common.h ../libretro_sw_frames.c ../libretro_sw_frames.h $(LRC)/include/retro_atomic.h

test_sw_frames: $(SW_FRAMES_DEPS)
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -o $@ test_sw_frames.c ../libretro_sw_frames.c $(LDFLAGS) $(LDLIBS) -lpthread

test_sw_frames_tsan: $(SW_FRAMES_DEPS)
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -o $@ test_sw_frames.c ../libretro_sw_frames.c $(LDFLAGS) $(LDLIBS) -lpthread

# The OpenGL renderer's frames to the frontend, on Mesa's software OpenGL
# through surfaceless EGL (libegl-dev and Mesa's EGL and drivers)
test_gl_frames: test_gl_frames.c test_common.h ../libretro_gl_frames.c ../libretro_gl_frames.h $(LRC)/include/retro_atomic.h
	$(CC) $(CFLAGS) -std=gnu99 -I../../../3rdparty/GL $(SANITIZE) -o $@ test_gl_frames.c ../libretro_gl_frames.c $(LDFLAGS) $(LDLIBS) -lEGL -lpthread

test_gl_fallback: test_gl_fallback.c test_common.h ../libretro_gl_fallback.c ../libretro_gl_fallback.h ../libretro_gl_frames.h
	$(CC) $(CFLAGS) -std=gnu99 -I../../../3rdparty/GL $(SANITIZE) -o $@ test_gl_fallback.c ../libretro_gl_fallback.c $(LDFLAGS) $(LDLIBS) -lEGL -lpthread -ldl

test_pad_values: test_pad_values.c test_common.h ../libretro_pad_values.c ../libretro_pad_values.h
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -o $@ test_pad_values.c ../libretro_pad_values.c $(LDFLAGS) $(LDLIBS)

test_input_poll: test_input_poll.c test_common.h ../libretro_input.c ../libretro_input.h
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -o $@ test_input_poll.c ../libretro_input.c $(LDFLAGS) $(LDLIBS)

# The module and the test as C89; libretro-common's conversion as it is built
test_audio_handover: test_audio_handover.c test_common.h ../libretro_audio_out.c ../libretro_audio_out.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_audio_handover.o test_audio_handover.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/libretro_audio_out.o ../libretro_audio_out.c
	$(CC) $(CFLAGS) -std=gnu99 -w $(SANITIZE) -o $@ obj/test_audio_handover.o obj/libretro_audio_out.o $(LRC)/audio/conversion/float_to_s16.c $(LRC)/features/features_cpu.c $(LDFLAGS) $(LDLIBS)

# The add-only lock-free map: the module and the test as C89
LF_U64_MAP_DEPS := test_lf_u64_map.c test_common.h ../../../Utilities/lf_u64_map.c ../../../Utilities/lf_u64_map.h

test_lf_u64_map: $(LF_U64_MAP_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_lf_u64_map.o test_lf_u64_map.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/lf_u64_map.o ../../../Utilities/lf_u64_map.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_lf_u64_map.o obj/lf_u64_map.o $(LDFLAGS) $(LDLIBS) -lpthread

test_lf_u64_map_tsan: $(LF_U64_MAP_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lf_u64_map_tsan.o test_lf_u64_map.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lf_u64_map_tsan.o ../../../Utilities/lf_u64_map.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_lf_u64_map_tsan.o obj/lf_u64_map_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

LF_ITEM_POOL_DEPS := test_lf_item_pool.c test_common.h ../../../Utilities/lf_item_pool.c ../../../Utilities/lf_item_pool.h

test_lf_item_pool: $(LF_ITEM_POOL_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_lf_item_pool.o test_lf_item_pool.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -DLF_POOL_TEST_WINDOW=lf_pool_test_window -c -o obj/lf_item_pool.o ../../../Utilities/lf_item_pool.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_lf_item_pool.o obj/lf_item_pool.o $(LDFLAGS) $(LDLIBS) -lpthread

test_lf_item_pool_tsan: $(LF_ITEM_POOL_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lf_item_pool_tsan.o test_lf_item_pool.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DLF_POOL_TEST_WINDOW=lf_pool_test_window -c -o obj/lf_item_pool_tsan.o ../../../Utilities/lf_item_pool.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_lf_item_pool_tsan.o obj/lf_item_pool_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

CPU_SLOTS_DEPS := test_cpu_slots.c test_common.h ../../Emu/CPU/cpu_slots.c ../../Emu/CPU/cpu_slots.h

test_cpu_slots: $(CPU_SLOTS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_cpu_slots.o test_cpu_slots.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/cpu_slots.o ../../Emu/CPU/cpu_slots.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_cpu_slots.o obj/cpu_slots.o $(LDFLAGS) $(LDLIBS) -lpthread

test_cpu_slots_tsan: $(CPU_SLOTS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_cpu_slots_tsan.o test_cpu_slots.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/cpu_slots_tsan.o ../../Emu/CPU/cpu_slots.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_cpu_slots_tsan.o obj/cpu_slots_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

# Calls run on one owner thread: the module and the test as C89
OWNER_CALLS_DEPS := test_owner_calls.c test_common.h ../../Emu/CPU/owner_calls.c ../../Emu/CPU/owner_calls.h
OWNER_CALLS_LRC  := $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c

test_owner_calls: $(OWNER_CALLS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_owner_calls.o test_owner_calls.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/owner_calls.o ../../Emu/CPU/owner_calls.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ obj/test_owner_calls.o obj/owner_calls.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_owner_calls_tsan: $(OWNER_CALLS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_owner_calls_tsan.o test_owner_calls.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/owner_calls_tsan.o ../../Emu/CPU/owner_calls.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_owner_calls_tsan.o obj/owner_calls_tsan.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The HLE modules' thread: the module and the test as C89
HLE_SERVICE_DEPS := test_hle_service.c test_common.h ../../Emu/Cell/Modules/hle_service.c ../../Emu/Cell/Modules/hle_service.h $(OWNER_CALLS_DEPS)

test_hle_service: $(HLE_SERVICE_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_hle_service.o test_hle_service.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/hle_service.o ../../Emu/Cell/Modules/hle_service.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/owner_calls_hle.o ../../Emu/CPU/owner_calls.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ obj/test_hle_service.o obj/hle_service.o obj/owner_calls_hle.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_hle_service_tsan: $(HLE_SERVICE_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_hle_service_tsan.o test_hle_service.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/hle_service_tsan.o ../../Emu/Cell/Modules/hle_service.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/owner_calls_hle_tsan.o ../../Emu/CPU/owner_calls.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_hle_service_tsan.o obj/hle_service_tsan.o obj/owner_calls_hle_tsan.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

JIT_HOST_SYMBOLS_DEPS := test_jit_host_symbols.c test_common.h ../../Emu/CPU/jit_host_symbols.c ../../Emu/CPU/jit_host_symbols.h

test_jit_host_symbols: $(JIT_HOST_SYMBOLS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_jit_host_symbols.o test_jit_host_symbols.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/jit_host_symbols.o ../../Emu/CPU/jit_host_symbols.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_jit_host_symbols.o obj/jit_host_symbols.o $(LDFLAGS) $(LDLIBS) -lpthread

test_jit_host_symbols_tsan: $(JIT_HOST_SYMBOLS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_jit_host_symbols_tsan.o test_jit_host_symbols.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/jit_host_symbols_tsan.o ../../Emu/CPU/jit_host_symbols.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_jit_host_symbols_tsan.o obj/jit_host_symbols_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

LF_SIG_TABLE_DEPS := test_lf_sig_table.c test_common.h ../../../Utilities/lf_sig_table.c ../../../Utilities/lf_sig_table.h

test_lf_sig_table: $(LF_SIG_TABLE_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_lf_sig_table.o test_lf_sig_table.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/lf_sig_table.o ../../../Utilities/lf_sig_table.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_lf_sig_table.o obj/lf_sig_table.o $(LDFLAGS) $(LDLIBS) -lpthread

test_lf_sig_table_tsan: $(LF_SIG_TABLE_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lf_sig_table_tsan.o test_lf_sig_table.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lf_sig_table_tsan.o ../../../Utilities/lf_sig_table.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_lf_sig_table_tsan.o obj/lf_sig_table_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

LF_HAZARD_DEPS := test_lf_hazard.c test_common.h ../../../Utilities/lf_hazard.c ../../../Utilities/lf_hazard.h

test_lf_hazard: $(LF_HAZARD_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_lf_hazard.o test_lf_hazard.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/lf_hazard.o ../../../Utilities/lf_hazard.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ obj/test_lf_hazard.o obj/lf_hazard.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_lf_hazard_tsan: $(LF_HAZARD_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lf_hazard_tsan.o test_lf_hazard.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lf_hazard_tsan.o ../../../Utilities/lf_hazard.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lf_hazard_tsan.o obj/lf_hazard_tsan.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

DMA_VIEWS_DEPS := test_dma_views.c test_common.h ../../Emu/RSX/Common/dma_views.c ../../Emu/RSX/Common/dma_views.h

test_dma_views: $(DMA_VIEWS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_dma_views.o test_dma_views.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/dma_views.o ../../Emu/RSX/Common/dma_views.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_dma_views.o obj/dma_views.o $(LDFLAGS) $(LDLIBS) -lpthread

test_dma_views_tsan: $(DMA_VIEWS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_dma_views_tsan.o test_dma_views.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/dma_views_tsan.o ../../Emu/RSX/Common/dma_views.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_dma_views_tsan.o obj/dma_views_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

# Directories of the emulator with no atomic_t left: it is being replaced
# by direct retro_atomic calls, a directory at a time, and none comes back
# Files of LLVM's (3rdparty/llvm) with no lock left: its locks are being
# taken out for retro_atomic, the ones recompiler threads share first
# The sources of LLVM's libraries that llvm.mk has built (after the lists
# for its build tools, which are not part of the core)
LLVM_BUILT := ${shell awk '/^LLVM_LIB_COMMON_SRCS/{on=1} on && /LLVM_SRC\)\/lib\// && / \\$$/{sub(/^.*LLVM_SRC\)\//,""); sub(/ \\$$/,""); print}' ../make/llvm/llvm.mk}
LLVM_LOCK_WORDS := std::mutex|sys::Mutex|SmartMutex|recursive_mutex|RWMutex|lock_guard|unique_lock|ScopedLock|ScopedReader|ScopedWriter|condition_variable|shared_mutex|scoped_lock|std::call_once\(|std::once_flag [A-Za-z_:]+;|std::promise|std::future|pthread_mutex|<mutex>|CRITICAL_SECTION
# Beside every library source llvm.mk builds: the headers and the parts
# included into sources that were changed for it
LLVM_LOCK_FREE := include/llvm/PassRegistry.h include/llvm/Support/ManagedStatic.h include/llvm/Support/Threading.h \
	lib/Support/Unix/DynamicLibrary.inc lib/Support/Windows/DynamicLibrary.inc \
	include/llvm/ExecutionEngine/ExecutionEngine.h lib/ExecutionEngine/MCJIT/MCJIT.h lib/ExecutionEngine/RuntimeDyld/RuntimeDyldImpl.h \
	include/llvm/Support/Timer.h include/llvm/CGData/CodeGenData.h include/llvm/CGData/StableFunctionMap.h lib/Support/Unix/Process.inc

ATOMIC_T_FREE := ../../Crypto ../../Loader ../../Emu/Audio ../../Emu/Io ../../Input ../../Emu/CPU

GUEST_FAULTS_DEPS := test_guest_faults.c ../../Emu/RSX/Common/guest_faults.c ../../Emu/RSX/Common/guest_faults.h
GUEST_FAULTS_SRC  := ../../Emu/RSX/Common/guest_faults.c $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c

test_guest_faults: $(GUEST_FAULTS_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ test_guest_faults.c $(GUEST_FAULTS_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_guest_faults_tsan: $(GUEST_FAULTS_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ test_guest_faults.c $(GUEST_FAULTS_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

RSX_WAKE_DEPS := test_rsx_wake.c test_common.h ../../Emu/RSX/Common/rsx_wake.c ../../Emu/RSX/Common/rsx_wake.h
RSX_WAKE_SRC  := ../../Emu/RSX/Common/rsx_wake.c $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c

test_rsx_wake: $(RSX_WAKE_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ test_rsx_wake.c $(RSX_WAKE_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_rsx_wake_tsan: $(RSX_WAKE_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ test_rsx_wake.c $(RSX_WAKE_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

RSX_HOLD_DEPS := test_rsx_hold.c test_common.h ../../Emu/RSX/Common/rsx_hold.c ../../Emu/RSX/Common/rsx_hold.h ../../Emu/RSX/Common/rsx_wake.c ../../Emu/RSX/Common/rsx_wake.h
RSX_HOLD_SRC  := ../../Emu/RSX/Common/rsx_hold.c $(RSX_WAKE_SRC)

test_rsx_hold: $(RSX_HOLD_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ test_rsx_hold.c $(RSX_HOLD_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_rsx_hold_tsan: $(RSX_HOLD_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ test_rsx_hold.c $(RSX_HOLD_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_rsx_timestamp: test_rsx_timestamp.c test_common.h ../../Emu/RSX/Common/rsx_timestamp.c ../../Emu/RSX/Common/rsx_timestamp.h
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -o $@ test_rsx_timestamp.c ../../Emu/RSX/Common/rsx_timestamp.c $(LDFLAGS) $(LDLIBS)

NET_WAKE_SRC := ../../Emu/Cell/lv2/sys_net/net_wake.c $(LRC)/net/net_socket.c $(LRC)/net/net_compat.c $(LRC)/features/features_cpu.c $(LRC)/rthreads/rthreads.c $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c

# BSD sockets want the POSIX names, so gnu89 here: C89 still, as -Wdeclaration-after-statement keeps it
test_net_wake: test_net_wake.c test_common.h ../../Emu/Cell/lv2/sys_net/net_wake.c ../../Emu/Cell/lv2/sys_net/net_wake.h
	$(CC) $(CFLAGS) -std=gnu89 -Wdeclaration-after-statement -DHAVE_THREADS $(SANITIZE) -o $@ test_net_wake.c $(NET_WAKE_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

OSK_TEXT_DEPS := test_osk_text.c test_common.h ../../Emu/Cell/Modules/osk_text.c ../../Emu/Cell/Modules/osk_text.h
OSK_TEXT_SRC  := ../../Emu/Cell/Modules/osk_text.c $(LRC)/rthreads/retro_waitaddr.c $(LRC)/rthreads/retro_eventcount.c $(LRC)/rthreads/rthreads.c

test_osk_text: $(OSK_TEXT_DEPS)
	$(CC) $(CFLAGS) $(TEST_STD) -DHAVE_THREADS $(SANITIZE) -o $@ test_osk_text.c $(OSK_TEXT_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_osk_text_tsan: $(OSK_TEXT_DEPS)
	$(CC) $(CFLAGS) $(TEST_STD) -DHAVE_THREADS -fsanitize=thread -o $@ test_osk_text.c $(OSK_TEXT_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

RPCN_REPLY_SLOTS_DEPS := test_rpcn_reply_slots.c test_common.h ../../Emu/NP/rpcn_reply_slots.c ../../Emu/NP/rpcn_reply_slots.h
RPCN_REPLY_SLOTS_SRC  := ../../Emu/NP/rpcn_reply_slots.c $(LRC)/rthreads/rthreads.c

test_rpcn_reply_slots: $(RPCN_REPLY_SLOTS_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS $(SANITIZE) -o $@ test_rpcn_reply_slots.c $(RPCN_REPLY_SLOTS_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

test_rpcn_reply_slots_tsan: $(RPCN_REPLY_SLOTS_DEPS)
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ test_rpcn_reply_slots.c $(RPCN_REPLY_SLOTS_SRC) $(LDFLAGS) $(LDLIBS) -lpthread

VK_INTERP_UPLOADS_DEPS := test_vk_interp_uploads.c test_common.h ../../Emu/RSX/VK/vk_interpreter_uploads.c ../../Emu/RSX/VK/vk_interpreter_uploads.h

test_vk_interp_uploads: $(VK_INTERP_UPLOADS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_vk_interp_uploads.o test_vk_interp_uploads.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/vk_interpreter_uploads.o ../../Emu/RSX/VK/vk_interpreter_uploads.c
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ obj/test_vk_interp_uploads.o obj/vk_interpreter_uploads.o $(LDFLAGS) $(LDLIBS)

# An lv2 mutex's owner and waiters in one 128-bit word, under ThreadSanitizer
test_lv2_ownq: test_lv2_ownq.c test_common.h ../../Emu/Cell/lv2/lv2_ownq.c ../../Emu/Cell/lv2/lv2_ownq.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_ownq.o test_lv2_ownq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_ownq.o ../../Emu/Cell/lv2/lv2_ownq.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_ownq.o obj/lv2_ownq.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# An lv2 event queue's events and waiters with no lock, under ThreadSanitizer
LV2_EVQ_DEPS := test_lv2_evq.c test_common.h ../../Emu/Cell/lv2/lv2_evq.c ../../Emu/Cell/lv2/lv2_evq.h ../../Emu/Cell/lv2/lv2_event_ring.c ../../Emu/Cell/lv2/lv2_event_ring.h

test_lv2_evq: $(LV2_EVQ_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_evq.o test_lv2_evq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DLV2_EVQ_TEST_WINDOW=lv2_evq_test_window -c -o obj/lv2_evq.o ../../Emu/Cell/lv2/lv2_evq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_evq_ring.o ../../Emu/Cell/lv2/lv2_event_ring.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_evq.o obj/lv2_evq.o obj/lv2_evq_ring.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The RPCN client's wake-ups, under ThreadSanitizer
test_rpcn_signal: test_rpcn_signal.c test_common.h ../../Emu/NP/rpcn_signal.c ../../Emu/NP/rpcn_signal.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_rpcn_signal.o test_rpcn_signal.c
	$(CC) $(CFLAGS) $(TEST_STD) -DHAVE_THREADS -fsanitize=thread -c -o obj/rpcn_signal.o ../../Emu/NP/rpcn_signal.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_rpcn_signal.o obj/rpcn_signal.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The cores the JIT's compiles may use at once, under ThreadSanitizer
test_jit_cores: test_jit_cores.c test_common.h ../../Emu/Cell/jit_cores.c ../../Emu/Cell/jit_cores.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_jit_cores.o test_jit_cores.c
	$(CC) $(CFLAGS) $(TEST_STD) -DHAVE_THREADS -fsanitize=thread -c -o obj/jit_cores.o ../../Emu/Cell/jit_cores.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_jit_cores.o obj/jit_cores.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The lv2 scheduler's run queue with no lock, under ThreadSanitizer
test_lv2_sched: test_lv2_sched.c test_common.h ../../Emu/Cell/lv2/lv2_sched.c ../../Emu/Cell/lv2/lv2_sched.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_sched.o test_lv2_sched.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DLV2_SCHED_TEST_WINDOW=lv2_sched_test_window -c -o obj/lv2_sched.o ../../Emu/Cell/lv2/lv2_sched.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_sched.o obj/lv2_sched.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The IO pages cellGcmSys maps and unmaps, marked with no lock, under ThreadSanitizer
test_gcm_io_pages: test_gcm_io_pages.c test_common.h ../../Emu/Cell/Modules/gcm_io_pages.c ../../Emu/Cell/Modules/gcm_io_pages.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_gcm_io_pages.o test_gcm_io_pages.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DGCM_IO_TEST_WINDOW=gcm_io_test_window -c -o obj/gcm_io_pages.o ../../Emu/Cell/Modules/gcm_io_pages.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_gcm_io_pages.o obj/gcm_io_pages.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# An lv2 lightweight mutex's signal and sleepers in one 128-bit word, under ThreadSanitizer
test_lv2_lwq: test_lv2_lwq.c test_common.h ../../Emu/Cell/lv2/lv2_lwq.c ../../Emu/Cell/lv2/lv2_lwq.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_lwq.o test_lv2_lwq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_lwq.o ../../Emu/Cell/lv2/lv2_lwq.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_lwq.o obj/lv2_lwq.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# An lv2 rwlock's owners and waiters in one 128-bit word, under ThreadSanitizer
test_lv2_rwq: test_lv2_rwq.c test_common.h ../../Emu/Cell/lv2/lv2_rwq.c ../../Emu/Cell/lv2/lv2_rwq.h ../../Emu/Cell/lv2/lv2_waitset.c ../../Emu/Cell/lv2/lv2_waitset.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_rwq.o test_lv2_rwq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DLV2_RWQ_TEST_WINDOW=lv2_rwq_test_window -c -o obj/lv2_rwq.o ../../Emu/Cell/lv2/lv2_rwq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_rwq_ws.o ../../Emu/Cell/lv2/lv2_waitset.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_rwq.o obj/lv2_rwq.o obj/lv2_rwq_ws.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# An lv2 event flag's bits and waiters with no lock, under ThreadSanitizer
LV2_FLAGQ_DEPS := test_lv2_flagq.c test_common.h ../../Emu/Cell/lv2/lv2_flagq.c ../../Emu/Cell/lv2/lv2_flagq.h ../../Emu/Cell/lv2/lv2_waitset.c ../../Emu/Cell/lv2/lv2_waitset.h

test_lv2_flagq: $(LV2_FLAGQ_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_flagq.o test_lv2_flagq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DLV2_FLAGQ_TEST_WINDOW=lv2_flagq_test_window -c -o obj/lv2_flagq.o ../../Emu/Cell/lv2/lv2_flagq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_flagq_ws.o ../../Emu/Cell/lv2/lv2_waitset.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_flagq.o obj/lv2_flagq.o obj/lv2_flagq_ws.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# An lv2 semaphore's count or waiters in one 128-bit word, under ThreadSanitizer
test_lv2_countq: test_lv2_countq.c test_common.h ../../Emu/Cell/lv2/lv2_countq.c ../../Emu/Cell/lv2/lv2_countq.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_countq.o test_lv2_countq.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_countq.o ../../Emu/Cell/lv2/lv2_countq.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_countq.o obj/lv2_countq.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The lv2 sync objects' lock-free pieces: the set of waiting threads and an event queue's ring, under ThreadSanitizer
LV2_WAITSET_DEPS := test_lv2_waitset.c test_common.h ../../Emu/Cell/lv2/lv2_waitset.c ../../Emu/Cell/lv2/lv2_waitset.h ../../Emu/Cell/lv2/lv2_event_ring.c ../../Emu/Cell/lv2/lv2_event_ring.h

test_lv2_waitset: $(LV2_WAITSET_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_lv2_waitset.o test_lv2_waitset.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_waitset.o ../../Emu/Cell/lv2/lv2_waitset.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/lv2_event_ring.o ../../Emu/Cell/lv2/lv2_event_ring.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lv2_waitset.o obj/lv2_waitset.o obj/lv2_event_ring.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# An lv2 timer's state: one 128-bit word changed with compare-and-swap, under ThreadSanitizer
test_timer_ctrl: test_timer_ctrl.c test_common.h ../../Emu/Cell/lv2/sys_timer_ctrl.c ../../Emu/Cell/lv2/sys_timer_ctrl.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_timer_ctrl.o test_timer_ctrl.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/sys_timer_ctrl.o ../../Emu/Cell/lv2/sys_timer_ctrl.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_timer_ctrl.o obj/sys_timer_ctrl.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# Changes posted from any thread, applied by one at a time (Utilities/lf_post.h)
test_lf_post: test_lf_post.c test_common.h ../../../Utilities/lf_post.h
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DLF_POST_TEST_WINDOW=sched_yield -include sched.h -c -o obj/test_lf_post.o test_lf_post.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_lf_post.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The NP libraries' memory pool, lock-free: alone and under ThreadSanitizer
NP_POOL_DEPS := test_np_pool.c test_common.h ../../Emu/NP/np_pool.c ../../Emu/NP/np_pool.h

test_np_pool: $(NP_POOL_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_np_pool.o test_np_pool.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/np_pool.o ../../Emu/NP/np_pool.c
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ obj/test_np_pool.o obj/np_pool.o $(LDFLAGS) $(LDLIBS)

test_np_pool_tsan: $(NP_POOL_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DNP_POOL_TEST_THREADS -c -o obj/test_np_pool_tsan.o test_np_pool.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/np_pool_tsan.o ../../Emu/NP/np_pool.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_np_pool_tsan.o obj/np_pool_tsan.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

VK_HEAP_DEPS := test_vk_heap.c test_common.h ../../Emu/RSX/VK/vkutils/vk_heap.c ../../Emu/RSX/VK/vkutils/vk_heap.h

test_vk_heap: $(VK_HEAP_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/test_vk_heap.o test_vk_heap.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/vk_heap.o ../../Emu/RSX/VK/vkutils/vk_heap.c
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ obj/test_vk_heap.o obj/vk_heap.o $(LDFLAGS) $(LDLIBS)

# The same heap shared by threads, none waiting for another, under ThreadSanitizer
test_vk_heap_tsan: $(VK_HEAP_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -DVK_HEAP_TEST_THREADS -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/test_vk_heap_tsan.o test_vk_heap.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/vk_heap_tsan.o ../../Emu/RSX/VK/vkutils/vk_heap.c
	$(CC) $(CFLAGS) -std=gnu99 -DHAVE_THREADS -fsanitize=thread -o $@ obj/test_vk_heap_tsan.o obj/vk_heap_tsan.o $(OWNER_CALLS_LRC) $(LDFLAGS) $(LDLIBS) -lpthread

# The core's Vulkan loader, through libretro-common's dylib
VK_LOADER_DEPS := test_vk_loader.c test_common.h ../../Emu/RSX/VK/vk_loader.c ../../Emu/RSX/VK/vk_loader.h

test_vk_loader: $(VK_LOADER_DEPS) obj/dynamic/dylib.o
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -isystem ../../../3rdparty/Vulkan-Headers/include -I../../Emu/RSX/VK -c -o obj/test_vk_loader.o test_vk_loader.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -DHAVE_DYLIB -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/vk_loader.o ../../Emu/RSX/VK/vk_loader.c
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ obj/test_vk_loader.o obj/vk_loader.o obj/dynamic/dylib.o $(LDFLAGS) $(LDLIBS) -ldl

VK_INTERP_DYNAMIC_DEPS := test_vk_interp_dynamic.c test_common.h ../../Emu/RSX/VK/vk_interpreter_dynamic.c ../../Emu/RSX/VK/vk_interpreter_dynamic.h

test_vk_interp_dynamic: $(VK_INTERP_DYNAMIC_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/test_vk_interp_dynamic.o test_vk_interp_dynamic.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -isystem ../../../3rdparty/Vulkan-Headers/include -c -o obj/vk_interpreter_dynamic.o ../../Emu/RSX/VK/vk_interpreter_dynamic.c
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ obj/test_vk_interp_dynamic.o obj/vk_interpreter_dynamic.o $(LDFLAGS) $(LDLIBS)

GUEST_PAGE_LOCKS_DEPS := test_guest_page_locks.c test_common.h ../../Emu/RSX/Common/guest_page_locks.c ../../Emu/RSX/Common/guest_page_locks.h

test_guest_page_locks: $(GUEST_PAGE_LOCKS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_guest_page_locks.o test_guest_page_locks.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/guest_page_locks.o ../../Emu/RSX/Common/guest_page_locks.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_guest_page_locks.o obj/guest_page_locks.o $(LDFLAGS) $(LDLIBS) -lpthread

test_guest_page_locks_tsan: $(GUEST_PAGE_LOCKS_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_guest_page_locks_tsan.o test_guest_page_locks.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/guest_page_locks_tsan.o ../../Emu/RSX/Common/guest_page_locks.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_guest_page_locks_tsan.o obj/guest_page_locks_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

# sys_ss's EEPROM table: the module and the test as C89
EEPROM_TABLE_DEPS := test_eeprom_table.c test_common.h ../../Emu/Cell/lv2/eeprom_table.c ../../Emu/Cell/lv2/eeprom_table.h

test_eeprom_table: $(EEPROM_TABLE_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_eeprom_table.o test_eeprom_table.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/eeprom_table.o ../../Emu/Cell/lv2/eeprom_table.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_eeprom_table.o obj/eeprom_table.o $(LDFLAGS) $(LDLIBS) -lpthread

test_eeprom_table_tsan: $(EEPROM_TABLE_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_eeprom_table_tsan.o test_eeprom_table.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/eeprom_table_tsan.o ../../Emu/Cell/lv2/eeprom_table.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_eeprom_table_tsan.o obj/eeprom_table_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

# The object cache's pack file: the module and the test as C89
OBJECT_PACK_DEPS := test_object_pack.c test_common.h ../../../Utilities/object_pack.c ../../../Utilities/object_pack.h

test_object_pack: $(OBJECT_PACK_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_object_pack.o test_object_pack.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/object_pack.o ../../../Utilities/object_pack.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_object_pack.o obj/object_pack.o $(LDFLAGS) $(LDLIBS) -lpthread

test_object_pack_tsan: $(OBJECT_PACK_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_object_pack_tsan.o test_object_pack.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/object_pack_tsan.o ../../../Utilities/object_pack.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_object_pack_tsan.o obj/object_pack_tsan.o $(LDFLAGS) $(LDLIBS) -lpthread

# The module and the test as C89; libretro-common's ring as it is built
AUDIO_RING_DEPS := test_audio_ring.c test_common.h ../libretro_audio_ring.c ../libretro_audio_ring.h $(LRC)/queues/retro_spsc.c $(LRC)/include/retro_spsc.h

test_audio_ring: $(AUDIO_RING_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/test_audio_ring.o test_audio_ring.c
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -c -o obj/libretro_audio_ring.o ../libretro_audio_ring.c
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -o $@ obj/test_audio_ring.o obj/libretro_audio_ring.o $(LRC)/queues/retro_spsc.c $(LDFLAGS) $(LDLIBS) -lpthread

test_audio_ring_tsan: $(AUDIO_RING_DEPS)
	@mkdir -p obj
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/test_audio_ring_tsan.o test_audio_ring.c
	$(CC) $(CFLAGS) $(TEST_STD) -fsanitize=thread -c -o obj/libretro_audio_ring_tsan.o ../libretro_audio_ring.c
	$(CC) $(CFLAGS) -std=gnu99 -fsanitize=thread -o $@ obj/test_audio_ring_tsan.o obj/libretro_audio_ring_tsan.o $(LRC)/queues/retro_spsc.c $(LDFLAGS) $(LDLIBS) -lpthread

test_audio_mix: test_audio_mix.c test_common.h ../../Emu/Cell/Modules/cellAudio_mix.c ../../Emu/Cell/Modules/cellAudio_mix.h
	$(CC) $(CFLAGS) $(TEST_STD) $(SANITIZE) -o $@ test_audio_mix.c ../../Emu/Cell/Modules/cellAudio_mix.c $(LDFLAGS) $(LDLIBS)

# The core's own code, searched for host clocks and sleeps
# std::lock_guard left in the core's own code (falls with each patch)
# Files whose state is the HLE thread's alone (Emu/Cell/Modules/hle_service.h)
HLE_OWNED := ../../Emu/Cell/Modules/cellImeJp.cpp ../../Emu/Cell/Modules/cellImeJp.h ../../Emu/Cell/Modules/cellHttp.cpp ../../Emu/Cell/Modules/cellHttp.h ../../Emu/Cell/Modules/cellSubDisplay.cpp ../../Emu/Cell/Modules/cellKb.cpp ../../Emu/Cell/Modules/cellScreenshot.cpp ../../Emu/Cell/Modules/cellScreenshot.h ../../Emu/Cell/Modules/cellPhotoImport.cpp ../../Emu/Cell/Modules/cellSearch.cpp ../../Emu/Cell/Modules/cellVoice.cpp ../../Emu/Cell/Modules/cellVoice.h

LOCK_GUARD_MAX := 3

CLOCK_FILES := $(filter-out ../libretro.h,$(wildcard ../*.c ../*.cpp ../*.h)) ../../Emu/RSX/VK/VKLibretro.cpp ../../Emu/RSX/VK/VKLibretro.h

# Each test also writes what the new code produces, for the reference
# tools to read back
check: all
	@# The host's TableGen tools link libretro-common's objects on their own,
	@# so one of them calling a function from a file not on the list only
	@# shows on a host where it is not a macro (retro_sleep_us on Windows):
	@# linked whole for Windows with MinGW, every object taken in
	@if ! command -v x86_64-w64-mingw32-gcc >/dev/null; then echo "host libretro-common: x86_64-w64-mingw32-gcc missing (gcc-mingw-w64-x86-64)"; exit 1; fi
	@rm -rf host_lrc_win && mkdir -p host_lrc_win && \
	for f in $$($(MAKE) -s --no-print-directory -C ../../.. llvm-host-lrc-srcs); do \
	   x86_64-w64-mingw32-gcc -c -O2 -w -I$(LRC)/include -o host_lrc_win/$$(echo $${f#./} | tr / _).o ../../../$$f || exit 1; done && \
	printf 'int main(void) { return 0; }\n' > host_lrc_win/main.c && \
	if x86_64-w64-mingw32-gcc -o host_lrc_win/link.exe host_lrc_win/main.c host_lrc_win/*.o 2> host_lrc_win/link.log; then echo "host libretro-common: links whole for Windows"; \
	else cat host_lrc_win/link.log; echo "host libretro-common: the TableGen tools' libretro-common objects do not link for Windows - add the file that defines the symbol to LLVM_HOST_LRC_SRCS"; exit 1; fi
	./test_zstd fixtures zstd_out.zst
	if command -v zstd >/dev/null; then zstd -q -t zstd_out.zst; fi
	./test_deflate fixtures deflate_out.gz
	gzip -t deflate_out.gz
	./test_rxml
	./test_png_rows
	./test_images
	./test_jpg_rows
	./test_http
	./test_bcn
	./tls_duplex_test.sh ./test_tls_duplex
	./test_yaml fixtures
	./test_font fixtures
	./test_np2 fixtures
	./test_rsx_scale fixtures
	./test_vpost fixtures
	./test_atracx fixtures
	./test_vdec fixtures
	./test_media fixtures
	./rec_check.sh rec_out
	./test_config_database ../config_database.dat
	./test_vblank_list ../vblank_unlock.txt
	./test_video_sink
	./test_video_sink_tsan
	./test_vk_images
	./test_vk_images_tsan
	./test_sw_frames
	./test_sw_frames_tsan
	./test_gl_frames
	./test_gl_fallback
	./test_pad_values
	./test_input_poll
	./test_audio_handover
	./test_audio_ring
	./test_audio_ring_tsan
	./test_lf_u64_map
	./test_lf_u64_map_tsan
	./test_lf_item_pool
	./test_lf_item_pool_tsan
	./test_lf_sig_table
	./test_lf_sig_table_tsan
	./test_jit_host_symbols
	./test_jit_host_symbols_tsan
	./test_cpu_slots
	./test_cpu_slots_tsan
	./test_owner_calls
	./test_owner_calls_tsan
	./test_hle_service
	./test_hle_service_tsan
	./test_lf_hazard
	./test_lf_hazard_tsan
	./test_dma_views
	./test_dma_views_tsan
	./test_guest_page_locks
	./test_guest_page_locks_tsan
	./test_guest_faults
	./test_guest_faults_tsan
	./test_vk_interp_uploads
	./test_vk_interp_dynamic
	./test_crypto_hashes
	./test_vk_heap
	./test_vk_heap_tsan
	./test_np_pool
	./test_np_pool_tsan
	./test_lf_post
	./test_timer_ctrl
	./test_lv2_waitset
	./test_lv2_countq
	./test_lv2_ownq
	./test_lv2_flagq
	./test_lv2_rwq
	./test_lv2_lwq
	./test_gcm_io_pages
	./test_lv2_evq
	./test_lv2_sched
	./test_jit_cores
	./test_rpcn_signal
	./test_rsx_wake
	./test_rsx_wake_tsan
	./test_rsx_hold
	./test_rsx_hold_tsan
	./test_rsx_timestamp
	./test_rpcn_reply_slots
	./test_rpcn_reply_slots_tsan
	./test_osk_text
	./test_osk_text_tsan
	./test_net_wake
	./test_vk_loader
	./test_eeprom_table
	./test_eeprom_table_tsan
	./test_object_pack
	./test_object_pack_tsan
	./test_audio_mix
	./no_host_clock.sh $(CLOCK_FILES)
	@if grep -rnE "timerfd_|CreateWaitableTimer|SetWaitableTimer|kqueue[ ]*\(|kevent[ ]*\(|epoll_wait|eventfd[ ]*\(" --include=*.cpp --include=*.h --include=*.hpp --include=*.c ../../Emu ../../Input ../../Loader ../../Crypto ../../util ../../../Utilities | grep -v "^../../../Utilities/Thread.cpp:"; then echo "a timer or event poll of the system's in the core: it waits on the guest's clock and on retro_waitaddr (rsxaudio_periodic_tmr)"; exit 1; else echo "no timer of the system's: only thread_ctrl::wait_for's, in Utilities/Thread.cpp, is left"; fi
	@# The game's watchdog has no thread and no clock of the host's: retro_run
	@# looks at it on the guest's clock (lv2_watchdog_tick)
	@if grep -nE "named_thread|thread_ctrl::wait_for|get_system_time[ ]*\(" ../../Emu/Cell/lv2/sys_game.cpp; then echo "sys_game: the watchdog is a thread on the host's clock again - retro_run looks at it on the guest's (lv2_watchdog_tick)"; exit 1; else echo "sys_game: the watchdog goes by the guest's clock, no thread"; fi
	@# The P2P streams' retransmits: no lock and no clock of the host's
	@if awk '/^class tcp_timeout_monitor/,/^u16 u2s_tcp_checksum/' ../../Emu/Cell/lv2/sys_net/lv2_socket_p2ps.cpp | grep -nE "steady_clock|shared_mutex|std::mutex|lock_guard|atomic_wait_timeout|wakey|get_system_time[ ]*\("; then echo "p2ps: the TCP timeout monitor locks or times on the host's clock again - it takes what changed on an mpsc_stack and waits on the guest's clock"; exit 1; else echo "p2ps: the TCP timeout monitor has no lock and goes by the guest's clock"; fi
	@# std::lock_guard is going from the core's own code: the count may only
	@# fall. Each patch that takes some out lowers LOCK_GUARD_MAX with it
	@n=$$(grep -rho "std::lock_guard" --include=*.cpp --include=*.h --include=*.hpp --include=*.c ../../Emu ../../Input ../../Loader ../../Crypto ../../util ../*.cpp ../*.h ../../../Utilities | wc -l); \
	if [ $$n -gt $(LOCK_GUARD_MAX) ]; then echo "std::lock_guard: $$n in the core, more than the $(LOCK_GUARD_MAX) left - the locks are going, not coming (AGENTS.md)"; exit 1; \
	elif [ $$n -lt $(LOCK_GUARD_MAX) ]; then echo "std::lock_guard: $$n in the core, fewer than LOCK_GUARD_MAX ($(LOCK_GUARD_MAX)): lower it to $$n"; exit 1; \
	else echo "std::lock_guard: $$n left in the core"; fi
	@# The IDs' waits - for threads entered on an ID to leave, for a withdrawal's answer, for a pinned object's look to end - are waits on a word, woken by whoever changes it: no spin
	@if grep -nE "yield\(\)|busy_wait|sleep_for|wait_for\(" ../../Emu/IdManager.h ../../../Utilities/lf_hazard.c; then echo "idm: a spin or a sleep is back in the IDs' waits - they wait on the word (retro_waitaddr)"; exit 1; else echo "idm: its waits wait on the word, no spin"; fi
	@# Save data takes no lock: one call at a time by a word claimed with compare-and-swap, a second told it is busy - never a semaphore, never a wait
	@if grep -nE "semaphore|sema\.h|std::mutex|shared_mutex|lock_guard|unique_lock[^ ]* lock\(g_fxo->get<savedata_manager>" ../../Emu/Cell/Modules/cellSaveData.cpp; then echo "cellSaveData: a lock or a semaphore is back - one call at a time is savedata_claim"; exit 1; else echo "cellSaveData: no lock, no semaphore"; fi
	@# The JIT's compile cores take no semaphore: a count taken with compare-and-swap, a compile with none free waiting on it until one is given back (jit_cores.c) - no spin
	@if grep -nE "semaphore|sema\\.h|core_lock|lock_guard lock\\(g_fxo->get<jit_core_allocator>" ../../Emu/Cell/PPUThread.cpp; then echo "PPU JIT: its compile cores are a semaphore again - they are jit_cores.c"; exit 1; elif grep -nE "busy_wait|sched_yield|sleep" ../../Emu/Cell/jit_cores.c; then echo "jit_cores: a spin or a sleep is in it"; exit 1; else echo "PPU JIT: compile cores with no semaphore, no spin"; fi
	@# No host semaphore in the core: sema.h is gone, the RPCN client's wake-ups are words waited on (rpcn_signal.c), the JIT's cores a count (jit_cores.c) - Vulkan's semaphores are the GPU's, not the host's
	@if [ -e ../../../Utilities/sema.h ] || grep -rnE "std::(binary|counting)_semaphore|include <semaphore>|sema\\.h|semaphore_base|\\bsem_(init|wait|post|t)\\b|\\bCreateSemaphore[AW]?\\(|dispatch_semaphore" --include=*.cpp --include=*.h --include=*.hpp --include=*.c ../../Emu ../../Input ../../Loader ../../Crypto ../../util ../*.cpp ../*.h ../../../Utilities; then echo "host semaphores: one is back in the core - wait on a word (retro_waitaddr) instead"; exit 1; else echo "host semaphores: none in the core"; fi
	@# No lock over the IDs: id_manager::g_mutex is gone - IDs are given out by a compare-and-swap on their keys, taken away by the thread holding the ID's deciding mark (idm::close, withdraw), selected with counted references
	@if grep -rnE "id_manager::g_mutex|extern shared_mutex g_mutex|shared_mutex id_manager::g_mutex" --include=*.cpp --include=*.h --include=*.hpp ../../Emu ../../Input ../../Loader ../*.cpp ../*.h; then echo "idm: the lock over the IDs is back - IDs are claimed by compare-and-swap, removed under the ID's deciding mark"; exit 1; else echo "idm: no lock over the IDs"; fi
	@# Vulkan's DMA blocks are the renderer's alone, and a query pool let go on any thread goes back to the renderer's cache through a stack with no lock (mpsc_stack)
	@if grep -nE "shared_mutex|std::mutex|lock_guard|unique_lock|reader_lock" ../../Emu/RSX/VK/VKDMA.cpp ../../Emu/RSX/VK/VKQueryPool.cpp ../../Emu/RSX/VK/VKQueryPool.h; then echo "VK: a lock is back in the DMA blocks or the query pool cache"; exit 1; else echo "VK: DMA blocks and query pool cache with no lock"; fi
	@# The offloader faulting on the renderer's locked memory waits on the queue status word for the renderer to clear its deadlock, no spin
	@if awk '/Wait for deadlock to clear/{f=1} f&&/utils::pause\(\)/{bad=1} f&&/clear_mem_fault_flag/{exit} END{exit !bad}' ../../Emu/RSX/VK/VKGSRender.cpp || ! grep -q "m_queue_status.notify_all();" ../../Emu/RSX/VK/VKGSRender.cpp; then echo "VK offloader: it spins for the renderer to clear its deadlock again"; exit 1; else echo "VK offloader: waits for its deadlock to clear, no spin"; fi
	@# The RSX offloader's jobs go through a ring made once (retro_spsc): no allocation for a job, no idle spin - it waits on its wake word; the renderer waiting for it waits on its own (rsx_wake.c), woken by what it has to answer
	@if grep -nE "lf_queue|lockless\.h|std::vector|std::this_thread::yield|utils::pause|busy_wait|\bnew " ../../Emu/RSX/RSXOffload.cpp ../../Emu/RSX/RSXOffload.h; then echo "RSX offloader: an allocation or a spin is back - jobs are packets in the ring, waits are on rsx_wake"; exit 1; elif grep -nE "yield|pause|sleep|mutex|lock" ../../Emu/RSX/Common/rsx_wake.c; then echo "rsx_wake: a spin, a sleep or a lock is in it"; exit 1; else echo "RSX offloader: no allocation per job, no spin"; fi
	@# A thread holding the renderer still (rsx::eng_lock) waits for it to hold, and the renderer held waits to be let go, on words (rsx_hold.c) - no spin
	@if grep -rn "external_interrupt_lock\|external_interrupt_ack" --include=*.cpp --include=*.h --include=*.hpp ../../Emu; then echo "RSX: the spun-on pause words are back - pause and wait_pause are rsx_hold.c"; exit 1; elif awk '/void thread::pause\(\)/{f=1} f&&/utils::pause|yield\(/{bad=1} /u32 thread::get_load/{f=0} END{exit !bad}' ../../Emu/RSX/RSXThread.cpp || grep -nE "yield|pause\(|sleep|mutex" ../../Emu/RSX/Common/rsx_hold.c; then echo "RSX: pause or wait_pause spins again"; exit 1; else echo "RSX: pause and wait_pause wait on words, no spin"; fi
	@# A PPU thread started waits for its caller to leave sys_ppu_thread_start, and the caller for it, on words (start_gate_open, sched_watch) - no spin, no clock
	@if awk '/^error_code sys_ppu_thread_start/{f=1} f&&/get_system_time|yield\(|busy_wait/{bad=1} f&&/^error_code sys_ppu_thread_rename/{exit} END{exit !bad}' ../../Emu/Cell/lv2/sys_ppu_thread.cpp || awk '/case ppu_cmd::entry_call:/{f=1} f&&/get_system_time|yield\(|busy_wait/{bad=1} f&&/case ppu_cmd::hle_call:/{exit} END{exit !bad}' ../../Emu/Cell/PPUThread.cpp; then echo "PPU thread start: a spin or a clock is back - the caller and the new thread wait on words"; exit 1; else echo "PPU thread start: no spin, no clock"; fi
	@# No sleep and no poll in the video decoder's close and reaper, the pad config event thread, or the priority read: waits on words
	@if grep -nE "wait_for\(|lf_queue<shared_ptr<vdec_context>>" ../../Emu/Cell/Modules/cellVdec.cpp ../../Emu/Cell/Modules/sys_io_.cpp || grep -n "std::this_thread::yield" ../../Emu/Cell/lv2/sys_ppu_thread.cpp; then echo "vdec / sys_io / ppu priority: a sleep, a poll or a yield is back"; exit 1; else echo "vdec / sys_io / ppu priority: no sleep, no poll"; fi
	@# The RSX's report timestamps never wait for the clock (rsx_timestamp.c)
	@if awk '/u64 thread::timestamp\(\)/{f=1} f&&/utils::pause|yield\(|busy_wait/{bad=1} f&&/^\t}/{exit} END{exit !bad}' ../../Emu/RSX/RSXThread.cpp; then echo "RSX timestamp: spins for the clock again - it is rsx_timestamp.c"; exit 1; elif grep -rnE "utils::pause|sleep_for|thread_ctrl::wait_for|busy_wait|this_thread::yield" ../../Emu/RSX/Overlays; then echo "overlays: a spin, a sleep or a poll is back - they wait on g_libretro_events (overlay_wait)"; exit 1; else echo "RSX timestamp, overlays: no spin, no sleep, no poll"; fi
	@# The AV UART takes no lock and polls nothing: a send and a receive at a time by claim words, each ring's side waited on by the words the other moves; the game's buffers are read and written in place
	@if grep -nE "shared_mutex|cond_variable|std::mutex|lock_guard|unique_lock|wait_unlock|make_unique<u8|make_shared<" ../../Emu/Cell/lv2/sys_uart.cpp ../../Emu/Cell/lv2/sys_uart.h || grep -n "thread_ctrl::wait_for" ../../Emu/Cell/lv2/sys_uart.cpp; then echo "sys_uart: a lock, a poll, a sleep or a buffer per call is back"; exit 1; else echo "sys_uart: no lock, no poll, no buffer per call"; fi
	@# The pads have no polling thread (retro_run drives them), and a pad plugged in mid-game allocates nothing
	@if grep -nE "thread_ctrl::wait_for|void operator\(\)\(\)" ../../Input/pad_thread.cpp ../../Input/pad_thread.h || grep -n "get_products_by_class" ../../Emu/Cell/Modules/cellPad.cpp; then echo "pads: the polling loop, or a vector per hotplug, is back"; exit 1; else echo "pads: no polling loop, nothing allocated per hotplug"; fi
	@# The OSK and message dialogs, the Cross Controller and the NP bandwidth test: no yield, no poll, no lock - waits on words (libretro_wait_on), the fake bandwidth test one word of guest time
	@if grep -nE "this_thread::yield|thread_ctrl::wait_for" ../../Emu/Cell/Modules/cellOskDialog.cpp ../../Emu/Cell/Modules/cellMsgDialog.cpp ../../Emu/Cell/Modules/cellCrossController.cpp ../../Emu/Cell/Modules/sceNpUtil.cpp || grep -nE "shared_mutex|lock_guard|named_thread" ../../Emu/Cell/Modules/sceNpUtil.cpp; then echo "dialogs / cross controller / np util: a yield, a poll, a lock or the test thread is back"; exit 1; else echo "dialogs / cross controller / np util: no yield, no poll, no lock"; fi
	@# The renderer waits on the GPU in the driver (fences, query results) and on words, never spinning: a skipped draw does not yield, the swapchain's readers and the recording sinks' users are waited for, the zcull hard sync does not spin first
	@if grep -nE "utils::pause|this_thread::yield|sleep_for|thread_ctrl::wait_for|busy_wait\(" ../../Emu/RSX/VK/VKQueryPool.cpp ../../Emu/RSX/VK/VKDraw.cpp ../../Emu/RSX/GL/GLDraw.cpp ../../Emu/RSX/VK/VKPresent.cpp ../../Emu/RSX/VK/vkutils/swapchain_core.h ../../Emu/RSX/rsx_cache.h ../../util/video_sink.h ../../util/video_provider.cpp || awk '/void fence::wait_flush|VkResult wait_for_fence/{f=1} f&&/utils::pause/{bad=1} f&&/^\t}/{f=0} END{exit !bad}' ../../Emu/RSX/VK/vkutils/sync.cpp || grep -n "busy_wait" ../../Emu/RSX/VK/VKGSRender.cpp; then echo "renderer: a spin, a yield or a sleep is back on a GPU or reader wait"; exit 1; else echo "renderer: GPU waits in the driver, no spin"; fi
	@# The recording encoder, the TAR extractor and the package install wait on words, not sleeps: the encoder for frames, blocks or a pause/flush/stop, its flushes for the encoder, the TAR reader and writer for each other
	@if grep -nE "sleep_for|this_thread::yield|thread_ctrl::wait_for" ../../util/media_utils.cpp ../../Loader/TAR.cpp ../../Emu/system_utils.cpp; then echo "encoder / TAR / pkg install: a sleep, a poll or a yield is back"; exit 1; else echo "encoder / TAR / pkg install: no sleep, no poll"; fi
	@# The RPCN client's replies and server info, and the NP handler's thread, are waited for on the client's event word, not polled
	@if grep -nE "sleep_for|thread_ctrl::wait_for" ../../Emu/NP/rpcn_client.cpp ../../Emu/NP/np_handler.cpp; then echo "RPCN / NP handler: a poll is back - they wait on rpcn_client::m_events"; exit 1; else echo "RPCN / NP handler: no poll"; fi
	@# The RPCN client's queues take no lock: notifications, replies and presence updates are pushed as nodes (mpsc_stack) and taken whole, sync replies are slots claimed by compare-and-swap (rpcn_reply_slots.c)
	@if grep -nE "mutex_notifs|mutex_replies|mutex_replies_sync|mutex_presence_updates" ../../Emu/NP/rpcn_client.cpp ../../Emu/NP/rpcn_client.h; then echo "RPCN: a queue lock is back"; exit 1; elif grep -nE "mutex_friends|mutex_messages|mutex_connected|mutex_authentified|sem_connected|sem_authentified|inst_mutex|mutex_rpcn" ../../Emu/NP/rpcn_client.cpp ../../Emu/NP/rpcn_client.h ../../Emu/NP/np_handler.cpp ../../Emu/NP/np_handler.h; then echo "RPCN: a friends, messages or connection lock is back - they are published states (retro_retire) and generation words"; exit 1; else echo "RPCN: queues, friends, messages and connection with no lock"; fi
	@# Score and TUS transactions take no lock and start no thread: registered in slots before the request goes, finished once by whichever of the reply, an abort or the timeout claims them first
	@if grep -nE "wake_cond|completion_cond|mutex_async_transactions|std::thread thread|trans_ctx->mutex|std::unique_lock<shared_mutex> lock, const shared_ptr" ../../Emu/NP/np_contexts.h ../../Emu/NP/np_contexts.cpp ../../Emu/NP/np_requests.cpp ../../Emu/NP/np_handler.h ../../Emu/NP/np_handler.cpp; then echo "NP transactions: a lock, a condition variable or a thread per request is back"; exit 1; else echo "NP transactions: no lock, no thread per request"; fi
	@# The PPU's far jumps and expected TOCs are published states read with no lock (retro_retire), not maps under a shared_mutex taken on every jump
	@if awk '/^struct ppu_far_jumps_t/{f=1} f&&/shared_mutex|reader_lock|lock_guard/{bad=1} f&&/^u32 ppu_get_far_jump/{exit} END{exit !bad}' ../../Emu/Cell/PPUThread.cpp || awk '/^struct ppu_toc_manager/{f=1} f&&/shared_mutex|reader_lock|lock_guard/{bad=1} f&&/^static void ppu_check_toc/{f=2} f==2&&/reader_lock|lock_guard/{bad=1} f==2&&/^}/{exit} END{exit !bad}' ../../Emu/Cell/PPUThread.cpp; then echo "PPU far jumps / TOC: a lock is back on the jump"; exit 1; else echo "PPU far jumps / TOC: no lock"; fi
	@# The VFS tree is published with compare-and-swap, a PRX's exports are guarded by its state and a busy count, the Gem positions are one word each, the audio backend's callbacks are the setting thread's: no lock in any
	@if grep -nE "shared_mutex|std::mutex|timed_mutex|lock_guard|lock_unlock" ../../Emu/VFS.cpp ../../Emu/Audio/AudioBackend.cpp ../../Emu/Audio/AudioBackend.h ../../Emu/Cell/lv2/sys_prx.cpp ../../Emu/Cell/lv2/sys_prx.h || awk '/struct gem_position/{f=1} f&&/mutex|lock_guard/{bad=1} f&&/^\t};/{exit} END{exit !bad}' ../../Emu/Cell/Modules/cellGem.cpp; then echo "VFS / PRX / Gem / audio backend: a lock is back"; exit 1; else echo "VFS / PRX / Gem / audio backend: no lock"; fi
	@# Guest page faults: the watched areas are slots taken by compare-and-swap, a faulted thread is marked in its own word, and a full queue is waited on for room - no lock, no clock
	@if grep -nE "pf_mutex|pf_entries\.mutex|sleep_until, 1000" ../../../Utilities/Thread.cpp ../../Emu/Cell/lv2/sys_mmapper.cpp ../../Emu/Cell/lv2/sys_mmapper.h ../../Emu/Cell/lv2/sys_ppu_thread.cpp || grep -nE "shared_mutex|std::mutex" ../../Emu/Cell/lv2/sys_mmapper.h; then echo "page faults: a lock or a clock is back"; exit 1; else echo "page faults: no lock, no clock"; fi
	@# The on-screen keyboard's text for the game is published whole and read in place (osk_text.c): no lock in cellOskDialog
	@if grep -nE "text_mtx|shared_mutex|std::mutex|lock_guard|unique_lock" ../../Emu/Cell/Modules/cellOskDialog.cpp ../../Emu/Cell/Modules/cellOskDialog.h; then echo "cellOskDialog: a lock is back"; exit 1; else echo "cellOskDialog: no lock"; fi
	@# sys_usbd's event waiters are the USB thread's, the PPU linkage tables and PRX names the HLE thread's, the PNG encoder's limits one word: no lock in any
	@if grep -nE "mutex_sq|shared_mutex|std::mutex|lock_guard|unique_lock|reader_lock" ../../Emu/Cell/lv2/sys_usbd.cpp ../../Emu/Cell/PPUModule.cpp ../../Emu/Cell/Modules/cellPngEnc.cpp; then echo "sys_usbd / PPU linkage / cellPngEnc: a lock is back"; exit 1; else echo "sys_usbd / PPU linkage / cellPngEnc: no lock"; fi
	@# The PPU JIT modules are the HLE thread's, the shared memory area is published once, sys_ss's allocations are bits, the AV chat callbacks one word each: no lock in any
	@if grep -nE "shared_mutex|std::mutex|lock_guard|unique_lock[^a-z_]|reader_lock" ../../Emu/Cell/Modules/sys_mmapper_.cpp ../../Emu/Cell/lv2/sys_ss.cpp ../../Emu/Cell/Modules/cellSysutilAvc.cpp ../../Emu/Cell/Modules/cellSysutilAvc2.cpp | grep -v hle_locks_t || awk '/struct jit_module_manager/{f=1} f&&/mutex|lock_guard/{bad=1} f&&/^\t};/{exit} END{exit !bad}' ../../Emu/Cell/PPUThread.cpp; then echo "PPU JIT modules / shared memory area / sys_ss / AV chat: a lock is back"; exit 1; else echo "PPU JIT modules / shared memory area / sys_ss / AV chat: no lock"; fi
	@# Trophies: the manager is the HLE thread's, the home menu's name published whole, registering paced by the game and retro_run's frames - no lock, no clock
	@if grep -nE "shared_mutex|std::mutex|lock_guard|scoped_lock|shared_lock|unique_lock[^a-z_]|reader_lock|get_system_time|wait_for\(" ../../Emu/Cell/Modules/sceNpTrophy.cpp ../../Emu/Cell/Modules/sceNpTrophy.h; then echo "trophies: a lock or a clock is back"; exit 1; else echo "trophies: no lock, no clock"; fi
	@# The network threads wait in poll() untimed, woken by a wake-up socket (net_wake.c); a socket's timeout is its waiter's own: no timed poll, no sleep
	@if grep -nE "poll\([^)]*, *(1|5)\)|Sleep\(" ../../Emu/Cell/lv2/sys_net/network_context.cpp ../../Emu/Cell/lv2/sys_net/sys_net_helpers.cpp || grep -nE "get_guest_system_time\(\) - ppu.start_time" ../../Emu/Cell/lv2/sys_net.cpp; then echo "network threads: a timed poll is back"; exit 1; else echo "network threads: untimed"; fi
	@# All of sys_net is the network thread's: sockets, queues, P2P ports, the threads to wake - handed to it (net_call), no lock anywhere in it
	@if grep -rnE --include=*.cpp --include=*.h "mutex|lock_guard|scoped_lock|unique_lock|reader_lock|lock_unlock" ../../Emu/Cell/lv2/sys_net.cpp ../../Emu/Cell/lv2/sys_net/; then echo "sys_net: a lock is back"; exit 1; else echo "sys_net: no lock"; fi
	@# A socket call reads and writes the game's buffer where it is: no vector in between, the P2P header gathered with the data, no spin on EAGAIN
	@if grep -nE "std::vector<u8> (res_buf|dest_buf|p2p_data)\(|buf_copy\(" ../../Emu/Cell/lv2/sys_net.cpp ../../Emu/Cell/lv2/sys_net/*.cpp || grep -nE "while \(np::sendto_possibly_ipv6" ../../Emu/Cell/lv2/sys_net/*.cpp; then echo "sys_net: a copy in between is back"; exit 1; else echo "sys_net: sent and received in place"; fi
	@# The NP handler's state is its thread's (on_np_thread); sceNpInit/Term one at a time by compare-and-swap; the signaling callbacks one word each: no lock in any
	@if grep -nE "mutex|lock_guard|scoped_lock|unique_lock|reader_lock" ../../Emu/NP/np_handler.cpp ../../Emu/NP/np_handler.h ../../Emu/NP/np_requests.cpp ../../Emu/NP/np_requests_gui.cpp ../../Emu/NP/np_notifications.cpp ../../Emu/NP/np_contexts.h ../../Emu/Cell/Modules/sceNp.cpp ../../Emu/Cell/Modules/sceNp2.cpp; then echo "NP handler: a lock is back"; exit 1; else echo "NP handler: no lock"; fi
	@# The signaling handler's peers and packets are its thread's (on_thread): no lock
	@if grep -nE "mutex|lock_guard|scoped_lock|unique_lock|reader_lock" ../../Emu/NP/signaling_handler.cpp ../../Emu/NP/signaling_handler.h; then echo "signaling handler: a lock is back"; exit 1; else echo "signaling handler: no lock"; fi
	@# The overlays shown are published whole (retro_retire) and read by the renderer with no lock; the input thread keeps its own stack
	@if grep -nE "mutex|lock_guard|reader_lock|lock_shared|unlock_shared" ../../Emu/RSX/Overlays/overlay_manager.cpp ../../Emu/RSX/Overlays/overlay_manager.h || grep -nE "lock\(\*m_overlay_manager\)|m_overlay_manager->lock" ../../Emu/RSX/VK/VKPresent.cpp ../../Emu/RSX/GL/GLPresent.cpp; then echo "overlay manager: a lock is back"; exit 1; else echo "overlay manager: no lock"; fi
	@# On-screen messages are handed to the renderer, a dialog's text published whole, the home menu's icons loaded once by compare-and-swap: no lock
	@if grep -nE "mutex|lock_guard" ../../Emu/RSX/Overlays/overlay_message.cpp ../../Emu/RSX/Overlays/overlay_message.h ../../Emu/RSX/Overlays/HomeMenu/overlay_home_icons.cpp || awk '/struct text_guard_t/{f=1} f&&/mutex|lock_guard/{bad=1} f&&/^\t\t};/{exit} END{exit !bad}' ../../Emu/RSX/Overlays/overlays.h; then echo "messages / text / icons: a lock is back"; exit 1; else echo "messages / text / icons: no lock"; fi
	@# A dialog is the renderer's: input and news are handed to it (post_call), no lock
	@if grep -nE "mutex|lock_guard" ../../Emu/RSX/Overlays/overlay_osk.cpp ../../Emu/RSX/Overlays/overlay_osk.h ../../Emu/RSX/Overlays/overlay_tabs.cpp ../../Emu/RSX/Overlays/overlay_tabs.h ../../Emu/RSX/Overlays/FriendsList/overlay_friends_list_dialog.cpp ../../Emu/RSX/Overlays/FriendsList/overlay_friends_list_dialog.h ../../Emu/RSX/Overlays/Network/overlay_recvmessage_dialog.cpp ../../Emu/RSX/Overlays/Network/overlay_recvmessage_dialog.h ../../Emu/RSX/Overlays/Network/overlay_sendmessage_dialog.cpp ../../Emu/RSX/Overlays/Network/overlay_sendmessage_dialog.h; then echo "dialogs: a lock is back - hand the call to the renderer (post_call)"; exit 1; else echo "dialogs: no lock"; fi
	@# A thread opening a dialog waits for it untimed (libretro_wait_on), not a millisecond at a time
	@if grep -rnE "atomic_wait_timeout|wait_for\(|std::this_thread::sleep" ../../Emu/RSX/Overlays; then echo "overlays: a timed wait is back - wait untimed on the event (libretro_wait_on)"; exit 1; else echo "overlays: no timed wait"; fi
	@# UPnP is the HTTP thread's; the IPv6 stand-ins are claimed slots; the IPv6 check is one thread's, the rest wait on its answer: no lock
	@if grep -nE "mutex|lock_guard" ../../Emu/NP/upnp_handler.cpp ../../Emu/NP/upnp_handler.h ../../Emu/NP/ip_address.cpp ../../Emu/NP/ip_address.h; then echo "upnp / ip address: a lock is back"; exit 1; else echo "upnp / ip address: no lock"; fi
	@# The virtual devices are a table published whole; a GL shader is compiled by the first thread to mark it: no lock
	@if awk '/class device_manager final/,/^\tstatic device_manager& get_device_manager/' ../../../Utilities/File.cpp | grep -nE "mutex|lock_guard|reader_lock" || grep -nE "mutex|lock_guard" ../../Emu/RSX/GL/glutils/program.h ../../Emu/RSX/GL/glutils/program.cpp; then echo "virtual devices / GL shaders: a lock is back"; exit 1; else echo "virtual devices / GL shaders: no lock"; fi
	@# The LLVM JIT's null functions are a table published whole: no lock
	@if grep -nE "null_mtx|lock_guard" ../../../Utilities/JITLLVM.cpp; then echo "LLVM null functions: a lock is back"; exit 1; else echo "LLVM null functions: no lock"; fi
	@# The guest's process-shared objects: a table read in place, changed by the HLE thread alone: no lock
	@if grep -nE "mutex|lock_guard|reader_lock" ../../Emu/IPC.h; then echo "IPC objects: a lock is back"; exit 1; else echo "IPC objects: no lock"; fi
	@# The Vulkan overlay passes draw with stages compiled when the core was built (vk_fixed_spirv.c): no GLSL compiled for them while a game runs
	@if grep -nE "vs_src|fs_src" ../../Emu/RSX/VK/VKOverlays.h ../../Emu/RSX/VK/VKOverlays.cpp ../../Emu/RSX/VK/VKResolveHelper.h ../../Emu/RSX/VK/VKResolveHelper.cpp; then echo "Vulkan overlay passes: GLSL compiled at run time again - add the stage to vk_fixed_shaders.h"; exit 1; else echo "Vulkan overlay passes: stages made when the core was built"; fi
	@# A compute task's source is the same on every GPU (its work group's size is a specialization constant), so its module is made when the core is built
	@if grep -nE "std::to_string\((optimal_group_size|cs_wave_x|cs_wave_y)\)" ../../Emu/RSX/VK/VKCompute.h ../../Emu/RSX/VK/VKCompute.cpp ../../Emu/RSX/VK/VKResolveHelper.cpp; then echo "Vulkan compute: a GPU's work group size written into the source again - leave it to local_size_x_id"; exit 1; else echo "Vulkan compute: the work group size is a specialization constant"; fi
	@# The driver's pipelines are kept between boots: the renderer hands the pipeline cache its file
	@if ! grep -q "keep_pipeline_cache(" ../../Emu/RSX/VK/VKGSRender.cpp; then echo "Vulkan: the pipeline cache is no longer kept between boots"; exit 1; else echo "Vulkan: the pipeline cache is kept between boots"; fi
	@# Mutexes nothing took are gone and do not come back
	@if grep -nE "mutex" ../../Emu/Cell/Modules/cellWebBrowser.cpp ../../Emu/Cell/Modules/cellRudp.cpp ../../Emu/Cell/Modules/cellRec.cpp ../../Emu/Io/camera_handler_base.h || grep -nE "shared_mutex" ../../Emu/Cell/Modules/cellFs.cpp; then echo "unused mutexes: one is back"; exit 1; else echo "unused mutexes: gone"; fi
	@# The PPU JIT's stubs by function address are an add-only map (lf_u64_map): no lock
	@if grep -nE "shared_mutex|reader_lock|lock_guard" ../../Emu/Cell/PPUThread.cpp; then echo "PPU: a lock is back"; exit 1; else echo "PPU: no lock"; fi
	@# The NetStart dialog's delay is the game's frames (libretro_wait_frames), not the host's clock
	@if grep -nE "wait_for\(|sleep_for" ../../Emu/Cell/Modules/cellNetCtl.cpp; then echo "cellNetCtl: a sleep on the host's clock is back"; exit 1; else echo "cellNetCtl: paced by frames"; fi
	@# The progress dialog, the pause message and closing the home menu after a load go by the game's frames, not the host's clock
	@if grep -nE "thread_ctrl::wait_for\(|thread_ctrl::wait_until\(|sleep_for" ../../Emu/system_progress.cpp || grep -nE "sleep_for\(2s\)|wait_for\(33.000\)" ../../Emu/System.cpp; then echo "progress / pause / home menu: a sleep on the host's clock is back"; exit 1; else echo "progress / pause / home menu: paced by frames"; fi
	@# RSX paused draws its overlays a frame at a time, no 16 ms clock
	@if grep -nE "wait_on\(state, old, 16000\)|wait_for\(30.000\)" ../../Emu/RSX/RSXThread.cpp; then echo "RSX: a timed pause wait is back"; exit 1; else echo "RSX: paused by frames"; fi
	@# The savestate's compression threads wait for a slot to empty on a counter bumped as data is taken, not a sleep
	@if grep -nE "thread_ctrl::wait_for\(|sleep_for" ../../util/serialization_ext.cpp; then echo "savestate compression: a sleep is back"; exit 1; else echo "savestate compression: no sleep"; fi
	@# libmixer's surround mixer, a block ahead of its port, waits for a frame, not a millisecond
	@if grep -nE "thread_ctrl::wait_for\(" ../../Emu/Cell/Modules/libmixer.cpp; then echo "libmixer: a sleep is back"; exit 1; else echo "libmixer: paced by frames"; fi
	@# A blocking message dialog is waited for untimed (libretro_wait_on), not a millisecond at a time
	@if grep -nE "atomic_wait_timeout" ../../Emu/Cell/Modules/cellMsgDialog.cpp; then echo "cellMsgDialog: a timed wait is back"; exit 1; else echo "cellMsgDialog: no timed wait"; fi
	@# cellGcmCallback waits for the RSX to leave the new command buffer on its get pointer (notified while it waits), not in a spin
	@if awk '/^s32 cellGcmCallback/,/^}/' ../../Emu/Cell/Modules/cellGcmSys.cpp | grep -nE "busy_wait|yield"; then echo "cellGcmCallback: the spin is back"; exit 1; else echo "cellGcmCallback: waits on get"; fi
	@# A PPU's yield and hardware thread switch are the guest's time, cut short by its state, not a sleep on the host's clock
	@if grep -nE "wait_for_accurate\(|s_dummy_atomic" ../../Emu/CPU/CPUThread.cpp; then echo "CPU: a host-clock nap is back"; exit 1; else echo "CPU: yields wait the guest's time"; fi
	@# cellRec's recorder waits on the sink, the audio thread's periods or a frame, and keeps the guest's time, not the host's
	@if grep -nE "thread_ctrl::wait_for\(|get_system_time\(" ../../Emu/Cell/Modules/cellRec.cpp ../../util/video_provider.cpp; then echo "cellRec: a sleep or the host's clock is back"; exit 1; else echo "cellRec: no sleep, the guest's clock"; fi
	@# cellVdec's decoder thread waits for room untimed: the game taking a picture, a close, a new sequence or a stop wakes it
	@if grep -nE "retro_waitaddr_wait\([^;]*, [0-9]+\);" ../../Emu/Cell/Modules/cellVdec.cpp || ! grep -q "{&ppu.state, std::bit_cast<u32>(old_state)}" ../../Emu/Cell/Modules/cellVdec.cpp; then echo "cellVdec: the wait for room polls again, or a stop no longer wakes it"; exit 1; else echo "cellVdec: the wait for room is untimed and a stop wakes it"; fi
	@# cellGem's thread updates the controllers at 10 Hz of the game's time, looked at once a frame, and waits after a savestate by frames
	@if grep -nE "wait_on\(m_wake_up, 0, timeout\)|wait_on\(m_done, 2, 5'000'000\)" ../../Emu/Cell/Modules/cellGem.cpp; then echo "cellGem: a host timer is back"; exit 1; else echo "cellGem: paced by frames"; fi
	@# the RSX thread waits for startup by frames and notifications, and the progress dialog waits for the renderer by frames
	@if grep -n "thread_ctrl::wait_for(1000)" ../../Emu/RSX/RSXThread.cpp || grep -n "atomic_wait_timeout(5" ../../Emu/system_progress.cpp; then echo "RSX startup: a poll or a host timeout is back"; exit 1; else echo "RSX startup: no poll, no host timeout"; fi
	@# the RSX driver wake-up delay is the guest's time, cut short by the thread's state: no host clock, no spin
	@if awk '/void thread::fifo_wake_delay/,/u32 thread::get_fifo_cmd/' ../../Emu/RSX/RSXThread.cpp | grep -nE "busy_wait|yield|get_system_time|wait_for\("; then echo "RSX wake-up delay: the host's clock or a spin is back"; exit 1; else echo "RSX wake-up delay: the guest's time"; fi
	@# the core's own files take no <mutex> or <condition_variable>
	@if grep -nE "#include <(mutex|condition_variable)>" ../libretro_core.cpp ../libretro_video.h; then echo "libretro: <mutex> or <condition_variable> is included again"; exit 1; else echo "libretro: no <mutex>, no <condition_variable>"; fi
	@# no host CPU and RAM monitor thread in the core: measuring goes in tools, not shipping code
	@if test -e ../../Emu/perf_monitor.cpp || grep -n "perf_monitor" ../../../Makefile.common ../../Emu/System.cpp; then echo "perf_monitor is back: a thread that wakes twice a second to log the host's CPU and RAM use"; exit 1; else echo "no perf_monitor thread"; fi
	@# the SPU LLVM compile thread picks what the SPUs run by looking at them as it picks, with no sampling thread on the host's clock
	@if grep -nE "SPU LLVM Profiler|prof_mutex|wait_for\(20, false\)" ../../Emu/Cell/SPUCommonRecompiler.cpp; then echo "SPU LLVM: the sampling thread is back"; exit 1; else echo "SPU LLVM: no sampling thread"; fi
	@# the RSX FIFO's recovery and a refused rename wait for retro_run's frames, not the host's clock
	@if grep -n "sleep_for(2ms)" ../../Emu/RSX/RSXThread.cpp || grep -nE "this_thread::yield|steady_clock" ../../Emu/VFS.cpp; then echo "RSX recovery / VFS rename: the host's clock or a yield loop is back"; exit 1; else echo "RSX recovery / VFS rename: by frames"; fi
	@# np_handler.h takes no <condition_variable>
	@if grep -n "#include <condition_variable>" ../../Emu/NP/np_handler.h; then echo "np_handler.h: <condition_variable> is included again"; exit 1; else echo "np_handler.h: no <condition_variable>"; fi
	@# no GDB server in the core: it listened on a TCP port in every game and looked for a connection every 5 ms
	@if test -e ../../Emu/GDB.cpp || grep -rn "gdb_server\|Emu/GDB.h" ../../Emu ../../../Makefile.common; then echo "the GDB server is back"; exit 1; else echo "no GDB server"; fi
	@# no RSX frame capture or replay in the core: nothing asks for a capture or boots one, and the replay polled on the host's clock
	@if test -e ../../Emu/RSX/Capture || grep -rnE "capture_current_frame|rsx_replay|BootRsxCapture|frame_capture_data" ../../Emu; then echo "the RSX capture or replay is back"; exit 1; else echo "no RSX capture or replay"; fi
	@# no CPU profiler thread in the core: measuring goes in tools, not shipping code
	@if grep -nE "cpu_prof|flush_profilers|get_cpu_program_usage_percent" ../../Emu/CPU/CPUThread.cpp ../../Emu/CPU/CPUThread.h ../../Emu/System.cpp; then echo "the CPU profiler is back"; exit 1; else echo "no CPU profiler"; fi
	@# turn_calls is gone (a caller queued on a turn waits: a lock without a lock object); it does not come back
	@if [ -e ../../Emu/CPU/turn_calls.h ] || [ -e ../../Emu/CPU/turn_calls.c ] || grep -rnE "turn_call\(|turn_calls\.h|turn_calls_t\b|turn_calls_cpp" --include=*.cpp --include=*.h --include=*.hpp --include=*.c ../../Emu ../../Input ../../Loader ../../Crypto ../../util ../*.cpp ../*.h ../../../Utilities; then echo "turn_calls: back in the core - it was taken out for good (AGENTS.md)"; exit 1; else echo "turn_calls: none in the core"; fi
	@# The toy portals are the USB thread's alone (usb_handler_thread::on_thread)
	@if grep -nE "mutex|lock_guard|unique_lock|reader_lock" ../../Emu/Io/Skylander.cpp ../../Emu/Io/Skylander.h ../../Emu/Io/Infinity.cpp ../../Emu/Io/Infinity.h ../../Emu/Io/Dimensions.cpp ../../Emu/Io/Dimensions.h ../../Emu/Io/KamenRider.cpp ../../Emu/Io/KamenRider.h; then echo "portals: a lock is back - the portals are the USB thread's, sys_usbd runs their transfers there"; exit 1; else echo "portals: no lock, the USB thread's"; fi
	@# cellAudio's periods go by the guest's clock: no sleep and no clock of the host's
	@if grep -nE "thread_ctrl::wait_for|get_system_time[ ]*\(" ../../Emu/Cell/Modules/cellAudio.cpp; then echo "cellAudio: a sleep or a clock of the host's is back - its periods go by the guest's clock (audio_ringbuffer::get_timestamp, wait_guest)"; exit 1; else echo "cellAudio: periods on the guest's clock"; fi
	@# The keyboards are the HLE thread's (hle_call): no lock in their handler, cellKb or the overlays' use
	@if grep -nE "m_mutex|lock_guard" ../../Emu/Io/KeyboardHandler.h ../../Emu/Io/KeyboardHandler.cpp ../../Emu/Cell/Modules/cellKb.cpp || grep -n "kb_handler.m_mutex" ../../Emu/RSX/Overlays/overlays.cpp; then echo "keyboards: a lock is back - their state is the HLE thread's (hle_call)"; exit 1; else echo "keyboards: no lock, the HLE thread's"; fi
	@# Modules whose state is the HLE thread's (hle_call): no lock in them
	@if grep -nE "shared_mutex|std::mutex|lock_guard|unique_lock|scoped_lock" $(HLE_OWNED); then echo "HLE modules: a lock is back - their state is the HLE thread's (hle_call)"; exit 1; else echo "HLE modules: no lock in $(words $(HLE_OWNED)) files, the HLE thread's"; fi
	@# cellAudio's state is its thread's (audio_call): no lock of its own
	@if grep -nE "shared_mutex|lock_audio|g_audio\.mutex|emu_cfg_upd_m|lock_guard lock\(mutex\)|unique_lock lock\(mutex\)" ../../Emu/Cell/Modules/cellAudio.cpp ../../Emu/Cell/Modules/cellAudio.h; then echo "cellAudio: a lock is back - its state is the cellAudio thread's (audio_call)"; exit 1; else echo "cellAudio: no lock, the cellAudio thread's"; fi
	@# The overlay cursors: requests in atomic words, the render thread's to take up
	@if grep -nE "shared_mutex|lock_guard|m_mutex" ../../Emu/RSX/Overlays/overlay_cursor.cpp ../../Emu/RSX/Overlays/overlay_cursor.h; then echo "cursors: a lock is back - their requests are words the render thread takes up"; exit 1; else echo "cursors: no lock"; fi
	@# The microphones are the microphone thread's (mic_call), timed on the guest's clock
	@if grep -nE "shared_mutex|lock_guard|unique_lock|steady_clock|thread_ctrl::wait" ../../Emu/Cell/Modules/cellMic.cpp ../../Emu/Cell/Modules/cellMic.h ../../Emu/Io/usb_microphone.cpp; then echo "microphones: a lock or a clock of the host's is back - they are the microphone thread's (mic_call), on the guest's clock"; exit 1; else echo "microphones: no lock, the microphone thread's, on the guest's clock"; fi
	@# The camera is the camera thread's (camera_call), timed on the guest's clock
	@if grep -nE "shared_mutex|lock_guard|unique_lock|reader_lock|steady_clock|thread_ctrl::wait|get_system_time[ ]*\(" ../../Emu/Cell/Modules/cellCamera.cpp ../../Emu/Cell/Modules/cellCamera.h || grep -n "g_camera.mutex" ../../Emu/Cell/Modules/cellGem.cpp; then echo "camera: a lock or a clock of the host's is back - it is the camera thread's (camera_call), on the guest's clock"; exit 1; else echo "camera: no lock, the camera thread's, on the guest's clock"; fi
	@# The audio hand-over and sys_io's gate: neither side spins or waits for the other
	@if grep -nE "yield\(\)|while \(!retro_atomic_cas|begin_exclusive|m_access" ../libretro_audio.cpp ../libretro_audio.h ../../Emu/Cell/Modules/sys_io_.cpp; then echo "audio hand-over / sys_io gate: a spin is back - nothing there waits for the other side"; exit 1; else echo "audio hand-over and sys_io gate: no spin"; fi
	@# cellAudioOut's settings: one 128-bit word an output, no lock
	@if grep -nE "shared_mutex|lock_guard|reader_lock|\.mtx" ../../Emu/Cell/Modules/cellAudioOut.cpp ../../Emu/Cell/Modules/cellAudioOut.h || grep -n "audio_out_cfg.mtx" ../../Emu/Audio/AudioBackend.cpp; then echo "cellAudioOut: a lock is back - an output's settings are one word (audio_out::change_settings)"; exit 1; else echo "cellAudioOut: no lock"; fi
	@# cellGame: the disc change state and callbacks are words, the data check's directory the HLE thread's
	@if grep -nE "std::mutex|shared_mutex|reader_lock|lock\((dcm\.)?mtx\)|lock\(mutex\)" ../../Emu/Cell/Modules/cellGame.cpp ../../Emu/Cell/Modules/cellGame.h; then echo "cellGame: a lock of its own is back - the disc change manager is words, the data check's directory the HLE thread's"; exit 1; else echo "cellGame: no lock of its own"; fi
	@# cellMusic and cellMusicDecode: no lock; the selection the HLE thread's, the decoding one thread's at a time by a word, the decoder's tracks handed over without a lock
	@if grep -nE "shared_mutex|lock_guard|reader_lock|scoped_lock|m_mtx|music\\.mtx|dec\\.mutex" ../../Emu/Cell/Modules/cellMusic.cpp ../../Emu/Cell/Modules/cellMusicDecode.cpp ../../util/media_utils.cpp; then echo "cellMusic: a lock is back - the selection is the HLE thread's, the decoding one thread's at a time (music_decode::busy), the decoder's tracks handed over without a lock"; exit 1; else echo "cellMusic: no lock"; fi
	@# libmixer: the mix and the players the HLE thread's (hle_call), the game's samples mixed straight in
	@if grep -nE "std::mutex|shared_mutex|lock_guard|g_surmx\.mutex" ../../Emu/Cell/Modules/libmixer.cpp; then echo "libmixer: a lock is back - the mix and the players are the HLE thread's (hle_call)"; exit 1; else echo "libmixer: no lock"; fi
	@# cellVdec: the pictures go to the game through a lock-free ring, the sequence state moves on by compare-and-swap
	@if grep -nE "shared_mutex|lock_guard|reader_lock|vdec->mutex|lock\{mutex\}" ../../Emu/Cell/Modules/cellVdec.cpp; then echo "cellVdec: a lock is back - the pictures go to the game through vdec_context's ring, the sequence state moves on by compare-and-swap"; exit 1; else echo "cellVdec: no lock of its own"; fi
	@# The RSX audio backend thread: news as bits of a word, the rings' writing side the data thread's, on the guest's clock
	@if grep -nE "state_update_m|state_update_c|ringbuf_mutex|cond_variable|get_system_time" ../../Emu/Cell/lv2/sys_rsxaudio.cpp ../../Emu/Cell/lv2/sys_rsxaudio.h; then echo "rsxaudio backend: a lock, a condition variable or the host's clock is back - it takes its news as bits of one word, the data thread carries out what it asks of the rings, and it goes by the guest's clock"; exit 1; else echo "rsxaudio backend: no lock, no host clock"; fi
	@# The RSX audio hardware's parameters: the HLE thread's, published through a sequence count (transactional_storage is gone)
	@if test -e ../../../Utilities/transactional_storage.h || grep -n "hw_param_ts\|universal_pool" ../../Emu/Cell/lv2/sys_rsxaudio.cpp ../../Emu/Cell/lv2/sys_rsxaudio.h; then echo "rsxaudio: transactional_storage is back - the hardware's parameters are the HLE thread's, published through a sequence count"; exit 1; else echo "rsxaudio: the hardware's parameters without a lock"; fi
	@# sys_rsxaudio: the object the data thread's (rsxaudio_data_call), no lock left in the file
	@if grep -nE "shared_mutex|lock_guard|reader_lock|unique_lock" ../../Emu/Cell/lv2/sys_rsxaudio.cpp ../../Emu/Cell/lv2/sys_rsxaudio.h; then echo "sys_rsxaudio: a lock is back - the object is the data thread's (rsxaudio_data_call)"; exit 1; else echo "sys_rsxaudio: no lock"; fi
	@# The log: no lock, and its writer thread waits on the lines instead of sleeping or spinning
	@if grep -nE "shared_mutex|lock_guard|g_mutex|yield\(\)|sleep_for" ../../util/logs.cpp; then echo "logs: a lock, a spin or a sleep is back - the registry is made before any thread, the writer waits on its lines' word"; exit 1; else echo "logs: no lock, no spin, no sleep"; fi
	@# sys_fs: no mount point lock; what took it runs on sys_fs's thread (fs_call), a read at an offset on the caller's
	@if grep -nE "shared_mutex|lock_guard|unique_lock|mp->mutex" ../../Emu/Cell/lv2/sys_fs.cpp ../../Emu/Cell/lv2/sys_fs.h || grep -rnE "mp->mutex|g_mp_sys_[a-z0-9_]+\.mutex" ../../Emu; then echo "sys_fs: a lock is back - what took a mount point's lock runs on sys_fs's thread (fs_call)"; exit 1; else echo "sys_fs: no lock"; fi
	@# The frontend's files take no lock: with no reads and writes at an offset, what uses the handle's position runs on one thread
	@if grep -nE "std::mutex|lock_guard|unique_lock|shared_mutex" ../libretro_vfs.cpp ../libretro_vfs.h; then echo "libretro_vfs: a lock is back - an older frontend's file is used on the serial thread (serial_call)"; exit 1; else echo "libretro_vfs: no lock"; fi
	@# lv2 semaphores: the count or the waiters in one 128-bit word, each operation one compare-and-swap (lv2_countq.c)
	@if grep -nE "shared_mutex|lock_guard|unique_lock|->mutex|\.mutex|turn_call|turn_calls|busy_wait" ../../Emu/Cell/lv2/sys_semaphore.cpp ../../Emu/Cell/lv2/sys_semaphore.h; then echo "sys_semaphore: a lock, a turn or a spin is back - the count or the waiters are one word (lv2_countq.c)"; exit 1; else echo "sys_semaphore: one word, no lock, no turn, no spin"; fi
	@# Event flags take no lock: the pattern and the waiters are lv2_flagq.c's
	@if grep -nE "shared_mutex|lock_guard|unique_lock|->mutex|\.mutex|lock_unlock" ../../Emu/Cell/lv2/sys_event_flag.cpp ../../Emu/Cell/lv2/sys_event_flag.h; then echo "sys_event_flag: a lock is back - the bits and waiters are lv2_flagq.c's"; exit 1; else echo "sys_event_flag: no lock"; fi
	@# Mutexes and conditions take no lock: the mutex's owner and waiters are one word (lv2_ownq.c), a condition's waiters a waiter set
	@if grep -nE "shared_mutex|lock_guard|unique_lock|reader_lock|->mutex\.|\.mutex\.|lock\((cond\.|cond->)?mutex(->|\.)mutex\)" ../../Emu/Cell/lv2/sys_mutex.cpp ../../Emu/Cell/lv2/sys_mutex.h ../../Emu/Cell/lv2/sys_cond.cpp; then echo "sys_mutex/sys_cond: a lock is back - the queues change in the mutex's turn (turn_call)"; exit 1; else echo "sys_mutex/sys_cond: no lock"; fi
	@# Reader-writer locks take no lock: the owners and waiters are lv2_rwq.c's
	@if grep -nE "shared_mutex|lock_guard|unique_lock|reader_lock|->mutex|\.mutex|lock_unlock" ../../Emu/Cell/lv2/sys_rwlock.cpp ../../Emu/Cell/lv2/sys_rwlock.h; then echo "sys_rwlock: a lock is back - the owners and waiters are lv2_rwq.c's"; exit 1; else echo "sys_rwlock: no lock"; fi
	@# Lightweight mutexes and conditions take no lock: an lwmutex's sleepers are lv2_lwq.c's word, an lwcond's waiters a waiter set
	@if grep -nE "shared_mutex|lock_guard|unique_lock|scoped_lock|reader_lock|->mutex\b|\.mutex\b|lock_unlock" ../../Emu/Cell/lv2/sys_lwmutex.cpp ../../Emu/Cell/lv2/sys_lwmutex.h ../../Emu/Cell/lv2/sys_lwcond.cpp ../../Emu/Cell/lv2/sys_lwcond.h; then echo "sys_lwmutex/sys_lwcond: a lock is back - the sleepers are lv2_lwq.c's word, an lwcond's waiters a waiter set"; exit 1; else echo "sys_lwmutex/sys_lwcond: no lock"; fi
	@# Event queues take no lock: their events are a ring and their waiters two words (lv2_evq.c)
	@if grep -nE "shared_mutex|lock_guard|unique_lock|reader_lock|lock_unlock|[.>]mutex\b" ../../Emu/Cell/lv2/sys_event.cpp ../../Emu/Cell/lv2/sys_event.h | grep -v "id_manager::g_mutex"; then echo "sys_event: a lock is back - the events and waiters are lv2_evq.c's"; exit 1; elif grep -nE "queue(->|\.)mutex" ../../Emu/Cell/SPUThread.cpp ../../Emu/Cell/lv2/sys_rsx.cpp; then echo "sys_event: a queue's lock is taken again - the events and waiters are lv2_evq.c's"; exit 1; else echo "sys_event: no lock"; fi
	@# The scheduler takes no lock and no turn: what it knows is one snapshot (lv2_sched.c), each sleep, wake, yield and priority set one change put in place with compare-and-swap
	@if grep -nE "lv2_obj::g_mutex|static shared_mutex g_mutex|lock(_guard)?[ ({]+g_mutex" ../../Emu/Cell/lv2/lv2.cpp ../../Emu/Cell/lv2/sys_sync.h; then echo "lv2 scheduler: its lock is back - what it knows is lv2_sched.c's snapshot"; exit 1; elif grep -nE "turn_call|turn_calls|g_turns|g_pending|ack_suspend|next_ppu" ../../Emu/Cell/lv2/lv2.cpp ../../Emu/Cell/lv2/sys_sync.h ../../Emu/Cell/PPUThread.h; then echo "lv2 scheduler: its turn or the linked run queue is back - what it knows is lv2_sched.c's snapshot"; exit 1; else echo "lv2 scheduler: no lock, no turn, one snapshot"; fi
	@# SPU thread groups take no lock: their state changes with compare-and-swap, the threads' flags following it
	@if grep -nE "group(->|\.)mutex|grp(->|\.)mutex" ../../Emu/Cell/lv2/sys_spu.cpp ../../Emu/Cell/lv2/sys_spu.h ../../Emu/Cell/SPUThread.cpp ../../Emu/CPU/CPUThread.cpp; then echo "SPU thread groups: a group's lock is back - its state changes with compare-and-swap"; exit 1; else echo "SPU thread groups: no lock"; fi
	@# Vulkan: the async texture scheduler's command buffer is chosen, synced and submitted in turn; a sampler reload is asked for with a flag, not a lock against the renderer's texture load
	@if grep -nE "shared_mutex|lock_guard|unique_lock|Utilities/mutex.h" ../../Emu/RSX/VK/VKAsyncScheduler.cpp ../../Emu/RSX/VK/VKAsyncScheduler.h || grep -nE "m_sampler_mutex" ../../Emu/RSX/VK/*.cpp ../../Emu/RSX/VK/*.h; then echo "vk: a lock is back on the async scheduler or the samplers"; exit 1; else echo "vk: no lock on the async scheduler or the samplers"; fi
	@# RSX: the invalidated memory range is one word, merged into by unmaps and taken off by the renderer with compare-and-swap
	@if grep -nE "m_mtx_task|m_memory_invalidated" ../../Emu/RSX/RSXThread.cpp ../../Emu/RSX/RSXThread.h; then echo "rsx: the task lock is back - the invalidated range is one word (compare-and-swap)"; exit 1; else echo "rsx: no task lock, the invalidated range one word"; fi
	@# RSX host memory manager: a protection is applied at once by the thread that asks; no queue shared, nothing to flush
	@if grep -nE "shared_mutex|lock_guard|unique_lock|Utilities/mutex.h|turn_call" ../../Emu/RSX/Host/MM.cpp ../../Emu/RSX/Host/MM.h || grep -rnE "deferred_mprotect|mm_flush|async_host_memory_manager" ../../Emu/RSX ../../Emu/system_config.h; then echo "rsx mm: a lock, a turn or the deferred protection queue is back - a protection is applied at once by the thread that asks"; exit 1; else echo "rsx mm: protections applied at once, no queue, no turn"; fi
	@# Vulkan memory accounting: counters, no table of allocations and no lock
	@# Descriptor sets are the renderer's: filled, bound and flushed on its thread, each on the flush list once
	@if grep -nE "turn_call|turn_calls|storage_turns|lock_guard|mutex" ../../Emu/RSX/VK/vkutils/descriptors.cpp ../../Emu/RSX/VK/vkutils/descriptors.h ../../Emu/RSX/VK/VKProgramPipeline.cpp; then echo "vk descriptors: a turn or a lock is back - the sets are the renderer's"; exit 1; else echo "vk descriptors: the renderer's, no turn"; fi
	@# The async transfer scheduler is the renderer's alone, and its command buffers are all made at its first use
	@if grep -nE "turn_call|turn_calls|lock_guard|mutex|m_async_command_queue.emplace_back" ../../Emu/RSX/VK/VKAsyncScheduler.cpp ../../Emu/RSX/VK/VKAsyncScheduler.h; then echo "vk async scheduler: a turn, a lock or a command buffer made mid-game is back - the scheduler is the renderer's, its command buffers made at its first use"; exit 1; else echo "vk async scheduler: the renderer's, no turn, nothing made mid-game"; fi
	@# Vulkan device memory: any thread allocates and frees, none waits for another (vk_heap_alloc_shared)
	@if grep -nE "turn_call|turn_calls|lock_guard|mutex" ../../Emu/RSX/VK/vkutils/memory.cpp ../../Emu/RSX/VK/vkutils/memory.h ../../Emu/RSX/VK/vkutils/vk_heap.c ../../Emu/RSX/VK/vkutils/vk_heap.h; then echo "vk heap: a turn or a lock is back - the blocks are taken with one compare-and-swap, a thread that finds them taken takes memory of its own"; exit 1; else echo "vk heap: shared with no turn and no lock"; fi
	@if grep -nE "shared_mutex|lock_guard|unique_lock|unordered_map|vmm_allocation_t" ../../Emu/RSX/VK/VKResourceManager.cpp ../../Emu/RSX/VK/VKResourceManager.h; then echo "vk vmm: the allocation table or its lock is back - each memory_block keeps its type, size and pool and the totals are counters"; exit 1; else echo "vk vmm: counters only, no table and no lock"; fi
	@# The Vulkan Memory Allocator is gone: device memory is the renderer's own C89 heap (vkutils/vk_heap.c)
	@if ls ../../../3rdparty/GPUOpen/VulkanMemoryAllocator/include ../../Emu/RSX/VK/VKMemAlloc.cpp 2>/dev/null || grep -rnE "vk_mem_alloc|Vma[A-Z]|vma[A-Z]" --include=*.cpp --include=*.h --include=*.c ../../Emu ../../../Makefile ../../../Makefile.common; then echo "vk: the Vulkan Memory Allocator is back - device memory is vk_heap.c's"; exit 1; else echo "vk: no VMA, device memory from vk_heap.c"; fi
	@# volk is gone: the Vulkan functions are the core's own list, loaded by vk_loader.c through libretro-common's dylib
	@if ls ../../../3rdparty/volk 2>/dev/null || grep -rnE "volk[A-Z]|<volk.h>|SOURCES_VOLK|libvolk" --include=*.cpp --include=*.h --include=*.c ../../Emu ../../../Makefile ../../../Makefile.common ../../../3rdparty/CMakeLists.txt ../shader_runner.h shader_runner.h 2>/dev/null; then echo "vk: volk is back - the functions are vk_loader.c's"; exit 1; else echo "vk: no volk, the functions from vk_loader.c"; fi
	@# The Vulkan renderer's flush queue and secondary command buffers are its own: no lock, no wait for flush requests
	@if grep -nE "m_flush_queue_mutex|m_secondary_cb_guard|consumer_wait|producer_wait|num_waiters" ../../Emu/RSX/VK/VKGSRender.cpp ../../Emu/RSX/VK/VKGSRender.h ../../Emu/RSX/VK/VKGSRenderTypes.hpp; then echo "vk: a flush queue or secondary command buffer lock is back - both are the renderer's alone"; exit 1; else echo "vk: the flush queue and secondary command buffers take no lock"; fi
	@# ZCULL's report pages are the renderer's: no lock, no pause of the renderer from another thread's fault
	@if grep -nE "m_pages_mutex|eng_lock|scoped_lock|lock_guard" ../../Emu/RSX/RSXZCULL.cpp ../../Emu/RSX/RSXZCULL.h; then echo "zcull: a lock on the report pages is back - another thread's fault on one is handed to the renderer (may_own)"; exit 1; else echo "zcull: the report pages take no lock"; fi
	@# OpenGL's samplers: another thread asks for a reload with a flag the renderer takes, no lock
	@if grep -n "m_sampler_mutex" ../../Emu/RSX/GL/*.cpp ../../Emu/RSX/GL/*.h; then echo "gl: the sampler lock is back - another thread asks with request_samplers_reload"; exit 1; else echo "gl: the samplers take no lock"; fi
	@# sys_rsx: the RSX state the syscalls change takes no lock
	@if grep -n "sys_rsx_mtx" ../../Emu/Cell/lv2/sys_rsx.cpp ../../Emu/RSX/Core/RSXContext.h; then echo "sys_rsx: the lock is back - the syscalls change the IO map, tiles, zcull and display buffers with no lock"; exit 1; else echo "sys_rsx: no lock"; fi
	@# OpenGL's shader interpreter: its caches are lock-free maps that only gain entries; the loading dialog waits on the compile count, not a timer
	@if grep -nE "m_(vs|fs|program)_cache_lock|sleep_for|turn_call|turn_calls|unordered_map|shared_ptr" ../../Emu/RSX/GL/GLShaderInterpreter.cpp ../../Emu/RSX/GL/GLShaderInterpreter.h; then echo "gl interpreter: a cache lock, a turn or a sleep is back - the caches are lock-free maps that only gain entries, the dialog waits on the count"; exit 1; else echo "gl interpreter: lock-free caches, no turn, no sleep"; fi
	@# Raw SPU MFC proxy command registers change in turn
	@if grep -n "mfc_prxy_mtx" ../../Emu/Cell/RawSPUThread.cpp ../../Emu/Cell/SPUThread.h; then echo "raw spu: the MFC proxy lock is back"; exit 1; else echo "raw spu: the MFC proxy takes no lock"; fi
	@# Raw SPU run control: a stop request waits on the stop count, run requests take no lock
	@if grep -n "run_ctrl_mtx" ../../Emu/Cell/RawSPUThread.cpp ../../Emu/Cell/SPUThread.h; then echo "raw spu: the run control lock is back - a stop request waits on stop_count"; exit 1; else echo "raw spu: run control takes no lock"; fi
	@# cellGcmSys: the pages being mapped are marked with compare-and-swap (gcm_io_pages.c), their table entries theirs alone
	@if grep -n "gcmio_mutex" ../../Emu/Cell/Modules/cellGcmSys.cpp ../../Emu/Cell/Modules/cellGcmSys.h; then echo "cellGcmSys: the IO lock is back - the pages a map works on are marked (gcm_io_pages.c), theirs alone"; exit 1; else echo "cellGcmSys: the IO map takes no lock"; fi
	@# P2P port: the RPCN and signaling messages go on stacks, no lock
	@if grep -nE "s_rpcn_mutex|s_sign_mutex" ../../Emu/Cell/lv2/sys_net/*.cpp ../../Emu/Cell/lv2/sys_net/*.h; then echo "p2p: a message queue lock is back - the messages go on mpsc_stacks"; exit 1; else echo "p2p: the message queues take no lock"; fi
	@# lv2 timers: their state one 128-bit word changed with compare-and-swap (sys_timer_ctrl.c), the queue they send to put in place whole (atomic_ptr)
	@if grep -nE "turn_call|turn_calls|lock_guard|shared_mutex|check_unlocked" ../../Emu/Cell/lv2/sys_timer.cpp ../../Emu/Cell/lv2/sys_timer.h; then echo "sys_timer: a turn or a lock is back - the timer's state is sys_timer_ctrl.c's word"; exit 1; else echo "sys_timer: no turn, no lock"; fi
	@# SPU thread groups: no turn - the state one word changed with compare-and-swap (`terminating` marked in it), the threads' suspend flags settled after, the joiner subscribed with compare-and-swap; no spin waiting for exit statuses or terminators
	@if grep -nE "turn_call|turn_calls|group->turns|set_terminate" ../../Emu/Cell/lv2/sys_spu.cpp ../../Emu/Cell/lv2/sys_spu.h ../../Emu/Cell/SPUThread.cpp ../../Emu/CPU/CPUThread.cpp; then echo "SPU thread groups: a group's turn is back - its state changes with compare-and-swap, the threads' flags settled after (settle_suspend)"; exit 1; elif grep -n "busy_wait(3000)" ../../Emu/Cell/lv2/sys_spu.cpp; then echo "SPU thread groups: the 3 ms spin in sys_spu_thread_group_terminate is back"; exit 1; else echo "SPU thread groups: no turn"; fi
	@# SPU ports, queue bindings and group event ports: atomic pointers, connected with compare-and-swap; threads placed with claimed places; no group turn
	@if grep -nE "shared_ptr<lv2_event_queue> spup\\[|std::pair<u32, shared_ptr<lv2_event_queue>>|shared_ptr<lv2_event_queue> ep_" ../../Emu/Cell/SPUThread.h ../../Emu/Cell/lv2/sys_spu.h; then echo "spu: a port, queue binding or group event port is a plain shared_ptr again - read by the SPU as it is connected, it is an atomic_ptr"; exit 1; else echo "spu: ports and bindings atomic, no turn"; fi
	@# lv2 event queues: the events a ring, the waiters two words (lv2_evq.c); no turn, no spin, here or where SPUs and the RSX use them
	@if grep -nE "turn_call|turn_calls|busy_wait|lv2_events_|->pq\\b|->sq\\b" ../../Emu/Cell/lv2/sys_event.cpp ../../Emu/Cell/lv2/sys_event.h; then echo "sys_event: a turn, a spin or the linked queues are back - the events and waiters are lv2_evq.c's"; exit 1; elif grep -nE "queue->turns|held->turns|lv2_events_|turn_calls_busy" ../../Emu/Cell/SPUThread.cpp ../../Emu/Cell/lv2/sys_rsx.cpp || grep -n "busy_wait" ../../Emu/Cell/lv2/sys_rsx.cpp; then echo "sys_event: a queue's turn or a spin on it is back in the SPU or RSX code"; exit 1; else echo "sys_event: no turn, no spin"; fi
	@# An RSX event the game's full queue cannot take: the PPU sending it is descheduled until the queue changes, with no timeout to send again on (lv2_evq_poke on destroy and disconnect)
	@if grep -nE "lv2_obj::sleep\(\*cpu, [0-9]" ../../Emu/Cell/lv2/sys_rsx.cpp; then echo "sys_rsx: a full queue's sender sleeps on a timeout again - it waits for the queue to change"; exit 1; else echo "sys_rsx: a full queue's sender waits for it to change"; fi
	@# The lv2 scheduler's run queue (lv2_sched.c): a snapshot put in place with one compare-and-swap; no lock, no turn, no sleep, no spin - the one wait a thread leaving for good makes for an edge being put to it (lv2_sched_settle)
	@if grep -nE "turn_call|turn_calls|busy_wait|sched_yield|mutex|\bsleep\(|usleep|thread_wait|cond_wait" ../../Emu/Cell/lv2/lv2_sched.c ../../Emu/Cell/lv2/lv2_sched.h; then echo "lv2_sched: a lock, a turn, a sleep or a spin is in it"; exit 1; elif [ $$(grep -c "retro_waitaddr_wait" ../../Emu/Cell/lv2/lv2_sched.c) -ne 1 ]; then echo "lv2_sched: a wait other than lv2_sched_settle's"; exit 1; else echo "lv2_sched: no lock, no turn, no spin; one wait, a thread's at its exit"; fi
	@# sys_rsx: the RSX context's state changed with no turn, the context and the device's address claimed with compare-and-swap, no sleep for the RSX thread
	@if grep -nE "sys_rsx_turns|wait_for\\(1000\\)|utils::pause" ../../Emu/Cell/lv2/sys_rsx.cpp ../../Emu/RSX/Core/RSXContext.h; then echo "sys_rsx: the context's turn, a sleep or a spin is back - claimed with compare-and-swap, the RSX thread waited for on its words"; exit 1; else echo "sys_rsx: no turn, no sleep, no spin"; fi
	@# cellGcmSys's IO map: no turn
	@if grep -nE "io_turns|turn_call" ../../Emu/Cell/Modules/cellGcmSys.cpp ../../Emu/Cell/Modules/cellGcmSys.h; then echo "cellGcmSys: the IO map's turn is back - the pages are marked (gcm_io_pages.c)"; exit 1; else echo "cellGcmSys: the IO map takes no turn"; fi
	@# A raw SPU's MFC proxy command: one 128-bit word, each register's write a compare-and-swap; no turn
	@if grep -nE "mfc_prxy_turns|mfc_prxy_write_state" ../../Emu/Cell/SPUThread.h ../../Emu/Cell/SPUThread.cpp ../../Emu/Cell/RawSPUThread.cpp; then echo "RawSPU: the proxy command's turn is back - it is one word (mfc_prxy_word)"; exit 1; else echo "RawSPU: the proxy command one word, no turn"; fi
	@# lv2 lwmutexes and lwconds: the signal and sleepers one word (lv2_lwq.c), an lwcond's waiters a waiter set; no turn, no spin
	@if grep -nE "turn_call|turn_calls|busy_wait|ppu_thread\\* sq|unqueue|change_control" ../../Emu/Cell/lv2/sys_lwmutex.cpp ../../Emu/Cell/lv2/sys_lwmutex.h ../../Emu/Cell/lv2/sys_lwcond.cpp ../../Emu/Cell/lv2/sys_lwcond.h; then echo "sys_lwmutex/sys_lwcond: a turn, a spin or the linked queues are back - the sleepers are lv2_lwq.c's word, an lwcond's waiters a waiter set"; exit 1; else echo "sys_lwmutex/sys_lwcond: no turn, no spin"; fi
	@# lv2 rwlocks: the owners and writers waiting one word, the readers waiting a waiter set (lv2_rwq.c); no turn, no spin
	@if grep -nE "turn_call|turn_calls|busy_wait|ppu_thread\\* [rw]q|unqueue" ../../Emu/Cell/lv2/sys_rwlock.cpp ../../Emu/Cell/lv2/sys_rwlock.h; then echo "sys_rwlock: a turn, a spin or the linked queues are back - the owners and waiters are lv2_rwq.c's"; exit 1; else echo "sys_rwlock: no turn, no spin"; fi
	@# lv2 event flags: the bits a word, the waiters a waiter set, a set claiming each it satisfies (lv2_flagq.c); no turn, no spin
	@if grep -nE "turn_call|turn_calls|busy_wait|ppu_thread\* sq|unqueue" ../../Emu/Cell/lv2/sys_event_flag.cpp ../../Emu/Cell/lv2/sys_event_flag.h; then echo "sys_event_flag: a turn, a spin or the linked queue is back - the bits and waiters are lv2_flagq.c's"; exit 1; else echo "sys_event_flag: no turn, no spin"; fi
	@# lv2 mutexes and conditions: the owner and waiters one word (lv2_ownq.c), a condition's waiters a waiter set (lv2_waitset.c); no turn, no spin
	@if grep -nE "turn_call|turn_calls|busy_wait|control_sq|reown" ../../Emu/Cell/lv2/sys_mutex.cpp ../../Emu/Cell/lv2/sys_mutex.h ../../Emu/Cell/lv2/sys_cond.cpp ../../Emu/Cell/lv2/sys_cond.h; then echo "sys_mutex/sys_cond: a turn, a spin or the linked queue is back - the owner and waiters are lv2_ownq.c's word"; exit 1; else echo "sys_mutex/sys_cond: one word each, no turn, no spin"; fi
	@# The Matching2 room cache: changes posted and applied by one thread at a time (lf_post.h), readers read the copy it publishes
	@if grep -nE "lock_guard|shared_mutex|turn_call|turn_calls" ../../Emu/NP/np_cache.cpp ../../Emu/NP/np_cache.h; then echo "np cache: a lock or a turn is back - changes are posted (lf_post), readers read the published copy"; exit 1; else echo "np cache: changes posted, copies read, no lock, no turn"; fi
	@# The disc locations are the boot's alone: no lock, no turn
	@if grep -nE "m_mutex|lock_guard|turn_call|turn_calls" ../../Emu/games_config.cpp ../../Emu/games_config.h; then echo "games_config: a lock or a turn is back - the list is the boot's alone"; exit 1; else echo "games_config: the boot's, no lock, no turn"; fi
	@# Emulated pad configurations: one thread loads or saves at a time, the polls read the button table under its sequence count
	@if grep -nE "m_mutex|lock_guard|turn_call|turn_calls" ../../Emu/Io/emulated_pad_config.h; then echo "emulated pads: a lock or a turn is back - one thread loads or saves at a time (m_writing), the polls read the table"; exit 1; else echo "emulated pads: no lock, no turn"; fi
	@# NP DNS hook: a fixed table of watched sockets, each with its replies on a lock-free stack; no lock, no turn
	@if grep -nE "lock_guard|shared_mutex|turn_call|turn_calls" ../../Emu/NP/np_dnshook.cpp ../../Emu/NP/np_dnshook.h; then echo "dnshook: a lock or a turn is back - a fixed table of sockets, replies on a lock-free stack each"; exit 1; else echo "dnshook: no lock, no turn"; fi
	@# The matching GUI's room cache: changes posted and applied by one thread at a time (lf_post.h), readers read the snapshot it publishes
	@if grep -nE "lock_guard|shared_mutex|turn_call|turn_calls" ../../Emu/NP/np_gui_cache.cpp ../../Emu/NP/np_gui_cache.h; then echo "np gui cache: a lock or a turn is back - changes are posted (lf_post), readers read a snapshot"; exit 1; else echo "np gui cache: changes posted, snapshots read, no lock, no turn"; fi
	@# The NP libraries' memory pool: lock-free bitmaps of granules (np_pool.c), no turn
	@if grep -nE "lock_guard|shared_mutex|turn_call|turn_calls|std::map<u32, u32> m_allocs" ../../Emu/NP/np_allocator.h; then echo "np allocator: a lock, a turn or the map of allocations is back - the pool is np_pool.c's bitmaps"; exit 1; else echo "np allocator: lock-free bitmaps (np_pool.c), no turn"; fi
	@# The disc locations are kept in memory for the session: no games.yml, no game folder scan
	@if grep -nE "yaml|save_nl|fs::file" ../../Emu/games_config.cpp ../../Emu/games_config.h || grep -nE "AddGamesFromDir|RemoveGameFromYml|AddGameToYml" ../../Emu/System.cpp ../../Emu/System.h; then echo "games_config: games.yml or the game folder scan is back - the core keeps the disc locations in memory"; exit 1; else echo "games_config: in memory, no games.yml"; fi
	@# No Big Picture mode: the core always has content from the frontend
	@if ls ../../Emu/RSX/Overlays/BigPicture ../../Emu/game_enumeration.h ../../Loader/iso_cache.cpp 2>/dev/null || grep -rniE "big_picture|BigPicture" --include=*.cpp --include=*.h ../../Emu ../*.cpp ../*.h; then echo "big picture mode is back - the core has no front end of its own"; exit 1; else echo "no big picture mode"; fi
	@# Always fullscreen: no fullscreen toggle, window events or window-system swapchain in the core
	@if grep -rnE "toggle_fullscreen|swapchain_WSI|wm_event|exclusive_fullscreen|start_fullscreen" --include=*.cpp --include=*.h --include=*.hpp ../../Emu ../*.cpp ../*.h; then echo "a fullscreen/windowed distinction is back - the frontend presents, the core is always fullscreen"; exit 1; else echo "no fullscreen/windowed distinction"; fi
	@# sys_config: the service events and each listener's are changed by posted changes, one thread at a time (lf_post.h); find_event reads a published copy
	@if grep -nE "m_mutex|mutex_service_events|lock_guard|reader_lock|turn_call|turn_calls" ../../Emu/Cell/lv2/sys_config.cpp ../../Emu/Cell/lv2/sys_config.h; then echo "sys_config: a lock or a turn is back - the events and the listeners' events are changed by posted changes (lf_post)"; exit 1; else echo "sys_config: changes posted, no lock, no turn"; fi
	@# The performance meter is gone: no timing statistics kept or logged, no clock read for them
	@if ls ../../Emu/perf_meter.cpp ../../Emu/perf_meter.hpp 2>/dev/null || grep -rnE "perf_meter|perf_stat_base|perf_report" --include=*.cpp --include=*.h --include=*.hpp ../../Emu ../../../Makefile.common; then echo "perf meter: it is back - the core keeps no timing statistics and logs none (AGENTS.md: no runtime logging)"; exit 1; else echo "perf meter: gone, no clock read for it on the hot paths"; fi
	@# LLVM's SHA-1, SHA-256 and old atomics are gone: libretro-common's lrc_hash and retro_atomic stand in for them
	@if ls ../../../3rdparty/llvm/llvm/llvm/lib/Support/SHA1.cpp ../../../3rdparty/llvm/llvm/llvm/lib/Support/SHA256.cpp ../../../3rdparty/llvm/llvm/llvm/lib/Support/Atomic.cpp ../../../3rdparty/llvm/llvm/llvm/include/llvm/Support/SHA1.h ../../../3rdparty/llvm/llvm/llvm/include/llvm/Support/SHA256.h ../../../3rdparty/llvm/llvm/llvm/include/llvm/Support/Atomic.h ../../../3rdparty/llvm/llvm/llvm/lib/Support/CRC.cpp ../../../3rdparty/llvm/llvm/llvm/include/llvm/Support/CRC.h ../../../3rdparty/llvm/llvm/llvm/lib/Support/xxhash.cpp ../../../3rdparty/llvm/llvm/llvm/include/llvm/Support/xxhash.h 2>/dev/null || grep -rn "llvm/Support/SHA1.h\|llvm/Support/SHA256.h\|llvm/Support/Atomic.h\|raw_sha1_ostream\|llvm/Support/CRC.h\|JamCRC\|xxHash64(\|MD5::body\|MD5_u32plus\|llvm/Support/xxhash.h\|xxh3_64bits" ../../../3rdparty/llvm/llvm/llvm/lib ../../../3rdparty/llvm/llvm/llvm/include ../../make/llvm/llvm.mk; then echo "llvm: its own SHA-1, SHA-256, MD5, CRC-32, XXH64, XXH3 or atomics are back - libretro-common's lrc_hash.h, encodings/crc32.h, encodings/xxh64.h, encodings/xxh3.h and retro_atomic are used instead"; exit 1; else echo "llvm: libretro-common's hashes and atomics, not its own"; fi
	@if ls ../../Crypto/md5.cpp ../../Crypto/md5.h ../../Crypto/sha256.cpp ../../Crypto/sha256.h ../../Crypto/sha1.cpp ../../Crypto/sha1.h ../../Crypto/aes.cpp ../../Crypto/aes.h ../../Crypto/aesni.cpp ../../Crypto/aesni.h ../../Crypto/ec.cpp ../../Crypto/ec.h 2>/dev/null || grep -rn "mbedtls_md5\|Crypto/md5\|md5\.h\"\|mbedtls_sha256\|Crypto/sha256\|\"sha256\.h\"\|sha1_context\|sha1_starts\|sha1_hmac\|Crypto/sha1\|\"sha1\.h\"\|aes_context\|aes_setkey_\|aes_crypt_\|rpcs3_aes_cmac\|Crypto/aes\|\"aes\.h\"\|aesni\|ecdsa_\|\"ec\.h\"\|Crypto/ec\.cpp" ../../Loader ../../Emu ../../Crypto ../*.cpp ../../Emu/CMakeLists.txt ../../../Makefile.common; then echo "Crypto: its own MD5, SHA-1, SHA-256 or AES, or the dead ECDSA check, is back - libretro-common's MD5_*, sha1_stream_*, sha256_stream_* (lrc_hash.h), hmac_sha1 (crypto/kdf.h) and aes_* (crypto/crypto.h) are used instead"; exit 1; else echo "Crypto: libretro-common's MD5, SHA-1, SHA-256 and AES, not its own"; fi
	@# The pads take no lock: pad::g_pad_mutex is gone, and a merge of
	@# upstream's code that takes it does not bring it back
	@if grep -rn "g_pad_mutex" ../../Emu ../../Input ../*.cpp ../*.h ../*.c; then echo "pads: pad::g_pad_mutex is back - the pads take no lock (AGENTS.md)"; exit 1; else echo "pads: no pad lock"; fi
	@# Nor does cellGem take one of its own for what it keeps
	@if grep -n "gem\.mtx\|lock(mtx)\|tracker\.mutex\|lock(mutex)" ../../Emu/Cell/Modules/cellGem.cpp; then echo "cellGem: a lock of its own is back - what cellGem and its tracker keep are records (AGENTS.md)"; exit 1; else echo "cellGem: no lock of its own"; fi
	@# cellAudio's ring is the one that lends
	@if grep -n "simple_ringbuf\|SetWriteCallback" ../../Emu/Cell/Modules/cellAudio.h ../../Emu/Cell/Modules/cellAudio.cpp; then echo "cellAudio: upstream's ring or its write callback is back - its audio is lent from libretro_audio_ring (AGENTS.md)"; exit 1; else echo "cellAudio: its ring lends"; fi
	@if grep -rn "simple_ringbuf" ../.. ../../../Utilities --include=*.h --include=*.hpp --include=*.cpp | grep -v "^\.\./\.\./libretro/tests/"; then echo "rings: upstream's simple_ringbuf is back - a ring between two threads is libretro-common's retro_spsc, or libretro_audio_ring for audio (AGENTS.md)"; exit 1; else echo "rings: no simple_ringbuf in the core"; fi
	@# A file's reads and writes share nothing between threads: no counters of the core's own
	@if grep -n "retro_atomic_size_t\|fetch_add_size" ../libretro_vfs.cpp; then echo "VFS: a counter every read or write adds to is back - threads on different files would wait on one another for it (CHANGELOG, 0091)"; exit 1; else echo "VFS: nothing counted"; fi
	@# The lv2 scheduler's wait lists and an event queue's events are retro_deque: no std::deque, no std::vector
	@if grep -n "std::deque<\|std::queue<\|std::vector<std::pair<u64" ../../Emu/Cell/lv2/lv2.cpp ../../Emu/Cell/lv2/sys_event.h ../../Emu/Cell/lv2/sys_event.cpp | grep -v "^[^:]*:[0-9]*:[[:space:]]*//"; then echo "queues: a std::deque or std::queue is back in the scheduler, the event queues, sys_usbd, or another place retro_deque replaced one - it allocates as its contents move along, all through play; libretro-common's retro_deque does not (AGENTS.md)"; exit 1; else echo "queues: no std::deque or std::queue where retro_deque replaced one"; fi
	@if grep -rn "std::deque<\|std::queue<" ../.. --include=*.h --include=*.hpp --include=*.cpp | grep -v "^\.\./\.\./libretro/tests/" | grep -v "^[^:]*:[0-9]*:[[:space:]]*//"; then echo "queues: a std::deque or a std::queue is back in the core - none is left, and one allocates as its contents move along; a queue is libretro-common's retro_deque (AGENTS.md)"; exit 1; else echo "queues: no std::deque or std::queue anywhere in the core"; fi
	@# Shaders are compiled by libretro-common's rslc: no glslang
	@if grep -rn "glslang/\|GlslangToSpv\|3rdparty::glslang\|3rdparty/glslang" ../.. ../../../Makefile ../../../Makefile.common ../../../3rdparty/CMakeLists.txt ../../../.gitmodules --include=*.h --include=*.hpp --include=*.cpp --include=CMakeLists.txt --include=*.cmake --include=Makefile --include=Makefile.common --include=.gitmodules | grep -v "^\.\./\.\./libretro/tests/"; then echo "shaders: glslang is back - the renderer's GLSL goes to SPIR-V through libretro-common's rslc (AGENTS.md)"; exit 1; else echo "shaders: no glslang"; fi
	@# The log is one plain file, and the core tells the logger it has started
	@if grep -n "deflate\|\.gz" ../../util/logs.cpp; then echo "log: a compressed copy of the log is back - it was deflated at the highest level while the game ran (CHANGELOG)"; exit 1; else echo "log: no compressed copy"; fi
	@if ! grep -q "logs::set_init" ../libretro_core.cpp || ! grep -q "set_ceiling(logs::level::fatal)" ../libretro_core.cpp; then echo "log: the core no longer says logs::set_init (every message is then kept in a list under one lock) or no longer turns logging off without the Detailed Log option"; exit 1; else echo "log: started, and off unless asked for"; fi
	@# What a game writes to its TTY goes to the log, when there is one: no TTY.log
	@if grep -rn "g_tty\b\|g_tty_size" ../.. --include=*.h --include=*.hpp --include=*.cpp | grep -v "^\.\./\.\./libretro/tests/"; then echo "TTY: the TTY.log file is back - a write to it for every line a game prints (CHANGELOG)"; exit 1; else echo "TTY: no TTY.log"; fi
	@# The caches that were many small files are packs
	@if grep -n "fs::write_file\|raw/%llX" ../../Emu/RSX/rsx_cache.h | grep -v "import_files\|%s/raw/%llX\.[vf]p\", root_path"; then echo "shader cache: a file a pipeline or a program is back - they are records in the cache's pack (AGENTS.md)"; exit 1; else echo "shader cache: one pack"; fi
	@# No statistics kept for a log on the way of every syscall
	@if grep -rn "ppu_syscall_usage" ../../Emu --include=*.h --include=*.cpp; then echo "syscalls: the usage counter is back - an add to a counter every PPU thread shares on each syscall, and a thread that wakes every second for it (CHANGELOG)"; exit 1; else echo "syscalls: not counted"; fi
	@if ! grep -q "if (ppu.syscall_history.count_debug_arguments) \[\[unlikely\]\] {" ../../Emu/Cell/PPUFunction.h; then echo "calls: every HLE call and syscall writes itself into the thread's history again, kept or not (CHANGELOG)"; exit 1; else echo "calls: history written only when kept"; fi
	@# atomic_t waits and notifies through libretro-common, with no engine of its own
	@if grep -n "futex\|s_hashtable\|NtWaitFor\|cond_handle" ../../util/atomic.cpp; then echo "atomics: RPCS3's own wait engine is back in util/atomic.cpp - waits and notifies go through libretro-common's retro_waitaddr (AGENTS.md)"; exit 1; else echo "atomics: wait and notify through retro_waitaddr"; fi
	@# A file read or write that goes through a buffer of the core's does not allocate one
	@if grep -n "std::vector<uchar> local_buf" ../../Emu/Cell/lv2/sys_fs.cpp; then echo "sys_fs: a vector allocated and zeroed for every read or write is back in op_read/op_write (CHANGELOG)"; exit 1; else echo "sys_fs: no buffer allocated for a read or a write"; fi
	@# The Vulkan shader interpreter's caches are looked up and added to with no lock, and time nothing
	@if grep -n "reader_lock\|shared_mutex\|lock_guard\|std::mutex\|steady_clock\|sleep_for" ../../Emu/RSX/VK/VKShaderInterpreter.cpp ../../Emu/RSX/VK/VKShaderInterpreter.h; then echo "interpreter: a lock, a clock or a sleep is back in the Vulkan shader interpreter - its caches are lock-free, its warm-up waits on its count (AGENTS.md)"; exit 1; else echo "interpreter: no lock, no clock, no sleep"; fi
	@# The pool of spare Vulkan images is put into and taken from with no lock
	@if grep -n "m_cached_pool_lock\|cached_images_erase\|m_cache_is_exiting" ../../Emu/RSX/VK/VKTextureCache.cpp ../../Emu/RSX/VK/VKTextureCache.h; then echo "image pool: the lock on the Vulkan texture cache's pool of spare images is back - it is a lock-free pool (Utilities/lf_item_pool.h)"; exit 1; else echo "image pool: no lock"; fi
	@# The Vulkan framebuffer cache is looked up with no lock
	@if grep -n "reader_lock\|shared_mutex\|lock_guard\|std::mutex\|unordered_map" ../../Emu/RSX/VK/VKFramebuffer.cpp; then echo "framebuffers: a lock is back on the Vulkan framebuffer cache - it is a lock-free table (Utilities/lf_sig_table.h)"; exit 1; else echo "framebuffers: no lock"; fi
	@# What the Vulkan renderer is done with is disposed of and deleted with no lock
	@if grep -n "m_eid_map\|reader_lock\|lock_guard\|shared_mutex\|std::function<void(void\*)>" ../../Emu/RSX/VK/VKResourceManager.h ../../Emu/RSX/VK/vkutils/garbage_collector.h; then echo "disposal: the lock on the Vulkan resource manager's disposal queue is back (CHANGELOG)"; exit 1; else echo "disposal: no lock"; fi
	@# A memory map with no unmap waiting, and a fault with no report page locked, take no lock
	@if grep -n "reader_lock" ../../Emu/RSX/RSXThread.cpp ../../Emu/RSX/RSXZCULL.cpp || grep -n "atomic_storage<u32>::load(m_invalidated_memory_range" ../../Emu/RSX/RSXThread.cpp || ! grep -q "if (!retro_atomic_load_acquire_int(&m_locked_page_count\[location\])) \[\[likely\]\]" ../../Emu/RSX/RSXZCULL.cpp || ! grep -q "if (waiting == no_invalidated_range) \[\[likely\]\]" ../../Emu/RSX/RSXThread.cpp; then echo "rsx notifications: a reader lock is back in RSXThread.cpp or RSXZCULL.cpp (CHANGELOG)"; exit 1; else echo "rsx notifications: no reader lock"; fi
	@# Looking a DMA mapping up and copying through one take no lock: map_dma looks in the views first
	@if grep -n "reader_lock\|g_dma_mutex" ../../Emu/RSX/VK/VKDMA.cpp || ! awk '/dma_mapping_handle map_dma/{f=1} f&&/rsx_dma_views_find_range/{v=1; exit} END{exit !v}' ../../Emu/RSX/VK/VKDMA.cpp; then echo "dma: finding or copying through a DMA mapping takes a lock again (CHANGELOG)"; exit 1; else echo "dma: readers take no lock"; fi
	@# Mutex syscalls find their mutex with no lock over all IDs
	@if grep -n "idm::get<\|idm::check<\|id_manager::g_mutex" ../../Emu/Cell/lv2/sys_mutex.cpp; then echo "ids: sys_mutex.cpp looks a mutex up under the lock over all IDs again - idm::pin and idm::still_named (CHANGELOG)"; exit 1; else echo "ids: sys_mutex takes no ID lock"; fi
	@# A guest write over a plain texture is handed to the renderer as a note, before the texture cache's lock is asked for
	@if ! awk '/^bool VKGSRender::on_access_violation/{f=1} f&&/note_guest_write/{n=NR} f&&/invalidate_address/{i=NR; exit} END{exit !(n&&i&&n<i)}' ../../Emu/RSX/VK/VKGSRender.cpp || ! grep -q "sync_guest_writes(cmd, extras...);" ../../Emu/RSX/Common/texture_cache.h; then echo "guest writes: a guest thread writing over a texture takes the texture cache's lock again (CHANGELOG)"; exit 1; else echo "guest writes: noted, not locked"; fi
	@if ! awk '/^bool VKGSRender::on_access_violation/{f=1} f&&/rsx_guest_faults_post/{p=NR} f&&/invalidate_address/{i=NR; exit} END{exit !(p&&i&&p<i)}' ../../Emu/RSX/VK/VKGSRender.cpp || ! awk '/^bool GLGSRender::on_access_violation/{f=1} f&&/rsx_guest_faults_post/{p=NR} f&&/invalidate_address/{i=NR; exit} END{exit !(p&&i&&p<i)}' ../../Emu/RSX/GL/GLGSRender.cpp || ! grep -q "serve_guest_faults();" ../../Emu/RSX/RSXThread.cpp || grep -q "m_flush_requests.producer_wait\|post_flush_request" ../../Emu/RSX/VK/VKGSRender.cpp ../../Emu/RSX/GL/GLGSRender.cpp || [ "$$(grep -c "handle_invalidated_memory_range();" ../../Emu/RSX/RSXThread.cpp)" != 4 ] || ! grep -q "request_invalidated_memory_handling();" ../../Emu/RSX/RSXThread.cpp; then echo "guest faults: a thread other than the renderer enters the texture cache again, or a paused renderer no longer answers (CHANGELOG)"; exit 1; else echo "guest faults: handed to the renderer"; fi
	@if grep -rn "m_cache_mutex" ../../Emu/RSX --include=*.h --include=*.cpp --include=*.hpp; then echo "texture cache: its lock is back - only the renderer enters the cache, and m_cache_depth says when it is inside (CHANGELOG)"; exit 1; else echo "texture cache: no lock"; fi
	@if awk '/^error_code sys_memory_free/{f=1} f&&/g_mutex\}|g_mutex\)/{bad=1} f&&/^}/{exit} END{exit !bad}' ../../Emu/Cell/lv2/sys_memory.cpp; then echo "sys_memory_free: takes the lock over all IDs again (CHANGELOG)"; exit 1; else echo "sys_memory_free: no ID lock"; fi
	@if awk '/^void spu_int_ctrl_t::set\(/{f=1} f&&/g_mutex/{bad=1} f&&/^}/{exit} END{exit !bad}' ../../Emu/Cell/SPUThread.cpp || ! grep -q "idm::close<lv2_obj, lv2_int_tag>(intrtag)" ../../Emu/Cell/lv2/sys_interrupt.cpp || grep -n "ctrl\.tag = \|int_ctrl\.tag = " ../../Emu/Cell/SPUThread.cpp ../../Emu/Cell/lv2/sys_spu.cpp; then echo "raw SPU interrupts: spu_int_ctrl_t::set takes the lock over all IDs again, a handler is established without letting it finish, or a tag is set without set_tag (CHANGELOG)"; exit 1; else echo "raw SPU interrupts: no ID lock"; fi
	@if grep -n "reader_lock lock(lv2_obj::g_mutex)" ../../Emu/Cell/lv2/sys_ppu_thread.cpp || awk '/^bool lv2_obj::is_scheduler_ready/{f=1} f&&/g_mutex/{bad=1} f&&/^}/{exit} END{exit !bad}' ../../Emu/Cell/lv2/lv2.cpp; then echo "scheduler: reading a priority, or asking whether the scheduler is ready, takes the scheduler's lock again (CHANGELOG)"; exit 1; else echo "scheduler: readers take no lock"; fi
	@if grep -rn --include=*.cpp --include=*.h --include=*.hpp "atomic_t<\|atomic_be_t<\|atomic_le_t<\|util/atomic.hpp" $(ATOMIC_T_FREE); then echo "atomic_t: back in a directory that was moved to retro_atomic - new code calls retro_atomic directly (AGENTS.md)"; exit 1; else echo "atomic_t: gone from $(words $(ATOMIC_T_FREE)) directories"; fi
	@bad=0; for s in $$(grep -oh "g_cfg\.[a-z_]*\.[a-z_0-9]*" ../../Emu/Cell/SPULLVMRecompiler.cpp ../../Emu/Cell/SPUCommonRecompiler.cpp ../../Emu/CPU/CPUTranslator.cpp ../../Emu/CPU/CPUTranslator.h | sort -u); do grep -q "put($$s" ../../Emu/Cell/SPULLVMRecompiler.cpp || { echo "SPU notes: $$s is read by the recompiler and is not part of a note's name (note_name in SPULLVMRecompiler.cpp): an object noted with one value of it would be loaded for another"; bad=1; }; done; if [ $$bad = 1 ]; then exit 1; else echo "SPU notes: named by every setting the recompiler reads"; fi
	@if grep -nE "$(LLVM_LOCK_WORDS)" $(addprefix ../../../3rdparty/llvm/llvm/llvm/,$(LLVM_BUILT) $(LLVM_LOCK_FREE)); then echo "llvm: a lock is back in a file of LLVM's that the core builds - none of them has one (CHANGELOG)"; exit 1; else echo "llvm: no lock in the $(words $(LLVM_BUILT)) files of its libraries that are built"; fi
	@if [ -f ../../../libllvm.a ] && command -v nm >/dev/null 2>&1; then if nm -A -u ../../../libllvm.a 2>/dev/null | grep -E "pthread_mutex|pthread_rwlock|pthread_cond|call_once|__once_|pthread_once|_ZNSt5mutex|_ZNSt15recursive|_ZNSt18condition|CriticalSection|SRWLock"; then echo "llvm: an object of LLVM's calls a lock, a condition or a once-flag of the system's"; exit 1; else echo "llvm: no object in libllvm.a calls a lock of the system's"; fi; fi
	@bad=0; for f in $(filter $(shell grep -v "^#" ../make/llvm/unlinked.txt),$(LLVM_BUILT)); do echo "llvm: $$f is in unlinked.txt and is built all the same (llvm.mk): no link takes anything from it - take it out of one or the other"; bad=1; done; if [ $$bad = 1 ]; then exit 1; else echo "llvm: $$(grep -vc "^#" ../make/llvm/unlinked.txt) files nothing links are not built"; fi
	@if grep -q "^DEPFLAGS := .*-MD)" ../../../Makefile; then echo "llvm: the core's files follow its headers"; else echo "Makefile: the core's files must be made again when a header of the tree's LLVM changes (DEPFLAGS, -MD): its classes are edited, and a core file compiled against one as it was is linked to it as it is"; exit 1; fi
	@if grep -n "Passes/PassBuilder.h\|PassBuilder pb\|PassBuilder(" ../../Emu/Cell/PPUThread.cpp ../../Emu/Cell/SPULLVMRecompiler.cpp ../../../Utilities/JITLLVM.cpp; then echo "llvm: PassBuilder is used again - it brings every pass LLVM has into the core; register what a pass needs in jit_register_analyses (CHANGELOG)"; exit 1; else echo "llvm: only the analyses the recompilers use"; fi
	@# Nor do the mice: no lock in the handler, none taken by those that read them
	@if grep -n "shared_mutex\|std::mutex\|lock_guard\|scoped_lock" ../../Emu/Io/MouseHandler.h ../../Emu/Io/MouseHandler.cpp ../libretro_mouse_handler.h ../libretro_mouse_handler.cpp || grep -rn "handler\.mutex\|take_pending()" ../../Emu/Cell/Modules/cellMouse.cpp ../../Emu/Cell/Modules/cellGem.cpp ../../Emu/Io/usio.cpp ../../Emu/Io/GunCon3.cpp ../../Emu/Io/TopShotElite.cpp ../../Emu/Io/TopShotFearmaster.cpp ../../Emu/Io/GameTablet.cpp; then echo "mice: the mouse handler's lock is back - a mouse is a record read whole (AGENTS.md)"; exit 1; else echo "mice: no mouse lock"; fi

# The C89 modules with a NEON path, built for ARM64 and run under emulation
# (gcc-aarch64-linux-gnu, qemu-user): what the x86 run cannot see of them.
# The module with the build's own standard for C (gnu99, with which GCC
# fuses a multiply and an add on ARM unless told otherwise), the test C89.
ARM64_CC  ?= aarch64-linux-gnu-gcc
ARM64_RUN ?= qemu-aarch64

check-arm64:
	@mkdir -p obj
	$(ARM64_CC) -O3 -std=gnu99 -Wall -I$(LRC)/include -c -o obj/cellAudio_mix.arm64.o ../../Emu/Cell/Modules/cellAudio_mix.c
	$(ARM64_CC) -O2 $(TEST_STD) -I$(LRC)/include -c -o obj/test_audio_mix.arm64.o test_audio_mix.c
	$(ARM64_CC) -static -o obj/test_audio_mix.arm64 obj/test_audio_mix.arm64.o obj/cellAudio_mix.arm64.o -lm
	$(ARM64_RUN) obj/test_audio_mix.arm64

clean:
	rm -rf obj $(TESTS) *_out.* rec_out host_lrc_win

.PHONY: all check check-arm64 clean
