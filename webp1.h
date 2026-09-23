/* ============================================================================
 * webp1.h - complete single-header WebP library (decode + encode)
 * Version 1.0.0
 *
 * Single C99 header, no dependencies (only <stdint.h>, <stddef.h>,
 * <string.h>), no heap usage (caller-provided buffers), no globals
 * (only static const tables), deterministic output, no threads.
 * Every function is `static`, so including this header (with or without
 * WEBP1_IMPLEMENTATION) in multiple translation units is safe.
 *
 * Usage:
 *   #define WEBP1_IMPLEMENTATION   // in exactly one TU (or several; safe)
 *   #include "webp1.h"
 *
 * Coverage:
 *   - RIFF container: simple lossy (VP8 ), simple lossless (VP8L),
 *     extended (VP8X) with ICCP / ANIM / ANMF / ALPH / VP8 / VP8L /
 *     EXIF / XMP and unknown-chunk skipping.
 *   - Lossy: VP8 key frames (RFC 6386), intra only (4x4 + 16x16 luma,
 *     8x8 chroma), 1/2/4/8 token partitions, segmentation, coef updates,
 *     skip blocks, simple + normal loop filters, BT.601 YUV<->RGB.
 *   - Lossless: VP8L incl. all 4 transforms, color cache, meta Huffman,
 *     LZ77 + plane codes; ALPH (raw + VP8L-compressed + filters).
 *   - Animation: full canvas assembly (blend / dispose / durations /
 *     loop count / background color).
 *   - Metadata passthrough: EXIF / XMP / ICCP readable on decode and
 *     writable on encode.
 *   - Encoder: lossless (VP8L) and lossy (VP8 intra key frames),
 *     stills + animation + metadata.
 *
 * Memory model (no heap):
 *   Decode/encode take caller buffers: output + scratch `work` buffer.
 *   If `work` is too small the call fails with WEBP1_ERR_NO_MEMORY and
 *   stores the recommended size in `*need` (when `need != NULL`); retry
 *   with at least that many bytes (deterministic, terminates).
 *   webp1_decode_work_bound() / webp1_encode_work_bound() give good
 *   first-try sizes. Output sizing: width*height*4 bytes for RGBA
 *   (webp1_rgba_size), webp1_encode_bound() for encoded bytes.
 *
 * Pixel format: 8-bit RGBA, row-major, top-down, non-premultiplied.
 * Encoder input stride is in bytes (0 == tight, width*4).
 *
 * Limits: width/height in [1,16384], pixels <= 2^32-1 (container rule).
 * webp1_encode_lossy() is limited to 16383 per side (VP8 stores the coded size
 * in 14 bits); use webp1_encode_lossless() above that.
 * VP8 interframes are rejected (still/animation WebP uses key frames).
 *
 * Spec references: RFC 6386 (VP8), WebP Container Specification,
 * WebP Lossless Bitstream Specification.
 *
 * Sections (implementation order; S-numbers mark the banner of each part):
 *   S0  Interface .......... config macros, error codes, public types + decls
 *   S1  Setup .............. helper macros, shared tables, bump allocator
 *   S2  Bit I/O ............ VP8L bit reader/writer, VP8 bool codec
 *   S3  VP8L decoder ....... lossless bitstream -> ARGB
 *   S4  VP8 decoder ........ lossy keyframes -> YUV -> RGBA
 *   S5  ALPH decoder ....... transparency chunk
 *   S6  Container + animation  RIFF/VP8X parsing, compositor, decode API
 *   S7  VP8L encoder ....... lossless search + emission
 *   S8  VP8 encoder ........ lossy analysis, RD search, token writing
 *   S9  Mux + encode API ... RIFF assembly, work bounds, public encoders
 * ========================================================================== */
#ifndef WEBP1_H
#define WEBP1_H

/* == S0: Interface (config macros, error codes, public types + decls) == */

#include <stddef.h>
#include <stdint.h>

#include <string.h>
#ifndef W1_CT_SB
/* Cross-color transform tile size (log2 pixels). The coefficient image is
 * a full VP8L sub-image, so its cost grows with the tile grid; on a real
 * camera photo 4x4 tiles cost 24 KB there while 32x32 tiles decorrelate
 * nearly as well and cost ~0.3 KB (385228 -> 362798 bytes). The synthetic
 * corpus is byte-identical or marginally smaller at 32x32. */
#define W1_CT_SB 5
#endif

#define WEBP1_VERSION_MAJOR 1
#define WEBP1_VERSION_MINOR 0
#define WEBP1_VERSION_PATCH 0
#define WEBP1_VERSION ((WEBP1_VERSION_MAJOR << 16) | \
                       (WEBP1_VERSION_MINOR << 8) | WEBP1_VERSION_PATCH)

/* Error codes (0 == success). */
#define WEBP1_OK 0
#define WEBP1_ERR_BAD_PARAM 1    /* NULL pointer, bad dims, bad stride, ... */
#define WEBP1_ERR_TOO_LARGE 2    /* dimensions / allocation size overflow */
#define WEBP1_ERR_TRUNCATED 3    /* input ends unexpectedly */
#define WEBP1_ERR_BAD_MAGIC 4    /* not a RIFF/WEBP file */
#define WEBP1_ERR_UNSUPPORTED 5  /* valid but unsupported (VP8 interframe,
                                    bad version, ...) */
#define WEBP1_ERR_CORRUPT 6      /* invalid bitstream contents */
#define WEBP1_ERR_NO_MEMORY 7    /* work buffer too small (*need set) */
#define WEBP1_ERR_OUTPUT_FULL 8  /* output buffer too small */

/* Cap on frames accepted by webp1_encode_anim() (the container itself has no
 * frame limit; this keeps bound arithmetic and buffers sane). */
#define WEBP1_MAX_ANIM_FRAMES 65535

#define WEBP1_MAX_DIM 16384

#define WEBP1_FOURCC(a, b, c, d) \
  ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | \
   ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

typedef struct webp1_info {
  int width, height;   /* canvas dimensions */
  int has_alpha;       /* transparency present */
  int is_lossless;     /* stills: 1 == VP8L, 0 == VP8; animated: -1 (mixed) */
  int is_animated;
  int frame_count;     /* 1 for stills */
  int loop_count;      /* ANIM loop count (0 == infinite); 0 for stills */
  uint32_t bg_color;   /* ANIM background as little-endian u32, byte order
                          B,G,R,A; 0 when no ANIM chunk */
  int has_exif, has_xmp, has_iccp;
} webp1_info_t;

typedef struct webp1_frame {
  int x, y, w, h;      /* frame rectangle on canvas */
  int duration_ms;
  int blend;           /* 1 == alpha-blend onto canvas, 0 == overwrite */
  int dispose;         /* 0 == keep, 1 == dispose to background */
  int has_alpha;
  int is_lossless;     /* 1 == VP8L, 0 == VP8 */
} webp1_frame_t;

typedef struct webp1_encode_opts {
  int quality;         /* lossy: 0..100 (default 75) */
  int lossless_level;  /* 0..9 effort/speed tradeoff (default 6); lossless
                          stream search depth, and for lossy the quantizer
                          sweep / RD refinement breadth (see w1_lossy_effort) */
  const uint8_t *iccp; size_t iccp_len;
  const uint8_t *exif; size_t exif_len;
  const uint8_t *xmp;  size_t xmp_len;
} webp1_encode_opts_t;

typedef struct webp1_anim_in {
  const uint8_t *rgba; /* full-canvas frame */
  size_t stride;       /* bytes per row (0 == tight) */
  int duration_ms;     /* >= 0 */
} webp1_anim_in_t;

static const char *webp1_error_name(int code);
static void webp1_encode_opts_init(webp1_encode_opts_t *o);
static size_t webp1_rgba_size(int w, int h);  /* w*h*4, or 0 on overflow */

static int webp1_info(const uint8_t *data, size_t size, webp1_info_t *info);
static size_t webp1_decode_work_bound(const webp1_info_t *info);
static int webp1_decode_rgba(const uint8_t *data, size_t size,
                             uint8_t *out, size_t out_cap,
                             uint8_t *work, size_t work_cap, size_t *need,
                             int *out_w, int *out_h);
static int webp1_frame_info(const uint8_t *data, size_t size, int index,
                            webp1_frame_t *frame);
/* Composite animation frames 0..index onto canvas (cleared to background
 * first). canvas_cap must hold width*height*4 bytes.
 * S4 cost note: each call recomposes frames 0..index, so decoding all N
 * frames one index at a time costs O(N^2) frame work. Callers playing every
 * frame should bound N or reuse a single composited canvas incrementally
 * rather than calling per index in a loop. Outputs are unchanged. */
static int webp1_anim_decode(const uint8_t *data, size_t size, int index,
                             uint8_t *canvas, size_t canvas_cap,
                             uint8_t *work, size_t work_cap, size_t *need);
/* Find the chunk_index-th (0-based) chunk with FourCC `fourcc` at top
 * level (payload pointer valid while `data` is). Absent chunk ->
 * success with *payload == NULL and *payload_len == 0. */
static int webp1_find_chunk(const uint8_t *data, size_t size, uint32_t fourcc,
                            int chunk_index, const uint8_t **payload,
                            size_t *payload_len);

static size_t webp1_encode_bound(int w, int h, int lossless,
                                 const webp1_encode_opts_t *o);
static size_t webp1_encode_work_bound(int w, int h, int lossless);
static int webp1_encode_lossless(const uint8_t *rgba, int w, int h,
                                 size_t stride,
                                 const webp1_encode_opts_t *o,
                                 uint8_t *out, size_t out_cap,
                                 size_t *out_len, uint8_t *work,
                                 size_t work_cap, size_t *need);
static int webp1_encode_lossy(const uint8_t *rgba, int w, int h, size_t stride,
                              const webp1_encode_opts_t *o,
                              uint8_t *out, size_t out_cap, size_t *out_len,
                              uint8_t *work, size_t work_cap, size_t *need);
static size_t webp1_encode_anim_bound(int n_frames, int w, int h,
                                      int lossless,
                                      const webp1_encode_opts_t *o);
static int webp1_encode_anim(const webp1_anim_in_t *frames, int n_frames,
                             int w, int h, int loop_count, uint32_t bg_color,
                             int lossless, const webp1_encode_opts_t *o,
                             uint8_t *out, size_t out_cap, size_t *out_len,
                             uint8_t *work, size_t work_cap, size_t *need);

#endif /* WEBP1_H */

#ifdef WEBP1_IMPLEMENTATION

/* == S1: Setup (helper macros, shared tables, bump allocator) == */

#if defined(__GNUC__) || defined(__clang__)
#define W1_UNUSED __attribute__((unused))
#else
#define W1_UNUSED
#endif
/* Force inlining for the small per-bit helpers the optimiser declines to
 * inline on its own (bit emission and cost accounting in hot loops). */
#if defined(_MSC_VER)
#define W1_FORCEINLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define W1_FORCEINLINE static __attribute__((always_inline)) inline
#else
#define W1_FORCEINLINE static
#endif
/* Auto-generated from RFC 6386 (values verified equal). Do not edit. */
static const uint8_t w1k_vp8_coef_upd[1056] = {
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255,
  249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255,
  234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255,
  250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255,
  254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255,
  234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255,
  255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255,
  255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255,
  234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255,
  251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255,
  255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255,
  255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255,
  248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255,
  255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255,
  255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255,
  248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255,
  255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255,
  250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
};
static const uint8_t w1k_vp8_coef_dflt[1056] = {
  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128,
  189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128,
  106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128,
    1,  98, 248, 255, 236, 226, 255, 255, 128, 128, 128,
  181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128,
   78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128,
    1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128,
  184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128,
   77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128,
    1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128,
  170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128,
   37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128,
    1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128,
  207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128,
  102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128,
    1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128,
  177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128,
   80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128,
    1,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  246,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  198,  35, 237, 223, 193, 187, 162, 160, 145, 155,  62,
  131,  45, 198, 221, 172, 176, 220, 157, 252, 221,   1,
   68,  47, 146, 208, 149, 167, 221, 162, 255, 223, 128,
    1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128,
  184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128,
   81,  99, 181, 242, 176, 190, 249, 202, 255, 255, 128,
    1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128,
   99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128,
   23,  91, 163, 242, 170, 187, 247, 210, 255, 255, 128,
    1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128,
  109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128,
   44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128,
    1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128,
   94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128,
   22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128,
    1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128,
  124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128,
   35,  77, 181, 251, 193, 211, 255, 205, 128, 128, 128,
    1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128,
  121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128,
   45,  99, 188, 251, 195, 217, 255, 224, 128, 128, 128,
    1,   1, 251, 255, 213, 255, 128, 128, 128, 128, 128,
  203,   1, 248, 255, 255, 128, 128, 128, 128, 128, 128,
  137,   1, 177, 255, 224, 255, 128, 128, 128, 128, 128,
  253,   9, 248, 251, 207, 208, 255, 192, 128, 128, 128,
  175,  13, 224, 243, 193, 185, 249, 198, 255, 255, 128,
   73,  17, 171, 221, 161, 179, 236, 167, 255, 234, 128,
    1,  95, 247, 253, 212, 183, 255, 255, 128, 128, 128,
  239,  90, 244, 250, 211, 209, 255, 255, 128, 128, 128,
  155,  77, 195, 248, 188, 195, 255, 255, 128, 128, 128,
    1,  24, 239, 251, 218, 219, 255, 205, 128, 128, 128,
  201,  51, 219, 255, 196, 186, 128, 128, 128, 128, 128,
   69,  46, 190, 239, 201, 218, 255, 228, 128, 128, 128,
    1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128,
  223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128,
  141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128,
    1,  16, 248, 255, 255, 128, 128, 128, 128, 128, 128,
  190,  36, 230, 255, 236, 255, 128, 128, 128, 128, 128,
  149,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
    1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128,
    1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128,
  213,  62, 250, 255, 255, 128, 128, 128, 128, 128, 128,
   55,  93, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
  202,  24, 213, 235, 186, 191, 220, 160, 240, 175, 255,
  126,  38, 182, 232, 169, 184, 228, 174, 255, 187, 128,
   61,  46, 138, 219, 151, 178, 240, 170, 255, 216, 128,
    1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128,
  166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128,
   39,  77, 162, 232, 172, 180, 245, 178, 255, 255, 128,
    1,  52, 220, 246, 198, 199, 249, 220, 255, 255, 128,
  124,  74, 191, 243, 183, 193, 250, 221, 255, 255, 128,
   24,  71, 130, 219, 154, 170, 243, 182, 255, 255, 128,
    1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128,
  149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128,
   28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128,
    1,  81, 230, 252, 204, 203, 255, 192, 128, 128, 128,
  123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128,
   20,  95, 153, 243, 164, 173, 255, 203, 128, 128, 128,
    1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128,
  168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128,
   47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128,
    1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128,
  141,  84, 213, 252, 201, 202, 255, 219, 128, 128, 128,
   42,  80, 160, 240, 162, 185, 255, 205, 128, 128, 128,
    1,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  244,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128,
  238,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128
};
static const uint8_t w1k_vp8_kf_bmode[900] = {
  231, 120,  48,  89, 115, 113, 120, 152, 112,
  152, 179,  64, 126, 170, 118,  46,  70,  95,
  175,  69, 143,  80,  85,  82,  72, 155, 103,
   56,  58,  10, 171, 218, 189,  17,  13, 152,
  144,  71,  10,  38, 171, 213, 144,  34,  26,
  114,  26,  17, 163,  44, 195,  21,  10, 173,
  121,  24,  80, 195,  26,  62,  44,  64,  85,
  170,  46,  55,  19, 136, 160,  33, 206,  71,
   63,  20,   8, 114, 114, 208,  12,   9, 226,
   81,  40,  11,  96, 182,  84,  29,  16,  36,
  134, 183,  89, 137,  98, 101, 106, 165, 148,
   72, 187, 100, 130, 157, 111,  32,  75,  80,
   66, 102, 167,  99,  74,  62,  40, 234, 128,
   41,  53,   9, 178, 241, 141,  26,   8, 107,
  104,  79,  12,  27, 217, 255,  87,  17,   7,
   74,  43,  26, 146,  73, 166,  49,  23, 157,
   65,  38, 105, 160,  51,  52,  31, 115, 128,
   87,  68,  71,  44, 114,  51,  15, 186,  23,
   47,  41,  14, 110, 182, 183,  21,  17, 194,
   66,  45,  25, 102, 197, 189,  23,  18,  22,
   88,  88, 147, 150,  42,  46,  45, 196, 205,
   43,  97, 183, 117,  85,  38,  35, 179,  61,
   39,  53, 200,  87,  26,  21,  43, 232, 171,
   56,  34,  51, 104, 114, 102,  29,  93,  77,
  107,  54,  32,  26,  51,   1,  81,  43,  31,
   39,  28,  85, 171,  58, 165,  90,  98,  64,
   34,  22, 116, 206,  23,  34,  43, 166,  73,
   68,  25, 106,  22,  64, 171,  36, 225, 114,
   34,  19,  21, 102, 132, 188,  16,  76, 124,
   62,  18,  78,  95,  85,  57,  50,  48,  51,
  193, 101,  35, 159, 215, 111,  89,  46, 111,
   60, 148,  31, 172, 219, 228,  21,  18, 111,
  112, 113,  77,  85, 179, 255,  38, 120, 114,
   40,  42,   1, 196, 245, 209,  10,  25, 109,
  100,  80,   8,  43, 154,   1,  51,  26,  71,
   88,  43,  29, 140, 166, 213,  37,  43, 154,
   61,  63,  30, 155,  67,  45,  68,   1, 209,
  142,  78,  78,  16, 255, 128,  34, 197, 171,
   41,  40,   5, 102, 211, 183,   4,   1, 221,
   51,  50,  17, 168, 209, 192,  23,  25,  82,
  125,  98,  42,  88, 104,  85, 117, 175,  82,
   95,  84,  53,  89, 128, 100, 113, 101,  45,
   75,  79, 123,  47,  51, 128,  81, 171,   1,
   57,  17,   5,  71, 102,  57,  53,  41,  49,
  115,  21,   2,  10, 102, 255, 166,  23,   6,
   38,  33,  13, 121,  57,  73,  26,   1,  85,
   41,  10,  67, 138,  77, 110,  90,  47, 114,
  101,  29,  16,  10,  85, 128, 101, 196,  26,
   57,  18,  10, 102, 102, 213,  34,  20,  43,
  117,  20,  15,  36, 163, 128,  68,   1,  26,
  138,  31,  36, 171,  27, 166,  38,  44, 229,
   67,  87,  58, 169,  82, 115,  26,  59, 179,
   63,  59,  90, 180,  59, 166,  93,  73, 154,
   40,  40,  21, 116, 143, 209,  34,  39, 175,
   57,  46,  22,  24, 128,   1,  54,  17,  37,
   47,  15,  16, 183,  34, 223,  49,  45, 183,
   46,  17,  33, 183,   6,  98,  15,  32, 183,
   65,  32,  73, 115,  28, 128,  23, 128, 205,
   40,   3,   9, 115,  51, 192,  18,   6, 223,
   87,  37,   9, 115,  59,  77,  64,  21,  47,
  104,  55,  44, 218,   9,  54,  53, 130, 226,
   64,  90,  70, 205,  40,  41,  23,  26,  57,
   54,  57, 112, 184,   5,  41,  38, 166, 213,
   30,  34,  26, 133, 152, 116,  10,  32, 134,
   75,  32,  12,  51, 192, 255, 160,  43,  51,
   39,  19,  53, 221,  26, 114,  32,  73, 255,
   31,   9,  65, 234,   2,  15,   1, 118,  73,
   88,  31,  35,  67, 102,  85,  55, 186,  85,
   56,  21,  23, 111,  59, 205,  45,  37, 192,
   55,  38,  70, 124,  73, 102,   1,  34,  98,
  102,  61,  71,  37,  34,  53,  31, 243, 192,
   69,  60,  71,  38,  73, 119,  28, 222,  37,
   68,  45, 128,  34,   1,  47,  11, 245, 171,
   62,  17,  19,  70, 146,  85,  55,  62,  70,
   75,  15,   9,   9,  64, 255, 184, 119,  16,
   37,  43,  37, 154, 100, 163,  85, 160,   1,
   63,   9,  92, 136,  28,  64,  32, 201,  85,
   86,   6,  28,   5,  64, 255,  25, 248,   1,
   56,   8,  17, 132, 137, 255,  55, 116, 128,
   58,  15,  20,  82, 135,  57,  26, 121,  40,
  164,  50,  31, 137, 154, 133,  25,  35, 218,
   51, 103,  44, 131, 131, 123,  31,   6, 158,
   86,  40,  64, 135, 148, 224,  45, 183, 128,
   22,  26,  17, 131, 240, 154,  14,   1, 209,
   83,  12,  13,  54, 192, 255,  68,  47,  28,
   45,  16,  21,  91,  64, 222,   7,   1, 197,
   56,  21,  39, 155,  60, 138,  23, 102, 213,
   85,  26,  85,  85, 128, 128,  32, 146, 171,
   18,  11,   7,  63, 144, 171,   4,   4, 246,
   35,  27,  10, 146, 174, 171,  12,  26, 128,
  190,  80,  35,  99, 180,  80, 126,  54,  45,
   85, 126,  47,  87, 176,  51,  41,  20,  32,
  101,  75, 128, 139, 118, 146, 116, 128,  85,
   56,  41,  15, 176, 236,  85,  37,   9,  62,
  146,  36,  19,  30, 171, 255,  97,  27,  20,
   71,  30,  17, 119, 118, 255,  17,  18, 138,
  101,  38,  60, 138,  55,  70,  43,  26, 142,
  138,  45,  61,  62, 219,   1,  81, 188,  64,
   32,  41,  20, 117, 151, 142,  20,  21, 163,
  112,  19,  12,  61, 195, 128,  48,   4,  24
};
static const uint16_t w1k_vp8_dc_q[128] = {
    4,   5,   6,   7,   8,   9,  10,  10,  11,  12,  13,  14,  15,
   16,  17,  17,  18,  19,  20,  20,  21,  21,  22,  22,  23,  23,
   24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,
   36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  46,
   47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
   60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,
   73,  74,  75,  76,  76,  77,  78,  79,  80,  81,  82,  83,  84,
   85,  86,  87,  88,  89,  91,  93,  95,  96,  98, 100, 101, 102,
  104, 106, 108, 110, 112, 114, 116, 118, 122, 124, 126, 128, 130,
  132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157
};
static const uint16_t w1k_vp8_ac_q[128] = {
    4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,  16,
   17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
   30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
   43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,
   56,  57,  58,  60,  62,  64,  66,  68,  70,  72,  74,  76,  78,
   80,  82,  84,  86,  88,  90,  92,  94,  96,  98, 100, 102, 104,
  106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137,
  140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173, 177,
  181, 185, 189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229,
  234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284
};
static const int8_t w1k_vp8l_plane_dx[120] = {
  0, 1, 1, -1, 0, 2, 1, -1, 2, -2, 2, -2, 0, 3, 1, -1, 3, -3, 2, -2, 3, -3, 0, 4, 1, -1, 4, -4, 3, -3, 2, -2, 4, -4, 0, 3, -3, 4, -4, 5, 1, -1, 5, -5, 2, -2, 5, -5, 4, -4, 3, -3, 5, -5, 0, 6, 1, -1, 6, -6, 2, -2, 6, -6, 4, -4, 5, -5, 3, -3, 6, -6, 0, 7, 1, -1, 5, -5, 7, -7, 4, -4, 6, -6, 2, -2, 7, -7, 3, -3, 7, -7, 5, -5, 6, -6, 8, 4, -4, 7, -7, 8, 8, 6, -6, 8, 5, -5, 7, -7, 8, 6, -6, 7, -7, 8, 7, -7, 8, 8
};
static const int8_t w1k_vp8l_plane_dy[120] = {
  1, 0, 1, 1, 2, 0, 2, 2, 1, 1, 2, 2, 3, 0, 3, 3, 1, 1, 3, 3, 2, 2, 4, 0, 4, 4, 1, 1, 3, 3, 4, 4, 2, 2, 5, 4, 4, 3, 3, 0, 5, 5, 1, 1, 5, 5, 2, 2, 4, 4, 5, 5, 3, 3, 6, 0, 6, 6, 1, 1, 6, 6, 2, 2, 5, 5, 4, 4, 6, 6, 3, 3, 7, 0, 7, 7, 5, 5, 1, 1, 6, 6, 4, 4, 7, 7, 2, 2, 7, 7, 3, 3, 6, 6, 5, 5, 0, 7, 7, 4, 4, 1, 2, 6, 6, 3, 7, 7, 5, 5, 4, 7, 7, 6, 6, 5, 7, 7, 6, 7
};
/* ---- Small static tables (values per RFC 6386 / VP8L spec) ---- */

/* Intra MB modes: DC=0 V=1 H=2 TM=3 B=4. Intra 4x4 modes: DC=0 TM=1 VE=2
 * HE=3 LD=4 RD=5 VR=6 VL=7 HD=8 HU=9. */
static const int8_t w1k_vp8_kf_ymode_tree[8] = { -4, 2, 4, 6, 0, -1, -2, -3 };
static const uint8_t w1k_vp8_kf_ymode_prob[4] = { 145, 156, 163, 128 };
static const int8_t w1k_vp8_uv_mode_tree[6] = { 0, 2, -1, 4, -2, -3 };
static const uint8_t w1k_vp8_kf_uv_prob[3] = { 142, 114, 183 };
static const int8_t w1k_vp8_bmode_tree[18] = {
  0, 2, -1, 4, -2, 6, 8, 12, -3, 10, -5, -6, -4, 14, -7, 16, -8, -9
};
static const int8_t w1k_vp8_seg_tree[6] = { 2, 4, 0, -1, -2, -3 };
/* 16x16 mode -> derived subblock mode (for neighbor contexts). */
static const uint8_t w1k_vp8_bmode_from_ymode[4] = { 0, 2, 3, 1 };

/* DCT token tree + ids: ZERO=0 ONE=1 TWO=2 THREE=3 FOUR=4 CAT1..6=5..10,
 * EOB=11. */
static const int8_t w1k_vp8_coeff_tree[22] = {
  -11, 2, 0, 4, -1, 6, 8, 12, -2, 10, -3, -4,
  14, 16, -5, -6, 18, 20, -7, -8, -9, -10
};
static const uint8_t w1k_vp8_pcat1[1] = { 159 };
static const uint8_t w1k_vp8_pcat2[2] = { 165, 145 };
static const uint8_t w1k_vp8_pcat3[3] = { 173, 148, 140 };
static const uint8_t w1k_vp8_pcat4[4] = { 176, 155, 140, 135 };
static const uint8_t w1k_vp8_pcat5[5] = { 180, 157, 141, 134, 130 };
static const uint8_t w1k_vp8_pcat6[11] =
  { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129 };
static const uint8_t *W1_UNUSED w1k_vp8_pcat_ptr[6] = {
  w1k_vp8_pcat1, w1k_vp8_pcat2, w1k_vp8_pcat3,
  w1k_vp8_pcat4, w1k_vp8_pcat5, w1k_vp8_pcat6
};
static const uint8_t w1k_vp8_pcat_nbits[6] = { 1, 2, 3, 4, 5, 11 };
static const uint16_t w1k_vp8_cat_base[6] = { 5, 7, 11, 19, 35, 67 };

static const uint8_t w1k_vp8_bands[16] =
  { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7 };
static const uint8_t w1k_vp8_zigzag[16] =
  { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t w1k_vp8_ctx_left[25] = {
  0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
  4, 4, 5, 5, 6, 6, 7, 7, 8
};
static const uint8_t w1k_vp8_ctx_above[25] = {
  0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
  4, 5, 4, 5, 6, 7, 6, 7, 8
};

/* VP8L: code-length-code order + RLE params. */
static const uint8_t w1k_vp8l_cl_order[19] =
  { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/* BT.601 full-range fixed-point constants (13-bit fraction). */
/* plane code (1..120) for (dx,dy), 0 if none. Auto-generated. */
static const uint8_t w1k_vp8l_plane_rev[8][17] = {
  {   0,   0,   0,   0,   0,   0,   0,   0,   0,   2,   6,  14,  24,  40,  56,  74,  97 },
  {   0,  80,  60,  44,  28,  18,  10,   4,   1,   3,   9,  17,  27,  43,  59,  79, 102 },
  {   0,  88,  64,  48,  34,  22,  12,   8,   5,   7,  11,  21,  33,  47,  63,  87, 103 },
  {   0,  92,  72,  54,  39,  30,  20,  16,  13,  15,  19,  29,  38,  53,  71,  91, 106 },
  {   0, 101,  84,  68,  50,  37,  32,  26,  23,  25,  31,  36,  49,  67,  83, 100, 111 },
  {   0, 110,  96,  78,  66,  52,  46,  42,  35,  41,  45,  51,  65,  77,  95, 109, 116 },
  {   0, 115, 105,  94,  82,  70,  62,  58,  55,  57,  61,  69,  81,  93, 104, 114, 119 },
  {   0, 118, 113, 108,  99,  90,  86,  76,  73,  75,  85,  89,  98, 107, 112, 117, 120 },
};
/* ---- Utils: safe math, LE access, bump allocator ---- */

static W1_UNUSED int w1_mul_overflows_size(size_t a, size_t b) {
  return a != 0 && b > (size_t)-1 / a;
}

/* Portable arithmetic shift right (floor divide by 2^k), exact match for
 * two's-complement `>>` on negatives without relying on it. */
static W1_UNUSED int w1_asr(int v, int k) {
  if (v >= 0) return v >> k;
  return -((((-v) - 1) >> k) + 1);
}

static W1_UNUSED int w1_clamp255(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static W1_UNUSED uint16_t w1_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static W1_UNUSED uint32_t w1_le24(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static W1_UNUSED uint32_t w1_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bump allocator over the caller work buffer. */
typedef struct w1_bump {
  uint8_t *base;
  size_t cap;
  size_t used;
  size_t need;   /* recommended total cap if allocation failed */
} w1_bump_t;

static W1_UNUSED void w1_bump_init(w1_bump_t *b, uint8_t *base, size_t cap) {
  if (base) {
    /* Align base so every allocation out of this arena is safe for
     * any <= 8-byte-aligned type even if the caller buffer is not. */
    uintptr_t a = (uintptr_t)base;
    size_t adj = (size_t)((8 - (a & 7u)) & 7u);
    if (adj > cap) adj = cap;
    base += adj; cap -= adj;
  }
  b->base = base; b->cap = cap; b->used = 0; b->need = 0;
}
static W1_UNUSED void *w1_bump_alloc(w1_bump_t *b, size_t n, size_t align) {
  size_t off = b->used;
  size_t m;
  if (align < 8) align = 8;   /* pointers/uint64 need 8 */
  m = align - 1;
  if (align && (off & m)) {
    if (off > (size_t)-1 - m) return NULL;
    off = (off + m) & ~m;
  }
  if (n > (size_t)-1 - off) return NULL;
  if (off + n > b->cap || !b->base) {
    size_t want = off + n;
    /* Deterministic slack so callers converge in few retries. */
    if (want <= ((size_t)-1 - 4096) / 5 * 4)
      want += (want >> 2) + 4096;
    if (want > b->need) b->need = want;
    return NULL;
  }
  b->used = off + n;
  return b->base + off;
}

/* Byte order: on a known little-endian target the ARGB -> RGBA output
 * stage is a single word store per pixel instead of four byte stores.
 * Every other target keeps the byte stores, so behaviour is identical. */
#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) ||                      \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&       \
     __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define W1_LITTLE_ENDIAN 1
#endif

/* == S2: Bit I/O (VP8L bit reader/writer, VP8 bool codec) == */

/* ---- LSB-first bit reader (VP8L) ---- */
typedef struct w1_br {
  const uint8_t *p;
  const uint8_t *end;
  uint32_t buf;
  int nbits;
  int eof;   /* sticky: read past end */
} w1_br_t;

static W1_UNUSED void w1_br_init(w1_br_t *r, const uint8_t *p, size_t n) {
  r->p = p; r->end = p + n; r->buf = 0; r->nbits = 0; r->eof = 0;
}
static W1_UNUSED uint32_t w1_br_bits(w1_br_t *r, int n) {
  uint32_t v;
  if (n == 0) return 0;
  if (n > 24) {
    uint32_t low = w1_br_bits(r, 16);
    return low | (w1_br_bits(r, n - 16) << 16);
  }
  while (r->nbits <= 24 && r->p < r->end) {
    r->buf |= (uint32_t)(*r->p++) << r->nbits;
    r->nbits += 8;
  }
  if (r->nbits >= n) {
    v = r->buf & (n == 32 ? 0xffffffffu : ((1u << n) - 1u));
    r->buf = n == 32 ? 0 : r->buf >> n;
    r->nbits -= n;
    return v;
  }
  v = r->buf;  /* whatever is left, zero-padded above */
  r->buf = 0;
  r->nbits = 0;
  r->eof = 1;
  return v;
}

/* ---- LSB-first bit writer (VP8L) ---- */
typedef struct w1_bw {
  uint8_t *p;
  uint8_t *end;
  uint32_t buf;
  int nbits;
  int err;   /* sticky overflow */
  size_t bytes;
} w1_bw_t;

static W1_UNUSED void w1_bw_init(w1_bw_t *w, uint8_t *out, size_t cap) {
  w->p = out; w->end = out ? out + cap : NULL;
  w->buf = 0; w->nbits = 0; w->err = 0; w->bytes = 0;
}
static W1_UNUSED void w1_bw_put(w1_bw_t *w, uint32_t v, int n) {
  if (w->err || n <= 0) return;
  if (!w->p) {
    w->bytes += (size_t)(w->nbits + n) >> 3;
    w->nbits = (w->nbits + n) & 7;
    return;
  }
  /* 16-bit pieces: vv << nbits never exceeds 23 bits. */
  while (n > 0) {
    int k = n > 16 ? 16 : n;
    w->buf |= (v & ((1u << (unsigned)k) - 1u)) << (unsigned)w->nbits;
    w->nbits += k;
    v >>= (unsigned)k; n -= k;
    while (w->nbits >= 8) {
      if (w->p >= w->end) { w->err = 1; return; }
      *w->p++ = (uint8_t)w->buf;
      w->bytes++;
      w->buf >>= 8;
      w->nbits -= 8;
    }
  }
}
static W1_UNUSED size_t w1_bw_flush(w1_bw_t *w, uint8_t *out) {
  (void)out;
  if (!w->p) {
    w->bytes += (size_t)(w->nbits + 7) >> 3;
    w->nbits = 0;
    return w->bytes;
  }
  while (w->nbits > 0) {
    if (w->p >= w->end) { w->err = 1; break; }
    *w->p++ = (uint8_t)w->buf;
    w->bytes++;
    w->buf >>= 8;
    w->nbits -= 8;
  }
  if (w->nbits < 0) w->nbits = 0;
  return w->bytes;
}

/* Left shift that renormalises a bool-coder range r in [1,255] back to
 * >= 128 (7 - floor(log2 r)); shared by the decoder and the encoder. */
static const uint8_t w1k_bool_norm[256] = {
  7,7,6,6,5,5,5,5,4,4,4,4,4,4,4,4,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* ---- VP8 bool decoder (RFC 6386 20.2) ---- */
typedef struct w1_bool {
  const uint8_t *p;
  size_t left;
  uint32_t range;
  uint32_t value;
  int bits;        /* bit_count */
  int empty;       /* bytes pulled past end (sticky count) */
} w1_bool_t;

static W1_UNUSED void w1_bool_init(w1_bool_t *d, const uint8_t *p,
                                  size_t n) {
  if (n >= 2) {
    d->value = ((uint32_t)p[0] << 8) | p[1];
    d->p = p + 2;
    d->left = n - 2;
  } else {
    d->value = 0;
    d->p = p;
    d->left = 0;
  }
  d->range = 255;
  d->bits = 0;
  d->empty = 0;
}
W1_FORCEINLINE int w1_bool_get(w1_bool_t *d, int prob) {
  uint32_t split = 1 + (((d->range - 1) * (uint32_t)prob) >> 8);
  uint32_t splat = split << 8;
  int v;
  if (d->value >= splat) {
    v = 1;
    d->range -= split;
    d->value -= splat;
  } else {
    v = 0;
    d->range = split;
  }
  /* Renormalise in one shift. Equivalent to the RFC bit loop: the byte
   * that loop ORs in when bit_count reaches 8 lands, after the remaining
   * shifts, at bit (bits + shift - 8); shift <= 7 so at most one byte. */
  {
    const int shift = w1k_bool_norm[d->range];
    if (shift) {
      d->value <<= shift;
      d->range <<= shift;
      d->bits += shift;
      if (d->bits >= 8) {
        d->bits -= 8;
        if (d->left) {
          d->value |= (uint32_t)*d->p++ << d->bits;
          d->left--;
        } else {
          d->empty++;
        }
      }
    }
  }
  return v;
}
W1_FORCEINLINE int w1_bool_bit(w1_bool_t *d) { return w1_bool_get(d, 128); }
W1_FORCEINLINE int w1_bool_uint(w1_bool_t *d, int n) {
  int z = 0, b;
  for (b = n - 1; b >= 0; b--) z |= w1_bool_bit(d) << b;
  return z;
}
static W1_UNUSED int w1_bool_int(w1_bool_t *d, int n) {
  int z = w1_bool_uint(d, n);
  return w1_bool_bit(d) ? -z : z;
}
static W1_UNUSED int w1_bool_maybe_int(w1_bool_t *d, int n) {
  return w1_bool_bit(d) ? w1_bool_int(d, n) : 0;
}
W1_FORCEINLINE int w1_bool_tree(w1_bool_t *d, const int8_t *t,
                                 const uint8_t *p, int start) {
  int i = start;
  while ((i = t[i + w1_bool_get(d, p[i >> 1])]) > 0) {}
  return -i;
}

/* ---- VP8 bool encoder (matches the decoder above) ---- */
typedef struct w1_benc {
  uint8_t *buf;
  uint8_t *end;
  size_t pos;
  uint32_t low;
  uint32_t range;
  int count;
  int err;   /* overflow or impossible carry */
} w1_benc_t;

static W1_UNUSED void w1_benc_init(w1_benc_t *e, uint8_t *out, size_t cap) {
  e->buf = out; e->end = out + cap; e->pos = 0;
  e->low = 0; e->range = 255; e->count = -24; e->err = 0;
}
W1_FORCEINLINE int w1_benc_norm_shift(uint32_t range) {
  return w1k_bool_norm[range & 255];
}
W1_FORCEINLINE void w1_benc_bool(w1_benc_t *e, int bit, int prob) {
  uint32_t split;
  int shift, count, offset;
  uint32_t range, low;
  if (e->err) return;
  count = e->count; range = e->range; low = e->low;
  split = 1 + (((range - 1) * (uint32_t)prob) >> 8);
  range = split;
  if (bit) {
    low += split;
    range = e->range - split;
  }
  if (range == 0) { e->err = 1; return; }   /* else norm_shift hangs */
  shift = w1_benc_norm_shift(range);
  range <<= shift;
  count += shift;
  if (count >= 0) {
    int x;
    offset = shift - count;
    if ((low << (offset - 1)) & 0x80000000u) {
      x = (int)e->pos - 1;
      while (x >= 0 && e->buf[x] == 0xff) { e->buf[x] = 0; x--; }
      if (x < 0) { e->err = 1; return; }
      e->buf[x] += 1;
    }
    if (e->pos >= (size_t)(e->end - e->buf)) { e->err = 1; return; }
    e->buf[e->pos++] = (uint8_t)((low >> (24 - offset)) & 0xff);
    shift = count;
    low = (low << offset) & 0xffffffu;
    count -= 8;
  }
  low <<= shift;
  e->count = count;
  e->low = low;
  e->range = range;
}
static W1_UNUSED void w1_benc_uint(w1_benc_t *e, int v, int n) {
  int b;
  for (b = n - 1; b >= 0; b--) w1_benc_bool(e, (v >> b) & 1, 128);
}
/* Signed-int mirror of w1_bool_int: magnitude uint, then sign (1 = neg). */
static W1_UNUSED void w1_benc_int(w1_benc_t *e, int v, int n) {
  int a = v < 0 ? -v : v;
  w1_benc_uint(e, a, n);
  w1_benc_bool(e, v < 0, 128);
}
/* Optional-int mirror of w1_bool_maybe_int (segment quants/filter). */
static W1_UNUSED void w1_benc_maybe_int(w1_benc_t *e, int v, int n) {
  w1_benc_bool(e, v != 0, 128);
  if (v) w1_benc_int(e, v, n);
}
static W1_UNUSED int w1_tree_find(const int8_t *t, int node, int v,
                                 int *bits, int depth) {
  int b;
  for (b = 0; b < 2; b++) {
    int c = t[node + b];
    bits[depth] = b;
    if (c <= 0) {
      if (-c == v) return depth + 1;
    } else {
      int r = w1_tree_find(t, c, v, bits, depth + 1);
      if (r >= 0) return r;
    }
  }
  return -1;
}
static W1_UNUSED void w1_benc_tree(w1_benc_t *e, const int8_t *t,
                                  const uint8_t *p, int v) {
  int bits[24], node = 0, i, n = w1_tree_find(t, 0, v, bits, 0);
  for (i = 0; i < n; i++) {
    w1_benc_bool(e, bits[i], p[node >> 1]);
    node = t[node + bits[i]];
  }
}
static W1_UNUSED size_t w1_benc_stop(w1_benc_t *e) {
  int i;
  for (i = 0; i < 32; i++) w1_benc_bool(e, 0, 128);
  while (!e->err && e->pos < 2) {
    if (e->pos >= (size_t)(e->end - e->buf)) { e->err = 1; break; }
    e->buf[e->pos++] = 0;
  }
  return e->pos;
}
/* == S3: VP8L decoder (lossless bitstream -> ARGB) == */

/* ---- VP8L lossless decoder ---- */
#define W1_VP8L_MAGIC 0x2f
#define W1_VP8L_MAX_CACHE_BITS 11
#define W1_VP8L_MAX_TRANSFORMS 64

/* Primary fast-decode table bits: a 2^W1_HUFF_FAST_BITS entry table maps
 * the next bits (stream order) to (length << 12) | symbol for every code
 * no longer than that, 0 = miss (fall back to the canonical walk). */
#define W1_HUFF_FAST_BITS 9
typedef struct w1_huff {
  uint16_t *syms;   /* canonical (length, symbol) order */
  uint16_t first[16];
  uint16_t count[16];
  uint16_t off[16];
  uint16_t *fast;   /* 2^W1_HUFF_FAST_BITS entries, or NULL */
  int single;
  int single_sym;
} w1_huff_t;

typedef struct w1_hgroup {
  w1_huff_t t[5];   /* green, red, blue, alpha, dist */
} w1_hgroup_t;

typedef struct w1_vp8l {
  w1_br_t br;
  w1_bump_t *bump;
  int err;          /* sticky: corrupt bitstream */
  int oom;          /* sticky: work buffer too small (distinct from corrupt;
                     * checked first so the need-retry protocol terminates) */
  uint32_t *cache;  /* active image color cache (or NULL) */
  int cache_size;
  int cache_bits;
} w1_vp8l_t;

/* Build canonical MSB-first table from code lengths. Requires a complete
 * tree unless there is exactly one symbol (which then consumes 0 bits). */
static W1_UNUSED int w1_huff_build(w1_vp8l_t *d, w1_huff_t *h,
                                  const int *lens, int n) {
  int count[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  int total = 0, i, len, code = 0, left = 1, off = 0;
  for (i = 0; i < n; i++) {
    int l = lens[i];
    if (l < 0 || l > 15) return 0;
    if (l) { count[l]++; total++; }
  }
  if (total == 0) return 0;
  if (total == 1) {
    for (i = 0; i < n; i++) if (lens[i]) break;
    h->single = 1;
    h->single_sym = i;
    h->syms = NULL;
    h->fast = NULL;
    return 1;
  }
  h->single = 0;
  for (len = 1; len <= 15; len++) {
    left <<= 1;
    left -= count[len];
    if (left < 0) return 0;   /* over-subscribed */
  }
  if (left > 0) return 0;     /* incomplete */
  h->syms = (uint16_t *)w1_bump_alloc(d->bump, (size_t)total * 2, 2);
  if (!h->syms) { d->oom = 1; return 0; }
  h->first[0] = 0; h->count[0] = 0; h->off[0] = 0;
  for (len = 1; len <= 15; len++) {
    code = (code + count[len - 1]) << 1;
    h->first[len] = (uint16_t)code;
    h->count[len] = (uint16_t)count[len];
    h->off[len] = (uint16_t)off;
    off += count[len];
  }
  for (i = 0; i < n; i++) {
    int l = lens[i];
    if (l) h->syms[h->off[l]++] = (uint16_t)i;
  }
  /* Restore off[] (consumed above). */
  off = 0;
  for (len = 1; len <= 15; len++) {
    h->off[len] = (uint16_t)off;
    off += count[len];
  }
  {
    const int fb = W1_HUFF_FAST_BITS, fsz = 1 << fb;
    h->fast = (uint16_t *)w1_bump_alloc(d->bump, (size_t)fsz * 2, 2);
    if (!h->fast) { d->oom = 1; return 0; }
    for (i = 0; i < fsz; i++) h->fast[i] = 0;
    for (len = 1; len <= fb; len++) {
      int k;
      for (k = 0; k < (int)count[len]; k++) {
        int sym = h->syms[h->off[len] + k], cb = h->first[len] + k, rev = 0, m;
        for (m = 0; m < len; m++) { rev = (rev << 1) | (cb & 1); cb >>= 1; }
        /* Entries whose low len bits equal the stream-order code. */
        for (m = 0; m < (1 << (fb - len)); m++)
          h->fast[rev | (m << len)] =
            (uint16_t)((unsigned)len << 12 | (unsigned)sym);
      }
    }
  }
  return 1;
}

static W1_UNUSED int w1_huff_dec(w1_vp8l_t *d, w1_huff_t *h) {
  int c = 0, len;
  if (h->single) return h->single_sym;
  while (d->br.nbits <= 24 && d->br.p < d->br.end) {
    d->br.buf |= (uint32_t)(*d->br.p++) << d->br.nbits;
    d->br.nbits += 8;
  }
  if (h->fast && d->br.nbits >= W1_HUFF_FAST_BITS) {
    uint16_t e = h->fast[(unsigned)d->br.buf & ((1u << W1_HUFF_FAST_BITS) - 1u)];
    if (e) {
      int fl = e >> 12;
      d->br.buf >>= fl;
      d->br.nbits -= fl;
      return e & 0xfff;
    }
  }
  if (d->br.nbits >= 15) {
    uint32_t bits = d->br.buf;
    for (len = 1; len <= 15; len++) {
      unsigned index;
      c = (c << 1) | (int)(bits & 1);
      bits >>= 1;
      index = (unsigned)(c - (int)h->first[len]);
      if (index < (unsigned)h->count[len]) {
        d->br.buf = bits;
        d->br.nbits -= len;
        return h->syms[h->off[len] + index];
      }
    }
    d->err = 1;
    return 0;
  }
  for (len = 1; len <= 15; len++) {
    c = (c << 1) | (int)w1_br_bits(&d->br, 1);
    if ((unsigned)(c - (int)h->first[len]) < (unsigned)h->count[len])
      return h->syms[h->off[len] + (uint16_t)(c - h->first[len])];
  }
  d->err = 1;
  return 0;
}

static W1_UNUSED int w1_vp8l_read_huff(w1_vp8l_t *d, int alphabet,
                                      w1_huff_t *h) {
  int *lens, i;
  if (d->br.eof) { d->err = 1; return 0; }
  if (alphabet < 1 || alphabet > 256 + 24 + 2048) { d->err = 1; return 0; }
  lens = (int *)w1_bump_alloc(d->bump, (size_t)alphabet * sizeof(int), 4);
  if (!lens) { d->oom = 1; return 0; }
  for (i = 0; i < alphabet; i++) lens[i] = 0;
  if (w1_br_bits(&d->br, 1)) {
    int n = (int)w1_br_bits(&d->br, 1) + 1;
    int first8 = (int)w1_br_bits(&d->br, 1);
    int s0 = (int)w1_br_bits(&d->br, first8 ? 8 : 1);
    if (s0 >= alphabet) { d->err = 1; return 0; }
    lens[s0] = 1;
    if (n == 2) {
      int s1 = (int)w1_br_bits(&d->br, 8);
      /* s1 == s0 is legal: one symbol takes the 0-bit code. */
      if (s1 >= alphabet) { d->err = 1; return 0; }
      lens[s1] = 1;
    }
  } else {
    int ncl = 4 + (int)w1_br_bits(&d->br, 4);
    int cl_lens[19] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0 };
    w1_huff_t clh;
    int max_sym, sym = 0, prev = 8;
    for (i = 0; i < ncl; i++)
      cl_lens[w1k_vp8l_cl_order[i]] = (int)w1_br_bits(&d->br, 3);
    if (!w1_huff_build(d, &clh, cl_lens, 19)) {
      if (!d->oom) d->err = 1;
      return 0;
    }
    if (w1_br_bits(&d->br, 1)) {
      int length_nbits = 2 + 2 * (int)w1_br_bits(&d->br, 3);
      max_sym = 2 + (int)w1_br_bits(&d->br, length_nbits);
      if (max_sym > alphabet || length_nbits > 16) { d->err = 1; return 0; }
    } else {
      max_sym = alphabet;
    }
    while (sym < alphabet) {
      int c, rep, fill;
      if (max_sym-- == 0) break;
      c = w1_huff_dec(d, &clh);
      if (d->err) return 0;
      if (c < 16) {
        lens[sym++] = c;
        if (c != 0) prev = c;
      } else {
        if (c == 16) { rep = 3 + (int)w1_br_bits(&d->br, 2); fill = prev; }
        else if (c == 17) { rep = 3 + (int)w1_br_bits(&d->br, 3); fill = 0; }
        else { rep = 11 + (int)w1_br_bits(&d->br, 7); fill = 0; }
        if (sym + rep > alphabet) { d->err = 1; return 0; }
        while (rep-- > 0) lens[sym++] = fill;
      }
    }
  }
  if (d->br.eof) { d->err = 1; return 0; }
  if (!w1_huff_build(d, h, lens, alphabet)) {
    if (!d->oom) d->err = 1;
    return 0;
  }
  return 1;
}

/* Read one group of 5 tables (green alphabet includes color cache). */
static W1_UNUSED int w1_vp8l_read_group(w1_vp8l_t *d, w1_hgroup_t *g,
                                       int cache_bits) {
  int ga = 256 + 24 + (cache_bits ? (1 << cache_bits) : 0);
  return w1_vp8l_read_huff(d, ga, &g->t[0]) &&
         w1_vp8l_read_huff(d, 256, &g->t[1]) &&
         w1_vp8l_read_huff(d, 256, &g->t[2]) &&
         w1_vp8l_read_huff(d, 256, &g->t[3]) &&
         w1_vp8l_read_huff(d, 40, &g->t[4]);
}

/* length/distance prefix value: off + extra + 1. */
static W1_UNUSED int w1_vp8l_prefix_val(w1_vp8l_t *d, int code) {
  if (code < 4) return code + 1;
  {
    int e = (code - 2) >> 1;
    int off = (2 + (code & 1)) << e;
    return off + (int)w1_br_bits(&d->br, e) + 1;
  }
}

static W1_UNUSED int w1_vp8l_plane_dist(int plane, int w) {
  int dd;
  if (plane > 120) return plane - 120;
  dd = (int)w1k_vp8l_plane_dx[plane - 1] +
       (int)w1k_vp8l_plane_dy[plane - 1] * w;
  return dd < 1 ? 1 : dd;
}

/* Shared color-cache store: the VP8L decoder (w1_vp8l_t) and the lossless
 * encoder search (w1_lez_t) carry the same table in different structs. */
static W1_UNUSED void w1_cache_store(uint32_t *cache, int cache_bits,
                                     uint32_t px) {
  if (cache) {
    uint32_t h = 0x1e35a7bdu * px;
    cache[h >> (32 - cache_bits)] = px;
  }
}

static W1_UNUSED void w1_vp8l_cache_insert(w1_vp8l_t *d, uint32_t px) {
  w1_cache_store(d->cache, d->cache_bits, px);
}

/* Decode pixels; ent == NULL selects the single group. */
static W1_UNUSED int w1_vp8l_pixels(w1_vp8l_t *d, uint32_t *pix, int w, int h,
                                   w1_hgroup_t *gs, int ngroups,
                                   const uint32_t *ent,
                                   int ent_w, int prefix_bits) {
  int total = w * h, pos = 0, x = 0, y = 0;
  int cache_on = d->cache != NULL;
  while (pos < total) {
    w1_hgroup_t *g = gs;
    int s;
    /* S2: truncated bitstream already exhausted -> stop before running the
     * full canvas on zero-filled bits (valid streams never set eof early). */
    if (d->br.eof) { d->err = 1; return 0; }
    if (ent) {
      int gi = (int)((ent[(y >> prefix_bits) * ent_w + (x >> prefix_bits)]
                      >> 8) & 0xffff);
      if (gi >= ngroups) { d->err = 1; return 0; }
      g = gs + gi;
    }
    s = w1_huff_dec(d, &g->t[0]);
    if (d->err) return 0;
    if (s < 256) {
      int r = w1_huff_dec(d, &g->t[1]);
      int b = w1_huff_dec(d, &g->t[2]);
      int a = w1_huff_dec(d, &g->t[3]);
      uint32_t px;
      if (d->err) return 0;
      px = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)s << 8) | (uint32_t)b;
      pix[pos++] = px;
      if (cache_on) w1_vp8l_cache_insert(d, px);
      if (++x == w) { x = 0; y++; }
    } else if (s < 280) {
      int len = w1_vp8l_prefix_val(d, s - 256);
      int dsym = w1_huff_dec(d, &g->t[4]);
      int dist;
      int i;
      if (d->err) return 0;
      dist = w1_vp8l_plane_dist(w1_vp8l_prefix_val(d, dsym), w);
      if (d->err) return 0;
      if (dist > pos || pos + len > total) { d->err = 1; return 0; }
      {
        uint32_t *dst = pix + pos;
        const uint32_t *src = dst - dist;
        if (cache_on) {
          for (i = 0; i < len; i++) {
            uint32_t px = src[i];
            dst[i] = px;
            w1_vp8l_cache_insert(d, px);
          }
        } else if (dist >= len) {
          memcpy(dst, src, (size_t)len * 4);   /* no overlap */
        } else if (dist == 1) {
          uint32_t px = src[0];                /* run of one pixel */
          for (i = 0; i < len; i++) dst[i] = px;
        } else {
          for (i = 0; i < len; i++) dst[i] = src[i];
        }
      }
      pos += len;
      /* x/y only feed the entropy-group lookup, so the match advances them
       * once instead of once per pixel. */
      x += len;
      if (x >= w) { y += x / w; x %= w; }
    } else {
      int idx = s - 280;
      uint32_t px;
      if (!cache_on || idx >= d->cache_size) { d->err = 1; return 0; }
      px = d->cache[idx];
      pix[pos++] = px;
      w1_vp8l_cache_insert(d, px);
      if (++x == w) { x = 0; y++; }
    }
  }
  return 1;
}

typedef struct w1_vp8l_tr {
  int type;        /* 0 predictor, 1 color, 2 green, 3 index */
  int aux;         /* 0/1: size_bits; 3: width_bits */
  int w_before;    /* image width before this transform (color index only) */
  int iw, ih;      /* sub-image dims */
  uint32_t *img;
} w1_vp8l_tr_t;

static W1_UNUSED uint32_t *w1_vp8l_sub_image(w1_vp8l_t *d, int w, int h);

/* Full level-0 image stream: transforms + cache + tables/meta + pixels.
 * pix must hold w*h u32. Consumes bits from d->br. */
static W1_UNUSED int w1_vp8l_main(w1_vp8l_t *d, uint32_t *pix, int w, int h);

/* Sub-image stream: cache + single tables + pixels (no meta/transforms). */
static W1_UNUSED uint32_t *w1_vp8l_sub_image(w1_vp8l_t *d, int w, int h) {
  uint32_t *pix;
  w1_hgroup_t g;
  uint32_t *oc;
  int ocb, ocs;
  int cache_bits = 0, i;
  if (d->br.eof) { d->err = 1; return NULL; }
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) {
    d->err = 1;
    return NULL;
  }
  if (w1_br_bits(&d->br, 1)) {
    cache_bits = (int)w1_br_bits(&d->br, 4);
    if (cache_bits < 1 || cache_bits > W1_VP8L_MAX_CACHE_BITS) {
      d->err = 1;
      return NULL;
    }
  }
  if (!w1_vp8l_read_group(d, &g, cache_bits)) return NULL;
  pix = (uint32_t *)w1_bump_alloc(d->bump, (size_t)w * (size_t)h * 4, 4);
  if (!pix) { d->oom = 1; return NULL; }
  /* Install fresh cache for this sub-image. */
  oc = d->cache; ocb = d->cache_bits; ocs = d->cache_size;
  d->cache = NULL; d->cache_size = 0; d->cache_bits = 0;
  if (cache_bits) {
    d->cache_size = 1 << cache_bits;
    d->cache_bits = cache_bits;
    d->cache = (uint32_t *)w1_bump_alloc(d->bump,
                                         (size_t)d->cache_size * 4, 4);
    if (!d->cache) {
      d->oom = 1;
      d->cache = oc; d->cache_bits = ocb; d->cache_size = ocs;
      return NULL;
    }
    for (i = 0; i < d->cache_size; i++) d->cache[i] = 0;
  }
  if (!w1_vp8l_pixels(d, pix, w, h, &g, 1, NULL, 0, 0)) {
    d->cache = oc; d->cache_bits = ocb; d->cache_size = ocs;
    return NULL;
  }
  d->cache = oc; d->cache_bits = ocb; d->cache_size = ocs;
  return pix;
}

static W1_UNUSED int w1_abs(int v) { return v < 0 ? -v : v; }

static W1_UNUSED uint32_t w1_vp8l_avg2(uint32_t a0, uint32_t a1) {
  return (((a0 ^ a1) & 0xfefefefeu) >> 1) + (a0 & a1);
}

static W1_UNUSED uint32_t w1_vp8l_avg3(uint32_t a0, uint32_t a1, uint32_t a2) {
  return w1_vp8l_avg2(w1_vp8l_avg2(a0, a2), a1);
}

static W1_UNUSED uint32_t w1_vp8l_avg4(uint32_t a0, uint32_t a1, uint32_t a2,
                                      uint32_t a3) {
  return w1_vp8l_avg2(w1_vp8l_avg2(a0, a1), w1_vp8l_avg2(a2, a3));
}

/* Whole-pixel select: returns a iff sum(|b-c| - |a-c|) <= 0. */
static W1_UNUSED uint32_t w1_vp8l_select(uint32_t a, uint32_t b, uint32_t c) {
  int i, score = 0;
  for (i = 0; i < 4; i++) {
    int ac = (int)((a >> (8 * i)) & 0xff);
    int bc = (int)((b >> (8 * i)) & 0xff);
    int cc = (int)((c >> (8 * i)) & 0xff);
    score += w1_abs(bc - cc) - w1_abs(ac - cc);
  }
  return score <= 0 ? a : b;
}

static W1_UNUSED uint32_t w1_vp8l_addsub_full(uint32_t c0, uint32_t c1,
                                            uint32_t c2) {
  int i;
  uint32_t out = 0;
  for (i = 0; i < 4; i++) {
    int a = (int)((c0 >> (8 * i)) & 0xff);
    int b = (int)((c1 >> (8 * i)) & 0xff);
    int c = (int)((c2 >> (8 * i)) & 0xff);
    out |= (uint32_t)w1_clamp255(a + b - c) << (8 * i);
  }
  return out;
}

static W1_UNUSED uint32_t w1_vp8l_addsub_half(uint32_t c0, uint32_t c1,
                                            uint32_t c2) {
  uint32_t ave = w1_vp8l_avg2(c0, c1);
  int i;
  uint32_t out = 0;
  for (i = 0; i < 4; i++) {
    int a = (int)((ave >> (8 * i)) & 0xff);
    int c = (int)((c2 >> (8 * i)) & 0xff);
    out |= (uint32_t)w1_clamp255(a + (a - c) / 2) << (8 * i);
  }
  return out;
}

static W1_UNUSED uint32_t w1_vp8l_add_pixels(uint32_t a, uint32_t b) {
  return ((a + b) & 0xffu) | ((((a >> 8) + (b >> 8)) & 0xffu) << 8) |
         ((((a >> 16) + (b >> 16)) & 0xffu) << 16) |
         ((((a >> 24) + (b >> 24)) & 0xffu) << 24);
}

static W1_UNUSED uint32_t w1_vp8l_predict(int mode, uint32_t L, uint32_t T,
                                         uint32_t TL, uint32_t TR) {
  switch (mode) {
  case 0: return 0xff000000u;
  case 1: return L;
  case 2: return T;
  case 3: return TR;
  case 4: return TL;
  case 5: return w1_vp8l_avg3(L, T, TR);
  case 6: return w1_vp8l_avg2(L, TL);
  case 7: return w1_vp8l_avg2(L, T);
  case 8: return w1_vp8l_avg2(TL, T);
  case 9: return w1_vp8l_avg2(T, TR);
  case 10: return w1_vp8l_avg4(L, TL, T, TR);
  case 11: return w1_vp8l_select(T, L, TL);
  case 12: return w1_vp8l_addsub_full(L, T, TL);
  default: return w1_vp8l_addsub_half(L, T, TL);
  }
}

/* One tile span of a TR-using predictor: the last column's top-right wraps
 * to the row start, so it is peeled off the main loop instead of costing a
 * bounds test per pixel. Used by w1_vp8l_invert. */
#define W1_PRED_SPAN(EXPR)                                                  \
  do {                                                                      \
    int xm = xe < cur_w - 1 ? xe : cur_w - 1;                               \
    for (; x < xm; x++) {                                                   \
      uint32_t L = row[x - 1], T = prev[x], TL = prev[x - 1],               \
               TR = prev[x + 1];                                            \
      row[x] = w1_vp8l_add_pixels(row[x], (EXPR));                          \
    }                                                                       \
    if (x < xe) {                                                           \
      uint32_t L = row[x - 1], T = prev[x], TL = prev[x - 1], TR = row[0];  \
      row[x] = w1_vp8l_add_pixels(row[x], (EXPR));                          \
      x++;                                                                  \
    }                                                                       \
  } while (0)

static W1_UNUSED void w1_vp8l_invert(w1_vp8l_t *d, uint32_t *pix, int w, int h,
                                    w1_vp8l_tr_t *trs, int ntr) {
  int cur_w = w, i, x, y;
  /* Forward pass to get stream width. */
  for (i = 0; i < ntr; i++)
    if (trs[i].type == 3)
      cur_w = (cur_w + (1 << trs[i].aux) - 1) >> trs[i].aux;
  for (i = ntr - 1; i >= 0; i--) {
    w1_vp8l_tr_t *t = &trs[i];
    if (t->type == 0) {
      int sb = t->aux, tw = t->iw;
      /* Row 0 is pure left prediction after the opaque-black seed, and
       * column 0 is pure top: both are lifted out of the inner loop. */
      if (h > 0) {
        pix[0] = w1_vp8l_add_pixels(pix[0], 0xff000000u);
        for (x = 1; x < cur_w; x++)
          pix[x] = w1_vp8l_add_pixels(pix[x], pix[x - 1]);
      }
      for (y = 1; y < h; y++) {
        uint32_t *row = pix + (size_t)y * cur_w;
        const uint32_t *prev = row - cur_w;
        const uint32_t *mrow = t->img + (size_t)(y >> sb) * tw;
        row[0] = w1_vp8l_add_pixels(row[0], prev[0]);
        x = 1;
        /* The mode is constant across a tile, so it is read once per tile
         * span and the span runs in a loop specialized for that mode. */
        while (x < cur_w) {
          int xe = ((x >> sb) + 1) << sb;
          int mode = (int)((mrow[x >> sb] >> 8) & 0xf);
          if (xe > cur_w) xe = cur_w;
          switch (mode) {
          case 0:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x], 0xff000000u);
            break;
          case 1:   /* left and top need no TR, so they skip the peel */
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x], row[x - 1]);
            break;
          case 2:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x], prev[x]);
            break;
          case 4:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x], prev[x - 1]);
            break;
          case 6:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x],
                         w1_vp8l_avg2(row[x - 1], prev[x - 1]));
            break;
          case 7:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x],
                         w1_vp8l_avg2(row[x - 1], prev[x]));
            break;
          case 8:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x],
                         w1_vp8l_avg2(prev[x - 1], prev[x]));
            break;
          case 11:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x],
                         w1_vp8l_select(prev[x], row[x - 1], prev[x - 1]));
            break;
          case 12:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x],
                         w1_vp8l_addsub_full(row[x - 1], prev[x],
                                             prev[x - 1]));
            break;
          case 13:
            for (; x < xe; x++)
              row[x] = w1_vp8l_add_pixels(row[x],
                         w1_vp8l_addsub_half(row[x - 1], prev[x],
                                             prev[x - 1]));
            break;
          default:  /* 3, 5, 9, 10: need TR, whose last column wraps */
            W1_PRED_SPAN(w1_vp8l_predict(mode, L, T, TL, TR));
            break;
          }
          x = xe;
        }
      }
    } else if (t->type == 1) {
      int sb = t->aux, tw = t->iw;
      for (y = 0; y < h; y++) {
        uint32_t *row = pix + (size_t)y * cur_w;
        const uint32_t *mrow = t->img + (size_t)(y >> sb) * tw;
        x = 0;
        while (x < cur_w) {
          int xe = ((x >> sb) + 1) << sb;
          uint32_t cc = mrow[x >> sb];
          int g2r = (int)(int8_t)(cc & 0xff);
          int g2b = (int)(int8_t)((cc >> 8) & 0xff);
          int r2b = (int)(int8_t)((cc >> 16) & 0xff);
          if (xe > cur_w) xe = cur_w;
          for (; x < xe; x++) {
            uint32_t px = row[x];
            int g = (int)(int8_t)((px >> 8) & 0xff);
            int r = (int)((px >> 16) & 0xff) + w1_asr(g2r * g, 5);
            int b = (int)(px & 0xff) + w1_asr(g2b * g, 5);
            r &= 0xff;
            b += w1_asr(r2b * (int)(int8_t)r, 5);
            row[x] = (px & 0xff00ff00u) |
              (((uint32_t)(r & 0xff)) << 16) | (uint32_t)(b & 0xff);
          }
        }
      }
    } else if (t->type == 2) {
      int n = cur_w * h, k;
      for (k = 0; k < n; k++) {
        uint32_t px = pix[k];
        int g = (px >> 8) & 0xff;
        pix[k] = (px & 0xff00ff00u) |
          ((((px >> 16) + (uint32_t)g) & 0xff) << 16) |
          (((px + (uint32_t)g) & 0xff));
      }
    } else {
      /* color index: expand in place, backwards */
      int wb = t->aux, bpp = 8 >> wb, mask = (1 << bpp) - 1;
      int w_new = t->w_before, ts = t->iw;
      for (y = h - 1; y >= 0; y--) {
        for (x = w_new - 1; x >= 0; x--) {
          uint32_t bundled = pix[y * cur_w + (x >> wb)];
          int idx = (int)(((bundled >> 8) >> ((x & ((1 << wb) - 1)) * bpp)) &
                          0xff) & mask;
          pix[y * w_new + x] = idx < ts ? t->img[idx] : 0;
        }
      }
      cur_w = w_new;
    }
  }
  (void)d;
}

static W1_UNUSED int w1_vp8l_main(w1_vp8l_t *d, uint32_t *pix, int w, int h) {
  w1_vp8l_tr_t *trs = NULL;
  int ntr = 0, cap = 0, cur_w = w;
  unsigned seen_tr = 0;
  int cache_bits = 0, i;
  int prefix_bits = 0, ngroups = 1;
  const uint32_t *ent = NULL;
  int ent_w = 0;
  w1_hgroup_t *gs = NULL;
  /* Transforms. */
  while (w1_br_bits(&d->br, 1)) {
    w1_vp8l_tr_t *t;
    int type;
    if (ntr >= W1_VP8L_MAX_TRANSFORMS) { d->err = 1; return 0; }
    if (ntr >= cap) {
      int ncap = cap ? cap * 2 : 4;
      w1_vp8l_tr_t *nt = (w1_vp8l_tr_t *)w1_bump_alloc(
        d->bump, (size_t)ncap * sizeof(*nt), 4);
      if (!nt) { d->oom = 1; return 0; }
      for (i = 0; i < ntr; i++) nt[i] = trs[i];
      trs = nt; cap = ncap;
    }
    t = &trs[ntr++];
    type = (int)w1_br_bits(&d->br, 2);
    if (seen_tr & (1u << type)) { d->err = 1; return 0; }
    seen_tr |= 1u << type;
    t->type = type;
    t->w_before = cur_w;
    if (type == 0 || type == 1) {
      int sb = (int)w1_br_bits(&d->br, 3) + 2;
      int tw = (cur_w + (1 << sb) - 1) >> sb;
      int th = (h + (1 << sb) - 1) >> sb;
      int k;
      t->aux = sb; t->iw = tw; t->ih = th;
      t->img = w1_vp8l_sub_image(d, tw, th);
      if (!t->img) return 0;
      if (type == 0) {
        for (k = 0; k < tw * th; k++)
          if (((t->img[k] >> 8) & 0xff) > 13) { d->err = 1; return 0; }
      }
    } else if (type == 2) {
      t->aux = 0; t->img = NULL;
    } else {
      int ts = (int)w1_br_bits(&d->br, 8) + 1;
      int wb = ts <= 2 ? 3 : (ts <= 4 ? 2 : (ts <= 16 ? 1 : 0));
      uint32_t prev = 0;
      t->aux = wb; t->iw = ts; t->ih = 1;
      t->img = w1_vp8l_sub_image(d, ts, 1);
      if (!t->img) return 0;
      for (i = 0; i < ts; i++) {
        /* undo delta coding */
        uint32_t v = t->img[i];
        v = ((v & 0xff000000u) + (prev & 0xff000000u)) |
            (((((v >> 16) & 0xff) + ((prev >> 16) & 0xff)) & 0xff) << 16) |
            (((((v >> 8) & 0xff) + ((prev >> 8) & 0xff)) & 0xff) << 8) |
            ((((v & 0xff) + (prev & 0xff)) & 0xff));
        prev = v;
        t->img[i] = v;
      }
      cur_w = (cur_w + (1 << wb) - 1) >> wb;
    }
    if (d->err) return 0;
  }
  /* Color cache info. */
  if (w1_br_bits(&d->br, 1)) {
    cache_bits = (int)w1_br_bits(&d->br, 4);
    if (cache_bits < 1 || cache_bits > W1_VP8L_MAX_CACHE_BITS) {
      d->err = 1;
      return 0;
    }
  }
  /* Meta prefix (entropy image) or single group. */
  if (w1_br_bits(&d->br, 1)) {
    prefix_bits = (int)w1_br_bits(&d->br, 3) + 2;
    ent_w = (cur_w + (1 << prefix_bits) - 1) >> prefix_bits;
    {
      int ent_h = (h + (1 << prefix_bits) - 1) >> prefix_bits;
      ent = w1_vp8l_sub_image(d, ent_w, ent_h);
      if (!ent) return 0;
      /* Group count = max group index over ALL entropy pixels + 1
       * (format-compatible; pixel 0 is not authoritative). */
      {
        int npx = ent_w * ent_h, gm = 0;
        for (i = 0; i < npx; i++) {
          int g = (int)((ent[i] >> 8) & 0xffff);
          if (g + 1 > gm) gm = g + 1;
        }
        ngroups = gm;
        if (ngroups > 200 || ngroups > npx) {
          /* Dense remap to [0, nnew). */
          int nnew = 0, *map;
          uint32_t *ew;
          map = (int *)w1_bump_alloc(d->bump,
                                     (size_t)ngroups * sizeof(int), 4);
          if (!map) { d->oom = 1; return 0; }
          for (i = 0; i < ngroups; i++) map[i] = -1;
          ew = (uint32_t *)ent;
          for (i = 0; i < npx; i++) {
            int g = (int)((ew[i] >> 8) & 0xffff);
            if (map[g] < 0) map[g] = nnew++;
            ew[i] = (uint32_t)map[g] << 8;
          }
          ngroups = nnew;
        }
        if (ngroups < 1) { d->err = 1; return 0; }
      }
    }
  }
  if (d->br.eof) { d->err = 1; return 0; }
  /* S1: preflight the worst-case Huffman-group memory in one shot so a
   * many-group stream reports an accurate *need instead of converging via
   * repeated 25% bump growth. Success path untouched when it already fits.
   * Per group <= 19376 B (green 13968 + red/blue/alpha 4608 + dist 240 +
   * w1_hgroup_t 560, incl. lens+syms+cl tables) plus 10 fast-decode
   * tables (2^9 x uint16: 5 group + 5 code-length tables), rounded up. */
  {
    size_t want = (size_t)ngroups * 30720u;
    size_t used = d->bump->used <= d->bump->cap ? d->bump->used : d->bump->cap;
    size_t left = d->bump->cap - used;
    if (left < want) {
      size_t total = want + (want >> 3) + 4096;
      d->oom = 1;
      if (total < want) total = (size_t)-1;      /* overflow: saturate */
      else if (used > (size_t)-1 - total) total = (size_t)-1;
      else total = used + total;
      if (total > d->bump->need) d->bump->need = total;
      return 0;
    }
  }
  gs = (w1_hgroup_t *)w1_bump_alloc(d->bump, (size_t)ngroups * sizeof(*gs),
                                   4);
  if (!gs) { d->oom = 1; return 0; }
  for (i = 0; i < ngroups; i++)
    if (!w1_vp8l_read_group(d, &gs[i], cache_bits)) return 0;
  /* Install cache for main pixels. */
  d->cache = NULL; d->cache_size = 0; d->cache_bits = 0;
  if (cache_bits) {
    d->cache_size = 1 << cache_bits;
    d->cache_bits = cache_bits;
    d->cache = (uint32_t *)w1_bump_alloc(d->bump, (size_t)d->cache_size * 4,
                                        4);
    if (!d->cache) { d->oom = 1; return 0; }
    for (i = 0; i < d->cache_size; i++) d->cache[i] = 0;
  }
  if (!w1_vp8l_pixels(d, pix, cur_w, h, gs, ngroups, ent, ent_w,
                      prefix_bits))
    return 0;
  d->cache = NULL; d->cache_size = 0; d->cache_bits = 0;
  w1_vp8l_invert(d, pix, w, h, trs, ntr);
  return !d->err && !d->br.eof;
}

static W1_UNUSED int w1_vp8l_peek_dims(const uint8_t *d, size_t n,
                                      int *w, int *h, int *alpha);

/* Shared tail of the two VP8L entries: run main, map outcome to API codes. */
static W1_UNUSED int w1_vp8l_run(w1_vp8l_t *d, uint32_t *pix, int w, int h) {
  if (!w1_vp8l_main(d, pix, w, h)) {
    if (d->oom) return WEBP1_ERR_NO_MEMORY;
    if (d->err || d->br.eof) return WEBP1_ERR_CORRUPT;
    return WEBP1_ERR_NO_MEMORY;
  }
  return WEBP1_OK;
}

/* Decode a full VP8L bitstream (with 1-byte header). pix holds w*h u32,
 * where w/h must equal the header dims. */
static W1_UNUSED int w1_vp8l_decode(const uint8_t *data, size_t size,
                                    uint32_t *pix, w1_bump_t *bump) {
  int w, h, alpha, rc;
  w1_vp8l_t d;
  rc = w1_vp8l_peek_dims(data, size, &w, &h, &alpha);
  (void)alpha;
  if (rc) return rc;
  memset(&d, 0, sizeof(d));
  d.bump = bump;
  w1_br_init(&d.br, data + 5, size - 5);
  return w1_vp8l_run(&d, pix, w, h);
}

/* Decode a raw VP8L image stream (ALPH: no header, implicit dims). */
static W1_UNUSED int w1_vp8l_decode_raw(const uint8_t *data, size_t size,
                                        int w, int h, uint32_t *pix,
                                        w1_bump_t *bump) {
  w1_vp8l_t d;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM)
    return WEBP1_ERR_CORRUPT;
  memset(&d, 0, sizeof(d));
  d.bump = bump;
  w1_br_init(&d.br, data, size);
  return w1_vp8l_run(&d, pix, w, h);
}
/* == S4: VP8 decoder (lossy keyframes -> YUV -> RGBA, RFC 6386) == */

/* ---- VP8 lossy decoder (key frames, intra only; RFC 6386) ---- */

#define W1_VP8_DC 0
#define W1_VP8_V 1
#define W1_VP8_H 2
#define W1_VP8_B 4

/* Token tree node indices. */
#define W1_N_EOB 0
#define W1_N_ZERO 1
#define W1_N_ONE 2
#define W1_N_LOW 3
#define W1_N_TWO 4
#define W1_N_THREE 5
#define W1_N_HIGHLOW 6
#define W1_N_CATONE 7
#define W1_N_CAT34 8
#define W1_N_CAT3 9
#define W1_N_CAT5 10

typedef struct w1_vp8d {
  int mb_w, mb_h;
  int use_simple, level, sharpness, delta_on;
  int8_t ref_delta[4], mode_delta[4];
  int seg_on, seg_upd_map, seg_abs;
  int8_t seg_q[4], seg_lf[4];
  uint8_t seg_tree_probs[3];
  int nparts;
  int q_index, y1dc_d, y2dc_d, y2ac_d, uvdc_d, uvac_d;
  int skip_on, skip_prob;
  int coef_upd;   /* number of coefficient probability updates in header */
  uint8_t coef_probs[4][8][3][11];
  int dqf[4][6];   /* per seg: Y1DC,Y1AC,UVDC,UVAC,Y2DC,Y2AC */
  w1_bool_t tok[8];
  uint8_t *above_tok;    /* 9*(mb_w+1) */
  uint8_t left_tok[9];
  uint8_t *above_ym;     /* mb_w+1 */
  uint8_t *above_brow;   /* 4*(mb_w+1) */
  uint8_t left_ym;
  uint8_t left_rcol[4];
  uint8_t *plane_y, *plane_u, *plane_v;
  int y_stride, uv_stride;
  uint8_t *mb_seg, *mb_ymode, *mb_eob;  /* mb_w*mb_h each */
} w1_vp8d_t;

static W1_UNUSED int16_t w1_wrap16(int v) {
  int w = v & 0xffff;
  return (int16_t)(w >= 32768 ? w - 65536 : w);
}

/* ---- Inverse transforms (exact port incl. 16-bit intermediate wrap) ---- */

static W1_UNUSED void w1_vp8_wht(const int16_t *in, int16_t *out) {
  int i, a1, b1, c1, d1, a2, b2, c2, d2;
  int16_t tmp[16], *tp = tmp;
  for (i = 0; i < 4; i++) {
    a1 = in[0] + in[12]; b1 = in[4] + in[8];
    c1 = in[4] - in[8]; d1 = in[0] - in[12];
    tp[0] = w1_wrap16(a1 + b1); tp[4] = w1_wrap16(c1 + d1);
    tp[8] = w1_wrap16(a1 - b1); tp[12] = w1_wrap16(d1 - c1);
    in++; tp++;
  }
  tp = tmp;
  for (i = 0; i < 4; i++) {
    a1 = tp[0] + tp[3]; b1 = tp[1] + tp[2];
    c1 = tp[1] - tp[2]; d1 = tp[0] - tp[3];
    a2 = a1 + b1; b2 = c1 + d1; c2 = a1 - b1; d2 = d1 - c1;
    out[0] = (int16_t)w1_asr(a2 + 3, 3);
    out[1] = (int16_t)w1_asr(b2 + 3, 3);
    out[2] = (int16_t)w1_asr(c2 + 3, 3);
    out[3] = (int16_t)w1_asr(d2 + 3, 3);
    tp += 4; out += 4;
  }
}

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define W1_HAVE_SSE2 1
#include <emmintrin.h>
#endif
#if defined(W1_HAVE_SSE2) && !defined(W1_NO_SIMD)
#define W1_USE_SSE2 1
#endif

#ifdef W1_USE_SSE2
static W1_UNUSED __m128i w1_mullo32_sse2(__m128i a, __m128i b) {
  __m128i even = _mm_mul_epu32(a, b);
  __m128i odd = _mm_mul_epu32(_mm_srli_si128(a, 4), _mm_srli_si128(b, 4));
  return _mm_unpacklo_epi64(_mm_unpacklo_epi32(even, odd),
                            _mm_unpackhi_epi32(even, odd));
}
static W1_UNUSED void w1_vp8e_transpose(__m128i *a, __m128i *b,
                                       __m128i *c, __m128i *d) {
  __m128i t0 = _mm_unpacklo_epi32(*a, *b), t1 = _mm_unpackhi_epi32(*a, *b);
  __m128i t2 = _mm_unpacklo_epi32(*c, *d), t3 = _mm_unpackhi_epi32(*c, *d);
  *a = _mm_unpacklo_epi64(t0, t2); *b = _mm_unpackhi_epi64(t0, t2);
  *c = _mm_unpacklo_epi64(t1, t3); *d = _mm_unpackhi_epi64(t1, t3);
}
#endif
static W1_UNUSED void w1_vp8_idct_add_scalar(uint8_t *dst, int stride,
                                     const uint8_t *pred,
                                     const int16_t *coef) {
  int i, a1, b1, c1, d1, t1, t2;
  int16_t tmp[16];
  int16_t *op = tmp;
  for (i = 0; i < 4; i++) {
    a1 = coef[0] + coef[8]; b1 = coef[0] - coef[8];
    t1 = w1_asr(coef[4] * 35468, 16);
    t2 = coef[12] + w1_asr(coef[12] * 20091, 16);
    c1 = t1 - t2;
    t1 = coef[4] + w1_asr(coef[4] * 20091, 16);
    t2 = w1_asr(coef[12] * 35468, 16);
    d1 = t1 + t2;
    op[0] = w1_wrap16(a1 + d1); op[12] = w1_wrap16(a1 - d1);
    op[4] = w1_wrap16(b1 + c1); op[8] = w1_wrap16(b1 - c1);
    coef++; op++;
  }
  coef = tmp;
  for (i = 0; i < 4; i++) {
    a1 = coef[0] + coef[2]; b1 = coef[0] - coef[2];
    t1 = w1_asr(coef[1] * 35468, 16);
    t2 = coef[3] + w1_asr(coef[3] * 20091, 16);
    c1 = t1 - t2;
    t1 = coef[1] + w1_asr(coef[1] * 20091, 16);
    t2 = w1_asr(coef[3] * 35468, 16);
    d1 = t1 + t2;
    dst[0] = (uint8_t)w1_clamp255(pred[0] + w1_asr(a1 + d1 + 4, 3));
    dst[3] = (uint8_t)w1_clamp255(pred[3] + w1_asr(a1 - d1 + 4, 3));
    dst[1] = (uint8_t)w1_clamp255(pred[1] + w1_asr(b1 + c1 + 4, 3));
    dst[2] = (uint8_t)w1_clamp255(pred[2] + w1_asr(b1 - c1 + 4, 3));
    coef += 4; dst += stride; pred += stride;
  }
}

#ifdef W1_USE_SSE2
static W1_UNUSED void w1_vp8_idct4(__m128i *p0, __m128i *p1,
                                  __m128i *p2, __m128i *p3) {
  __m128i a = _mm_add_epi32(*p0, *p2), b = _mm_sub_epi32(*p0, *p2);
  __m128i packed = _mm_packs_epi32(*p1, *p3);
  __m128i m1 = _mm_mulhi_epi16(packed, _mm_set1_epi16(-30068));
  __m128i m2 = _mm_mulhi_epi16(packed, _mm_set1_epi16(20091));
  __m128i s1 = _mm_srai_epi16(m1, 15), s2 = _mm_srai_epi16(m2, 15);
  __m128i c = _mm_sub_epi32(_mm_add_epi32(*p1, _mm_unpacklo_epi16(m1, s1)),
                            _mm_add_epi32(*p3, _mm_unpackhi_epi16(m2, s2)));
  __m128i d = _mm_add_epi32(_mm_add_epi32(*p1, _mm_unpacklo_epi16(m2, s2)),
                            _mm_add_epi32(*p3, _mm_unpackhi_epi16(m1, s1)));
  *p0 = _mm_add_epi32(a, d); *p1 = _mm_add_epi32(b, c);
  *p2 = _mm_sub_epi32(b, c); *p3 = _mm_sub_epi32(a, d);
}
#endif
static W1_UNUSED void w1_vp8_idct_add(uint8_t *dst, int stride,
                                     const uint8_t *pred, const int16_t *coef) {
#ifdef W1_USE_SSE2
  __m128i v[4], zero = _mm_setzero_si128(), four = _mm_set1_epi32(4);
  int i;
  for (i = 0; i < 4; i++) {
    v[i] = _mm_loadl_epi64((const __m128i *)(const void *)(coef + 4 * i));
    v[i] = _mm_unpacklo_epi16(v[i], _mm_srai_epi16(v[i], 15));
  }
  w1_vp8_idct4(&v[0], &v[1], &v[2], &v[3]);
  for (i = 0; i < 4; i++) v[i] = _mm_srai_epi32(_mm_slli_epi32(v[i], 16), 16);
  w1_vp8e_transpose(&v[0], &v[1], &v[2], &v[3]);
  w1_vp8_idct4(&v[0], &v[1], &v[2], &v[3]);
  w1_vp8e_transpose(&v[0], &v[1], &v[2], &v[3]);
  for (i = 0; i < 4; i++) {
    uint32_t px;
    __m128i pr, res;
    memcpy(&px, pred + i * stride, 4);
    pr = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)px), zero), zero);
    res = _mm_add_epi32(pr, _mm_srai_epi32(_mm_add_epi32(v[i], four), 3));
    res = _mm_packs_epi32(res, res); res = _mm_packus_epi16(res, res);
    px = (uint32_t)_mm_cvtsi128_si32(res);
    memcpy(dst + i * stride, &px, 4);
  }
#else
  w1_vp8_idct_add_scalar(dst, stride, pred, coef);
#endif
}

/* Add DC-only residue (provably identical to full IDCT of [d,0,...]). */
static W1_UNUSED void w1_vp8_dc_add(uint8_t *dst, int stride,
                                   const uint8_t *pred, int d) {
  int r, c, v = w1_asr(d + 4, 3);
  for (r = 0; r < 4; r++) {
    for (c = 0; c < 4; c++)
      dst[r * stride + c] = (uint8_t)w1_clamp255(pred[r * stride + c] + v);
  }
}

/* Block residue class: 0 = all zero, 1 = DC only, 2 = full. */
static W1_UNUSED int w1_vp8_block_class(const int16_t *coef) {
#ifdef W1_USE_SSE2
  const __m128i zero = _mm_setzero_si128();
  const unsigned m0 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi16(
      _mm_loadu_si128((const __m128i *)(const void *)coef), zero));
  const unsigned m1 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi16(
      _mm_loadu_si128((const __m128i *)(const void *)(coef + 8)), zero));
  if ((m0 & m1) == 0xffffu) return 0;
  if ((m0 & 0xfffeu) == 0xfffeu && m1 == 0xffffu) return 1;
  return 2;
#else
  int i;
  if (coef[0] == 0) {
    for (i = 1; i < 16; i++) if (coef[i]) return 2;
    return 0;
  }
  for (i = 1; i < 16; i++) if (coef[i]) return 2;
  return 1;
#endif
}

/* ---- Intra prediction ---- */
/* ab[0]=above[-1](corner), ab[1..8]=above[0..7]; lf[0]=left[-1],
 * lf[1..4]=left rows 0..3. */

static W1_UNUSED void w1_pred_bdc(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int r, c, dc = (lf[1] + lf[2] + lf[3] + lf[4] + ab[1] + ab[2] + ab[3] +
                  ab[4] + 4) >> 3;
  for (r = 0; r < 4; r++)
    for (c = 0; c < 4; c++) d[r * s + c] = (uint8_t)dc;
}
static W1_UNUSED void w1_pred_btm(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int r, c, p = ab[0];
  for (r = 0; r < 4; r++)
    for (c = 0; c < 4; c++)
      d[r * s + c] = (uint8_t)w1_clamp255(lf[r + 1] + ab[c + 1] - p);
}
static W1_UNUSED void w1_pred_bve(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int r, c;
  (void)lf;
  for (c = 0; c < 4; c++)
    d[c] = (uint8_t)((ab[c] + 2 * ab[c + 1] + ab[c + 2] + 2) >> 2);
  for (r = 1; r < 4; r++)
    for (c = 0; c < 4; c++) d[r * s + c] = d[c];
}
static W1_UNUSED void w1_pred_bhe(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int r, c;
  (void)ab;
  for (r = 0; r < 4; r++) {
    int v = (lf[r] + 2 * lf[r + 1] + (r < 3 ? lf[r + 2] : lf[r + 1]) + 2) >> 2;
    for (c = 0; c < 4; c++) d[r * s + c] = (uint8_t)v;
  }
}
static W1_UNUSED void w1_pred_bld(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int p[7], r, c;
  (void)lf;
  for (c = 0; c < 6; c++)
    p[c] = (ab[c + 1] + 2 * ab[c + 2] + ab[c + 3] + 2) >> 2;
  p[6] = (ab[7] + 2 * ab[8] + ab[8] + 2) >> 2;
  for (r = 0; r < 4; r++)
    for (c = 0; c < 4; c++)
      d[r * s + c] = (uint8_t)(r + c < 6 ? p[r + c] : p[6]);
}
static W1_UNUSED void w1_pred_brd(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int p0 = (lf[1] + 2 * ab[0] + ab[1] + 2) >> 2;
  int p1 = (ab[0] + 2 * ab[1] + ab[2] + 2) >> 2;
  int p2 = (ab[1] + 2 * ab[2] + ab[3] + 2) >> 2;
  int p3 = (ab[2] + 2 * ab[3] + ab[4] + 2) >> 2;
  int p4 = (lf[2] + 2 * lf[1] + ab[0] + 2) >> 2;
  int p5 = (lf[3] + 2 * lf[2] + lf[1] + 2) >> 2;
  int p6 = (lf[4] + 2 * lf[3] + lf[2] + 2) >> 2;
  d[0] = (uint8_t)p0; d[1] = (uint8_t)p1; d[2] = (uint8_t)p2;
  d[3] = (uint8_t)p3;
  d[s] = (uint8_t)p4; d[s + 1] = (uint8_t)p0; d[s + 2] = (uint8_t)p1;
  d[s + 3] = (uint8_t)p2;
  d[2 * s] = (uint8_t)p5; d[2 * s + 1] = (uint8_t)p4;
  d[2 * s + 2] = (uint8_t)p0; d[2 * s + 3] = (uint8_t)p1;
  d[3 * s] = (uint8_t)p6; d[3 * s + 1] = (uint8_t)p5;
  d[3 * s + 2] = (uint8_t)p4; d[3 * s + 3] = (uint8_t)p0;
}
static W1_UNUSED void w1_pred_bvr(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int p0 = (ab[0] + ab[1] + 1) >> 1, p1 = (ab[1] + ab[2] + 1) >> 1;
  int p2 = (ab[2] + ab[3] + 1) >> 1, p3 = (ab[3] + ab[4] + 1) >> 1;
  int p4 = (lf[1] + 2 * ab[0] + ab[1] + 2) >> 2;
  int p5 = (ab[0] + 2 * ab[1] + ab[2] + 2) >> 2;
  int p6 = (ab[1] + 2 * ab[2] + ab[3] + 2) >> 2;
  int p7 = (ab[2] + 2 * ab[3] + ab[4] + 2) >> 2;
  int p8 = (lf[2] + 2 * lf[1] + ab[0] + 2) >> 2;
  int p9 = (lf[3] + 2 * lf[2] + lf[1] + 2) >> 2;
  d[0] = (uint8_t)p0; d[1] = (uint8_t)p1; d[2] = (uint8_t)p2;
  d[3] = (uint8_t)p3;
  d[s] = (uint8_t)p4; d[s + 1] = (uint8_t)p5; d[s + 2] = (uint8_t)p6;
  d[s + 3] = (uint8_t)p7;
  d[2 * s] = (uint8_t)p8; d[2 * s + 1] = (uint8_t)p0;
  d[2 * s + 2] = (uint8_t)p1; d[2 * s + 3] = (uint8_t)p2;
  d[3 * s] = (uint8_t)p9; d[3 * s + 1] = (uint8_t)p4;
  d[3 * s + 2] = (uint8_t)p5; d[3 * s + 3] = (uint8_t)p6;
}
static W1_UNUSED void w1_pred_bvl(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int p0 = (ab[1] + ab[2] + 1) >> 1, p1 = (ab[2] + ab[3] + 1) >> 1;
  int p2 = (ab[3] + ab[4] + 1) >> 1, p3 = (ab[4] + ab[5] + 1) >> 1;
  int p4 = (ab[1] + 2 * ab[2] + ab[3] + 2) >> 2;
  int p5 = (ab[2] + 2 * ab[3] + ab[4] + 2) >> 2;
  int p6 = (ab[3] + 2 * ab[4] + ab[5] + 2) >> 2;
  int p7 = (ab[4] + 2 * ab[5] + ab[6] + 2) >> 2;
  int p8 = (ab[5] + 2 * ab[6] + ab[7] + 2) >> 2;
  int p9 = (ab[6] + 2 * ab[7] + ab[8] + 2) >> 2;
  (void)lf;
  d[0] = (uint8_t)p0; d[1] = (uint8_t)p1; d[2] = (uint8_t)p2;
  d[3] = (uint8_t)p3;
  d[s] = (uint8_t)p4; d[s + 1] = (uint8_t)p5; d[s + 2] = (uint8_t)p6;
  d[s + 3] = (uint8_t)p7;
  d[2 * s] = (uint8_t)p1; d[2 * s + 1] = (uint8_t)p2;
  d[2 * s + 2] = (uint8_t)p3; d[2 * s + 3] = (uint8_t)p8;
  d[3 * s] = (uint8_t)p5; d[3 * s + 1] = (uint8_t)p6;
  d[3 * s + 2] = (uint8_t)p7; d[3 * s + 3] = (uint8_t)p9;
}

static W1_UNUSED void w1_pred_bhd(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int p0 = (lf[1] + ab[0] + 1) >> 1;
  int p1 = (lf[1] + 2 * ab[0] + ab[1] + 2) >> 2;
  int p2 = (ab[0] + 2 * ab[1] + ab[2] + 2) >> 2;
  int p3 = (ab[1] + 2 * ab[2] + ab[3] + 2) >> 2;
  int p4 = (lf[2] + lf[1] + 1) >> 1;
  int p5 = (lf[2] + 2 * lf[1] + ab[0] + 2) >> 2;
  int p6 = (lf[3] + lf[2] + 1) >> 1;
  int p7 = (lf[3] + 2 * lf[2] + lf[1] + 2) >> 2;
  int p8 = (lf[4] + lf[3] + 1) >> 1;
  int p9 = (lf[4] + 2 * lf[3] + lf[2] + 2) >> 2;
  d[0] = (uint8_t)p0; d[1] = (uint8_t)p1; d[2] = (uint8_t)p2;
  d[3] = (uint8_t)p3;
  d[s] = (uint8_t)p4; d[s + 1] = (uint8_t)p5; d[s + 2] = (uint8_t)p0;
  d[s + 3] = (uint8_t)p1;
  d[2 * s] = (uint8_t)p6; d[2 * s + 1] = (uint8_t)p7;
  d[2 * s + 2] = (uint8_t)p4; d[2 * s + 3] = (uint8_t)p5;
  d[3 * s] = (uint8_t)p8; d[3 * s + 1] = (uint8_t)p9;
  d[3 * s + 2] = (uint8_t)p6; d[3 * s + 3] = (uint8_t)p7;
}
static W1_UNUSED void w1_pred_bhu(uint8_t *d, int s, const uint8_t *ab,
                                 const uint8_t *lf) {
  int p0 = (lf[1] + lf[2] + 1) >> 1;
  int p1 = (lf[1] + 2 * lf[2] + lf[3] + 2) >> 2;
  int p2 = (lf[2] + lf[3] + 1) >> 1;
  int p3 = (lf[2] + 2 * lf[3] + lf[4] + 2) >> 2;
  int p4 = (lf[3] + lf[4] + 1) >> 1;
  int p5 = (lf[3] + 2 * lf[4] + lf[4] + 2) >> 2;
  int p6 = lf[4];
  (void)ab;
  d[0] = (uint8_t)p0; d[1] = (uint8_t)p1; d[2] = (uint8_t)p2;
  d[3] = (uint8_t)p3;
  d[s] = (uint8_t)p2; d[s + 1] = (uint8_t)p3; d[s + 2] = (uint8_t)p4;
  d[s + 3] = (uint8_t)p5;
  d[2 * s] = (uint8_t)p4; d[2 * s + 1] = (uint8_t)p5;
  d[2 * s + 2] = (uint8_t)p6; d[2 * s + 3] = (uint8_t)p6;
  d[3 * s] = (uint8_t)p6; d[3 * s + 1] = (uint8_t)p6;
  d[3 * s + 2] = (uint8_t)p6; d[3 * s + 3] = (uint8_t)p6;
}
/* 16x16 / 8x8 whole-block modes. above[0..n), left[0..n), corner. */
static W1_UNUSED void w1_pred_dc_n(uint8_t *d, int s, const uint8_t *above,
                                  const uint8_t *left, int n) {
  int i, r, c, dc = 0;
  for (i = 0; i < n; i++) dc += left[i] + above[i];
  dc = (dc + n) >> (n == 16 ? 5 : (n == 8 ? 4 : 3));
  for (r = 0; r < n; r++)
    for (c = 0; c < n; c++) d[r * s + c] = (uint8_t)dc;
}
static W1_UNUSED void w1_pred_v_n(uint8_t *d, int s, const uint8_t *above,
                                 int n) {
  int r, c;
  for (r = 0; r < n; r++)
    for (c = 0; c < n; c++) d[r * s + c] = above[c];
}
static W1_UNUSED void w1_pred_h_n(uint8_t *d, int s, const uint8_t *left,
                                 int n) {
  int r, c;
  for (r = 0; r < n; r++)
    for (c = 0; c < n; c++) d[r * s + c] = left[r];
}
static W1_UNUSED void w1_pred_tm_n(uint8_t *d, int s, const uint8_t *above,
                                  const uint8_t *left, int corner, int n) {
  int r, c;
  for (r = 0; r < n; r++)
    for (c = 0; c < n; c++)
      d[r * s + c] = (uint8_t)w1_clamp255(left[r] + above[c] - corner);
}

/* ---- Token decoding (exact port of RFC 6386 tokens.c) ---- */

/* Extra bits are read MSB-first; stored tables are MSB-first. */
static W1_UNUSED int w1_vp8_token_extra(w1_bool_t *tb, int t) {
  const uint8_t *p = w1k_vp8_pcat_ptr[t - 5];
  int n = w1k_vp8_pcat_nbits[t - 5], b, v = 0;
  for (b = 0; b < n; b++) v = (v << 1) | w1_bool_get(tb, p[b]);
  return v;
}

static W1_UNUSED uint32_t w1_vp8_mb_tokens(w1_vp8d_t *b, w1_bool_t *tb,
                                          int col, int ymode, int seg,
                                          int16_t coeffs[25][16]) {
  int i, stop, type;
  int *dqf;
  uint32_t eob = 0;
  uint8_t *left = b->left_tok, *above = b->above_tok + 9 * col;
  if (ymode != W1_VP8_B) {
    i = 24; stop = 25; type = 1;
  } else {
    i = 0; stop = 16; type = 3;
  }
  for (;;) {
    int li = w1k_vp8_ctx_left[i], ai = w1k_vp8_ctx_above[i];
    int start = (type == 0) ? 1 : 0;
    int c = start, t = left[li] + above[ai];
    const uint8_t *pr;
    int is_y1 = (type == 0 || type == 3);
    dqf = is_y1 ? &b->dqf[seg][0] : (type == 1 ? &b->dqf[seg][4] :
                                                 &b->dqf[seg][2]);
  eob_check:
    pr = &b->coef_probs[type][w1k_vp8_bands[c]][t][0];
    if (!w1_bool_get(tb, pr[W1_N_EOB])) goto block_done;
  zero_check:
    if (!w1_bool_get(tb, pr[W1_N_ZERO])) {
      if (c == 15) goto block_done;
      c++;
      t = 0;
      pr = &b->coef_probs[type][w1k_vp8_bands[c]][0][0];
      goto zero_check;
    }
    if (1) {
      int val;
      if (!w1_bool_get(tb, pr[W1_N_ONE])) {
        val = 1; t = 1;
      } else if (!w1_bool_get(tb, pr[W1_N_LOW])) {
        if (!w1_bool_get(tb, pr[W1_N_TWO])) val = 2;
        else if (!w1_bool_get(tb, pr[W1_N_THREE])) val = 3;
        else val = 4;
        t = 2;
      } else if (!w1_bool_get(tb, pr[W1_N_HIGHLOW])) {
        if (!w1_bool_get(tb, pr[W1_N_CATONE]))
          val = 5 + w1_vp8_token_extra(tb, 5);
        else
          val = 7 + w1_vp8_token_extra(tb, 6);
        t = 2;
      } else if (!w1_bool_get(tb, pr[W1_N_CAT34])) {
        if (!w1_bool_get(tb, pr[W1_N_CAT3]))
          val = 11 + w1_vp8_token_extra(tb, 7);
        else
          val = 19 + w1_vp8_token_extra(tb, 8);
        t = 2;
      } else if (!w1_bool_get(tb, pr[W1_N_CAT5])) {
        val = 35 + w1_vp8_token_extra(tb, 9);
        t = 2;
      } else {
        val = 67 + w1_vp8_token_extra(tb, 10);
        t = 2;
      }
      {
        int sgn = w1_bool_bit(tb);
        int v = (sgn ? -val : val) * dqf[c ? 1 : 0];
        coeffs[i][w1k_vp8_zigzag[c]] = w1_wrap16(v);
      }
      if (c == 15) goto block_done;
      c++;
      goto eob_check;
    }
  block_done:
    eob |= (uint32_t)(c > 1) << i;
    {
      int has = (c != start);
      eob |= (uint32_t)has << 31;
      left[li] = above[ai] = (uint8_t)has;
    }
    if (++i == stop) {
      if (stop == 25) {
        type = 0; i = 0; stop = 16;
      } else if (stop == 16 && type != 2) {
        type = 2; stop = 24;
      } else {
        break;
      }
    }
  }
  return eob;
}

/* ---- Frame header ---- */

static W1_UNUSED void w1_vp8_parse_coeff_probs(w1_bool_t *ctl, w1_vp8d_t *b) {
  int t, bb, cc, nn;
  memcpy(b->coef_probs, w1k_vp8_coef_dflt, 1056);
  b->coef_upd = 0;
  for (t = 0; t < 4; t++)
    for (bb = 0; bb < 8; bb++)
      for (cc = 0; cc < 3; cc++)
        for (nn = 0; nn < 11; nn++)
          if (w1_bool_get(ctl, w1k_vp8_coef_upd[((t * 8) + bb) * 33 +
                                                  cc * 11 + nn])) {
            b->coef_upd++;
            b->coef_probs[t][bb][cc][nn] = (uint8_t)w1_bool_uint(ctl, 8);
          }
}

static W1_UNUSED int w1_vp8_parse_header(w1_bool_t *ctl, w1_vp8d_t *b) {
  int i;
  memset(b->seg_q, 0, 4); memset(b->seg_lf, 0, 4);
  memset(b->ref_delta, 0, 4); memset(b->mode_delta, 0, 4);
  b->seg_tree_probs[0] = b->seg_tree_probs[1] = b->seg_tree_probs[2] = 255;
  if (w1_bool_bit(ctl)) return WEBP1_ERR_UNSUPPORTED;  /* color space must be 0 */
  (void)w1_bool_bit(ctl);   /* clamping type: 0 (spec) or 1 (range-guaranteed) */
  b->seg_on = w1_bool_bit(ctl);
  b->seg_upd_map = 0; b->seg_abs = 0;
  if (b->seg_on) {
    b->seg_upd_map = w1_bool_bit(ctl);
    if (w1_bool_bit(ctl)) {
      b->seg_abs = w1_bool_bit(ctl);
      for (i = 0; i < 4; i++)
        b->seg_q[i] = (int8_t)w1_bool_maybe_int(ctl, 7);
      for (i = 0; i < 4; i++)
        b->seg_lf[i] = (int8_t)w1_bool_maybe_int(ctl, 6);
    }
    if (b->seg_upd_map)
      for (i = 0; i < 3; i++)
        b->seg_tree_probs[i] = w1_bool_bit(ctl)
          ? (uint8_t)w1_bool_uint(ctl, 8) : 255;
  }
  b->use_simple = w1_bool_bit(ctl);
  b->level = w1_bool_uint(ctl, 6);
  b->sharpness = w1_bool_uint(ctl, 3);
  b->delta_on = w1_bool_bit(ctl);
  if (b->delta_on && w1_bool_bit(ctl)) {
    for (i = 0; i < 4; i++)
      b->ref_delta[i] = (int8_t)w1_bool_maybe_int(ctl, 6);
    for (i = 0; i < 4; i++)
      b->mode_delta[i] = (int8_t)w1_bool_maybe_int(ctl, 6);
  }
  b->nparts = 1 << w1_bool_uint(ctl, 2);
  b->q_index = w1_bool_uint(ctl, 7);
  b->y1dc_d = w1_bool_maybe_int(ctl, 4);
  b->y2dc_d = w1_bool_maybe_int(ctl, 4);
  b->y2ac_d = w1_bool_maybe_int(ctl, 4);
  b->uvdc_d = w1_bool_maybe_int(ctl, 4);
  b->uvac_d = w1_bool_maybe_int(ctl, 4);
  w1_bool_bit(ctl);   /* refresh_entropy (KF state is fresh anyway) */
  w1_vp8_parse_coeff_probs(ctl, b);
  b->skip_on = w1_bool_bit(ctl);
  b->skip_prob = b->skip_on ? w1_bool_uint(ctl, 8) : 0;
  return WEBP1_OK;
}

static W1_UNUSED int w1_vp8_clamp_q(int q) {
  return q < 0 ? 0 : (q > 127 ? 127 : q);
}
static W1_UNUSED void w1_vp8_dequant(w1_vp8d_t *b) {
  int i;
  for (i = 0; i < 4; i++) {
    int q = b->q_index;
    int y2ac;
    if (b->seg_on) q = b->seg_abs ? b->seg_q[i] : q + b->seg_q[i];
    b->dqf[i][0] = w1k_vp8_dc_q[w1_vp8_clamp_q(q + b->y1dc_d)];
    b->dqf[i][1] = w1k_vp8_ac_q[w1_vp8_clamp_q(q)];
    b->dqf[i][2] = w1k_vp8_dc_q[w1_vp8_clamp_q(q + b->uvdc_d)];
    if (b->dqf[i][2] > 132) b->dqf[i][2] = 132;
    b->dqf[i][3] = w1k_vp8_ac_q[w1_vp8_clamp_q(q + b->uvac_d)];
    b->dqf[i][4] = w1k_vp8_dc_q[w1_vp8_clamp_q(q + b->y2dc_d)] * 2;
    y2ac = w1k_vp8_ac_q[w1_vp8_clamp_q(q + b->y2ac_d)] * 155 / 100;
    b->dqf[i][5] = y2ac < 8 ? 8 : y2ac;
  }
}

/* ---- Keyframe MB mode decode ---- */

static W1_UNUSED int w1_vp8_read_seg(w1_bool_t *ctl, w1_vp8d_t *b) {
  return w1_bool_get(ctl, b->seg_tree_probs[0])
    ? 2 + w1_bool_get(ctl, b->seg_tree_probs[2])
    : w1_bool_get(ctl, b->seg_tree_probs[1]);
}

static W1_UNUSED void w1_vp8_mb_modes(w1_bool_t *ctl, w1_vp8d_t *b, int col,
                                     int *ymode, int *uvmode,
                                     uint8_t bmodes[16]) {
  int i, ym = w1_bool_tree(ctl, w1k_vp8_kf_ymode_tree, w1k_vp8_kf_ymode_prob,
                           0);
  *ymode = ym;
  if (ym == W1_VP8_B) {
    int ya = b->above_ym[col], yl = b->left_ym;
    for (i = 0; i < 16; i++) {
      int a = (i < 4) ? (ya == W1_VP8_B ? b->above_brow[4 * col + i]
                                        : w1k_vp8_bmode_from_ymode[ya])
                      : bmodes[i - 4];
      int l = (!(i & 3)) ? (yl == W1_VP8_B ? b->left_rcol[i >> 2]
                                           : w1k_vp8_bmode_from_ymode[yl])
                         : bmodes[i - 1];
      bmodes[i] = (uint8_t)w1_bool_tree(ctl, w1k_vp8_bmode_tree,
                                       &w1k_vp8_kf_bmode[a * 90 + l * 9], 0);
    }
    for (i = 0; i < 4; i++) {
      b->above_brow[4 * col + i] = bmodes[12 + i];
      b->left_rcol[i] = bmodes[3 + 4 * i];
    }
  }
  *uvmode = w1_bool_tree(ctl, w1k_vp8_uv_mode_tree, w1k_vp8_kf_uv_prob, 0);
  b->above_ym[col] = (uint8_t)ym;
  b->left_ym = (uint8_t)ym;
}

/* ---- Macroblock reconstruction (in place in the frame planes) ---- */

static W1_UNUSED void w1_vp8_block_residue(uint8_t *dst, int stride,
                                          const int16_t *coef) {
  int cl = w1_vp8_block_class(coef);
  if (cl == 1) w1_vp8_dc_add(dst, stride, dst, coef[0]);
  else if (cl == 2) w1_vp8_idct_add(dst, stride, dst, coef);
}

/* Build 4x4 neighbor arrays ab[0..8] (above[-1..7]) and lf[0..4]. */
static W1_UNUSED void w1_vp8_block_neighbors(w1_vp8d_t *b, int mbx, int mby,
                                            int bx, int by,
                                            const uint8_t *bab,
                                            const uint8_t *blf,
                                            int corner, uint8_t *ab,
                                            uint8_t *lf) {
  int x0 = mbx * 16 + bx * 4, y0 = mby * 16 + by * 4, k;
  int ys = b->y_stride;
  const uint8_t *yp = b->plane_y;
  /* above[0..3] */
  if (by > 0) {
    for (k = 0; k < 4; k++) ab[1 + k] = yp[(y0 - 1) * ys + x0 + k];
  } else {
    for (k = 0; k < 4; k++) ab[1 + k] = bab[bx * 4 + k];
  }
  /* above[4..7]: top block-row uses the border; rightmost blocks reuse
   * the MB-level extension (copy_down rule). */
  if (by == 0) {
    for (k = 0; k < 4; k++) ab[5 + k] = bab[bx * 4 + 4 + k];
  } else if (bx < 3) {
    for (k = 0; k < 4; k++) ab[5 + k] = yp[(y0 - 1) * ys + x0 + 4 + k];
  } else {
    for (k = 0; k < 4; k++) ab[5 + k] = bab[16 + k];
  }
  /* corner (x0-1, y0-1), shared by ab[0] and lf[0] */
  if (bx == 0 && by == 0) {
    ab[0] = lf[0] = (uint8_t)corner;
  } else if (by == 0) {
    ab[0] = lf[0] = bab[bx * 4 - 1];
  } else if (bx == 0 && mbx == 0) {
    ab[0] = lf[0] = 129;
  } else {
    ab[0] = lf[0] = yp[(y0 - 1) * ys + x0 - 1];
  }
  /* left rows 0..3 */
  if (bx > 0) {
    for (k = 0; k < 4; k++) lf[1 + k] = yp[(y0 + k) * ys + x0 - 1];
  } else {
    for (k = 0; k < 4; k++) lf[1 + k] = blf[by * 4 + k];
  }
}

static W1_UNUSED void w1_vp8_luma_edges(w1_vp8d_t *b, int mbx, int mby,
                                       int ymode, uint8_t *bab,
                                       uint8_t *blf, int *corner) {
  int j, ys = b->y_stride;
  for (j = 0; j < 16; j++) {
    if (mbx == 0)
      blf[j] = (ymode == W1_VP8_DC && mby > 0)
        ? b->plane_y[(mby * 16 - 1) * ys + mbx * 16 + j] : 129;
    else
      blf[j] = b->plane_y[(mby * 16 + j) * ys + mbx * 16 - 1];
  }
  for (j = 0; j < 16; j++) {
    if (mby == 0)
      bab[j] = (ymode == W1_VP8_DC && mbx > 0) ? blf[j] : 127;
    else
      bab[j] = b->plane_y[(mby * 16 - 1) * ys + mbx * 16 + j];
  }
  for (j = 0; j < 4; j++) {
    if (mby == 0) bab[16 + j] = 127;
    else if (mbx == b->mb_w - 1) bab[16 + j] = bab[15];
    else bab[16 + j] = b->plane_y[(mby * 16 - 1) * ys + mbx * 16 + 16 + j];
  }
  *corner = mby == 0 ? 127 : (mbx == 0 ? 129
    : b->plane_y[(mby * 16 - 1) * ys + mbx * 16 - 1]);
}

static W1_UNUSED void w1_vp8_predict_b4(w1_vp8d_t *b, int mbx, int mby, int i,
                                       int mode, const uint8_t *bab,
                                       const uint8_t *blf, int corner) {
  int bx = i & 3, by = i >> 2, ys = b->y_stride;
  uint8_t ab[9], lf[5];
  uint8_t *dp = b->plane_y + (mby * 16 + by * 4) * ys + mbx * 16 + bx * 4;
  w1_vp8_block_neighbors(b, mbx, mby, bx, by, bab, blf, corner, ab, lf);
  switch (mode) {
  case 0: w1_pred_bdc(dp, ys, ab, lf); break;
  case 1: w1_pred_btm(dp, ys, ab, lf); break;
  case 2: w1_pred_bve(dp, ys, ab, lf); break;
  case 3: w1_pred_bhe(dp, ys, ab, lf); break;
  case 4: w1_pred_bld(dp, ys, ab, lf); break;
  case 5: w1_pred_brd(dp, ys, ab, lf); break;
  case 6: w1_pred_bvr(dp, ys, ab, lf); break;
  case 7: w1_pred_bvl(dp, ys, ab, lf); break;
  case 8: w1_pred_bhd(dp, ys, ab, lf); break;
  default: w1_pred_bhu(dp, ys, ab, lf); break;
  }
}


static W1_UNUSED void w1_vp8_mb_predict(w1_vp8d_t *b, int mbx, int mby,
                                       int ymode, int uvmode,
                                       const uint8_t bmodes[16]) {
  int j, ys = b->y_stride, uvs = b->uv_stride;
  (void)bmodes;   /* B-mode luma is predicted per-block by reconstructor */
  uint8_t bab[20], blf[16], cabu[8], clfu[8], cabv[8], clfv[8];
  int corner, ccorneru, ccornerv;
  uint8_t *yp = b->plane_y + mby * 16 * ys + mbx * 16;
  uint8_t *up = b->plane_u + mby * 8 * uvs + mbx * 8;
  uint8_t *vp = b->plane_v + mby * 8 * uvs + mbx * 8;
  w1_vp8_luma_edges(b, mbx, mby, ymode, bab, blf, &corner);
  if (ymode == W1_VP8_B) {
    /* 4x4 luma is predicted block-by-block by the reconstructor
     * (each block needs already-reconstructed neighbors). */
  } else if (ymode == W1_VP8_DC) {
    w1_pred_dc_n(yp, ys, bab, blf, 16);
  } else if (ymode == W1_VP8_V) {
    w1_pred_v_n(yp, ys, bab, 16);
  } else if (ymode == W1_VP8_H) {
    w1_pred_h_n(yp, ys, blf, 16);
  } else {
    w1_pred_tm_n(yp, ys, bab, blf, corner, 16);
  }
  /* chroma borders + prediction, per plane */
  for (j = 0; j < 8; j++) {
    if (mbx == 0) {
      clfu[j] = (uvmode == W1_VP8_DC && mby > 0)
        ? b->plane_u[(mby * 8 - 1) * uvs + mbx * 8 + j] : 129;
      clfv[j] = (uvmode == W1_VP8_DC && mby > 0)
        ? b->plane_v[(mby * 8 - 1) * uvs + mbx * 8 + j] : 129;
    } else {
      clfu[j] = b->plane_u[(mby * 8 + j) * uvs + mbx * 8 - 1];
      clfv[j] = b->plane_v[(mby * 8 + j) * uvs + mbx * 8 - 1];
    }
    if (mby == 0) {
      cabu[j] = (uvmode == W1_VP8_DC && mbx > 0) ? clfu[j] : 127;
      cabv[j] = (uvmode == W1_VP8_DC && mbx > 0) ? clfv[j] : 127;
    } else {
      cabu[j] = b->plane_u[(mby * 8 - 1) * uvs + mbx * 8 + j];
      cabv[j] = b->plane_v[(mby * 8 - 1) * uvs + mbx * 8 + j];
    }
  }
  ccorneru = mby == 0 ? 127 : (mbx == 0 ? 129
    : b->plane_u[(mby * 8 - 1) * uvs + mbx * 8 - 1]);
  ccornerv = mby == 0 ? 127 : (mbx == 0 ? 129
    : b->plane_v[(mby * 8 - 1) * uvs + mbx * 8 - 1]);
  if (uvmode == W1_VP8_DC) {
    w1_pred_dc_n(up, uvs, cabu, clfu, 8);
    w1_pred_dc_n(vp, uvs, cabv, clfv, 8);
  } else if (uvmode == W1_VP8_V) {
    w1_pred_v_n(up, uvs, cabu, 8);
    w1_pred_v_n(vp, uvs, cabv, 8);
  } else if (uvmode == W1_VP8_H) {
    w1_pred_h_n(up, uvs, clfu, 8);
    w1_pred_h_n(vp, uvs, clfv, 8);
  } else {
    w1_pred_tm_n(up, uvs, cabu, clfu, ccorneru, 8);
    w1_pred_tm_n(vp, uvs, cabv, clfv, ccornerv, 8);
  }
}
static W1_UNUSED void w1_vp8_mb_recon(w1_vp8d_t *b, int mbx, int mby,
                                     int ymode, int uvmode,
                                     const uint8_t bmodes[16],
                                     int16_t coeffs[25][16]) {
  int i, r, c, ys = b->y_stride, uvs = b->uv_stride;
  uint8_t *yp = b->plane_y + mby * 16 * ys + mbx * 16;
  uint8_t *up = b->plane_u + mby * 8 * uvs + mbx * 8;
  uint8_t *vp = b->plane_v + mby * 8 * uvs + mbx * 8;
  int16_t y2o[16];
  uint8_t bab[20], blf[16];
  int corner;
  /* Chroma + (for 16x16 modes) luma prediction. B-mode luma is done
   * block-by-block below, interleaved with residue. */
  w1_vp8_mb_predict(b, mbx, mby, ymode, uvmode, bmodes);
  if (ymode == W1_VP8_B) {
    int bi;
    w1_vp8_luma_edges(b, mbx, mby, ymode, bab, blf, &corner);
    for (bi = 0; bi < 16; bi++) {
      int bx = bi & 3, by = bi >> 2;
      w1_vp8_predict_b4(b, mbx, mby, bi, bmodes[bi], bab, blf, corner);
      w1_vp8_block_residue(yp + by * 4 * ys + bx * 4, ys, coeffs[bi]);
    }
  }
  if (ymode != W1_VP8_B) {
    /* Y2 block -> DC of each Y block */
    int allzero = 1, dconly = 1;
    for (i = 0; i < 16; i++) {
      if (coeffs[24][i]) {
        allzero = 0;
        if (i) dconly = 0;
      }
    }
    if (!allzero) {
      if (dconly) {
        int v = w1_asr(coeffs[24][0] + 3, 3);
        for (i = 0; i < 16; i++) y2o[i] = (int16_t)v;
      } else {
        w1_vp8_wht(coeffs[24], y2o);
      }
      for (i = 0; i < 16; i++) coeffs[i][0] = y2o[i];
    }
  }
  if (ymode != W1_VP8_B) {
    for (i = 0; i < 16; i++) {
      int bx = i & 3, by = i >> 2;
      w1_vp8_block_residue(yp + by * 4 * ys + bx * 4, ys, coeffs[i]);
    }
  }
  for (i = 0; i < 4; i++) {
    r = i >> 1; c = i & 1;
    w1_vp8_block_residue(up + r * 4 * uvs + c * 4, uvs, coeffs[16 + i]);
    w1_vp8_block_residue(vp + r * 4 * uvs + c * 4, uvs, coeffs[20 + i]);
  }
}


/* ---- Loop filter (exact port of RFC 6386 dixie_loopfilter.c) ---- */

W1_FORCEINLINE int w1_lf_sat8(int x) {
  return x < -128 ? -128 : (x > 127 ? 127 : x);
}
W1_FORCEINLINE int w1_lf_hev(const uint8_t *p, int s, int th) {
  return w1_abs((int)p[-2 * s] - (int)p[-s]) > th ||
         w1_abs((int)p[s] - (int)p[0]) > th;
}
W1_FORCEINLINE int w1_lf_simple_th(const uint8_t *p, int s, int lim) {
  return (4 * w1_abs((int)p[-s] - (int)p[0]) +
          w1_abs((int)p[-2 * s] - (int)p[s])) <= 2 * lim + 1;
}
W1_FORCEINLINE int w1_lf_normal_th(const uint8_t *p, int s, int e, int i) {
  return w1_lf_simple_th(p, s, 2 * e + i) &&
         w1_abs((int)p[-4 * s] - (int)p[-3 * s]) <= i &&
         w1_abs((int)p[-3 * s] - (int)p[-2 * s]) <= i &&
         w1_abs((int)p[-2 * s] - (int)p[-s]) <= i &&
         w1_abs((int)p[3 * s] - (int)p[2 * s]) <= i &&
         w1_abs((int)p[2 * s] - (int)p[s]) <= i &&
         w1_abs((int)p[s] - (int)p[0]) <= i;
}
W1_FORCEINLINE void w1_lf_common(uint8_t *p, int s, int outer) {
  int a = 3 * ((int)p[0] - (int)p[-s]);
  int f1, f2;
  if (outer) a += w1_lf_sat8((int)p[-2 * s] - (int)p[s]);
  f1 = w1_asr(a + 4, 3);
  f2 = w1_asr(a + 3, 3);
  if (f1 > 15) f1 = 15; else if (f1 < -16) f1 = -16;
  if (f2 > 15) f2 = 15; else if (f2 < -16) f2 = -16;
  p[-s] = (uint8_t)w1_clamp255((int)p[-s] + f2);
  p[0] = (uint8_t)w1_clamp255((int)p[0] - f1);
  if (!outer) {
    a = w1_asr(f1 + 1, 1);
    p[-2 * s] = (uint8_t)w1_clamp255((int)p[-2 * s] + a);
    p[s] = (uint8_t)w1_clamp255((int)p[s] - a);
  }
}
W1_FORCEINLINE void w1_lf_mb_edge(uint8_t *p, int s) {
  int w = w1_lf_sat8(w1_lf_sat8((int)p[-2 * s] - (int)p[s]) +
                     3 * ((int)p[0] - (int)p[-s]));
  int a = w1_asr(27 * w + 63, 7);
  p[-s] = (uint8_t)w1_clamp255((int)p[-s] + a);
  p[0] = (uint8_t)w1_clamp255((int)p[0] - a);
  a = w1_asr(18 * w + 63, 7);
  p[-2 * s] = (uint8_t)w1_clamp255((int)p[-2 * s] + a);
  p[s] = (uint8_t)w1_clamp255((int)p[s] - a);
  a = w1_asr(9 * w + 63, 7);
  p[-3 * s] = (uint8_t)w1_clamp255((int)p[-3 * s] + a);
  p[2 * s] = (uint8_t)w1_clamp255((int)p[2 * s] - a);
}
static W1_UNUSED void w1_lf_mb_v(uint8_t *p, int s, int e, int ii, int hev,
                                int n) {
  int k;
  for (k = 0; k < n; k++) {
    if (w1_lf_normal_th(p, 1, e, ii)) {
      if (w1_lf_hev(p, 1, hev)) w1_lf_common(p, 1, 1);
      else w1_lf_mb_edge(p, 1);
    }
    p += s;
  }
}
static W1_UNUSED void w1_lf_mb_h(uint8_t *p, int s, int e, int ii, int hev,
                                int n) {
  int k;
  for (k = 0; k < n; k++) {
    if (w1_lf_normal_th(p, s, e, ii)) {
      if (w1_lf_hev(p, s, hev)) w1_lf_common(p, s, 1);
      else w1_lf_mb_edge(p, s);
    }
    p++;
  }
}
static W1_UNUSED void w1_lf_sub_v(uint8_t *p, int s, int e, int ii, int hev,
                                 int n) {
  int k;
  for (k = 0; k < n; k++) {
    if (w1_lf_normal_th(p, 1, e, ii))
      w1_lf_common(p, 1, w1_lf_hev(p, 1, hev));
    p += s;
  }
}
static W1_UNUSED void w1_lf_sub_h(uint8_t *p, int s, int e, int ii, int hev,
                                 int n) {
  int k;
  for (k = 0; k < n; k++) {
    if (w1_lf_normal_th(p, s, e, ii))
      w1_lf_common(p, s, w1_lf_hev(p, s, hev));
    p++;
  }
}
static W1_UNUSED void w1_lf_simple_v(uint8_t *p, int s, int lim) {
  int k;
  for (k = 0; k < 16; k++) {
    if (w1_lf_simple_th(p, 1, lim)) w1_lf_common(p, 1, 1);
    p += s;
  }
}
static W1_UNUSED void w1_lf_simple_h(uint8_t *p, int s, int lim) {
  int k;
  for (k = 0; k < 16; k++) {
    if (w1_lf_simple_th(p, s, lim)) w1_lf_common(p, s, 1);
    p++;
  }
}
static W1_UNUSED void w1_vp8_filter_mb(w1_vp8d_t *b, int mbx, int mby) {
  int idx = mby * b->mb_w + mbx;
  int seg = b->mb_seg[idx], ym = b->mb_ymode[idx];
  int flt = b->level, e, ii, hev;
  uint8_t *yp = b->plane_y + mby * 16 * b->y_stride + mbx * 16;
  uint8_t *up = b->plane_u + mby * 8 * b->uv_stride + mbx * 8;
  uint8_t *vp = b->plane_v + mby * 8 * b->uv_stride + mbx * 8;
  if (b->seg_on)
    flt = b->seg_abs ? b->seg_lf[seg] : flt + b->seg_lf[seg];
  if (flt > 63) flt = 63; else if (flt < 0) flt = 0;
  if (b->delta_on) {
    flt += b->ref_delta[0];
    if (ym == W1_VP8_B) flt += b->mode_delta[0];
  }
  if (flt > 63) flt = 63; else if (flt < 0) flt = 0;
  e = flt;
  if (!e) return;
  ii = flt;
  if (b->sharpness) {
    ii >>= b->sharpness > 4 ? 2 : 1;
    if (ii > 9 - b->sharpness) ii = 9 - b->sharpness;
  }
  if (ii < 1) ii = 1;
  hev = flt >= 15 ? 1 : 0;
  if (flt >= 40) hev++;
  if (b->use_simple) {
    int sub = b->mb_eob[idx] || ym == W1_VP8_B;
    int mb_lim = (e + 2) * 2 + ii, b_lim = e * 2 + ii;
    if (mbx) w1_lf_simple_v(yp, b->y_stride, mb_lim);
    if (sub) {
      w1_lf_simple_v(yp + 4, b->y_stride, b_lim);
      w1_lf_simple_v(yp + 8, b->y_stride, b_lim);
      w1_lf_simple_v(yp + 12, b->y_stride, b_lim);
    }
    if (mby) w1_lf_simple_h(yp, b->y_stride, mb_lim);
    if (sub) {
      w1_lf_simple_h(yp + 4 * b->y_stride, b->y_stride, b_lim);
      w1_lf_simple_h(yp + 8 * b->y_stride, b->y_stride, b_lim);
      w1_lf_simple_h(yp + 12 * b->y_stride, b->y_stride, b_lim);
    }
  } else {
    int sub = b->mb_eob[idx] || ym == W1_VP8_B;
    if (mbx) {
      w1_lf_mb_v(yp, b->y_stride, e + 2, ii, hev, 16);
      w1_lf_mb_v(up, b->uv_stride, e + 2, ii, hev, 8);
      w1_lf_mb_v(vp, b->uv_stride, e + 2, ii, hev, 8);
    }
    if (sub) {
      w1_lf_sub_v(yp + 4, b->y_stride, e, ii, hev, 16);
      w1_lf_sub_v(yp + 8, b->y_stride, e, ii, hev, 16);
      w1_lf_sub_v(yp + 12, b->y_stride, e, ii, hev, 16);
      w1_lf_sub_v(up + 4, b->uv_stride, e, ii, hev, 8);
      w1_lf_sub_v(vp + 4, b->uv_stride, e, ii, hev, 8);
    }
    if (mby) {
      w1_lf_mb_h(yp, b->y_stride, e + 2, ii, hev, 16);
      w1_lf_mb_h(up, b->uv_stride, e + 2, ii, hev, 8);
      w1_lf_mb_h(vp, b->uv_stride, e + 2, ii, hev, 8);
    }
    if (sub) {
      w1_lf_sub_h(yp + 4 * b->y_stride, b->y_stride, e, ii, hev, 16);
      w1_lf_sub_h(yp + 8 * b->y_stride, b->y_stride, e, ii, hev, 16);
      w1_lf_sub_h(yp + 12 * b->y_stride, b->y_stride, e, ii, hev, 16);
      w1_lf_sub_h(up + 4 * b->uv_stride, b->uv_stride, e, ii, hev, 8);
      w1_lf_sub_h(vp + 4 * b->uv_stride, b->uv_stride, e, ii, hev, 8);
    }
  }
}
/* ---- Main VP8 decode entry ---- */

typedef struct w1_vp8_frame {
  uint8_t *y, *u, *v;
  int y_stride, uv_stride;
  int w, h;
} w1_vp8_frame_t;

static W1_UNUSED int w1_vp8_peek_dims(const uint8_t *d, size_t n, int *w,
                                     int *h);

static W1_UNUSED int w1_vp8_decode(const uint8_t *data, size_t size,
                                  w1_bump_t *bump, w1_vp8_frame_t *fr) {
  uint32_t tag;
  int frame_type, w, h, rc;
  size_t part0, po;
  int row, col, parts, i;
  w1_bool_t ctl;
  w1_vp8d_t s;
  const uint8_t *tok_base;
  size_t tok_avail;
  size_t y_sz, uv_sz;
  uint8_t *mem;
  if (!data || !bump || !fr) return WEBP1_ERR_BAD_PARAM;
  if (size < 10) return WEBP1_ERR_TRUNCATED;
  tag = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16);
  frame_type = (int)(tag & 1);
  if (frame_type) return WEBP1_ERR_UNSUPPORTED;   /* interframe */
  if (!((tag >> 4) & 1)) return WEBP1_ERR_UNSUPPORTED;   /* hidden frame */
  if ((tag >> 3) & 1) return WEBP1_ERR_UNSUPPORTED;   /* experimental */
  if (data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a)
    return WEBP1_ERR_UNSUPPORTED;
  w = data[6] | (data[7] << 8);
  w &= 0x3fff;
  h = data[8] | (data[9] << 8);
  h &= 0x3fff;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM)
    return WEBP1_ERR_CORRUPT;
  part0 = (size_t)((tag >> 5) & 0x7ffff);
  if (size <= part0 + 10) return WEBP1_ERR_TRUNCATED;
  memset(&s, 0, sizeof(s));
  s.mb_w = (w + 15) / 16;
  s.mb_h = (h + 15) / 16;
  /* S3: validate the control partition before allocating ~392 MB of frame
   * planes for attacker-controlled dims (parse touches only scalar/table
   * fields, never the planes, so output is identical for valid streams). */
  w1_bool_init(&ctl, data + 10, part0);
  rc = w1_vp8_parse_header(&ctl, &s);
  if (rc) return rc;
  if (ctl.empty) return WEBP1_ERR_CORRUPT;
  s.y_stride = s.mb_w * 16;
  s.uv_stride = s.mb_w * 8;
  y_sz = (size_t)s.mb_w * 16 * (size_t)s.mb_h * 16;
  uv_sz = (size_t)s.mb_w * 8 * (size_t)s.mb_h * 8;
  mem = (uint8_t *)w1_bump_alloc(bump, y_sz + 2 * uv_sz, 1);
  s.above_tok = (uint8_t *)w1_bump_alloc(bump, 9 * (size_t)(s.mb_w + 1), 1);
  s.above_ym = (uint8_t *)w1_bump_alloc(bump, (size_t)(s.mb_w + 1), 1);
  s.above_brow = (uint8_t *)w1_bump_alloc(bump, 4 * (size_t)(s.mb_w + 1), 1);
  s.mb_seg = (uint8_t *)w1_bump_alloc(bump,
                                      3 * (size_t)s.mb_w * (size_t)s.mb_h, 1);
  if (!mem || !s.above_tok || !s.above_ym || !s.above_brow || !s.mb_seg)
    return WEBP1_ERR_NO_MEMORY;
  s.plane_y = mem;
  s.plane_u = mem + y_sz;
  s.plane_v = mem + y_sz + uv_sz;
  memset(s.above_tok, 0, 9 * (size_t)(s.mb_w + 1));
  memset(s.above_ym, 0, (size_t)(s.mb_w + 1));
  memset(s.above_brow, 0, 4 * (size_t)(s.mb_w + 1));
  s.mb_ymode = s.mb_seg + (size_t)s.mb_w * (size_t)s.mb_h;
  s.mb_eob = s.mb_ymode + (size_t)s.mb_w * (size_t)s.mb_h;
  /* Token partitions. */
  parts = s.nparts;
  tok_base = data + 10 + part0;
  tok_avail = size - 10 - part0;
  if (tok_avail < 3 * (size_t)(parts - 1)) return WEBP1_ERR_TRUNCATED;
  po = 3 * (size_t)(parts - 1);
  for (i = 0; i < parts; i++) {
    size_t psz;
    if (i < parts - 1) {
      psz = w1_le24(tok_base + 3 * i);
    } else {
      psz = tok_avail - po;
    }
    if (psz > tok_avail - po) return WEBP1_ERR_TRUNCATED;
    w1_bool_init(&s.tok[i], tok_base + po, psz);
    po += psz;
  }
  w1_vp8_dequant(&s);
  /* Decode rows (MB modes + tokens + reconstruct). */
  for (row = 0; row < s.mb_h; row++) {
    w1_bool_t *tb = &s.tok[row % parts];
    memset(s.left_tok, 0, 9);
    s.left_ym = 0;
    for (col = 0; col < s.mb_w; col++) {
      int seg = 0, skip = 0, ymode, uvmode, idx = row * s.mb_w + col;
      uint8_t bmodes[16];
      int16_t coeffs[25][16];
      uint32_t eob;
      if (s.seg_upd_map) seg = w1_vp8_read_seg(&ctl, &s);
      if (s.skip_on) skip = w1_bool_get(&ctl, s.skip_prob);
      for (i = 0; i < 16; i++) bmodes[i] = 0;
      w1_vp8_mb_modes(&ctl, &s, col, &ymode, &uvmode, bmodes);
      memset(coeffs, 0, sizeof(coeffs));
      if (skip) {
        int k;
        for (k = 0; k < 8; k++)
          s.left_tok[k] = s.above_tok[9 * col + k] = 0;
        if (ymode != W1_VP8_B)
          s.left_tok[8] = s.above_tok[9 * col + 8] = 0;
        eob = 0;
      } else {
        eob = w1_vp8_mb_tokens(&s, tb, col, ymode, seg, coeffs);
      }
      s.mb_seg[idx] = (uint8_t)seg;
      s.mb_ymode[idx] = (uint8_t)ymode;
      s.mb_eob[idx] = eob ? 1 : 0;
      w1_vp8_mb_recon(&s, col, row, ymode, uvmode, bmodes, coeffs);
    }
  }
  /* Overconsumption = truncated/corrupt: bool reads past a partition end
   * zero-fill (memory-safe) but decode garbage. Pad bytes left inside a
   * partition remain legal. Matches reference end-of-partition errors. */
  if (ctl.empty) return WEBP1_ERR_CORRUPT;
  for (i = 0; i < parts; i++) if (s.tok[i].empty) return WEBP1_ERR_CORRUPT;
  /* Loop filter over the whole frame. */
  if (s.level) {
    for (row = 0; row < s.mb_h; row++)
      for (col = 0; col < s.mb_w; col++) w1_vp8_filter_mb(&s, col, row);
  }
  fr->y = s.plane_y; fr->u = s.plane_u; fr->v = s.plane_v;
  fr->y_stride = s.y_stride; fr->uv_stride = s.uv_stride;
  fr->w = w; fr->h = h;
  return WEBP1_OK;
}
/* ---- YUV->RGBA (4-tap fancy upsampling; bit-exact reconstruction) ---- */


static W1_UNUSED int w1_clip8(int v) {
  return v < 0 ? 0 : (v > 16383 ? 255 : (v >> 6));
}
static W1_UNUSED void w1_fancy_px(int y, int u, int v, uint8_t *dst) {
  int mh_y = (y * 19077) >> 8;
  dst[0] = (uint8_t)w1_clip8(mh_y + ((v * 26149) >> 8) - 14234);
  dst[1] = (uint8_t)w1_clip8(mh_y - ((u * 6419) >> 8) -
                             ((v * 13320) >> 8) + 8708);
  dst[2] = (uint8_t)w1_clip8(mh_y + ((u * 33050) >> 8) - 17685);
  dst[3] = 255;
}
#ifdef W1_USE_SSE2
/* Lane-wise 32x32->low-32 multiply (SSE2 has no pmulld). All inputs are
 * small non-negative Y/U/V values, so the unsigned 32-bit product is exact. */

/* BT.601 conversion for 8 pixels: y points at 8 luma bytes, u/v at 8
 * interpolated chroma ints each. Writes 32 interleaved RGBA bytes.
 * Bit-exact with w1_fancy_px (clip8 == clamp(v>>6, 0..255), realised by
 * the signed->unsigned pack saturation at the end). */
static W1_UNUSED void w1_fancy_px8_sse2(const uint8_t *y, const int *u,
                                        const int *v, uint8_t *dst) {
  const __m128i zero = _mm_setzero_si128();
  __m128i yb = _mm_loadl_epi64((const __m128i *)(const void *)y);
  __m128i y16 = _mm_unpacklo_epi8(yb, zero);
  __m128i y0 = _mm_unpacklo_epi16(y16, zero);
  __m128i y1 = _mm_unpackhi_epi16(y16, zero);
  __m128i u0 = _mm_loadu_si128((const __m128i *)(const void *)u);
  __m128i u1 = _mm_loadu_si128((const __m128i *)(const void *)(u + 4));
  __m128i v0 = _mm_loadu_si128((const __m128i *)(const void *)v);
  __m128i v1 = _mm_loadu_si128((const __m128i *)(const void *)(v + 4));
  __m128i mhy0 = _mm_srai_epi32(w1_mullo32_sse2(y0, _mm_set1_epi32(19077)), 8);
  __m128i mhy1 = _mm_srai_epi32(w1_mullo32_sse2(y1, _mm_set1_epi32(19077)), 8);
  __m128i vr0 = _mm_srai_epi32(w1_mullo32_sse2(v0, _mm_set1_epi32(26149)), 8);
  __m128i vr1 = _mm_srai_epi32(w1_mullo32_sse2(v1, _mm_set1_epi32(26149)), 8);
  __m128i ug0 = _mm_srai_epi32(w1_mullo32_sse2(u0, _mm_set1_epi32(6419)), 8);
  __m128i ug1 = _mm_srai_epi32(w1_mullo32_sse2(u1, _mm_set1_epi32(6419)), 8);
  __m128i vg0 = _mm_srai_epi32(w1_mullo32_sse2(v0, _mm_set1_epi32(13320)), 8);
  __m128i vg1 = _mm_srai_epi32(w1_mullo32_sse2(v1, _mm_set1_epi32(13320)), 8);
  __m128i ub0 = _mm_srai_epi32(w1_mullo32_sse2(u0, _mm_set1_epi32(33050)), 8);
  __m128i ub1 = _mm_srai_epi32(w1_mullo32_sse2(u1, _mm_set1_epi32(33050)), 8);
  __m128i r0 = _mm_srai_epi32(
      _mm_sub_epi32(_mm_add_epi32(mhy0, vr0), _mm_set1_epi32(14234)), 6);
  __m128i r1 = _mm_srai_epi32(
      _mm_sub_epi32(_mm_add_epi32(mhy1, vr1), _mm_set1_epi32(14234)), 6);
  __m128i g0 = _mm_srai_epi32(
      _mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(mhy0, ug0), vg0),
                    _mm_set1_epi32(8708)), 6);
  __m128i g1 = _mm_srai_epi32(
      _mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(mhy1, ug1), vg1),
                    _mm_set1_epi32(8708)), 6);
  __m128i b0 = _mm_srai_epi32(
      _mm_sub_epi32(_mm_add_epi32(mhy0, ub0), _mm_set1_epi32(17685)), 6);
  __m128i b1 = _mm_srai_epi32(
      _mm_sub_epi32(_mm_add_epi32(mhy1, ub1), _mm_set1_epi32(17685)), 6);
  {
    __m128i r16 = _mm_packs_epi32(r0, r1);
    __m128i g16 = _mm_packs_epi32(g0, g1);
    __m128i b16 = _mm_packs_epi32(b0, b1);
    __m128i a16 = _mm_set1_epi16(255);
    __m128i rg_lo = _mm_unpacklo_epi16(r16, g16);
    __m128i rg_hi = _mm_unpackhi_epi16(r16, g16);
    __m128i ba_lo = _mm_unpacklo_epi16(b16, a16);
    __m128i ba_hi = _mm_unpackhi_epi16(b16, a16);
    __m128i p0 = _mm_packus_epi16(rg_lo, ba_lo);
    __m128i p1 = _mm_packus_epi16(rg_hi, ba_hi);
    __m128i q0 = _mm_unpacklo_epi16(p0, _mm_srli_si128(p0, 8));
    __m128i q1 = _mm_unpacklo_epi16(p1, _mm_srli_si128(p1, 8));
    _mm_storeu_si128((__m128i *)(void *)dst, q0);
    _mm_storeu_si128((__m128i *)(void *)(dst + 16), q1);
  }
}
/* Fancy-upsample a chroma row pair and convert, mirroring w1_fancy_pair.
 * Direct per-pixel chroma formulas are algebraically identical to the
 * scalar recursion; groups of 8 interior pixels go through w1_fancy_px8_sse2
 * and the boundary pixels use the scalar reference. */
static W1_UNUSED void w1_fancy_pair_sse2(
    const uint8_t *top_y, const uint8_t *bot_y, const uint8_t *top_u,
    const uint8_t *top_v, const uint8_t *cur_u, const uint8_t *cur_v,
    uint8_t *top_dst, uint8_t *bot_dst, int len) {
  int i, pend = (len & 1) ? (len - 1) : (len - 2);
  {
    int tu = top_u[0], tv = top_v[0], cu = cur_u[0], cv = cur_v[0];
    w1_fancy_px(top_y[0], (3 * tu + cu + 2) >> 2, (3 * tv + cv + 2) >> 2,
                top_dst);
    if (bot_y)
      w1_fancy_px(bot_y[0], (3 * cu + tu + 2) >> 2, (3 * cv + tv + 2) >> 2,
                  bot_dst);
  }
  for (i = 1; i + 7 <= pend; i += 8) {
    int j, ub[8], vb[8], ubb[8], vbb[8];
    int base = (i - 1) >> 1;
    for (j = 0; j < 4; j++) {
      int p = base + j, q = p + 1;
      int a = top_u[p], b = top_u[q], c = cur_u[p], d = cur_u[q];
      int d12 = (a + b + c + d + 8 + 2 * (b + c)) >> 3;
      int d03 = (a + b + c + d + 8 + 2 * (a + d)) >> 3;
      ub[2 * j] = (d12 + a) >> 1;      ubb[2 * j] = (d03 + c) >> 1;
      ub[2 * j + 1] = (d03 + b) >> 1;  ubb[2 * j + 1] = (d12 + d) >> 1;
      a = top_v[p]; b = top_v[q]; c = cur_v[p]; d = cur_v[q];
      d12 = (a + b + c + d + 8 + 2 * (b + c)) >> 3;
      d03 = (a + b + c + d + 8 + 2 * (a + d)) >> 3;
      vb[2 * j] = (d12 + a) >> 1;      vbb[2 * j] = (d03 + c) >> 1;
      vb[2 * j + 1] = (d03 + b) >> 1;  vbb[2 * j + 1] = (d12 + d) >> 1;
    }
    w1_fancy_px8_sse2(top_y + i, ub, vb, top_dst + (size_t)i * 4);
    if (bot_y)
      w1_fancy_px8_sse2(bot_y + i, ubb, vbb, bot_dst + (size_t)i * 4);
  }
  for (; i <= pend; i++) {
    int lo = (i - 1) >> 1, hi = lo + 1;
    int a = top_u[lo], b = top_u[hi], c = cur_u[lo], d = cur_u[hi];
    int d12 = (a + b + c + d + 8 + 2 * (b + c)) >> 3;
    int d03 = (a + b + c + d + 8 + 2 * (a + d)) >> 3;
    int ut, vt, ub2, vb2;
    if (i & 1) { ut = (d12 + a) >> 1; ub2 = (d03 + c) >> 1; }
    else       { ut = (d03 + b) >> 1; ub2 = (d12 + d) >> 1; }
    a = top_v[lo]; b = top_v[hi]; c = cur_v[lo]; d = cur_v[hi];
    d12 = (a + b + c + d + 8 + 2 * (b + c)) >> 3;
    d03 = (a + b + c + d + 8 + 2 * (a + d)) >> 3;
    if (i & 1) { vt = (d12 + a) >> 1; vb2 = (d03 + c) >> 1; }
    else       { vt = (d03 + b) >> 1; vb2 = (d12 + d) >> 1; }
    w1_fancy_px(top_y[i], ut, vt, top_dst + (size_t)i * 4);
    if (bot_y)
      w1_fancy_px(bot_y[i], ub2, vb2, bot_dst + (size_t)i * 4);
  }
  if (!(len & 1)) {
    int col = (len >> 1) - 1;
    int tu = top_u[col], tv = top_v[col], cu = cur_u[col], cv = cur_v[col];
    w1_fancy_px(top_y[len - 1], (3 * tu + cu + 2) >> 2,
                (3 * tv + cv + 2) >> 2, top_dst + (size_t)(len - 1) * 4);
    if (bot_y)
      w1_fancy_px(bot_y[len - 1], (3 * cu + tu + 2) >> 2,
                  (3 * cv + tv + 2) >> 2, bot_dst + (size_t)(len - 1) * 4);
  }
}
#endif
static W1_UNUSED void w1_fancy_pair(const uint8_t *top_y,
                                   const uint8_t *bot_y,
                                   const uint8_t *top_u, const uint8_t *top_v,
                                   const uint8_t *cur_u, const uint8_t *cur_v,
                                   uint8_t *top_dst, uint8_t *bot_dst,
                                   int len) {
#ifdef W1_USE_SSE2
  w1_fancy_pair_sse2(top_y, bot_y, top_u, top_v, cur_u, cur_v, top_dst,
                     bot_dst, len);
#else
  int x, last = (len - 1) >> 1;
  int tl_u = top_u[0], tl_v = top_v[0], l_u = cur_u[0], l_v = cur_v[0];
  w1_fancy_px(top_y[0], (3 * tl_u + l_u + 2) >> 2,
              (3 * tl_v + l_v + 2) >> 2, top_dst);
  if (bot_y)
    w1_fancy_px(bot_y[0], (3 * l_u + tl_u + 2) >> 2,
                (3 * l_v + tl_v + 2) >> 2, bot_dst);
  for (x = 1; x <= last; x++) {
    int t_u = top_u[x], t_v = top_v[x], c_u = cur_u[x], c_v = cur_v[x];
    int d12_u = (tl_u + t_u + l_u + c_u + 8 + 2 * (t_u + l_u)) >> 3;
    int d12_v = (tl_v + t_v + l_v + c_v + 8 + 2 * (t_v + l_v)) >> 3;
    int d03_u = (tl_u + t_u + l_u + c_u + 8 + 2 * (tl_u + c_u)) >> 3;
    int d03_v = (tl_v + t_v + l_v + c_v + 8 + 2 * (tl_v + c_v)) >> 3;
    w1_fancy_px(top_y[2 * x - 1], (d12_u + tl_u) >> 1,
                (d12_v + tl_v) >> 1, top_dst + (2 * x - 1) * 4);
    w1_fancy_px(top_y[2 * x], (d03_u + t_u) >> 1, (d03_v + t_v) >> 1,
                top_dst + (2 * x) * 4);
    if (bot_y) {
      w1_fancy_px(bot_y[2 * x - 1], (d03_u + l_u) >> 1, (d03_v + l_v) >> 1,
                  bot_dst + (2 * x - 1) * 4);
      w1_fancy_px(bot_y[2 * x], (d12_u + c_u) >> 1, (d12_v + c_v) >> 1,
                  bot_dst + (2 * x) * 4);
    }
    tl_u = t_u; tl_v = t_v; l_u = c_u; l_v = c_v;
  }
  if (!(len & 1)) {
    w1_fancy_px(top_y[len - 1], (3 * tl_u + l_u + 2) >> 2,
                (3 * tl_v + l_v + 2) >> 2, top_dst + (len - 1) * 4);
    if (bot_y)
      w1_fancy_px(bot_y[len - 1], (3 * l_u + tl_u + 2) >> 2,
                  (3 * l_v + tl_v + 2) >> 2, bot_dst + (len - 1) * 4);
  }
#endif
}
static W1_UNUSED void w1_vp8_yuv_to_rgba(const w1_vp8_frame_t *f,
                                        uint8_t *out) {
  int w = f->w, h = f->h, k, nkl;
  const uint8_t *y = f->y, *u = f->u, *v = f->v;
  w1_fancy_pair(y, NULL, u, v, u, v, out, NULL, w);
  nkl = (h - 1) / 2;
  for (k = 1; k <= nkl; k++) {
    w1_fancy_pair(y + (2 * k - 1) * f->y_stride, y + 2 * k * f->y_stride,
                  u + (k - 1) * f->uv_stride, v + (k - 1) * f->uv_stride,
                  u + k * f->uv_stride, v + k * f->uv_stride,
                  out + (size_t)(2 * k - 1) * (size_t)w * 4,
                  out + (size_t)(2 * k) * (size_t)w * 4, w);
  }
  if (!(h & 1)) {
    int hc = (h + 1) / 2;
    w1_fancy_pair(y + (h - 1) * f->y_stride, NULL,
                  u + (hc - 1) * f->uv_stride, v + (hc - 1) * f->uv_stride,
                  u + (hc - 1) * f->uv_stride, v + (hc - 1) * f->uv_stride,
                  out + (size_t)(h - 1) * (size_t)w * 4, NULL, w);
  }
}

/* == S5: ALPH decoder (transparency chunk) == */

/* ---- ALPH decoder ---- */

static W1_UNUSED int w1_grad_pred(int a, int b, int c) {
  int g = a + b - c;
  return g < 0 ? 0 : (g > 255 ? 255 : g);
}
/* Unfilter one row; prev == NULL for the first row. In-place safe. */
static W1_UNUSED void w1_alph_unfilter(int filter, const uint8_t *prev,
                                      const uint8_t *in, uint8_t *out,
                                      int w) {
  int i;
  if (filter == 0) {
    if (out != in) for (i = 0; i < w; i++) out[i] = in[i];
  } else if (filter == 1) {
    int pred = prev ? prev[0] : 0;
    for (i = 0; i < w; i++) { pred = (pred + in[i]) & 0xff; out[i] = (uint8_t)pred; }
  } else if (filter == 2) {
    if (!prev) {
      int pred = 0;
      for (i = 0; i < w; i++) { pred = (pred + in[i]) & 0xff; out[i] = (uint8_t)pred; }
    } else {
      for (i = 0; i < w; i++) out[i] = (uint8_t)((prev[i] + in[i]) & 0xff);
    }
  } else {
    if (!prev) {
      int pred = 0;
      for (i = 0; i < w; i++) { pred = (pred + in[i]) & 0xff; out[i] = (uint8_t)pred; }
    } else {
      int top = prev[0], top_left = top, left = top;
      for (i = 0; i < w; i++) {
        top = prev[i];
        left = (in[i] + w1_grad_pred(left, top, top_left)) & 0xff;
        top_left = top;
        out[i] = (uint8_t)left;
      }
    }
  }
}

static W1_UNUSED int w1_alph_decode(const uint8_t *data, size_t size, int w,
                                   int h, w1_bump_t *bump, uint8_t *alpha) {
  int method, filter, pre, rsrv, y;
  size_t npix;
  if (size < 1) return WEBP1_ERR_TRUNCATED;
  method = data[0] & 3;
  filter = (data[0] >> 2) & 3;
  pre = (data[0] >> 4) & 3;
  rsrv = (data[0] >> 6) & 3;
  if (method > 1 || pre > 1 || rsrv != 0) return WEBP1_ERR_CORRUPT;
  npix = (size_t)w * (size_t)h;
  if (method == 0) {
    const uint8_t *deltas;
    if (size - 1 < npix) return WEBP1_ERR_TRUNCATED;
    deltas = data + 1;
    for (y = 0; y < h; y++) {
      w1_alph_unfilter(filter, y ? alpha + (y - 1) * w : NULL,
                       deltas + (size_t)y * (size_t)w, alpha +
                       (size_t)y * (size_t)w, w);
    }
    return WEBP1_OK;
  } else {
    uint32_t *pix;
    int i, n = w * h, rc;
    pix = (uint32_t *)w1_bump_alloc(bump, npix * 4, 4);
    if (!pix) return WEBP1_ERR_NO_MEMORY;
    rc = w1_vp8l_decode_raw(data + 1, size - 1, w, h, pix, bump);
    if (rc) return rc;
    for (i = 0; i < n; i++) alpha[i] = (uint8_t)((pix[i] >> 8) & 0xff);
    for (y = 0; y < h; y++) {
      /* Unfilter in place (row-at-a-time: prev row already done). */
      uint8_t *row = alpha + (size_t)y * (size_t)w;
      /* copy row to scratch? No: horizontal reads in[i] once; vertical/
       * gradient read prev row (done) — in-place safe except the row
       * itself is both in and out: horizontal: out[i] depends on
       * out[i-1] and in[i] (unread) — safe. vertical: out[i] =
       * prev[i]+in[i] — safe. gradient: same — safe. */
      w1_alph_unfilter(filter, y ? row - w : NULL, row, row, w);
    }
    return WEBP1_OK;
  }
}

/* == S6: Container + animation (RIFF/VP8X parsing, compositor, decode API) == */

/* ---- Container parsing ---- */

#define W1_FCC_VP8 WEBP1_FOURCC('V', 'P', '8', ' ')
#define W1_FCC_VP8L WEBP1_FOURCC('V', 'P', '8', 'L')
#define W1_FCC_VP8X WEBP1_FOURCC('V', 'P', '8', 'X')
#define W1_FCC_ANIM WEBP1_FOURCC('A', 'N', 'I', 'M')
#define W1_FCC_ANMF WEBP1_FOURCC('A', 'N', 'M', 'F')
#define W1_FCC_ALPH WEBP1_FOURCC('A', 'L', 'P', 'H')
#define W1_FCC_ICCP WEBP1_FOURCC('I', 'C', 'C', 'P')
#define W1_FCC_EXIF WEBP1_FOURCC('E', 'X', 'I', 'F')
#define W1_FCC_XMP WEBP1_FOURCC('X', 'M', 'P', ' ')

#define W1_FLAG_ANIM 0x02
#define W1_FLAG_XMP 0x04
#define W1_FLAG_EXIF 0x08
#define W1_FLAG_ALPHA 0x10
#define W1_FLAG_ICC 0x20

typedef struct w1_chunk {
  uint32_t fcc;
  const uint8_t *pay;
  size_t len;
} w1_chunk_t;

/* 1 = chunk read, 0 = end of input, -1 = truncated. */
static W1_UNUSED int w1_next_chunk(const uint8_t **pp, const uint8_t *end,
                                  w1_chunk_t *c) {
  const uint8_t *p = *pp;
  size_t len, adv;
  if (p >= end) return 0;
  if ((size_t)(end - p) < 8) return 0;   /* ignore trailing bytes */
  c->fcc = w1_le32(p);
  len = w1_le32(p + 4);
  c->pay = p + 8;
  c->len = len;
  if (len > (size_t)(end - c->pay)) return -1;
  if (len > (size_t)-1 - 9) return -1;  /* 8+len+(len&1) must not wrap (32-bit size_t) */
  adv = 8 + len + (len & 1);
  if (adv > (size_t)(end - p)) return -1;   /* missing pad byte */
  *pp = p + adv;
  return 1;
}

typedef struct w1_file {
  int is_vp8x;
  int canvas_w, canvas_h;
  int anim_flag, alpha_flag;
  int has_anim;
  uint32_t bg;
  int loop;
  int n_frames;
  /* still components */
  const uint8_t *img;
  size_t img_len;
  int img_is_vp8l;
  const uint8_t *alph;
  size_t alph_len;
  int has_exif, has_xmp, has_iccp;
} w1_file_t;

/* VP8/VP8L dimension peek (for info / frame scan). */
static W1_UNUSED int w1_vp8_peek_dims(const uint8_t *d, size_t n, int *w,
                                     int *h) {
  uint32_t raw, tag;
  if (n < 10) return WEBP1_ERR_TRUNCATED;
  if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a)
    return WEBP1_ERR_UNSUPPORTED;
  tag = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
  if (tag & 1) return WEBP1_ERR_UNSUPPORTED;             /* interframe */
  if (!((tag >> 4) & 1)) return WEBP1_ERR_UNSUPPORTED;   /* hidden frame */
  raw = (uint32_t)d[6] | ((uint32_t)d[7] << 8) | ((uint32_t)d[8] << 16) |
        ((uint32_t)d[9] << 24);
  *w = (int)(raw & 0x3fff);
  *h = (int)((raw >> 16) & 0x3fff);
  if (*w < 1 || *h < 1 || *w > WEBP1_MAX_DIM || *h > WEBP1_MAX_DIM)
    return WEBP1_ERR_CORRUPT;
  return WEBP1_OK;
}
static W1_UNUSED int w1_vp8l_peek_dims(const uint8_t *d, size_t n, int *w,
                                      int *h, int *alpha) {
  uint32_t b;
  if (n < 5) return WEBP1_ERR_TRUNCATED;
  if (d[0] != W1_VP8L_MAGIC) return WEBP1_ERR_CORRUPT;
  b = w1_le32(d + 1);
  *w = (int)((b & 0x3fff) + 1);
  *h = (int)(((b >> 14) & 0x3fff) + 1);
  *alpha = (int)((b >> 28) & 1);
  if ((b >> 29) != 0) return WEBP1_ERR_UNSUPPORTED;
  if (*w < 1 || *h < 1 || *w > WEBP1_MAX_DIM || *h > WEBP1_MAX_DIM)
    return WEBP1_ERR_CORRUPT;
  return WEBP1_OK;
}

/* Validate the RIFF header and return the declared end of the container.
 * Bytes past *end are outside the RIFF and must not be scanned. */
static W1_UNUSED int w1_riff_bounds(const uint8_t *data, size_t size,
                                    const uint8_t **end) {
  size_t riff;
  if (!data) return WEBP1_ERR_BAD_PARAM;
  if (size < 12) return WEBP1_ERR_TRUNCATED;
  if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WEBP", 4))
    return WEBP1_ERR_BAD_MAGIC;
  riff = w1_le32(data + 4);
  if (riff < 4) return WEBP1_ERR_CORRUPT;       /* must hold "WEBP" */
  if (riff > size - 8) return WEBP1_ERR_TRUNCATED;  /* declares more than supplied */
  *end = data + 8 + riff;
  return WEBP1_OK;
}

static W1_UNUSED int w1_parse_file(const uint8_t *data, size_t size,
                                  w1_file_t *f) {
  const uint8_t *p, *end;
  w1_chunk_t c;
  int r, first = 1;
  if (!data || !f) return WEBP1_ERR_BAD_PARAM;
  memset(f, 0, sizeof(*f));
  r = w1_riff_bounds(data, size, &end);
  if (r) return r;
  p = data + 12;
  for (;;) {
    r = w1_next_chunk(&p, end, &c);
    if (r < 0) return WEBP1_ERR_TRUNCATED;
    if (r == 0) break;
    if (first) {
      first = 0;
      if (c.fcc == W1_FCC_VP8X) {
        f->is_vp8x = 1;
        if (c.len < 10) return WEBP1_ERR_CORRUPT;
        f->anim_flag = (c.pay[0] & W1_FLAG_ANIM) != 0;
        f->alpha_flag = (c.pay[0] & W1_FLAG_ALPHA) != 0;
        f->canvas_w = (int)w1_le24(c.pay + 4) + 1;
        f->canvas_h = (int)w1_le24(c.pay + 7) + 1;
        if (f->canvas_w < 1 || f->canvas_h < 1 ||
            f->canvas_w > WEBP1_MAX_DIM || f->canvas_h > WEBP1_MAX_DIM)
          return WEBP1_ERR_CORRUPT;
      } else if (c.fcc == W1_FCC_VP8) {
        int w, h, rc = w1_vp8_peek_dims(c.pay, c.len, &w, &h);
        if (rc == WEBP1_ERR_UNSUPPORTED) {
          /* Defer bad-magic to decode (report UNSUPPORTED there too). */
          f->canvas_w = 0;
        } else if (rc) {
          return rc;
        } else {
          f->canvas_w = w; f->canvas_h = h;
        }
        f->img = c.pay; f->img_len = c.len; f->img_is_vp8l = 0;
        f->n_frames = 1;
      } else if (c.fcc == W1_FCC_VP8L) {
        int w, h, a, rc = w1_vp8l_peek_dims(c.pay, c.len, &w, &h, &a);
        if (rc) return rc;
        f->canvas_w = w; f->canvas_h = h;
        f->img = c.pay; f->img_len = c.len; f->img_is_vp8l = 1;
        f->n_frames = 1;
      } else {
        return WEBP1_ERR_CORRUPT;
      }
      continue;
    }
    if (!f->is_vp8x) continue;   /* ignore trailing chunks in simple files */
    switch (c.fcc) {
    case W1_FCC_ANIM:
      if (c.len < 6) return WEBP1_ERR_CORRUPT;
      if (f->n_frames > 0) return WEBP1_ERR_CORRUPT;   /* ANIM precedes ANMF */
      f->has_anim = 1;
      f->bg = w1_le32(c.pay);
      f->loop = w1_le16(c.pay + 4);
      break;
    case W1_FCC_ANMF:
      if (!f->has_anim) return WEBP1_ERR_CORRUPT;      /* ANIM required first */
      f->n_frames++;
      break;
    case W1_FCC_VP8:
      if (!f->anim_flag && !f->img) {
        f->img = c.pay; f->img_len = c.len; f->img_is_vp8l = 0;
      }
      break;
    case W1_FCC_VP8L:
      if (!f->anim_flag && !f->img) {
        f->img = c.pay; f->img_len = c.len; f->img_is_vp8l = 1;
      }
      break;
    case W1_FCC_ALPH:
      if (!f->anim_flag && f->img) return WEBP1_ERR_CORRUPT; /* ALPH before VP8 */
      if (!f->anim_flag && !f->alph) { f->alph = c.pay; f->alph_len = c.len; }
      break;
    case W1_FCC_EXIF: f->has_exif = 1; break;
    case W1_FCC_XMP: f->has_xmp = 1; break;
    case W1_FCC_ICCP: f->has_iccp = 1; break;
    default: break;   /* unknown chunks ignored */
    }
  }
  if (!f->is_vp8x) {
    if (!f->img) return WEBP1_ERR_CORRUPT;   /* no image chunk inside RIFF */
    return WEBP1_OK;
  }
  if (f->anim_flag) {
    if (f->n_frames < 1 || !f->has_anim) return WEBP1_ERR_CORRUPT;
  } else {
    if (!f->img) return WEBP1_ERR_CORRUPT;
    f->n_frames = 1;
  }
  return WEBP1_OK;
}

/* ANMF frame header. */
typedef struct w1_anmf {
  int x, y, w, h;
  int dur;
  int blend;    /* 1 == alpha-blend, 0 == overwrite */
  int dispose;
  const uint8_t *sub;
  size_t sub_len;
} w1_anmf_t;

static W1_UNUSED int w1_parse_anmf(const uint8_t *p, size_t len,
                                  const w1_file_t *f, w1_anmf_t *a) {
  long x, y;
  if (len < 16) return WEBP1_ERR_CORRUPT;
  x = (long)w1_le24(p) * 2;
  y = (long)w1_le24(p + 3) * 2;
  a->w = (int)w1_le24(p + 6) + 1;
  a->h = (int)w1_le24(p + 9) + 1;
  a->dur = (int)w1_le24(p + 12);
  a->blend = (p[15] & 2) ? 0 : 1;
  a->dispose = p[15] & 1;
  a->sub = p + 16;
  a->sub_len = len - 16;
  if (a->w < 1 || a->h < 1 || a->w > WEBP1_MAX_DIM || a->h > WEBP1_MAX_DIM)
    return WEBP1_ERR_CORRUPT;
  if (x < 0 || y < 0 || x + a->w > f->canvas_w || y + a->h > f->canvas_h)
    return WEBP1_ERR_CORRUPT;
  a->x = (int)x; a->y = (int)y;
  return WEBP1_OK;
}

/* Find image components inside ANMF sub-chunks (or still frame). */
typedef struct w1_frame_img {
  const uint8_t *img;
  size_t img_len;
  int is_vp8l;
  const uint8_t *alph;
  size_t alph_len;
} w1_frame_img_t;

static W1_UNUSED int w1_find_frame_img(const uint8_t *p, size_t len,
                                      w1_frame_img_t *o) {
  const uint8_t *end = p + len;
  w1_chunk_t c;
  int r, seen_img = 0;
  memset(o, 0, sizeof(*o));
  for (;;) {
    r = w1_next_chunk(&p, end, &c);
    if (r < 0) return WEBP1_ERR_TRUNCATED;
    if (r == 0) break;
    if (c.fcc == W1_FCC_VP8 && !o->img) {
      o->img = c.pay; o->img_len = c.len; o->is_vp8l = 0; seen_img = 1;
    } else if (c.fcc == W1_FCC_VP8L && !o->img) {
      o->img = c.pay; o->img_len = c.len; o->is_vp8l = 1; seen_img = 1;
    } else if (c.fcc == W1_FCC_ALPH) {
      if (seen_img || o->alph) return WEBP1_ERR_CORRUPT;  /* ALPH before image */
      o->alph = c.pay; o->alph_len = c.len;
    }
  }
  if (!o->img) return WEBP1_ERR_CORRUPT;
  return WEBP1_OK;
}

/* Decode one image (VP8/VP8L + optional ALPH) to RGBA. */
static W1_UNUSED int w1_decode_image(const uint8_t *img, size_t img_len,
                                    int is_vp8l, const uint8_t *alph,
                                    size_t alph_len, int use_alph, int w,
                                    int h, w1_bump_t *bump, uint8_t *rgba) {
  if (is_vp8l) {
    uint32_t *pix;
    size_t npix = (size_t)w * (size_t)h, i;
    int rc, bw, bh, ba;
    rc = w1_vp8l_peek_dims(img, img_len, &bw, &bh, &ba);
    if (rc) return rc;
    if (bw != w || bh != h) return WEBP1_ERR_CORRUPT;
    (void)ba;
    pix = (uint32_t *)w1_bump_alloc(bump, npix * 4, 4);
    if (!pix) return WEBP1_ERR_NO_MEMORY;
    rc = w1_vp8l_decode(img, img_len, pix, bump);
    if (rc) return rc;
#ifdef W1_LITTLE_ENDIAN
    /* ARGB -> RGBA is a red/blue swap: one word store per pixel. */
    for (i = 0; i < npix; i++) {
      uint32_t v = pix[i];
      v = (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
      memcpy(rgba + 4 * i, &v, 4);
    }
#else
    for (i = 0; i < npix; i++) {
      uint32_t v = pix[i];
      rgba[4 * i] = (uint8_t)(v >> 16);
      rgba[4 * i + 1] = (uint8_t)(v >> 8);
      rgba[4 * i + 2] = (uint8_t)v;
      rgba[4 * i + 3] = (uint8_t)(v >> 24);
    }
#endif
    return WEBP1_OK;
  } else {
    w1_vp8_frame_t fr;
    int rc, bw, bh;
    rc = w1_vp8_peek_dims(img, img_len, &bw, &bh);
    if (rc) return rc;
    if (bw != w || bh != h) return WEBP1_ERR_CORRUPT;
    rc = w1_vp8_decode(img, img_len, bump, &fr);
    if (rc) return rc;
    w1_vp8_yuv_to_rgba(&fr, rgba);
    if (use_alph && alph) {
      uint8_t *alpha = (uint8_t *)w1_bump_alloc(bump,
                                               (size_t)w * (size_t)h, 1);
      size_t i, npix;
      if (!alpha) return WEBP1_ERR_NO_MEMORY;
      rc = w1_alph_decode(alph, alph_len, w, h, bump, alpha);
      if (rc) return rc;
      npix = (size_t)w * (size_t)h;
      for (i = 0; i < npix; i++) rgba[4 * i + 3] = alpha[i];
    }
    return WEBP1_OK;
  }
}

/* ---- Animation compositor (bit-exact reconstruction) ---- */

/* dst = src OVER dst (Porter-Duff source-over). Callers pass the canvas
 * as dst and the decoded frame row as src. */
static W1_UNUSED void w1_blend_row(uint8_t *dst, const uint8_t *src, int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint32_t sa = src[4 * i + 3];
    uint32_t c;
    if (sa == 255) {
      dst[4 * i] = src[4 * i]; dst[4 * i + 1] = src[4 * i + 1];
      dst[4 * i + 2] = src[4 * i + 2]; dst[4 * i + 3] = src[4 * i + 3];
      continue;
    }
    if (sa == 0) continue;
    {
      uint32_t da = dst[4 * i + 3];
      uint32_t df = (da * (256 - sa)) >> 8;
      uint32_t ba = sa + df;
      uint32_t scale = (1u << 24) / ba;
      for (c = 0; c < 3; c++) {
        uint32_t u = src[4 * i + c] * sa + dst[4 * i + c] * df;
        dst[4 * i + c] = (uint8_t)((u * scale) >> 24);
      }
      dst[4 * i + 3] = (uint8_t)ba;
    }
  }
}

static W1_UNUSED void w1_zero_rect(uint8_t *canvas, int cw, int x, int y,
                                  int w, int h) {
  int r;
  for (r = 0; r < h; r++)
    memset(canvas + ((size_t)(y + r) * (size_t)cw + (size_t)x) * 4, 0,
           (size_t)w * 4);
}

/* Frame alpha presence for key-frame logic (VP8: ALPH chunk; VP8L: hdr). */
static W1_UNUSED int w1_frame_has_alpha(const w1_frame_img_t *o) {
  int w, h, a;
  if (!o->is_vp8l) return o->alph != NULL;
  if (w1_vp8l_peek_dims(o->img, o->img_len, &w, &h, &a)) return 0;
  return a;
}

/* Compose frames 0..index onto canvas (canvas pre-zeroed by caller for
 * frame 0; this handles incremental dispose). */
static W1_UNUSED int w1_anim_compose(const uint8_t *data, size_t size,
                                    const w1_file_t *f, int index,
                                    uint8_t *canvas, w1_bump_t *bump) {
  const uint8_t *p = data + 12, *end = data + size;
  int cw = f->canvas_w, ch = f->canvas_h, k = -1;
  int prev_key = 0, px = 0, py = 0, pw = 0, ph = 0, prev_dispose = 0;
  int prev_full = 0;
  w1_chunk_t c;
  int r;
  uint8_t *tmp;
  size_t frame_mark = 0;
  size_t tmp_sz = (size_t)cw * (size_t)ch * 4;
  (void)ch;
  r = w1_riff_bounds(data, size, &end);
  if (r) return r;
  tmp = (uint8_t *)w1_bump_alloc(bump, tmp_sz, 1);
  if (!tmp) return WEBP1_ERR_NO_MEMORY;
  memset(canvas, 0, tmp_sz);
  for (;;) {
    w1_anmf_t a;
    w1_frame_img_t o;
    int rc, key, full, has_alpha;
    r = w1_next_chunk(&p, end, &c);
    if (r < 0) return WEBP1_ERR_TRUNCATED;
    if (r == 0) break;
    if (c.fcc != W1_FCC_ANMF) continue;
    if (++k > index) break;
    rc = w1_parse_anmf(c.pay, c.len, f, &a);
    if (rc) return rc;
    rc = w1_find_frame_img(a.sub, a.sub_len, &o);
    if (rc) return rc;
    full = (a.x == 0 && a.y == 0 && a.w == cw && a.h == ch);
    has_alpha = w1_frame_has_alpha(&o);
    if (k == 0) {
      key = 1;
    } else if ((!has_alpha || !a.blend) && full) {
      key = 1;
    } else {
      key = prev_dispose && (prev_full || prev_key);
    }
    if (k > 0 && prev_dispose) w1_zero_rect(canvas, cw, px, py, pw, ph);
    {
      /* This frame's decoder scratch is dead once it is composited, so roll
       * the arena back after each frame: a caller sizing work with
       * webp1_decode_work_bound() (one codec's worth) can then reach deep
       * frame indices without NO_MEMORY. */
      size_t mark = bump->used;
      rc = w1_decode_image(o.img, o.img_len, o.is_vp8l, o.alph, o.alph_len, 1,
                           a.w, a.h, bump, tmp);
      if (rc) return rc;
      frame_mark = mark;
    }
    if (k == 0 || !a.blend || key) {
      int rr;
      for (rr = 0; rr < a.h; rr++) {
        memcpy(canvas + ((size_t)(a.y + rr) * (size_t)cw + (size_t)a.x) * 4,
               tmp + (size_t)rr * (size_t)a.w * 4, (size_t)a.w * 4);
      }
    } else if (!prev_dispose) {
      int rr;
      for (rr = 0; rr < a.h; rr++) {
        w1_blend_row(canvas + ((size_t)(a.y + rr) * (size_t)cw + (size_t)a.x)
                     * 4, tmp + (size_t)rr * (size_t)a.w * 4, a.w);
      }
    } else {
      /* Blend outside prev rect; raw copy inside it. */
      int rr;
      for (rr = 0; rr < a.h; rr++) {
        int cy = a.y + rr, cx;
        uint8_t *cd = canvas + ((size_t)cy * (size_t)cw + (size_t)a.x) * 4;
        uint8_t *sp = tmp + (size_t)rr * (size_t)a.w * 4;
        for (cx = 0; cx < a.w; cx++) {
          int ix = (cy >= py && cy < py + ph && a.x + cx >= px &&
                    a.x + cx < px + pw);
          if (ix) {
            cd[4 * cx] = sp[4 * cx]; cd[4 * cx + 1] = sp[4 * cx + 1];
            cd[4 * cx + 2] = sp[4 * cx + 2]; cd[4 * cx + 3] = sp[4 * cx + 3];
          } else {
            w1_blend_row(cd + 4 * cx, sp + 4 * cx, 1);
          }
        }
      }
    }
    bump->used = frame_mark;      /* scratch reusable from here on */
    prev_key = key; px = a.x; py = a.y; pw = a.w; ph = a.h;
    prev_dispose = a.dispose; prev_full = full;
  }
  if (k < index) return WEBP1_ERR_CORRUPT;   /* fewer frames than parsed */
  return WEBP1_OK;
}

/* ---- Public decode API ---- */

static W1_UNUSED const char *webp1_error_name(int code) {
  switch (code) {
  case WEBP1_OK: return "OK";
  case WEBP1_ERR_BAD_PARAM: return "bad parameter";
  case WEBP1_ERR_TOO_LARGE: return "too large";
  case WEBP1_ERR_TRUNCATED: return "truncated input";
  case WEBP1_ERR_BAD_MAGIC: return "bad magic";
  case WEBP1_ERR_UNSUPPORTED: return "unsupported";
  case WEBP1_ERR_CORRUPT: return "corrupt bitstream";
  case WEBP1_ERR_NO_MEMORY: return "work buffer too small";
  case WEBP1_ERR_OUTPUT_FULL: return "output buffer too small";
  default: return "unknown error";
  }
}

static W1_UNUSED void webp1_encode_opts_init(webp1_encode_opts_t *o) {
  if (!o) return;
  o->quality = 75;
  o->lossless_level = 6;
  o->iccp = NULL; o->iccp_len = 0;
  o->exif = NULL; o->exif_len = 0;
  o->xmp = NULL; o->xmp_len = 0;
}

static W1_UNUSED size_t webp1_rgba_size(int w, int h) {
  if (w < 1 || h < 1) return 0;
  if ((size_t)w > (size_t)-1 / 4 / (size_t)h) return 0;
  return (size_t)w * (size_t)h * 4;
}

static W1_UNUSED int webp1_info(const uint8_t *data, size_t size,
                               webp1_info_t *info) {
  w1_file_t f;
  int rc = w1_parse_file(data, size, &f);
  if (rc) return rc;
  if (!info) return WEBP1_ERR_BAD_PARAM;
  memset(info, 0, sizeof(*info));
  info->width = f.canvas_w;
  info->height = f.canvas_h;
  info->is_animated = f.anim_flag && f.is_vp8x;
  info->frame_count = info->is_animated ? f.n_frames : 1;
  info->loop_count = f.has_anim ? f.loop : 0;
  info->bg_color = f.has_anim ? f.bg : 0;
  info->has_exif = f.has_exif;
  info->has_xmp = f.has_xmp;
  info->has_iccp = f.has_iccp;
  if (info->is_animated) {
    /* OR alpha over frames. */
    const uint8_t *p = data + 12, *end = data + size;
    w1_chunk_t c;
    int r, any_alpha = 0;
    info->is_lossless = -1;
    r = w1_riff_bounds(data, size, &end);
    if (r) return r;
    for (;;) {
      r = w1_next_chunk(&p, end, &c);
      if (r <= 0) break;
      if (c.fcc != W1_FCC_ANMF) continue;
      {
        w1_anmf_t a;
        w1_frame_img_t o;
        if (w1_parse_anmf(c.pay, c.len, &f, &a)) continue;
        if (w1_find_frame_img(a.sub, a.sub_len, &o)) continue;
        if (w1_frame_has_alpha(&o)) { any_alpha = 1; break; }
      }
    }
    info->has_alpha = any_alpha;
  } else if (f.img_is_vp8l) {
    int w, h, a;
    info->is_lossless = 1;
    rc = w1_vp8l_peek_dims(f.img, f.img_len, &w, &h, &a);
    if (rc) return rc;
    info->has_alpha = a;
    if (f.is_vp8x && (w != f.canvas_w || h != f.canvas_h))
      return WEBP1_ERR_CORRUPT;
  } else {
    info->is_lossless = 0;
    if (f.is_vp8x && f.canvas_w > 0) {
      int w, h;
      rc = w1_vp8_peek_dims(f.img, f.img_len, &w, &h);
      if (rc) return rc;
      if (w != f.canvas_w || h != f.canvas_h) return WEBP1_ERR_CORRUPT;
      info->has_alpha = f.alpha_flag && f.alph != NULL;
    } else if (!f.is_vp8x) {
      int w, h;
      rc = w1_vp8_peek_dims(f.img, f.img_len, &w, &h);
      if (rc) return rc;   /* bad magic -> UNSUPPORTED */
      info->has_alpha = 0;
    } else {
      return WEBP1_ERR_CORRUPT;
    }
  }
  return WEBP1_OK;
}

/* Saturating accumulator for bound math (stays exact on 64-bit for all
 * legal dims; saturates instead of wrapping on 32-bit giants). */
static W1_UNUSED size_t w1_bound_add(size_t acc, size_t a, size_t b) {
  size_t p;
  if (a && b > (size_t)-1 / a) return (size_t)-1;
  p = a * b;
  if (acc > (size_t)-1 - p) return (size_t)-1;
  return acc + p;
}

static W1_UNUSED size_t webp1_decode_work_bound(const webp1_info_t *info) {
  size_t mw, mh, mbs, px, groups, bound = 0;
  int need_vp8, need_vp8l;
  if (!info || info->width < 1 || info->height < 1) return 0;
  if (info->width > WEBP1_MAX_DIM || info->height > WEBP1_MAX_DIM) return 0;
  if (w1_mul_overflows_size((size_t)info->width, (size_t)info->height))
    return 0;
  mw = ((size_t)info->width + 15) / 16;
  mh = ((size_t)info->height + 15) / 16;
  mbs = mw * mh;
  px = (size_t)info->width * (size_t)info->height;
  /* Stills use one codec; animations may mix both. Alpha (ALPH) is VP8L. */
  need_vp8 = info->is_animated || info->is_lossless == 0;
  need_vp8l = info->is_animated || info->is_lossless != 0 || info->has_alpha;
  if (need_vp8) {
    bound = w1_bound_add(bound, mbs, 384 + 8);
    bound = w1_bound_add(bound, mw + 1, 64);
    bound = w1_bound_add(bound, 8192, 1);
  }
  if (need_vp8l) {
    /* ~1 huffman group per 4K pixels covers real encoders (cap 256);
     * adversarial files fall back to need-reporting. 256*32KB=8MB worst,
     * negligible vs pixel buffers on large images, avoids retries. */
    groups = px / 4096 + 1;
    if (groups > 256) groups = 256;
    bound = w1_bound_add(bound, groups, 32768);
    bound = w1_bound_add(bound, 65536, 1);   /* caches, structs, remap */
    bound = w1_bound_add(bound, px, 1);      /* transform sub-images */
    if (!info->is_animated)
      bound = w1_bound_add(bound, px, 4);    /* ARGB pixels */
  }
  if (info->is_animated || info->has_alpha)
    bound = w1_bound_add(bound, px, 4);      /* canvas / frame temp */
  if (info->is_animated)
    bound = w1_bound_add(bound, px, 4);      /* second temp */
  if (bound == (size_t)-1) return 0;
  return bound;
}

static W1_UNUSED int w1_check_out_rgba(int w, int h, size_t cap) {
  size_t need = webp1_rgba_size(w, h);
  if (need == 0) return WEBP1_ERR_TOO_LARGE;
  if (cap < need) return WEBP1_ERR_OUTPUT_FULL;
  return WEBP1_OK;
}

static W1_UNUSED int webp1_decode_rgba(const uint8_t *data, size_t size,
                                      uint8_t *out, size_t out_cap,
                                      uint8_t *work, size_t work_cap,
                                      size_t *need, int *out_w, int *out_h) {
  w1_file_t f;
  w1_bump_t bump;
  int rc = w1_parse_file(data, size, &f);
  if (rc) return rc;
  if (!out) return WEBP1_ERR_BAD_PARAM;
  if (f.is_vp8x && (f.canvas_w < 1 || f.canvas_h < 1))
    return WEBP1_ERR_CORRUPT;
  if (!f.is_vp8x && f.canvas_w < 1) {
    /* Simple file with undecodable dims (bad VP8 magic): surface the
     * underlying error. */
    int w, h;
    if (f.img_is_vp8l) return WEBP1_ERR_CORRUPT;  /* peeked OK in parse */
    return w1_vp8_peek_dims(f.img, f.img_len, &w, &h);
  }
  rc = w1_check_out_rgba(f.canvas_w, f.canvas_h, out_cap);
  if (rc) return rc;
  w1_bump_init(&bump, work, work_cap);
  if (f.is_vp8x && f.anim_flag) {
    rc = w1_anim_compose(data, size, &f, 0, out, &bump);
  } else {
    int use_alph = !f.is_vp8x ? 0 : (f.alpha_flag && f.alph != NULL);
    if (!f.is_vp8x) use_alph = 0;
    rc = w1_decode_image(f.img, f.img_len, f.img_is_vp8l, f.alph, f.alph_len,
                         use_alph, f.canvas_w, f.canvas_h, &bump, out);
  }
  if (rc == WEBP1_ERR_NO_MEMORY && need) *need = bump.need;
  if (rc == WEBP1_OK) {
    if (out_w) *out_w = f.canvas_w;
    if (out_h) *out_h = f.canvas_h;
  }
  return rc;
}

static W1_UNUSED int webp1_frame_info(const uint8_t *data, size_t size,
                                     int index, webp1_frame_t *frame) {
  w1_file_t f;
  int rc = w1_parse_file(data, size, &f);
  if (rc) return rc;
  if (!frame) return WEBP1_ERR_BAD_PARAM;
  memset(frame, 0, sizeof(*frame));
  if (!(f.is_vp8x && f.anim_flag)) {
    int w, h, a;
    if (index != 0) return WEBP1_ERR_BAD_PARAM;
    if (f.canvas_w < 1) return WEBP1_ERR_CORRUPT;
    if (f.img_is_vp8l) {
      rc = w1_vp8l_peek_dims(f.img, f.img_len, &w, &h, &a);
      if (rc) return rc;
      frame->has_alpha = a;
      frame->is_lossless = 1;
    } else {
      rc = w1_vp8_peek_dims(f.img, f.img_len, &w, &h);
      if (rc) return rc;
      frame->has_alpha = f.is_vp8x && f.alpha_flag && f.alph != NULL;
      frame->is_lossless = 0;
    }
    frame->x = 0; frame->y = 0;
    frame->w = f.canvas_w; frame->h = f.canvas_h;
    return WEBP1_OK;
  } else {
    const uint8_t *p = data + 12, *end = data + size;
    w1_chunk_t c;
    int r, k = -1;
    if (index < 0 || index >= f.n_frames) return WEBP1_ERR_BAD_PARAM;
    r = w1_riff_bounds(data, size, &end);
    if (r) return r;
    for (;;) {
      r = w1_next_chunk(&p, end, &c);
      if (r < 0) return WEBP1_ERR_TRUNCATED;
      if (r == 0) break;
      if (c.fcc != W1_FCC_ANMF) continue;
      if (++k == index) {
        w1_anmf_t a;
        w1_frame_img_t o;
        rc = w1_parse_anmf(c.pay, c.len, &f, &a);
        if (rc) return rc;
        rc = w1_find_frame_img(a.sub, a.sub_len, &o);
        if (rc) return rc;
        frame->x = a.x; frame->y = a.y;
        frame->w = a.w; frame->h = a.h;
        frame->duration_ms = a.dur;
        frame->blend = a.blend;
        frame->dispose = a.dispose;
        frame->has_alpha = w1_frame_has_alpha(&o);
        frame->is_lossless = o.is_vp8l;
        return WEBP1_OK;
      }
    }
    return WEBP1_ERR_CORRUPT;
  }
}

static W1_UNUSED int webp1_anim_decode(const uint8_t *data, size_t size,
                                      int index, uint8_t *canvas,
                                      size_t canvas_cap, uint8_t *work,
                                      size_t work_cap, size_t *need) {
  w1_file_t f;
  w1_bump_t bump;
  int rc = w1_parse_file(data, size, &f);
  if (rc) return rc;
  if (!canvas) return WEBP1_ERR_BAD_PARAM;
  if (!(f.is_vp8x && f.anim_flag)) {
    if (index != 0) return WEBP1_ERR_BAD_PARAM;
    return webp1_decode_rgba(data, size, canvas, canvas_cap, work,
                             work_cap, need, NULL, NULL);
  }
  if (index < 0 || index >= f.n_frames) return WEBP1_ERR_BAD_PARAM;
  if (f.canvas_w < 1 || f.canvas_h < 1) return WEBP1_ERR_CORRUPT;
  rc = w1_check_out_rgba(f.canvas_w, f.canvas_h, canvas_cap);
  if (rc) return rc;
  w1_bump_init(&bump, work, work_cap);
  rc = w1_anim_compose(data, size, &f, index, canvas, &bump);
  if (rc == WEBP1_ERR_NO_MEMORY && need) *need = bump.need;
  return rc;
}

static W1_UNUSED int webp1_find_chunk(const uint8_t *data, size_t size,
                                     uint32_t fourcc, int chunk_index,
                                     const uint8_t **payload,
                                     size_t *payload_len) {
  const uint8_t *p, *end;
  w1_chunk_t c;
  int r, seen = 0;
  if (!data || !payload || !payload_len || chunk_index < 0)
    return WEBP1_ERR_BAD_PARAM;
  *payload = NULL;
  *payload_len = 0;
  r = w1_riff_bounds(data, size, &end);
  if (r) return r;
  p = data + 12;
  for (;;) {
    r = w1_next_chunk(&p, end, &c);
    if (r < 0) return WEBP1_ERR_TRUNCATED;
    if (r == 0) break;
    if (c.fcc == fourcc && seen++ == chunk_index) {
      *payload = c.pay;
      *payload_len = c.len;
      return WEBP1_OK;
    }
  }
  return WEBP1_OK;
}
/* == S7: VP8L encoder (lossless search + emission) ==
 * Single entropy group, LZ77 + Huffman, optional subtract-green,
 * predictor transform and color cache. No color/index transforms. == */

/* Write an MSB-first Huffman code (len <= 15). */
/* Emit a Huffman code. `code` is already bit-reversed (w1_huff_codes stores
 * it reversed) so the LSB-first writer produces the MSB-first wire order
 * with no per-symbol reversal. */
static W1_UNUSED void w1_bw_code(w1_bw_t *w, int code, int len) {
  if (len <= 0) return;
  w1_bw_put(w, (uint32_t)code, len);
}

/* Inverse of w1_vp8l_prefix_val: value v>=1 -> (code, extra, ebits).
 * Closed form: hb = floor(log2(v-1)) gives the code pair directly, so no
 * scan over the 60 prefix codes per call (this is in the LZ candidate loop). */
static W1_UNUSED void w1_prefix_inv(int v, int *code, int *extra, int *ebits) {
  int hb = 0;
  unsigned t;
  if (v <= 4) { *code = v - 1; *extra = 0; *ebits = 0; return; }
  /* Branch-down bit length of v-1 (same floor(log2) the shift loop
   * computed, without up to 20 dependent iterations). */
  t = (unsigned)(v - 1);
  if (t >= 0x10000u) { hb += 16; t >>= 16; }
  if (t >= 0x100u)   { hb += 8;  t >>= 8; }
  if (t >= 0x10u)    { hb += 4;  t >>= 4; }
  if (t >= 0x4u)     { hb += 2;  t >>= 2; }
  if (t >= 0x2u)     { hb += 1; }
  *ebits = hb - 1;
  *code = 2 * hb + (int)(((unsigned)(v - 1) >> (hb - 1)) & 1u);
  *extra = (v - 1) & ((1 << *ebits) - 1);
}

/* Distance -> plane value (plane code 1..120 or dist+120). */
static W1_UNUSED int w1_dist_plane(int dist, int w) {
  int yoff = dist / w, xoff = dist - yoff * w;
  if (yoff < 8 && xoff >= 0 && xoff <= 8) {
    int p = (int)w1k_vp8l_plane_rev[yoff][xoff + 8];
    if (p >= 1 && p <= 120 &&
        (int)w1k_vp8l_plane_dx[p - 1] +
        (int)w1k_vp8l_plane_dy[p - 1] * w == dist)
      return p;
  }
  return dist + 120;
}

/* counts[0..n-1] -> lens[] (0 = unused). tmp needs 5*n ints.
 * Returns #nonzero. Lengths limited to max_len (15, or 7 for CL). */
static W1_UNUSED int w1_huff_node_less(const int *freq, int a, int b) {
  return freq[a] < freq[b] || (freq[a] == freq[b] && a < b);
}

static W1_UNUSED void w1_huff_sort_nodes(int *order, int m,
                                         const int *freq) {
  int start, end;
  for (start = m / 2 - 1; start >= 0; start--) {
    int root = start;
    for (;;) {
      int child = root * 2 + 1, swap = root, t;
      if (child >= m) break;
      if (w1_huff_node_less(freq, order[swap], order[child])) swap = child;
      if (child + 1 < m &&
          w1_huff_node_less(freq, order[swap], order[child + 1]))
        swap = child + 1;
      if (swap == root) break;
      t = order[root]; order[root] = order[swap]; order[swap] = t;
      root = swap;
    }
  }
  for (end = m - 1; end > 0; end--) {
    int root = 0, t = order[0];
    order[0] = order[end]; order[end] = t;
    for (;;) {
      int child = root * 2 + 1, swap = root;
      if (child >= end) break;
      if (w1_huff_node_less(freq, order[swap], order[child])) swap = child;
      if (child + 1 < end &&
          w1_huff_node_less(freq, order[swap], order[child + 1]))
        swap = child + 1;
      if (swap == root) break;
      t = order[root]; order[root] = order[swap]; order[swap] = t;
      root = swap;
    }
  }
}

static W1_UNUSED int w1_huff_pop_node(const int *freq, const int *order,
                                      int m, int *leaf, int *inner,
                                      int next) {
  if (*leaf < m && (*inner >= next ||
      w1_huff_node_less(freq, order[*leaf], *inner)))
    return order[(*leaf)++];
  return (*inner)++;
}

static W1_UNUSED int w1_huff_lengths(const int *counts, int n, int max_len,
                                     uint8_t *lens, int *tmp) {
  int *freq = tmp, *parent = tmp + 2 * n, *order = tmp + 4 * n;
  int i, m = 0, k, max_count = 1;
  int64_t floor_min = 1;
  for (i = 0; i < n; i++) lens[i] = 0;
  for (i = 0; i < n; i++) if (counts[i] > 0) {
    order[m++] = i;
    if (counts[i] > max_count) max_count = counts[i];
  }
  if (m == 0) return 0;
  if (m == 1) { lens[order[0]] = 1; return 1; }
  /* Raising every small count to a common floor shortens the tree until it
   * fits max_len (count-floor optimal length-limited tree). Unlike clamping plus an
   * integer Kraft repair, every trial is a real Huffman tree, so the codes
   * always form a complete tree the decoder accepts. */
  for (;;) {
    int next = n, inner = n, leaf = 0, a, b, max_depth = 0;
    for (i = 0; i < n; i++)
      if (counts[i] > 0)
        freq[i] = counts[i] < floor_min ? (int)floor_min : counts[i];
    w1_huff_sort_nodes(order, m, freq);
    for (k = 0; k < m - 1; k++) {
      a = w1_huff_pop_node(freq, order, m, &leaf, &inner, next);
      b = w1_huff_pop_node(freq, order, m, &leaf, &inner, next);
      freq[next] = freq[a] + freq[b]; parent[a] = next; parent[next] = -1;
      parent[b] = next; next++;
    }
    for (i = 0; i < m; i++) {
      int depth = 0, p = order[i], root = next - 1;
      while (p != root) { p = parent[p]; depth++; }
      if (depth < 1) depth = 1;
      lens[order[i]] = (uint8_t)depth;
      if (depth > max_depth) max_depth = depth;
    }
    if (max_depth <= max_len) break;
    if (floor_min >= max_count) {
      /* All counts level: use the complete almost-uniform tree (m is at
       * most the alphabet size, so it always fits max_len here). */
      int bits = 0, r;
      while ((m - 1) >> bits) bits++;
      if (bits > max_len) bits = max_len;
      r = (1 << bits) - m;
      for (i = 0; i < m; i++)
        lens[order[i]] = (uint8_t)(i < r ? bits - 1 : bits);
      break;
    }
    floor_min *= 2;
    if (floor_min > max_count) floor_min = max_count;
  }
  return m;
}

/* Canonical MSB-first codes from lens (decoder-compatible assignment).
 * codes[s] valid where lens[s] != 0. */
static W1_UNUSED void w1_huff_codes(const uint8_t *lens, int n, int *codes) {
  int count[16], first[16], len, code = 0, i;
  for (len = 0; len < 16; len++) count[len] = 0;
  for (i = 0; i < n; i++) if (lens[i]) count[lens[i]]++;
  for (len = 1; len <= 15; len++) {
    code = (code + count[len - 1]) << 1;
    first[len] = code;
  }
  for (i = 0; i < n; i++)
    if (lens[i]) {
      /* Store bit-reversed: the canonical code c is MSB-first; reversing it
       * into the low bits lets w1_bw_code emit without a per-symbol
       * reversal (the writer is LSB-first). */
      int l = lens[i], c = first[l]++, r = 0, b;
      for (b = 0; b < l; b++) r = (r << 1) | ((c >> b) & 1);
      codes[i] = r;
    }
}

static W1_UNUSED int w1_huff_rle_greedy(const uint8_t *lens, int last,
                                         int *ssym, int *sext) {
  int i = 0, cnt = 0;
  while (i <= last) {
    int c = lens[i], r0 = 1, r;
    while (i + r0 <= last && lens[i + r0] == c) r0++;
    r = r0;
    if (c == 0) {
      while (r > 0) {
        if (r < 3) {
          while (r-- > 0) { ssym[cnt] = 0; sext[cnt++] = 0; }
        } else if (r <= 10) {
          ssym[cnt] = 17; sext[cnt++] = r - 3; r = 0;
        } else {
          int take = r > 138 ? 138 : r;
          ssym[cnt] = 18; sext[cnt++] = take - 11; r -= take;
        }
      }
    } else {
      ssym[cnt] = c; sext[cnt++] = 0; r--;
      while (r >= 3) {
        int take = r > 6 ? 6 : r;
        ssym[cnt] = 16; sext[cnt++] = take - 3; r -= take;
      }
      while (r-- > 0) { ssym[cnt] = c; sext[cnt++] = 0; }
    }
    i += r0;
  }
  return cnt;
}

static W1_UNUSED int w1_huff_rle_optimized(const uint8_t *lens, int last,
                                            const uint8_t *cost,
                                            int *ssym, int *sext) {
  int i = 0, cnt = 0;
  while (i <= last) {
    int c = lens[i], run = 1, r, a, best_a = 0, best_b = 0, best_c;
    uint64_t best;
    while (i + run <= last && lens[i + run] == c) run++;
    r = run;
    best_c = run;
    if (c == 0) {
      best = (uint64_t)r * (cost[0] ? cost[0] : 64);
      for (a = 0; a <= r / 11; a++) {
        int literals;
        for (literals = 0; literals < 3 && literals <= r; literals++) {
          int rem = r - literals, bmin, bmax, b;
          if (rem < 11 * a || rem > 138 * a + 10 * ((rem + 2) / 3)) continue;
          bmin = rem > 138 * a ? (rem - 138 * a + 9) / 10 : 0;
          bmax = (rem - 11 * a) / 3;
          if (bmin > bmax) continue;
          b = bmin;
          {
            uint64_t v = (uint64_t)a * ((cost[18] ? cost[18] : 64) + 7) +
                         (uint64_t)b * ((cost[17] ? cost[17] : 64) + 3) +
                         (uint64_t)literals * (cost[0] ? cost[0] : 64);
            if (v < best) { best = v; best_a = a; best_b = b; best_c = literals; }
          }
        }
      }
      {
        int extra = r - 11 * best_a - 3 * best_b - best_c, j;
        for (j = 0; j < best_a; j++) {
          int add = extra > 127 ? 127 : extra;
          ssym[cnt] = 18; sext[cnt++] = add; extra -= add;
        }
        for (j = 0; j < best_b; j++) {
          int add = extra > 7 ? 7 : extra;
          ssym[cnt] = 17; sext[cnt++] = add; extra -= add;
        }
        for (j = 0; j < best_c; j++) { ssym[cnt] = 0; sext[cnt++] = 0; }
      }
    } else {
      int remain = r - 1, best_lit = remain;
      ssym[cnt] = c; sext[cnt++] = 0;
      best = (uint64_t)remain * (cost[c] ? cost[c] : 64);
      for (a = 1; a * 3 <= remain; a++) {
        int literals = remain > 6 * a ? remain - 6 * a : 0;
        uint64_t v = (uint64_t)a * ((cost[16] ? cost[16] : 64) + 2) +
                     (uint64_t)literals * (cost[c] ? cost[c] : 64);
        if (v < best) { best = v; best_a = a; best_lit = literals; }
      }
      {
        int extra = remain - 3 * best_a - best_lit, j;
        for (j = 0; j < best_a; j++) {
          int add = extra > 3 ? 3 : extra;
          ssym[cnt] = 16; sext[cnt++] = add; extra -= add;
        }
        for (j = 0; j < best_lit; j++) { ssym[cnt] = c; sext[cnt++] = 0; }
      }
    }
    i += run;
  }
  return cnt;
}

static W1_UNUSED int w1_huff_rle_finish(int n, int last, int *ssym,
                                        int *sext, int cnt, int *use_len,
                                        int *max_sym) {
  *max_sym = cnt;
  *use_len = cnt < n;
  if (!*use_len || cnt < 2) {
    int r = n - 1 - last;
    *use_len = 0;
    while (r >= 11) {
      int take = r > 138 ? 138 : r;
      ssym[cnt] = 18; sext[cnt++] = take - 11; r -= take;
    }
    if (r >= 3) { ssym[cnt] = 17; sext[cnt++] = r - 3; r = 0; }
    while (r-- > 0) { ssym[cnt] = 0; sext[cnt++] = 0; }
  }
  return cnt;
}

static W1_UNUSED uint64_t w1_huff_rle_bits(const int *ssym,
                                            const uint8_t *cl_lens,
                                            int cnt, int ncl, int cl_nz,
                                            int use_len, int max_sym) {
  uint64_t bits = 6 + (uint64_t)3 * ncl;
  int k;
  if (use_len) {
    int N = 0;
    while (N < 7 && max_sym - 2 >= (1 << (2 + 2 * N))) N++;
    bits += 5 + 2 * N;
  }
  for (k = 0; k < cnt; k++) {
    int s = ssym[k];
    if (cl_nz > 1) bits += cl_lens[s];
    bits += s == 16 ? 2 : (s == 17 ? 3 : (s == 18 ? 7 : 0));
  }
  return bits;
}

/* Emit one Huffman table (simple or normal path). seq needs 2*n ints. */
static W1_UNUSED void w1_huff_emit(w1_bw_t *bw, const uint8_t *lens, int n,
                                   int *seq) {
  int *ssym = seq, *sext = seq + n;
  int i, nz = 0, s0 = 0, s1 = 0;
  for (i = 0; i < n; i++)
    if (lens[i]) { nz++; if (nz == 1) s0 = i; else if (nz == 2) s1 = i; }
  /* Simple path stores symbols in 1 or 8 bits: only usable when every
   * symbol is < 256 (green length/cache symbols need the normal path). */
  if (nz <= 2 && s0 < 256 && (nz < 2 || s1 < 256)) {
    int two = (nz == 2), big = (s0 > 1);
    w1_bw_put(bw, 1, 1);
    w1_bw_put(bw, (uint32_t)two, 1);
    w1_bw_put(bw, (uint32_t)big, 1);
    w1_bw_put(bw, (uint32_t)s0, big ? 8 : 1);
    if (two) w1_bw_put(bw, (uint32_t)s1, 8);
    return;
  }
  {
    int last = n - 1, cnt, use_len, max_sym, ncl, k, cl_nz;
    int cl_counts[19], cl_codes[19], htmp[5 * 19];
    uint8_t cl_lens[19];
    uint64_t chosen_bits;
    while (last > 0 && lens[last] == 0) last--;
    cnt = w1_huff_rle_greedy(lens, last, ssym, sext);
    cnt = w1_huff_rle_finish(n, last, ssym, sext, cnt, &use_len, &max_sym);
    for (k = 0; k < 19; k++) cl_counts[k] = 0;
    for (k = 0; k < cnt; k++) cl_counts[ssym[k]]++;
    cl_nz = w1_huff_lengths(cl_counts, 19, 7, cl_lens, htmp);
    {
      uint64_t original_bits, candidate_bits;
      int original_ncl = 4, candidate_ncl, candidate_nz;
      for (k = 18; k >= 0; k--)
        if (cl_lens[w1k_vp8l_cl_order[k]]) { original_ncl = k + 1; break; }
      if (original_ncl < 4) original_ncl = 4;
      original_bits = w1_huff_rle_bits(ssym, cl_lens, cnt, original_ncl,
          cl_nz, use_len, max_sym);
      cnt = w1_huff_rle_optimized(lens, last, cl_lens, ssym, sext);
      cnt = w1_huff_rle_finish(n, last, ssym, sext, cnt, &use_len, &max_sym);
      for (k = 0; k < 19; k++) cl_counts[k] = 0;
      for (k = 0; k < cnt; k++) cl_counts[ssym[k]]++;
      candidate_nz = w1_huff_lengths(cl_counts, 19, 7, cl_lens, htmp);
      candidate_ncl = 4;
      for (k = 18; k >= 0; k--)
        if (cl_lens[w1k_vp8l_cl_order[k]]) { candidate_ncl = k + 1; break; }
      if (candidate_ncl < 4) candidate_ncl = 4;
      candidate_bits = w1_huff_rle_bits(ssym, cl_lens, cnt, candidate_ncl,
          candidate_nz, use_len, max_sym);
      if (candidate_bits >= original_bits) {
        cnt = w1_huff_rle_greedy(lens, last, ssym, sext);
        cnt = w1_huff_rle_finish(n, last, ssym, sext, cnt, &use_len, &max_sym);
        for (k = 0; k < 19; k++) cl_counts[k] = 0;
        for (k = 0; k < cnt; k++) cl_counts[ssym[k]]++;
        cl_nz = w1_huff_lengths(cl_counts, 19, 7, cl_lens, htmp);
        chosen_bits = original_bits;
      } else {
        cl_nz = candidate_nz;
        chosen_bits = candidate_bits;
      }
    }
    /* All-literal candidate: when every nonzero length is equal (near-uniform
     * symbol counts), one code-length symbol covers the whole alphabet and a
     * single-symbol CL tree codes each of the n entries in 0 bits, so the
     * RLE-free path costs header + n*0. Noise-128's red/blue tables are 256
     * eights: 39 bits vs 181 RLE bits (libwebp 119). */
    {
      int lit_counts[19], lit_codes[19], lit_nz, lit_ncl, i2;
      uint8_t lit_lens[19];
      uint64_t lit_bits = 6;
      for (k = 0; k < 19; k++) lit_counts[k] = 0;
      for (i2 = 0; i2 < n; i2++) lit_counts[lens[i2] & 15]++;
      lit_nz = w1_huff_lengths(lit_counts, 19, 7, lit_lens, htmp);
      lit_ncl = 4;
      for (k = 18; k >= 0; k--)
        if (lit_lens[w1k_vp8l_cl_order[k]]) { lit_ncl = k + 1; break; }
      if (lit_ncl < 4) lit_ncl = 4;
      lit_bits += (uint64_t)3 * lit_ncl;
      if (lit_nz > 1)
        for (i2 = 0; i2 < n; i2++) lit_bits += lit_lens[lens[i2]];
      if (lit_bits < chosen_bits) {
        w1_huff_codes(lit_lens, 19, lit_codes);
        w1_bw_put(bw, 0, 1);
        w1_bw_put(bw, (uint32_t)(lit_ncl - 4), 4);
        for (k = 0; k < lit_ncl; k++)
          w1_bw_put(bw, lit_lens[w1k_vp8l_cl_order[k]], 3);
        w1_bw_put(bw, 0, 1);   /* all n entries, no length limit */
        if (lit_nz > 1)
          for (i2 = 0; i2 < n; i2++)
            w1_bw_code(bw, lit_codes[lens[i2]], lit_lens[lens[i2]]);
        return;
      }
    }
    w1_huff_codes(cl_lens, 19, cl_codes);
    ncl = 4;
    for (k = 18; k >= 0; k--)
      if (cl_lens[w1k_vp8l_cl_order[k]]) { ncl = k + 1; break; }
    if (ncl < 4) ncl = 4;
    w1_bw_put(bw, 0, 1);
    w1_bw_put(bw, (uint32_t)(ncl - 4), 4);
    for (k = 0; k < ncl; k++)
      w1_bw_put(bw, cl_lens[w1k_vp8l_cl_order[k]], 3);
    w1_bw_put(bw, (uint32_t)use_len, 1);
    if (use_len) {
      int N = 0;
      while (N < 7 && max_sym - 2 >= (1 << (2 + 2 * N))) N++;
      w1_bw_put(bw, (uint32_t)N, 3);
      w1_bw_put(bw, (uint32_t)(max_sym - 2), 2 + 2 * N);
    }
    for (k = 0; k < cnt; k++) {
      int s = ssym[k];
      if (cl_nz > 1) w1_bw_code(bw, cl_codes[s], cl_lens[s]);
      if (s == 16) w1_bw_put(bw, (uint32_t)sext[k], 2);
      else if (s == 17) w1_bw_put(bw, (uint32_t)sext[k], 3);
      else if (s == 18) w1_bw_put(bw, (uint32_t)sext[k], 7);
    }
  }
}

/* Header-aware code lengths. Huffman lengths minimise the payload alone;
 * on near-uniform tables (noise) the 7/8/9 mix saves a few payload bits
 * and costs hundreds to transmit, while a flatter tree (fewer distinct
 * lengths) serialises in a handful of RLE codes. Each length limit maps to
 * a count floor in w1_huff_lengths, i.e. a progressively flatter real tree,
 * so the sweep from the free Huffman depth down to the uniform depth is a
 * one-parameter family; payload + emitted header bits picks the member.
 * best holds n bytes of scratch for the winner. */
static W1_UNUSED int w1_huff_lengths_hdr(const int *counts, int n,
                                         uint8_t *lens, int *tmp, int *seq,
                                         uint8_t *best) {
  int m, i, limit, maxd = 0, floor_d = 0;
  uint64_t best_bits;
  m = w1_huff_lengths(counts, n, 15, lens, tmp);
  if (m <= 2) return m;
  for (i = 0; i < n; i++) if (lens[i] > maxd) maxd = lens[i];
  while ((m - 1) >> floor_d) floor_d++;
  if (maxd <= floor_d) return m;
  {
    w1_bw_t bw;
    w1_bw_init(&bw, NULL, 0);
    w1_huff_emit(&bw, lens, n, seq);
    best_bits = (uint64_t)bw.bytes * 8 + (uint64_t)bw.nbits;
    for (i = 0; i < n; i++)
      best_bits += (uint64_t)(unsigned)counts[i] * lens[i];
  }
  memcpy(best, lens, (size_t)n);
  for (limit = maxd - 1; limit >= floor_d; limit--) {
    uint64_t bits;
    w1_bw_t bw;
    w1_huff_lengths(counts, n, limit, lens, tmp);
    w1_bw_init(&bw, NULL, 0);
    w1_huff_emit(&bw, lens, n, seq);
    bits = (uint64_t)bw.bytes * 8 + (uint64_t)bw.nbits;
    for (i = 0; i < n; i++) bits += (uint64_t)(unsigned)counts[i] * lens[i];
    if (bits < best_bits) { best_bits = bits; memcpy(best, lens, (size_t)n); }
  }
  memcpy(lens, best, (size_t)n);
  return m;
}

#define W1_LE_MAX_DIST 1048456
#define W1_LE_MAX_LEN 4096
#define W1_LE_INF 0x3fffffff
#ifndef W1_LZ_OPT_MAXN
#define W1_LZ_OPT_MAXN 16384
#endif
/* Soft price (bits) the DP charges for a symbol the current lens leave
 * uncoded; the support loop then adds whatever the parse actually used. */
#ifndef W1_LZ_OPT_ABSENT
#define W1_LZ_OPT_ABSENT 16
#endif
#define W1_LZ_OPT_NC 6
#ifndef W1_LZ_OPT_LEVEL
#define W1_LZ_OPT_LEVEL 9
#endif
#ifndef W1_LE_UNI_PORTFOLIO_MAXN
#define W1_LE_UNI_PORTFOLIO_MAXN 16384
#endif
#ifndef W1_LE_UNI_PORTFOLIO_K
#define W1_LE_UNI_PORTFOLIO_K 2
#endif

#define W1_LE_NG 2328
#define W1_LE_OFF_R 2328
#define W1_LE_OFF_B 2584
#define W1_LE_OFF_A 2840
#define W1_LE_OFF_D 3096
#define W1_LE_NC (3096 + 40)
/* Clustered entropy stream: at most this many groups, clustered from
 * 2^W1_LE_GRP_PB pixel tiles by per-tile residual entropy. */
#define W1_LE_MAXG 8
#define W1_LE_GRP_PB 4
#define W1_LE_GRP_BANDS 8
#define W1_LE_GRP_MAXTILES 4096

/* Recorded parse (token memo). A tokenize pass is a pure function of the
 * pixel array, its cost tables, the search budget, the cache size and the
 * group layout, so a pass that would repeat an earlier one (the emit after
 * the accounting pass that sized it, or a trial stream re-encoded for real)
 * replays the recorded tokens through the sinks instead of parsing again.
 * The key fields identify the computation; lens holds the tables the
 * tokens were parsed under (per group for the clustered stream). Slots are
 * only trusted inside one w1_le_stream call, where the pixel array is
 * constant. */
#define W1_TK_FINISH 1
#define W1_TK_GROUP 2
#define W1_TK_LEGACY 3
typedef struct {
  int valid, kind, w, h, level, cache_bits, depth, ngroups, ntok;
  const uint32_t *pix;
  uint32_t *tok;               /* max_n packed tokens (W1_LZ_BK_*) */
  uint8_t *lens;               /* W1_LE_MAXG * W1_LE_NC */
} w1_le_tk_t;

/* Candidate store for one match-table fill (w1_lz_fill). The table search
 * visits the same-hash predecessors of every position newest-first, bounded
 * by the search depth; a linked chain makes those hops dependent random
 * loads, which dominates the fill on large arrays. Instead a counting sort
 * groups every bucket's chained positions into one ascending segment of
 * list, each segment preceded by a W1_LZ_NOBKT sentinel, and slot[pos] is
 * the position's own slot in that segment. The newest candidate below pos
 * is then simply list[slot[pos] - 1], and the walk steps down the segment,
 * visiting the same candidates in the same order a chain pointer would.
 * Candidate set, visit order and budget accounting are identical to the
 * chain walk, so every match table is bit-identical: the old chain's
 * self-loop at position 0 (tab[0] was zeroed after the build, so a walk
 * reaching 0 re-measured it until the budget ran out) now ends at the
 * sentinel, which cannot change a result. slot[] is rewritable: the build
 * pass fills it with bucket ids, the scatter rewrites it in place with the
 * slot. One store is shared by all fills: a fill completes before its
 * table is published, and only the published tab[] is read later. */
#define W1_LZ_NOBKT 0xffffffffu
typedef struct {
  uint32_t *list;   /* n+nb+1: chained positions per bucket, ascending */
  uint32_t *slot;   /* n: own slot in list, W1_LZ_NOBKT if unchained */
  int *off;         /* nb+1: segment starts (sentinel at off[h]-1) */
  int *cur;         /* nb: scatter cursors */
  int *head;        /* nb: chain heads (sample + fallback walk) */
  int hbits, nb;
} w1_lz_cand_t;
/* Searches sampled with the chain walk before choosing it for the rest of
 * the fill; the scatter is only built when the sampled hop rate (chain
 * steps per search) says the sequential walk will pay it back. */
#ifndef W1_LZ_SAMPLE
#define W1_LZ_SAMPLE 4096
#endif
/* The sample adopts the sequential walk when its average walk is long
 * enough; a short walk (matches found in the first few candidates, or few
 * candidates per bucket) stays cheaper on the chain, where no scatter has
 * to be built. */
#ifndef W1_LZ_LIST_MINHOP
#define W1_LZ_LIST_MINHOP 32
#endif

/* Encoder scratch context (carved from the bump once per encode). */
typedef struct {
  int max_n;
  /* Per-position best-match tables (w1_lz_fill), two slots so a sub-image
   * pass between two passes over the main image does not evict the main
   * table. A slot is identified by the geometry, the search budget and the
   * content hash of the array it was filled for. The fill victim is the
   * slot whose cache is worth least: tab_uses (hits since the fill) times
   * the pixels a refill would re-search, with tab_lru breaking ties by
   * recency. A table nothing has re-read yet (worth 0) is evicted before
   * one many passes read, and among re-read tables the bigger refill is
   * kept. */
  uint32_t *tab[2];
  /* Runner-up tables (same slots, DP only, NULL otherwise): the closer,
   * shorter match the longest-match rule displaced (or the nearest chain
   * candidate), whose distance code is often far cheaper. */
  uint32_t *near[2];
  int tab_n[2], tab_w[2], tab_iters[2], tab_valid[2], tab_lru;
  uint64_t tab_hash[2];
  uint32_t tab_uses[2];
  w1_lz_cand_t cand;           /* shared fill scratch (w1_lz_cand_t) */
  w1_le_tk_t tk[2];            /* recorded parses (w1_le_stream trials) */
  int *counts;                 /* 2328 + 256*3 + 40 */
  int *counts2;                /* second buffer (predictor gate) */
  int *counts3;                /* branch-B counts save */
  int *counts4;                /* uniform-trial estimator scratch */
  /* Uniform-predictor portfolio (w1_vp8l_encode_full): uni_force >= 0
   * makes every predictor branch use that single mode; uni_rank/uni_est
   * report the branch's uniform-mode ranking back to the portfolio. */
  int uni_force, uni_n, uni_rank[14];
  uint64_t uni_est[14];
  /* Subtract-green duel (w1_vp8l_encode_full): sg_force 0/1 pins the
   * branch off/on and skips the loser's trial entirely; -1 keeps the
   * estimator gate. sg_used reports the branch the run took, or -1 when
   * the stream cannot depend on it (hopeless/uniform fast path, which
   * already decides green by an exact A/B duel, and the palette-only
   * shortcut, which never applies green) so the caller can skip the
   * re-encode outright. sb_hint/sb_used carry the predictor tile size the
   * same way: w1_le_pick_sb ranks it from the pre-green image, so both
   * sides of the duel search for and find the same one. sb_alt reports a
   * second tile size the search liked but did not return, or -1. */
  int sg_force, sg_used, sb_hint, sb_used, sb_alt;
  /* Palette duel (w1_vp8l_encode_full): pal_force 1 accepts the
   * palette+spatial trial whenever the branch produces one, bypassing the
   * estimate that decides it; -1 keeps the estimate. pal_seen reports that
   * a candidate existed and the estimate turned it down, so the caller
   * knows a pinned re-encode could come out different. */
  int pal_force, pal_seen;
  uint8_t *lens; int *codes;   /* same layout, bytes vs ints */
  int *seq;                    /* 2 * 2328 (tree emit) */
  int *htmp;                   /* 5 * 2328 (huffman build) */
  int *phist;                  /* 14 * 1024 (pixel-major predict) */
  uint32_t *cache;             /* 2048 */
  int *gcounts;                /* W1_LE_MAXG * W1_LE_NC (parse counts) */
  int *grcounts;               /* W1_LE_MAXG * W1_LE_NC (support base) */
  uint8_t *glens;              /* W1_LE_MAXG * W1_LE_NC */
  int *gcodes;                 /* W1_LE_MAXG * W1_LE_NC */
  uint32_t *dp;                /* optimal-parse DP costs (opt_cap + 1) */
  uint32_t *back;              /* optimal-parse decisions (opt_cap) */
  int opt_cap;                 /* 0 = greedy only */
  /* Winning tiling from w1_le_pick_sb. The branch that follows runs the
   * same predict + resolve on the same src at the same sb, and resolve
   * re-tokenizes the whole image once per tie trial, so the result is
   * carried over instead of recomputed. sv_sum is a checksum of the src
   * that produced it: the buffers are reused between calls, so the
   * pointer alone does not establish that the contents still match. */
  uint32_t *sv_dst;            /* saved residuals (max_n) */
  uint8_t *sv_modes;           /* saved per-tile modes */
  const uint32_t *sv_src;
  uint64_t sv_sum;
  int sv_w, sv_h, sv_sb, sv_level, sv_cb, sv_ok;
} w1_le_ctx_t;

/* Cheap content checksum for the saved-tiling guard above. */
static W1_UNUSED uint64_t w1_le_sum(const uint32_t *p, int n) {
  uint64_t s = 1469598103934665603ULL;
  int i;
  for (i = 0; i < n; i++) { s ^= p[i]; s *= 1099511628211ULL; }
  return s ^ ((uint64_t)(unsigned)n * 2654435761ULL);
}

static W1_UNUSED int w1_le_tk_match(const w1_le_tk_t *tk, int kind,
                                    const uint32_t *pix, int w, int h,
                                    int level, int cache_bits, int depth) {
  return tk && tk->valid && tk->kind == kind && tk->pix == pix &&
         tk->w == w && tk->h == h && tk->level == level &&
         tk->cache_bits == cache_bits && tk->depth == depth;
}

static W1_UNUSED void w1_le_tk_set(w1_le_tk_t *tk, int kind,
                                   const uint32_t *pix, int w, int h,
                                   int level, int cache_bits, int depth,
                                   int ntok, const uint8_t *lens,
                                   int ngroups) {
  tk->valid = 1; tk->kind = kind; tk->pix = pix; tk->w = w; tk->h = h;
  tk->level = level; tk->cache_bits = cache_bits; tk->depth = depth;
  tk->ntok = ntok; tk->ngroups = ngroups;
  memcpy(tk->lens, lens, (size_t)ngroups * W1_LE_NC);
}

/* Pixel-pair hash (libwebp's GetPixPairHash64 constants). */
static W1_UNUSED unsigned w1_lz_hash2(uint32_t a, uint32_t b, int hbits) {
  return (b * 0x1e35a7bdu + a * 0x5bd1e996u) >> (32 - hbits);
}

/* Content hash of a pixel array, keyed with its length: four independent
 * FNV-style lanes so the O0 build overlaps the multiply chains. Change
 * detection only (a buffer rewritten in place between passes must not
 * reuse the old match table). */
static W1_UNUSED uint64_t w1_lz_pixhash(const uint32_t *pix, int n) {
  const uint64_t P = 0x100000001b3ull;
  uint64_t h0 = 0xcbf29ce484222325ull ^ (uint64_t)(unsigned)n;
  uint64_t h1 = 0x9e3779b97f4a7c15ull, h2 = 0x6a09e667f3bcc909ull;
  uint64_t h3 = 0xbb67ae8584caa73bull;
  int i = 0;
  for (; i + 4 <= n; i += 4) {
    h0 = (h0 ^ pix[i]) * P;
    h1 = (h1 ^ pix[i + 1]) * P;
    h2 = (h2 ^ pix[i + 2]) * P;
    h3 = (h3 ^ pix[i + 3]) * P;
  }
  for (; i < n; i++) h0 = (h0 ^ pix[i]) * P;
  h0 = (h0 ^ h1) * P;
  h0 = (h0 ^ h2) * P;
  return (h0 ^ h3) * P;
}

/* Match search state + cost model + sinks. */
typedef struct {
  const uint32_t *pix;
  int w, n;
  w1_le_ctx_t *ctx;         /* owner of the match tables + hash heads */
  uint32_t *tab;            /* the match table once w1_lz_prepare ran */
  uint32_t *near;           /* nearest-match table (DP only, else NULL) */
  int depth;                /* fill budget: chain candidates per position */
  const uint8_t *lg, *lr, *lb, *la, *ld;   /* cost model */
  const uint8_t *eg, *er, *eb, *ea, *ed;   /* emit lengths (final H) */
  int sg, sr, sb, sa, sd;   /* table is single-symbol: emit 0 bits */
  int cache_bits, cache_size;
  uint32_t *cache;
  int *cg, *cr, *cb, *ca, *cd;
  const int *codes_g, *codes_r, *codes_b, *codes_a, *codes_d;
  w1_bw_t *bw;
  const uint32_t *group_map;
  int group_width;
  int group_shift;
  int ngroups;
  int group_realcost;   /* use per-group lens as the parse cost model */
  int *group_counts[W1_LE_MAXG];
  const uint8_t *group_lens[W1_LE_MAXG];
  const int *group_codes[W1_LE_MAXG];
  int group_single[W1_LE_MAXG][5];
  /* Bounded optimal parse (dp/back non-NULL + opt set): forward DP over
   * token starts under the current cost model instead of greedy+lazy. */
  int opt;
  uint32_t *dp;         /* n + 1 cumulative token costs */
  uint32_t *back;       /* n packed winning decisions (see w1_lz_run_opt) */
  /* Token memo: tk_out records every token the parse emits (packed
   * W1_LZ_BK_*); tk_in replays a recorded stream instead of parsing. */
  uint32_t *tk_out; int tk_n;
  const uint32_t *tk_in; int tk_in_n;
  int rep_dist;         /* distance of the last emitted match (candidate) */
  int soft;             /* DP: price absent symbols instead of forbidding them */
} w1_lez_t;

static W1_UNUSED int w1_lz_symcost(const uint8_t *lens, int s, int extra) {
  return lens[s] ? (int)lens[s] + extra : W1_LE_INF;
}

/* Best (cheapest) dist option for dist. Returns total dist cost, or INF. */
static W1_UNUSED int w1_lz_distcost(const w1_lez_t *lz, int dist, int *dsym,
                                    int *dextra, int *debits) {
  int c1 = W1_LE_INF, c2 = W1_LE_INF, s1 = 0, e1 = 0, b1 = 0, s2 = 0, e2 = 0,
      b2 = 0;
  int plane = w1_dist_plane(dist, lz->w);
  if (plane <= 120) {
    w1_prefix_inv(plane, &s1, &e1, &b1);
    if (s1 < 40) c1 = w1_lz_symcost(lz->ld, s1, b1);
  }
  w1_prefix_inv(dist + 120, &s2, &e2, &b2);
  if (s2 < 40) c2 = w1_lz_symcost(lz->ld, s2, b2);
  /* Both forms uncoded (a soft DP parse may still take the match): report
   * the direct form so the count pass sees the symbol it will need. */
  if (c2 < c1 || (c1 >= W1_LE_INF && plane > 120)) {
    *dsym = s2; *dextra = e2; *debits = b2; return c2;
  }
  *dsym = s1; *dextra = e1; *debits = b1; return c1;
}

/* Every match length costs the same as the *base* length of its prefix code
 * (the symbol and its extra-bit count are constant inside one code), so
 * these 24 bases bracket every distinct length cost. */
static const int w1k_lz_bases[24] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
  513, 769, 1025, 1537, 2049, 3073
};
/* Packed token / backtrack decision: 0 literal, 1 cache literal, else
 * (dist << 12) | (len - 2). Matches are always >= 4096, so >= 2 tags a
 * match and the two literal forms stay distinct. */
#define W1_LZ_BK_CACHE 1u
#define W1_LZ_BK_MATCH(len, dist) \
  ((((uint32_t)(dist)) << 12) | (uint32_t)((len) - 2))
#define W1_LZ_BK_ISMATCH(b) ((b) >= 2u)
/* Match table entry: (dist << 12) | (len - 1); 0 = no match (dist >= 1 for
 * every match, so every real entry is >= 4096). */
#define W1_LZ_TAB(len, dist) \
  ((((uint32_t)(dist)) << 12) | (uint32_t)((len) - 1))
#define W1_LZ_TAB_LEN(t) ((int)((t) & 0xfffu) + 1)
#define W1_LZ_TAB_DIST(t) ((int)((t) >> 12))

/* Common prefix length of a and b, at most max; 0 straight away when the
 * pixel at offset best differs (a candidate that cannot beat the incumbent
 * is never extended). */
static W1_UNUSED int w1_lz_matchlen(const uint32_t *a, const uint32_t *b,
                                    int best, int max) {
  int len = 0;
  if (best >= max || a[best] != b[best]) return 0;
#ifdef W1_USE_SSE2
  while (len + 4 <= max) {
    __m128i va = _mm_loadu_si128((const __m128i *)(const void *)(a + len));
    __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)(b + len));
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(va, vb)) != 65535) break;
    len += 4;
  }
#endif
  while (len < max && a[len] == b[len]) len++;
  return len;
}

/* Longest match at every position, filled once per pixel array: the search
 * does not depend on the cost model, only the parse does (w1_lz_find), so
 * every pass over the same pixels reads one table instead of searching
 * again. Modeled on libwebp's VP8LHashChainFill:
 *  - positions are bucketed by a 2-pixel hash, and a run of one colour
 *    hashes (colour, pixels left in the run) so a run-interior position
 *    reaches only end-aligned positions of earlier runs (the only alignment
 *    that can match past the run's end), never the run's own predecessors;
 *  - the search walks backward, so a match found at pos extends to pos-1
 *    for free while the pixel before both intervals agrees, which covers
 *    the interior of every long match without a walk;
 *  - the walk is bounded by `iters` candidates, the distance-1 and
 *    one-row-up candidates are always tried first, and a candidate is only
 *    extended when it can beat the incumbent (pixel at offset best_len).
 * Two walks produce the same result:
 *  - w1_lz_search_link follows the predecessor chain built in tab[]; its
 *    hops are dependent random loads, cheap while chains are short;
 *  - w1_lz_search reads the counting-sorted segments (see w1_lz_cand_t),
 *    whose hops are sequential.
 * w1_lz_fill samples the first searches with the chain walk and only pays
 * for the segment scatter when the sample's hop rate says the sequential
 * walk will pay it back (long walks, e.g. photo overlaid with text). */
static W1_UNUSED void w1_lz_search(const uint32_t *pix, w1_lz_cand_t *cd,
                                   int n, int w, int iters, int base,
                                   int *blen, int *bdist, uint32_t *near) {
  const uint32_t *ps = pix + base;
  int max_len = n - base, iter = iters, best_len = *blen, best_dist = *bdist;
  int min_pos = base > W1_LE_MAX_DIST ? base - W1_LE_MAX_DIST : 0;
  uint32_t so = cd->slot[base];
  int idx = -1, cand;
  /* Match length at the first candidate once it has been measured for
   * `near`, so the walk below does not measure the same candidate a second
   * time; -1 = not measured. */
  int first_l = -1;
  if (so != W1_LZ_NOBKT) {
    idx = (int)so - 1;
    cand = (int)cd->list[idx];
  } else cand = -1;
  if (max_len > W1_LE_MAX_LEN) max_len = W1_LE_MAX_LEN;
  if (near) {
    /* Nearest candidate (one extra prefix scan per searched base); the
     * walk below replaces it with the runner-up when a farther candidate
     * wins. */
    int l = cand >= min_pos ? w1_lz_matchlen(pix + cand, ps, 0, max_len) : 0;
    *near = l >= 2 ? W1_LZ_TAB(l, base - cand) : 0;
    if (cand >= min_pos) first_l = l;
  }
  if (base >= w && best_dist != w) {
    int l = w1_lz_matchlen(ps - w, ps, best_len, max_len);
    if (l > best_len) { best_len = l; best_dist = w; }
    iter--;
  }
  if (best_dist != 1) {
    int l = w1_lz_matchlen(ps - 1, ps, best_len, max_len);
    if (l > best_len) { best_len = l; best_dist = 1; }
    iter--;
  }
  if (best_len < max_len) {
    uint32_t best_px = ps[best_len];
    while (cand >= min_pos && --iter > 0) {
      int l;
      if (first_l >= 0) {
        /* Same candidate the `near` probe just measured: reuse it. The
         * skipped pix[] test only decides whether l can exceed best_len,
         * which the l > best_len below decides exactly. */
        l = first_l;
        first_l = -1;
      } else if (pix[cand + best_len] == best_px) {
        l = w1_lz_matchlen(pix + cand, ps, 0, max_len);
      } else l = 0;
      if (l > best_len) {
        /* Runner-up: the match this one displaces is closer (the walk runs
         * nearest-first), so it carries a cheaper distance code. */
        if (near && best_len >= 2) *near = W1_LZ_TAB(best_len, best_dist);
        best_len = l; best_dist = base - cand;
        if (best_len >= max_len) break;
        best_px = ps[best_len];
      }
      cand = (int)cd->list[--idx];
    }
  }
  *blen = best_len; *bdist = best_dist;
}

/* Chain walk: same candidates and order as w1_lz_search, through the
 * predecessor links in chain[] (= tab before the results overwrite them).
 * `hops` optionally counts walk steps (the fill's sampling probe). */
static W1_UNUSED void w1_lz_search_link(const uint32_t *pix,
                                        const int32_t *chain, int n, int w,
                                        int iters, int base, int *blen,
                                        int *bdist, uint32_t *near,
                                        uint64_t *hops) {
  const uint32_t *ps = pix + base;
  int max_len = n - base, iter = iters, best_len = *blen, best_dist = *bdist;
  int min_pos = base > W1_LE_MAX_DIST ? base - W1_LE_MAX_DIST : 0;
  int cand = chain[base];
  /* Match length at the first chain candidate once it has been measured
   * for `near`, so the walk below does not measure the same candidate a
   * second time; -1 = not measured. */
  int first_l = -1;
  if (max_len > W1_LE_MAX_LEN) max_len = W1_LE_MAX_LEN;
  if (near) {
    int l = cand >= min_pos ? w1_lz_matchlen(pix + cand, ps, 0, max_len) : 0;
    *near = l >= 2 ? W1_LZ_TAB(l, base - cand) : 0;
    if (cand >= min_pos) first_l = l;
  }
  if (base >= w && best_dist != w) {
    int l = w1_lz_matchlen(ps - w, ps, best_len, max_len);
    if (l > best_len) { best_len = l; best_dist = w; }
    iter--;
  }
  if (best_dist != 1) {
    int l = w1_lz_matchlen(ps - 1, ps, best_len, max_len);
    if (l > best_len) { best_len = l; best_dist = 1; }
    iter--;
  }
  if (best_len < max_len) {
    uint32_t best_px = ps[best_len];
    for (; cand >= min_pos && --iter > 0; cand = chain[cand]) {
      int l;
      if (hops) (*hops)++;
      if (first_l >= 0) {
        l = first_l;
        first_l = -1;
      } else {
        if (pix[cand + best_len] != best_px) continue;
        l = w1_lz_matchlen(pix + cand, ps, 0, max_len);
      }
      if (l > best_len) {
        if (near && best_len >= 2) *near = W1_LZ_TAB(best_len, best_dist);
        best_len = l; best_dist = base - cand;
        if (best_len >= max_len) break;
        best_px = ps[best_len];
      }
    }
  }
  *blen = best_len; *bdist = best_dist;
}

/* Newest candidate below `pos`, used to keep the runner-up table sliding
 * with the match (chain[xx] in the chain walk). */
static W1_UNUSED int w1_lz_first_below(const w1_lz_cand_t *cd, int pos) {
  uint32_t so = cd->slot[pos];
  return so == W1_LZ_NOBKT ? -1 : (int)cd->list[(int)so - 1];
}

/* Counting sort of the bucket ids in slot[] (see w1_lz_cand_t): off[] holds
 * the bucket sizes on entry and the segment starts on exit, one sentinel
 * slot precedes each segment, and slot[] is rewritten to each position's
 * own slot. Only called when the fill's sample adopts the sequential walk. */
static W1_UNUSED void w1_lz_list_build(w1_lz_cand_t *cd, int n) {
  uint32_t *list = cd->list, *slot = cd->slot;
  int *off = cd->off, *cur = cd->cur;
  int h, nb = cd->nb, pos, acc = 0;
  for (h = 0; h < nb; h++) { int c = off[h]; acc++; off[h] = acc; acc += c; }
  off[nb] = acc;
  for (h = 0; h < nb; h++) {
    list[off[h] - 1] = W1_LZ_NOBKT;
    cur[h] = off[h];
  }
  for (pos = 0; pos < n - 1; pos++) {
    unsigned bh = slot[pos];
    if (bh != W1_LZ_NOBKT) {
      int s = cur[bh]++;
      list[s] = (uint32_t)pos;
      slot[pos] = (uint32_t)s;
    }
  }
}

static W1_UNUSED void w1_lz_fill(uint32_t *tab, uint32_t *near,
                                 const uint32_t *pix, int n, int w, int iters,
                                 w1_lz_cand_t *cd) {
  int32_t *chain = (int32_t *)(void *)tab;
  int hbits = cd->hbits, nb = cd->nb;
  uint32_t *slot = cd->slot;
  int *off = cd->off, *head = cd->head;
  int pos, base, h, comp, use_list = 0, nsample = 0;
  uint64_t hops = 0;
  if (n <= 2) {
    for (pos = 0; pos < n; pos++) { tab[pos] = 0; if (near) near[pos] = 0; }
    return;
  }
  if (near) { near[0] = 0; near[n - 1] = 0; }
  /* Hash/link pass: chain every chained position into its bucket and
   * record the bucket in slot[] (rewritten to the list slot by a later
   * scatter). Unchained run interiors get the -1 terminator the chain walk
   * expects. */
  memset(head, 0xff, (size_t)4 << hbits);
  comp = (pix[0] == pix[1]);
  for (pos = 0; pos < n - 2;) {
    int comp_next = (pix[pos + 1] == pix[pos + 2]);
    if (comp && comp_next) {
      uint32_t v = pix[pos];
      int len = 1;
      while (pos + len + 2 < n && pix[pos + len + 2] == v) len++;
      if (len > W1_LE_MAX_LEN) {
        /* Deep inside a run every position matches its predecessor at the
         * maximum length; leave those unchained (the distance-1 probe
         * finds that match). */
        memset(chain + pos, 0xff, (size_t)(len - W1_LE_MAX_LEN) * 4);
        memset(slot + pos, 0xff, (size_t)(len - W1_LE_MAX_LEN) * 4);
        pos += len - W1_LE_MAX_LEN;
        len = W1_LE_MAX_LEN;
      }
      while (len) {
        h = (int)w1_lz_hash2(v, (uint32_t)len--, hbits);
        slot[pos] = (uint32_t)h;
        chain[pos] = head[h];
        head[h] = pos++;
      }
      comp = 0;
    } else {
      h = (int)w1_lz_hash2(pix[pos], pix[pos + 1], hbits);
      slot[pos] = (uint32_t)h;
      chain[pos] = head[h];
      head[h] = pos++;
      comp = comp_next;
    }
  }
  h = (int)w1_lz_hash2(pix[n - 2], pix[n - 1], hbits);
  slot[n - 2] = (uint32_t)h;
  chain[n - 2] = head[h];
  tab[0] = 0;
  tab[n - 1] = 0;
  for (base = n - 2; base > 0;) {
    int best_len = 0, best_dist = 0, max_base, nr_len = 0, nr_dist = 0;
    uint32_t nr = 0;
    if (!use_list && nsample < W1_LZ_SAMPLE) {
      w1_lz_search_link(pix, chain, n, w, iters, base, &best_len, &best_dist,
                        near ? &nr : NULL, &hops);
      if (++nsample >= W1_LZ_SAMPLE) {
        if (hops >= (uint64_t)nsample * W1_LZ_LIST_MINHOP) {
          memset(off, 0, ((size_t)nb + 1) * 4);
          for (pos = 0; pos < n - 1; pos++)
            if (slot[pos] != W1_LZ_NOBKT) off[slot[pos]]++;
          w1_lz_list_build(cd, n);
          use_list = 1;
        }
      }
    } else if (use_list) {
      w1_lz_search(pix, cd, n, w, iters, base, &best_len, &best_dist,
                   near ? &nr : NULL);
    } else {
      w1_lz_search_link(pix, chain, n, w, iters, base, &best_len, &best_dist,
                        near ? &nr : NULL, NULL);
    }
    /* Store, then slide left while the two intervals keep matching. */
    max_base = base;
    tab[base] = best_dist ? W1_LZ_TAB(best_len, best_dist) : 0;
    if (near) {
      near[base] = nr;
      if (nr) { nr_len = W1_LZ_TAB_LEN(nr); nr_dist = W1_LZ_TAB_DIST(nr); }
    }
    for (;;) {
      base--;
      if (best_dist == 0 || base == 0) break;
      if (base < best_dist || pix[base - best_dist] != pix[base]) break;
      /* The nearest match slides along too while its pixels keep
       * matching; where it breaks the newest candidate below `base` is
       * probed afresh (one prefix scan, no walk). */
      if (near) {
        if (nr_dist && base >= nr_dist && pix[base - nr_dist] == pix[base]) {
          if (nr_len < W1_LE_MAX_LEN) nr_len++;
        } else {
          int cand = use_list ? w1_lz_first_below(cd, base) : (int)chain[base];
          int ml = n - base, l;
          int min_pos = base > W1_LE_MAX_DIST ? base - W1_LE_MAX_DIST : 0;
          if (ml > W1_LE_MAX_LEN) ml = W1_LE_MAX_LEN;
          l = cand >= min_pos ? w1_lz_matchlen(pix + cand, pix + base, 0, ml)
                              : 0;
          if (l >= 2) { nr_len = l; nr_dist = base - cand; }
          else nr_dist = 0;
        }
        near[base] = nr_dist ? W1_LZ_TAB(nr_len, nr_dist) : 0;
      }
      /* At the length cap a closer interval of the same length may exist:
       * re-search after a cap's worth of sliding (never for distance 1,
       * nothing beats it). */
      if (best_len == W1_LE_MAX_LEN && best_dist != 1 &&
          base + W1_LE_MAX_LEN < max_base) break;
      if (best_len < W1_LE_MAX_LEN) { best_len++; max_base = base; }
      tab[base] = W1_LZ_TAB(best_len, best_dist);
    }
  }
}

/* Point lz at the match table of its pixel array, filling a slot when no
 * slot holds this array (geometry, budget and content hash). Replayed
 * passes never come here. */
static W1_UNUSED void w1_lz_prepare(w1_lez_t *lz) {
  w1_le_ctx_t *ctx = lz->ctx;
  uint64_t hsh = w1_lz_pixhash(lz->pix, lz->n);
  int s;
  for (s = 0; s < 2; s++)
    if (ctx->tab_valid[s] && ctx->tab_n[s] == lz->n &&
        ctx->tab_w[s] == lz->w && ctx->tab_iters[s] == lz->depth &&
        ctx->tab_hash[s] == hsh)
      break;
  if (s < 2) {
    if (ctx->tab_uses[s] < 0x40000000u) ctx->tab_uses[s]++;
  } else {
    if (!ctx->tab_valid[0]) s = 0;
    else if (!ctx->tab_valid[1]) s = 1;
    else {
      uint64_t w0 = (uint64_t)ctx->tab_uses[0] * (uint64_t)ctx->tab_n[0];
      uint64_t w1 = (uint64_t)ctx->tab_uses[1] * (uint64_t)ctx->tab_n[1];
      s = w0 == w1 ? ctx->tab_lru : (w0 < w1 ? 0 : 1);
    }
    ctx->tab_valid[s] = 1; ctx->tab_n[s] = lz->n; ctx->tab_w[s] = lz->w;
    ctx->tab_iters[s] = lz->depth; ctx->tab_hash[s] = hsh;
    ctx->tab_uses[s] = 0;
    w1_lz_fill(ctx->tab[s], ctx->near[s], lz->pix, lz->n, lz->w, lz->depth,
               &ctx->cand);
  }
  ctx->tab_lru = 1 - s;
  lz->tab = ctx->tab[s];
  lz->near = ctx->near[s];
}

/* Longest length whose prefix symbol is coded (lens != 0), at most len;
 * 0 when none is. */
static W1_UNUSED int w1_lz_codable_len(const uint8_t *lg, int len, int *sym,
                                       int *ebits) {
  int extra;
  for (;;) {
    w1_prefix_inv(len, sym, &extra, ebits);
    if (lg[256 + *sym]) return len;
    if (*sym == 0) return 0;
    len = w1k_lz_bases[*sym] - 1;   /* longest length of the code below */
  }
}

/* Match at pos under the current cost model. The table holds the longest
 * match; the cheapest per pixel is what the stream wants, and a slightly
 * shorter match at a cheap distance (the previous pixel, the pixel above)
 * often costs far fewer bits than a longer one at a far distance. The two
 * neighbour candidates are re-measured here (a prefix scan bounded by the
 * table length) and the candidate with the lowest bits per covered pixel
 * wins. Every length is shortened to the longest coded prefix symbol.
 * *blen = 0 if none. */
static W1_UNUSED void w1_lz_find(w1_lez_t *lz, int pos, int *blen, int *bdist,
                                 int *bcost) {
  uint32_t t = lz->tab[pos];
  const uint32_t *ps = lz->pix + pos;
  int len, dist, sym, ebits, dc, de, deb, dcost, k, tl;
  uint64_t best_q = UINT64_MAX;
  *blen = 0; *bdist = 0; *bcost = W1_LE_INF;
  if (!t) return;
  tl = W1_LZ_TAB_LEN(t);
  for (k = 0; k < 4; k++) {
    int c;
    if (k == 0) { len = tl; dist = W1_LZ_TAB_DIST(t); }
    else {
      dist = k == 1 ? 1 : k == 2 ? lz->w : lz->rep_dist;
      if (dist < 1 || pos < dist || dist == W1_LZ_TAB_DIST(t)) continue;
      if ((k == 2 && dist == 1) ||
          (k == 3 && (dist == 1 || dist == lz->w))) continue;
      len = w1_lz_matchlen(ps - dist, ps, 1, tl);
      if (len < 2) continue;
    }
    dcost = w1_lz_distcost(lz, dist, &dc, &de, &deb);
    if (dcost >= W1_LE_INF) continue;
    len = w1_lz_codable_len(lz->lg, len, &sym, &ebits);
    if (len < 2) continue;
    c = (int)lz->lg[256 + sym] + ebits + dcost;
    /* bits per pixel, scaled; ties keep the longer match */
    {
      uint64_t q = (unsigned)c < 65536u
          ? ((uint32_t)c << 16) / (unsigned)len
          : ((uint64_t)c << 16) / (unsigned)len;
      if (q < best_q || (q == best_q && len > *blen)) {
        best_q = q; *blen = len; *bdist = dist; *bcost = c;
      }
    }
  }
}

static W1_UNUSED void w1_lz_cache_insert(w1_lez_t *lz, uint32_t px) {
  w1_cache_store(lz->cache, lz->cache_bits, px);
}

/* Cost of the literal-or-cache token at pos (cache hit wins ties). */
static W1_UNUSED int w1_lz_litcost(w1_lez_t *lz, int pos, int *use_cache) {
  uint32_t px = lz->pix[pos];
  int g = (int)((px >> 8) & 0xff), r = (int)((px >> 16) & 0xff);
  int b = (int)(px & 0xff), a = (int)((px >> 24) & 0xff);
  int c = W1_LE_INF;
  *use_cache = 0;
  if (lz->lg[g] && lz->lr[r] && lz->lb[b] && lz->la[a])
    c = (int)lz->lg[g] + (int)lz->lr[r] + (int)lz->lb[b] + (int)lz->la[a];
  if (lz->cache) {
    uint32_t h = 0x1e35a7bdu * px;
    int idx = (int)(h >> (32 - lz->cache_bits));
    if (lz->cache[idx] == px && lz->lg[280 + idx] &&
        (int)lz->lg[280 + idx] <= c) {
      c = (int)lz->lg[280 + idx];
      *use_cache = 1;
    }
  }
  return c;
}

static W1_UNUSED void w1_lz_emit_lit(w1_lez_t *lz, int pos, int use_cache) {
  uint32_t px = lz->pix[pos];
  if (use_cache) {
    uint32_t h = 0x1e35a7bdu * px;
    int idx = (int)(h >> (32 - lz->cache_bits));
    if (lz->bw) {
      if (!lz->sg) w1_bw_code(lz->bw, lz->codes_g[280 + idx], lz->eg[280 + idx]);
    } else lz->cg[280 + idx]++;
  } else {
    int g = (int)((px >> 8) & 0xff), r = (int)((px >> 16) & 0xff);
    int b = (int)(px & 0xff), a = (int)((px >> 24) & 0xff);
    if (lz->bw) {
      if (!lz->sg) w1_bw_code(lz->bw, lz->codes_g[g], lz->eg[g]);
      if (!lz->sr) w1_bw_code(lz->bw, lz->codes_r[r], lz->er[r]);
      if (!lz->sb) w1_bw_code(lz->bw, lz->codes_b[b], lz->eb[b]);
      if (!lz->sa) w1_bw_code(lz->bw, lz->codes_a[a], lz->ea[a]);
    } else {
      lz->cg[g]++; lz->cr[r]++; lz->cb[b]++; lz->ca[a]++;
    }
  }
  w1_lz_cache_insert(lz, px);
}

static W1_UNUSED void w1_lz_emit_match(w1_lez_t *lz, int pos, int len,
                                       int dist) {
  int lc, le, leb, dc, de, deb, k;
  w1_prefix_inv(len, &lc, &le, &leb);
  w1_lz_distcost(lz, dist, &dc, &de, &deb);
  if (lz->bw) {
    if (!lz->sg) w1_bw_code(lz->bw, lz->codes_g[256 + lc], lz->eg[256 + lc]);
    w1_bw_put(lz->bw, (uint32_t)le, leb);
    if (!lz->sd) w1_bw_code(lz->bw, lz->codes_d[dc], lz->ed[dc]);
    w1_bw_put(lz->bw, (uint32_t)de, deb);
  } else {
    lz->cg[256 + lc]++; lz->cd[dc]++;
  }
  for (k = 0; k < len; k++) w1_lz_cache_insert(lz, lz->pix[pos + k]);
}

/* One tokenize run: counts (bw == NULL) or emits. lazy = lazy matching. */
static W1_UNUSED void w1_lz_group_sink(w1_lez_t *lz, int pos) {
  int x = pos % lz->w, y = pos / lz->w;
  int sh = lz->group_shift;
  int g = (int)(lz->group_map[(y >> sh) * lz->group_width + (x >> sh)] >> 8);
  if (g < 0 || g >= lz->ngroups) g = 0;
  if (lz->group_realcost && lz->group_lens[g]) {
    /* Real per-group cost model (iterative parse); the emit-only branch
     * below additionally installs codes and single-symbol flags. */
    const uint8_t *l = lz->group_lens[g];
    lz->lg = l; lz->lr = l + W1_LE_OFF_R; lz->lb = l + W1_LE_OFF_B;
    lz->la = l + W1_LE_OFF_A; lz->ld = l + W1_LE_OFF_D;
  }
  if (lz->bw) {
    const uint8_t *l = lz->group_lens[g];
    const int *c = lz->group_codes[g];
    lz->eg = l; lz->er = l + W1_LE_OFF_R; lz->eb = l + W1_LE_OFF_B;
    lz->ea = l + W1_LE_OFF_A; lz->ed = l + W1_LE_OFF_D;
    lz->codes_g = c; lz->codes_r = c + W1_LE_OFF_R;
    lz->codes_b = c + W1_LE_OFF_B; lz->codes_a = c + W1_LE_OFF_A;
    lz->codes_d = c + W1_LE_OFF_D;
    lz->sg = lz->group_single[g][0]; lz->sr = lz->group_single[g][1];
    lz->sb = lz->group_single[g][2]; lz->sa = lz->group_single[g][3];
    lz->sd = lz->group_single[g][4];
  } else {
    int *c = lz->group_counts[g];
    lz->cg = c; lz->cr = c + W1_LE_OFF_R; lz->cb = c + W1_LE_OFF_B;
    lz->ca = c + W1_LE_OFF_A; lz->cd = c + W1_LE_OFF_D;
  }
}

static W1_UNUSED void w1_lz_record(w1_lez_t *lz, uint32_t bk) {
  if (lz->tk_out) lz->tk_out[lz->tk_n++] = bk;
}

/* Replay a recorded token stream through the sinks (counting or emitting).
 * The parse that produced it read the same cost tables, so the emitted
 * symbols are identical, and the cache evolves identically because every
 * pixel is inserted in order whichever token covers it. */
static W1_UNUSED void w1_lz_replay(w1_lez_t *lz) {
  int pos = 0, i;
  if (lz->cache)
    memset(lz->cache, 0, (size_t)lz->cache_size * sizeof(*lz->cache));
  for (i = 0; i < lz->tk_in_n; i++) {
    uint32_t bk = lz->tk_in[i];
    if (lz->group_map) w1_lz_group_sink(lz, pos);
    if (W1_LZ_BK_ISMATCH(bk)) {
      int len = (int)(bk & 0xfffu) + 2;
      w1_lz_emit_match(lz, pos, len, (int)(bk >> 12));
      pos += len;
    } else {
      w1_lz_emit_lit(lz, pos, bk == W1_LZ_BK_CACHE);
      pos++;
    }
  }
}

/* Bounded dynamic-programming parse. Forward pass: dp[x] is the cheapest
 * cost to cover [0,x); at each position relax the literal/cache token and
 * the table's match at its full reach plus every prefix-code base below it
 * (each base is the cheapest length of its code). All relaxations use the
 * same integer cost model (symbol lens + extra bits) the emit path pays,
 * so dp[n] is the optimal cost for this model over the table's matches.
 * back[] then replays the winning token sequence through the normal emit
 * functions. Cache state is parse-independent (every pixel is inserted in
 * order no matter how it is covered), so the forward pass and the replay
 * see the same cache. */
/* DP cost of a length/distance symbol: absent symbols are not forbidden
 * (the greedy parse skips them) but priced at W1_LZ_OPT_ABSENT bits, so a
 * parse that wants one takes it and the support replay adds the code.
 * Forbidding them makes the DP detour around the missing symbol with
 * whatever is cheap under the current model (photo 1024: 61 two-pixel
 * matches at dist w in place of one 4096-pixel match). */
static W1_UNUSED int w1_lz_symcost_soft(const uint8_t *lens, int s,
                                        int extra, int soft) {
  if (lens[s]) return (int)lens[s] + extra;
  return soft ? W1_LZ_OPT_ABSENT + extra : W1_LE_INF;
}
static W1_UNUSED int w1_lz_distcost_soft(const w1_lez_t *lz, int dist) {
  int c1 = W1_LE_INF, c2 = W1_LE_INF, s, e, b;
  int plane = w1_dist_plane(dist, lz->w);
  if (plane <= 120) {
    w1_prefix_inv(plane, &s, &e, &b);
    if (s < 40) c1 = w1_lz_symcost_soft(lz->ld, s, b, lz->soft);
  }
  w1_prefix_inv(dist + 120, &s, &e, &b);
  if (s < 40) c2 = w1_lz_symcost_soft(lz->ld, s, b, lz->soft);
  return c2 < c1 ? c2 : c1;
}

/* Relax dp over this position's match candidates (cl/cd/cc = length,
 * distance and distance cost, nc entries): each candidate's full length,
 * then every prefix-code base below it (a shorter token at the same
 * distance may land on a cheaper continuation). bc[b] = coded length cost
 * of base b, INF when that length symbol is absent from the current
 * tables.
 *
 * All candidates relax the same scattered dp[] slots, and a relax is a
 * min, so the base loop runs once over those slots with the cheapest
 * distance that can reach each base rather than once per candidate. */
static W1_UNUSED void w1_lz_opt_relax(uint32_t *dp, uint32_t *back,
                                      uint64_t base, int pos, const int *cl,
                                      const int *cd, const int *cc, int nc,
                                      const int *bc, const uint8_t *lg,
                                      int soft) {
  int sym, extra, ebits, c, b, j, maxlen = 0;
  uint64_t v;
  for (j = 0; j < nc; j++) {
    if (cl[j] > maxlen) maxlen = cl[j];
    if (cl[j] < 2) continue;
    w1_prefix_inv(cl[j], &sym, &extra, &ebits);
    c = w1_lz_symcost_soft(lg, 256 + sym, ebits, soft);
    if (c >= W1_LE_INF) continue;
    v = base + (uint64_t)c + (uint64_t)cc[j];
    if (v < dp[pos + cl[j]]) {
      dp[pos + cl[j]] = (uint32_t)(v < W1_LE_INF ? v : W1_LE_INF - 1);
      back[pos + cl[j]] = W1_LZ_BK_MATCH(cl[j], cd[j]);
    }
  }
  for (b = 1; b < 24; b++) {
    int bl = w1k_lz_bases[b], bd = 0, bcost = W1_LE_INF;
    if (bl > maxlen) break;
    if (bl < 2) continue;
    if (bc[b] >= W1_LE_INF) continue;
    /* Cheapest distance among the candidates long enough to reach bl
     * (ties keep the earliest candidate, as a per-candidate pass would). */
    for (j = 0; j < nc; j++)
      if (cl[j] >= bl && cc[j] < bcost) { bcost = cc[j]; bd = cd[j]; }
    if (bcost >= W1_LE_INF) continue;
    v = base + (uint64_t)bc[b] + (uint64_t)bcost;
    if (v < dp[pos + bl]) {
      dp[pos + bl] = (uint32_t)(v < W1_LE_INF ? v : W1_LE_INF - 1);
      back[pos + bl] = W1_LZ_BK_MATCH(bl, bd);
    }
  }
}

static W1_UNUSED void w1_lz_run_opt(w1_lez_t *lz) {
  int pos, i, n = lz->n;
  uint32_t *dp = lz->dp, *back = lz->back;
  int bc[24], skip_end = 0, md[W1_LZ_OPT_NC], mdc[W1_LZ_OPT_NC];
  int nb_end[W1_LZ_OPT_NC], nb_d[W1_LZ_OPT_NC];
  const uint8_t *mld[W1_LZ_OPT_NC];
  uint64_t skip_thr = 0;
  if (lz->cache)
    memset(lz->cache, 0, (size_t)lz->cache_size * sizeof(*lz->cache));
  for (i = 0; i <= n; i++) dp[i] = W1_LE_INF;
  dp[0] = 0;
  for (i = 0; i < W1_LZ_OPT_NC; i++) {
    md[i] = mdc[i] = nb_end[i] = 0; mld[i] = NULL;
    nb_d[i] = i == 1 ? 1 : i == 2 ? lz->w : 0;
  }
  for (i = 0; i < 24; i++) {
    int sym, extra, ebits;
    w1_prefix_inv(w1k_lz_bases[i], &sym, &extra, &ebits);
    bc[i] = w1_lz_symcost_soft(lz->lg, 256 + sym, ebits, lz->soft);
  }
  for (pos = 0; pos < n; pos++) {
    uint64_t base = dp[pos];
    int lc, use_cache, len, dist, dcost, k, j, nc = 0, c0len = 0;
    int cl[W1_LZ_OPT_NC], cd[W1_LZ_OPT_NC], cc[W1_LZ_OPT_NC];
    uint32_t t;
    /* Inside a long match every position holds the same match slid along
     * (w1_lz_fill), so a path entering it no cheaper than the match's own
     * start can only lose to the match: skip those positions. Keeps the
     * DP near O(tokens) on matchy data. */
    if (pos < skip_end && base >= skip_thr) {
      w1_lz_cache_insert(lz, lz->pix[pos]);
      continue;
    }
    if (lz->group_map) w1_lz_group_sink(lz, pos);
    lc = w1_lz_litcost(lz, pos, &use_cache);
    /* A literal whose symbol is not in the current tables still has to be
     * representable for the DP to stay complete; cost it high but finite
     * and let the support replay add the symbol (same guarantee the
     * support loop gives the greedy parse). */
    if (lc >= W1_LE_INF) lc = W1_LE_INF >> 2;
    if (base + (uint64_t)lc < dp[pos + 1]) {
      uint64_t v = base + (uint64_t)lc;
      dp[pos + 1] = (uint32_t)(v < W1_LE_INF ? v : W1_LE_INF - 1);
      back[pos + 1] = use_cache ? W1_LZ_BK_CACHE : 0u;
    }
    t = lz->tab[pos];
    if (!t) { w1_lz_cache_insert(lz, lz->pix[pos]); continue; }
    len = W1_LZ_TAB_LEN(t);
    dist = W1_LZ_TAB_DIST(t);
    /* Repeat distances: the table's last two distinct distances stay live
     * as candidates, so the DP can restart a cheap distance the
     * longest-match rule abandoned (text rows, tiled art). A new distance
     * invalidates that slot's slid length. */
    if (dist != nb_d[3]) {
      nb_d[4] = nb_d[3]; nb_end[4] = nb_end[3];
      nb_d[3] = dist; nb_end[3] = 0;
    }
    /* Table match, then the previous pixel, the pixel above and the repeat
     * distances (cheap distance codes the table's longest-match rule may
     * have skipped). A neighbour match measured once is valid for every
     * position inside it (one pixel shorter each step), so each pixel is
     * compared at most once per candidate over the whole pass. */
    for (k = 0; k < W1_LZ_OPT_NC; k++) {
      if (k == 5) {
        uint32_t t2 = lz->near ? lz->near[pos] : 0;
        if (!t2) continue;
        len = W1_LZ_TAB_LEN(t2); dist = W1_LZ_TAB_DIST(t2);
        if (dist == W1_LZ_TAB_DIST(t)) continue;
        for (j = 1; j < 5; j++) if (nb_d[j] == dist) break;
        if (j < 5) continue;
      } else if (k) {
        int d = nb_d[k];
        if (d == dist || pos < d || !d) continue;
        for (j = 1; j < k; j++) if (nb_d[j] == d) break;
        if (j < k) continue;
        if (pos < nb_end[k]) {
          len = nb_end[k] - pos;
        } else {
          if (lz->pix[pos - d] != lz->pix[pos]) continue;
          len = w1_lz_matchlen(lz->pix + pos - d, lz->pix + pos, 1,
                               n - pos < W1_LE_MAX_LEN ? n - pos
                                                       : W1_LE_MAX_LEN);
          nb_end[k] = pos + len;
        }
        if (len > W1_LE_MAX_LEN) len = W1_LE_MAX_LEN;
        dist = d;
      }
      if (len < 2) continue;
      /* Consecutive positions carry the same slid distance: memo the
       * distance cost (a division per lookup) per candidate slot. */
      if (dist != md[k] || lz->ld != mld[k]) {
        md[k] = dist; mld[k] = lz->ld;
        mdc[k] = w1_lz_distcost_soft(lz, dist);
      }
      dcost = mdc[k];
      if (dcost >= W1_LE_INF) continue;
      if (!k) c0len = len;
      cl[nc] = len; cd[nc] = dist; cc[nc] = dcost; nc++;
    }
    if (nc) {
      w1_lz_opt_relax(dp, back, base, pos, cl, cd, cc, nc, bc, lz->lg,
                      lz->soft);
      /* Only the table match (candidate 0) states the skip window. */
      if (c0len >= 32 && dp[pos + c0len] < W1_LE_INF &&
          pos + c0len > skip_end) {
        skip_end = pos + c0len; skip_thr = dp[pos + c0len];
      }
    }
    w1_lz_cache_insert(lz, lz->pix[pos]);
  }
  /* back[x] holds the winning token *ending* at x, so walk back from n to
   * peel the path into dp[] (reverse token order), then replay it forward.
   * dp[] is dead after the forward pass, so it doubles as the token list. */
  {
    int nt = 0;
    for (pos = n; pos > 0;) {
      uint32_t bk = back[pos];
      dp[nt++] = bk;
      if (W1_LZ_BK_ISMATCH(bk)) pos -= (int)(bk & 0xfffu) + 2;
      else pos--;
    }
    for (pos = 0, i = nt - 1; i >= 0; i--) {
      uint32_t bk = dp[i];
      if (lz->group_map) w1_lz_group_sink(lz, pos);
      w1_lz_record(lz, bk);
      if (W1_LZ_BK_ISMATCH(bk)) {
        int len = (int)(bk & 0xfffu) + 2;
        w1_lz_emit_match(lz, pos, len, (int)(bk >> 12));
        pos += len;
      } else {
        w1_lz_emit_lit(lz, pos, bk == W1_LZ_BK_CACHE);
        pos++;
      }
    }
  }
}

/* One tokenize run: counts (bw == NULL) or emits. lazy = lazy matching.
 * With tk_in set the recorded tokens are replayed instead. */
static W1_UNUSED void w1_lz_run(w1_lez_t *lz, int lazy) {
  int pos = 0;
  if (lz->tk_in) {
    w1_lz_replay(lz);
    return;
  }
  w1_lz_prepare(lz);
  lz->tk_n = 0;
  if (lz->opt && lz->dp && lz->back) {
    w1_lz_run_opt(lz);
    return;
  }
  if (lz->cache)
    memset(lz->cache, 0, (size_t)lz->cache_size * sizeof(*lz->cache));
  while (pos < lz->n) {
    int len1, dist1, cost1, lit_c, use_cache, take = 0;
    if (lz->group_map) w1_lz_group_sink(lz, pos);
    w1_lz_find(lz, pos, &len1, &dist1, &cost1);
    lit_c = w1_lz_litcost(lz, pos, &use_cache);
    /* A match covers len1 pixels: compare against that many literals
     * (capped at 16; costs above ~58/literal can never win anyway). */
    if (cost1 < W1_LE_INF && len1 >= 2 &&
        (uint64_t)cost1 < (uint64_t)lit_c * (uint64_t)(len1 < 16 ? len1 : 16)) {
      /* Long matches: skip the lazy peek (rarely changes the call). */
      if (!lazy || pos + 1 >= lz->n || len1 >= 64) {
        take = 1;
      } else {
        int len2, dist2, cost2;
        w1_lz_find(lz, pos + 1, &len2, &dist2, &cost2);
        take = (cost2 >= W1_LE_INF || cost1 <= lit_c + cost2);
      }
    }
    if (take) {
      w1_lz_record(lz, W1_LZ_BK_MATCH(len1, dist1));
      w1_lz_emit_match(lz, pos, len1, dist1);
      lz->rep_dist = dist1;
      pos += len1;
    } else {
      w1_lz_record(lz, use_cache ? W1_LZ_BK_CACHE : 0u);
      w1_lz_emit_lit(lz, pos, use_cache);
      pos++;
    }
  }
}

static const uint16_t w1_le_depths[10] = {
  8, 32, 64, 128, 256, 384, 384, 1024, 1536, 2048
};
/* Palette index streams are packed (2^wb pixels per index byte), so a given
 * match-search depth costs ~2^wb less than the same depth on ARGB. Without a
 * floor the low levels lose long matches on the index stream: text 512 L0
 * (2 colours -> 8 pixels/byte) coded 1470 B where depth-640 L6 codes 666 B.
 * The floor is expressed as a level, not just a depth, so the estimate that
 * builds the Huffman tables and the final emit use the same search (at L0
 * there is no H1 support replay to paper over a mismatch). */
#ifndef W1_LE_PAL_LEVEL
#define W1_LE_PAL_LEVEL 5
#endif
static W1_UNUSED int w1_le_idx_level(int level) {
  return level < W1_LE_PAL_LEVEL ? W1_LE_PAL_LEVEL : level;
}
static const uint8_t w1_le_cachebits[10] = {
  0, 4, 5, 6, 6, 7, 7, 8, 8, 9
};

static W1_UNUSED void w1_le_zero_counts(w1_le_ctx_t *ctx) {
  int i;
  for (i = 0; i < W1_LE_NC; i++) ctx->counts[i] = 0;
}

/* Guarantee pass B/C can always emit a literal: every channel value
 * PRESENT in the image gets count >= 1 (pass B only emits literals for
 * present values, so it can never get stuck). Length/distance codes are
 * NOT forced: unusable matches are skipped. present = 4x256 bytes. */
static W1_UNUSED void w1_le_scan_present(const uint32_t *pix, int n,
                                         uint8_t *present) {
  int i;
  for (i = 0; i < 1024; i++) present[i] = 0;
  for (i = 0; i < n; i++) {
    uint32_t px = pix[i];
    present[(px >> 8) & 0xff] = 1;
    present[256 + ((px >> 16) & 0xff)] = 1;
    present[512 + (px & 0xff)] = 1;
    present[768 + ((px >> 24) & 0xff)] = 1;
  }
}
static W1_UNUSED void w1_le_force_present(w1_le_ctx_t *ctx,
                                          const uint8_t *present) {
  int i;
  for (i = 0; i < 256; i++) {
    if (present[i] && ctx->counts[i] < 1) ctx->counts[i] = 1;
    if (present[256 + i] && ctx->counts[W1_LE_OFF_R + i] < 1)
      ctx->counts[W1_LE_OFF_R + i] = 1;
    if (present[512 + i] && ctx->counts[W1_LE_OFF_B + i] < 1)
      ctx->counts[W1_LE_OFF_B + i] = 1;
    if (present[768 + i] && ctx->counts[W1_LE_OFF_A + i] < 1)
      ctx->counts[W1_LE_OFF_A + i] = 1;
  }
}

static W1_UNUSED void w1_le_build_tables(w1_le_ctx_t *ctx, int cache_size) {
  int ga = 280 + cache_size;
  uint8_t best[W1_LE_NG];
  w1_huff_lengths_hdr(ctx->counts, ga, ctx->lens, ctx->htmp, ctx->seq, best);
  w1_huff_lengths_hdr(ctx->counts + W1_LE_OFF_R, 256, ctx->lens + W1_LE_OFF_R,
                      ctx->htmp, ctx->seq, best);
  w1_huff_lengths_hdr(ctx->counts + W1_LE_OFF_B, 256, ctx->lens + W1_LE_OFF_B,
                      ctx->htmp, ctx->seq, best);
  w1_huff_lengths_hdr(ctx->counts + W1_LE_OFF_A, 256, ctx->lens + W1_LE_OFF_A,
                      ctx->htmp, ctx->seq, best);
  w1_huff_lengths_hdr(ctx->counts + W1_LE_OFF_D, 40, ctx->lens + W1_LE_OFF_D,
                      ctx->htmp, ctx->seq, best);
  w1_huff_codes(ctx->lens, ga, ctx->codes);
  w1_huff_codes(ctx->lens + W1_LE_OFF_R, 256, ctx->codes + W1_LE_OFF_R);
  w1_huff_codes(ctx->lens + W1_LE_OFF_B, 256, ctx->codes + W1_LE_OFF_B);
  w1_huff_codes(ctx->lens + W1_LE_OFF_A, 256, ctx->codes + W1_LE_OFF_A);
  w1_huff_codes(ctx->lens + W1_LE_OFF_D, 40, ctx->codes + W1_LE_OFF_D);
}

/* Codes from lens alone (a replayed stream restores its recorded lens). */
static W1_UNUSED void w1_le_build_codes(w1_le_ctx_t *ctx, int cache_size) {
  w1_huff_codes(ctx->lens, 280 + cache_size, ctx->codes);
  w1_huff_codes(ctx->lens + W1_LE_OFF_R, 256, ctx->codes + W1_LE_OFF_R);
  w1_huff_codes(ctx->lens + W1_LE_OFF_B, 256, ctx->codes + W1_LE_OFF_B);
  w1_huff_codes(ctx->lens + W1_LE_OFF_A, 256, ctx->codes + W1_LE_OFF_A);
  w1_huff_codes(ctx->lens + W1_LE_OFF_D, 40, ctx->codes + W1_LE_OFF_D);
}

/* Score a counts vector with real Huffman lens (mirror of
 * w1_le_build_tables). Returns sum counts*len over the 5 groups:
 * entropy-aware bits for gate comparisons. Clobbers lens/htmp. */
static W1_UNUSED uint64_t w1_le_score(w1_le_ctx_t *ctx, const int *counts,
                                      int cache_size) {
  int ga = 280 + cache_size, i;
  uint64_t t = 0;
  w1_huff_lengths(counts, ga, 15, ctx->lens, ctx->htmp);
  w1_huff_lengths(counts + W1_LE_OFF_R, 256, 15,
                  ctx->lens + W1_LE_OFF_R, ctx->htmp);
  w1_huff_lengths(counts + W1_LE_OFF_B, 256, 15,
                  ctx->lens + W1_LE_OFF_B, ctx->htmp);
  w1_huff_lengths(counts + W1_LE_OFF_A, 256, 15,
                  ctx->lens + W1_LE_OFF_A, ctx->htmp);
  w1_huff_lengths(counts + W1_LE_OFF_D, 40, 15,
                  ctx->lens + W1_LE_OFF_D, ctx->htmp);
  for (i = 0; i < ga; i++)
    t += (uint64_t)(unsigned)counts[i] * ctx->lens[i];
  {
    /* R/B/A planes share the 256-bin layout; D (40 bins) is separate. */
    static const int offs[3] = {W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A};
    int p;
    for (p = 0; p < 3; p++) {
      int o = offs[p], j;
      for (j = 0; j < 256; j++)
        t += (uint64_t)(unsigned)counts[o + j] * ctx->lens[o + j];
    }
  }
  for (i = 0; i < 40; i++)
    t += (uint64_t)(unsigned)counts[W1_LE_OFF_D + i] *
         ctx->lens[W1_LE_OFF_D + i];
  /* Length/dist extra bits ride with the symbols (w1_prefix_inv: ebits
   * are a function of the symbol alone). */
  for (i = 0; i < 24; i++)
    t += (uint64_t)(unsigned)counts[256 + i] *
         (uint64_t)(i < 4 ? 0 : (i - 2) >> 1);
  for (i = 0; i < 40; i++)
    t += (uint64_t)(unsigned)counts[W1_LE_OFF_D + i] *
         (uint64_t)(i < 4 ? 0 : (i - 2) >> 1);
  return t;
}

/* Shared LZ scratch setup for the estimators (estimate + est2). */
static W1_UNUSED void w1_le_lz_setup(w1_le_ctx_t *ctx, w1_lez_t *lz,
                                     const uint32_t *pix, int w, int h,
                                     int level, int cache_bits) {
  memset(lz, 0, sizeof(*lz));
  lz->pix = pix; lz->w = w; lz->n = w * h;
  lz->ctx = ctx;
  lz->depth = (int)w1_le_depths[level];
  lz->cache_bits = cache_bits;
  lz->cache_size = cache_bits ? (1 << cache_bits) : 0;
  lz->cache = cache_bits ? ctx->cache : NULL;
}

static W1_UNUSED uint64_t w1_le_estimate(w1_le_ctx_t *ctx,
                                         const uint32_t *pix, int w, int h,
                                         int level, int cache_bits,
                                         const uint8_t *flat, int *counts) {
  int k;
  w1_lez_t lz;
  w1_le_lz_setup(ctx, &lz, pix, w, h, level, cache_bits);
  lz.lg = flat; lz.lr = flat + W1_LE_OFF_R; lz.lb = flat + W1_LE_OFF_B;
  lz.la = flat + W1_LE_OFF_A; lz.ld = flat + W1_LE_OFF_D;
  for (k = 0; k < W1_LE_NC; k++) counts[k] = 0;
  lz.cg = counts; lz.cr = counts + W1_LE_OFF_R;
  lz.cb = counts + W1_LE_OFF_B; lz.ca = counts + W1_LE_OFF_A;
  lz.cd = counts + W1_LE_OFF_D;
  w1_lz_run(&lz, level >= 1);
  /* Flat-cost tokens are entropy-blind; score them with real lens. */
  return w1_le_score(ctx, counts,
                     cache_bits ? (1 << cache_bits) : 0);
}

/* Exact bit cost of transmitting the five Huffman tables described by
 * ctx->lens (mirrors w1_le_finish's w1_huff_emit calls). This is the
 * table-transmission term w1_le_score omits. */
static W1_UNUSED uint64_t w1_le_tablebits(w1_le_ctx_t *ctx, int ga,
                                         uint8_t *tmp, size_t cap) {
  w1_bw_t bw;
  w1_bw_init(&bw, tmp, cap);
  w1_huff_emit(&bw, ctx->lens, ga, ctx->seq);
  w1_huff_emit(&bw, ctx->lens + W1_LE_OFF_R, 256, ctx->seq);
  w1_huff_emit(&bw, ctx->lens + W1_LE_OFF_B, 256, ctx->seq);
  w1_huff_emit(&bw, ctx->lens + W1_LE_OFF_A, 256, ctx->seq);
  w1_huff_emit(&bw, ctx->lens + W1_LE_OFF_D, 40, ctx->seq);
  return (uint64_t)(size_t)(bw.p - tmp) * 8 + (uint64_t)bw.nbits;
}

/* LZ-aware estimate of the main stream. w1_le_estimate tokenizes with
 * flat costs, so its counts (and score) can badly misjudge the final
 * real-lens tokenization. Here the tokenizer is re-run against the
 * current lens a few times (flat -> H1 -> H2 -> ...) toward the fixed
 * point the final encode reaches, and the exact table-transmission bits
 * are added. c1/c2 are caller scratch (c1 = pass-A counts, c2 = pass-B);
 * ctx->lens ends up holding the final lens. w1_le_est2_from is the
 * iteration alone, for a caller whose own pass A just left its H1 lens in
 * ctx->lens. */
static W1_UNUSED uint64_t w1_le_est2_from(w1_le_ctx_t *ctx,
                                           const uint32_t *pix, int w, int h,
                                           int level, int cache_bits,
                                           int *c2) {
  int cache_size = cache_bits ? (1 << cache_bits) : 0, k, it;
  w1_lez_t lz;
  uint64_t s = 0;
  uint8_t lens_prev[W1_LE_NC];
  w1_le_lz_setup(ctx, &lz, pix, w, h, level, cache_bits);
  for (it = 0; it < 3; it++) {
    /* Fixed point: the tokenize+score round is a pure function of
     * (pix, depth, lens). If the lens rebuilt from this iteration's counts
     * equal the lens it read, the next round would reproduce identical
     * counts, lens and score — so stop early. The returned s is exactly
     * the value the full loop would produce (byte-neutral, VTune-verified
     * as a top redundant pass). */
    memcpy(lens_prev, ctx->lens, (size_t)W1_LE_NC);
    lz.lg = ctx->lens; lz.lr = ctx->lens + W1_LE_OFF_R;
    lz.lb = ctx->lens + W1_LE_OFF_B; lz.la = ctx->lens + W1_LE_OFF_A;
    lz.ld = ctx->lens + W1_LE_OFF_D;
    for (k = 0; k < W1_LE_NC; k++) c2[k] = 0;
    lz.cg = c2; lz.cr = c2 + W1_LE_OFF_R; lz.cb = c2 + W1_LE_OFF_B;
    lz.ca = c2 + W1_LE_OFF_A; lz.cd = c2 + W1_LE_OFF_D;
    w1_lz_run(&lz, level >= 1);
    s = w1_le_score(ctx, c2, cache_size);
    if (!memcmp(lens_prev, ctx->lens, (size_t)W1_LE_NC)) break;
  }
  {
    uint8_t tbuf[8192];
    s += w1_le_tablebits(ctx, 280 + cache_size, tbuf, sizeof tbuf);
  }
  return s;
}
static W1_UNUSED uint64_t w1_le_est2(w1_le_ctx_t *ctx, const uint32_t *pix,
                                      int w, int h, int level, int cache_bits,
                                      const uint8_t *flat, int *c1, int *c2) {
  w1_le_estimate(ctx, pix, w, h, level, cache_bits, flat, c1);
  return w1_le_est2_from(ctx, pix, w, h, level, cache_bits, c2);
}

/* Tables + tokens from pass-A counts in ctx->counts (present[] scanned
 * from the final image; H1 covers it so pass B/C never get stuck). With a
 * memo slot the final parse is recorded and the emit replays it; a slot
 * that already holds this stream's parse (a sizing trial re-encoded for
 * real) skips straight to the emit under the recorded tables. */
static W1_UNUSED void w1_le_finish(w1_le_ctx_t *ctx, const uint32_t *pix,
                                   int w, int h, int level, int depth,
                                   int cache_bits, const uint8_t *flat,
                                   const uint8_t *present, w1_bw_t *bw,
                                   w1_le_tk_t *tk) {
  int n = w * h, cache_size = cache_bits ? (1 << cache_bits) : 0;
  int ga = 280 + cache_size, replay;
  w1_lez_t lz;
  memset(&lz, 0, sizeof(lz));
  lz.pix = pix; lz.w = w; lz.n = n;
  lz.ctx = ctx;
  lz.depth = depth;
  lz.cache_bits = cache_bits; lz.cache_size = cache_size;
  lz.cache = cache_bits ? ctx->cache : NULL;
  lz.cg = ctx->counts; lz.cr = ctx->counts + W1_LE_OFF_R;
  lz.cb = ctx->counts + W1_LE_OFF_B; lz.ca = ctx->counts + W1_LE_OFF_A;
  lz.cd = ctx->counts + W1_LE_OFF_D;
  /* Optimal parse for every table-driven pass (pass B, support replays and
   * the final emit all re-derive the same DP from the same lens). opt_cap is
   * non-zero when the public level allows it (W1_LZ_OPT_LEVEL) and the image
   * is within W1_LZ_OPT_MAXN; every other call stays greedy. */
  lz.opt = (level >= 1 && ctx->dp && n <= ctx->opt_cap);
  lz.dp = ctx->dp; lz.back = ctx->back;
  replay = w1_le_tk_match(tk, W1_TK_FINISH, pix, w, h, level, cache_bits,
                          depth);
  if (replay) {
    memcpy(ctx->lens, tk->lens, (size_t)W1_LE_NC);
    w1_le_build_codes(ctx, cache_size);
    lz.lg = ctx->lens; lz.lr = ctx->lens + W1_LE_OFF_R;
    lz.lb = ctx->lens + W1_LE_OFF_B; lz.la = ctx->lens + W1_LE_OFF_A;
    lz.ld = ctx->lens + W1_LE_OFF_D;
  } else {
    int recorded = 0;
    w1_le_force_present(ctx, present);
    w1_le_build_tables(ctx, cache_size);
    if (level >= 1) {
      /* Pass B under H1 -> emitted counts. */
      lz.lg = ctx->lens; lz.lr = ctx->lens + W1_LE_OFF_R;
      lz.lb = ctx->lens + W1_LE_OFF_B; lz.la = ctx->lens + W1_LE_OFF_A;
      lz.ld = ctx->lens + W1_LE_OFF_D;
      w1_le_zero_counts(ctx);
      lz.soft = 1;
      w1_lz_run(&lz, 1);
      /* Build the output tables from only the symbols the parse actually
       * emitted. Replay the emit under those tables and add any symbol the
       * replay introduces; support grows monotonically so the loop
       * terminates (usually in one iteration) and the final tables cover
       * the exact final parse. Forced-present slots that only ever rode
       * along inside a match/cache token otherwise keep whole alphabets
       * off the compact simple-code form (alpha checkerboard 76 -> 64
       * bytes). The pass that ends the loop is the final parse: its tokens
       * are recorded for the emit. */
      w1_le_build_tables(ctx, cache_size);
      {
        int sup, k2, missing;
        for (sup = 0; sup < 8; sup++) {
          lz.lg = ctx->lens; lz.lr = ctx->lens + W1_LE_OFF_R;
          lz.lb = ctx->lens + W1_LE_OFF_B; lz.la = ctx->lens + W1_LE_OFF_A;
          lz.ld = ctx->lens + W1_LE_OFF_D;
          for (k2 = 0; k2 < W1_LE_NC; k2++) ctx->counts3[k2] = 0;
          lz.cg = ctx->counts3; lz.cr = ctx->counts3 + W1_LE_OFF_R;
          lz.cb = ctx->counts3 + W1_LE_OFF_B; lz.ca = ctx->counts3 + W1_LE_OFF_A;
          lz.cd = ctx->counts3 + W1_LE_OFF_D;
          lz.tk_out = tk ? tk->tok : NULL;
          lz.soft = (sup < 7);   /* last round: only coded symbols */
          w1_lz_run(&lz, 1);
          lz.tk_out = NULL;
          missing = 0;
          for (k2 = 0; k2 < W1_LE_NC; k2++)
            if (ctx->counts3[k2] > 0 && ctx->lens[k2] == 0) missing = 1;
          if (!missing) { recorded = (tk != NULL); break; }
          for (k2 = 0; k2 < W1_LE_NC; k2++)
            if (ctx->counts3[k2] > ctx->counts[k2])
              ctx->counts[k2] = ctx->counts3[k2];
          w1_le_build_tables(ctx, cache_size);
        }
        lz.cg = NULL; lz.cr = NULL; lz.cb = NULL; lz.ca = NULL; lz.cd = NULL;
      }
    } else {
      /* L0: keep flat costs for the emit run (reproduces pass A). */
      lz.lg = flat; lz.lr = flat + W1_LE_OFF_R; lz.lb = flat + W1_LE_OFF_B;
      lz.la = flat + W1_LE_OFF_A; lz.ld = flat + W1_LE_OFF_D;
    }
    if (tk) {
      if (recorded)
        w1_le_tk_set(tk, W1_TK_FINISH, pix, w, h, level, cache_bits, depth,
                     lz.tk_n, ctx->lens, 1);
      else tk->valid = 0;
    }
  }
  w1_huff_emit(bw, ctx->lens, ga, ctx->seq);
  w1_huff_emit(bw, ctx->lens + W1_LE_OFF_R, 256, ctx->seq);
  w1_huff_emit(bw, ctx->lens + W1_LE_OFF_B, 256, ctx->seq);
  w1_huff_emit(bw, ctx->lens + W1_LE_OFF_A, 256, ctx->seq);
  w1_huff_emit(bw, ctx->lens + W1_LE_OFF_D, 40, ctx->seq);
  {
    int k, ng = 0, nr = 0, nb = 0, na = 0, nd = 0;
    for (k = 0; k < ga; k++) if (ctx->lens[k]) ng++;
    for (k = 0; k < 256; k++) if (ctx->lens[W1_LE_OFF_R + k]) nr++;
    for (k = 0; k < 256; k++) if (ctx->lens[W1_LE_OFF_B + k]) nb++;
    for (k = 0; k < 256; k++) if (ctx->lens[W1_LE_OFF_A + k]) na++;
    for (k = 0; k < 40; k++) if (ctx->lens[W1_LE_OFF_D + k]) nd++;
    lz.sg = (ng == 1); lz.sr = (nr == 1); lz.sb = (nb == 1);
    lz.sa = (na == 1); lz.sd = (nd == 1);
  }
  lz.bw = bw;
  lz.eg = ctx->lens; lz.er = ctx->lens + W1_LE_OFF_R;
  lz.eb = ctx->lens + W1_LE_OFF_B; lz.ea = ctx->lens + W1_LE_OFF_A;
  lz.ed = ctx->lens + W1_LE_OFF_D;
  lz.codes_g = ctx->codes; lz.codes_r = ctx->codes + W1_LE_OFF_R;
  lz.codes_b = ctx->codes + W1_LE_OFF_B; lz.codes_a = ctx->codes + W1_LE_OFF_A;
  lz.codes_d = ctx->codes + W1_LE_OFF_D;
  lz.cg = NULL; lz.cr = NULL; lz.cb = NULL; lz.ca = NULL; lz.cd = NULL;
  if (tk && tk->valid) { lz.tk_in = tk->tok; lz.tk_in_n = tk->ntok; }
  w1_lz_run(&lz, level >= 1);
}

/* Encode tables + tokens for one image stream (sub-images: fresh pass A).
 * The cache bit is written by the caller. tk: optional memo slot (see
 * w1_le_finish); a slot holding this stream's parse skips pass A too. */
static W1_UNUSED void w1_le_encode_tokens(w1_le_ctx_t *ctx,
                                          const uint32_t *pix, int w, int h,
                                          int level, int cache_bits,
                                          const uint8_t *flat, w1_bw_t *bw,
                                          w1_le_tk_t *tk) {
  uint8_t present[1024];
  int depth = (int)w1_le_depths[level];
  if (!w1_le_tk_match(tk, W1_TK_FINISH, pix, w, h, level, cache_bits,
                      depth)) {
    w1_le_scan_present(pix, w * h, present);
    w1_le_estimate(ctx, pix, w, h, level, cache_bits, flat, ctx->counts);
  }
  w1_le_finish(ctx, pix, w, h, level, depth, cache_bits, flat, present, bw,
               tk);
}

static W1_UNUSED int w1_le_uniform(const uint32_t *pix, int n);

static W1_UNUSED int w1_le_literal_group(int x, int y, int w, int h, int side) {
  if (side == 0) return y < 4;
  if (side == 1) return y >= ((h - 1) & ~3);
  if (side == 2) return x < 4;
  if (side == 3) return x >= ((w - 1) & ~3);
  return 0;
}

static W1_UNUSED void w1_le_literal_stream(w1_le_ctx_t *ctx,
                                            const uint32_t *pix, int w, int h,
                                            const uint8_t *flat, uint32_t *meta,
                                            int side, w1_bw_t *bw) {
  static const int offsets[4] = {0, W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A};
  static const int shifts[4] = {8, 16, 0, 24};
  int groups = side < 0 ? 1 : 2, single[2][4], g, c, i, x, y;
  uint64_t payload_bits = 0;
  /* A sub-4x4 image has a single entropy pixel. The decoder collapses a
   * multi-group map when ngroups > the entropy-pixel count, which would make
   * it read the wrong table, so keep one group there (and classify all pixels
   * as group 0 so the per-pixel table selection stays consistent). */
  if (groups == 2 && ((w + 3) >> 2) * ((h + 3) >> 2) < 2) {
    groups = 1;
    side = -1;
  }
  w1_bw_put(bw, 0, 1);
  w1_bw_put(bw, groups == 2, 1);
  if (groups == 2) {
    int tw = (w + 3) >> 2, th = (h + 3) >> 2;
    w1_bw_put(bw, 0, 3);
    for (y = 0; y < th; y++)
      for (x = 0; x < tw; x++)
        meta[y * tw + x] = (uint32_t)w1_le_literal_group(x * 4, y * 4, w, h, side) << 8;
    w1_bw_put(bw, 0, 1);
    w1_le_encode_tokens(ctx, meta, tw, th, 1, 0, flat, bw, NULL);
  }
  for (g = 0; g < groups; g++) {
    w1_le_zero_counts(ctx);
    if (groups == 1 && w1_le_uniform(pix, w * h)) {
      for (c = 0; c < 4; c++)
        ctx->counts[offsets[c] + ((pix[0] >> shifts[c]) & 255)] = w * h;
    } else
    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++) {
        uint32_t p = pix[y * w + x];
        if (w1_le_literal_group(x, y, w, h, side) != g) continue;
        for (c = 0; c < 4; c++) ctx->counts[offsets[c] + ((p >> shifts[c]) & 255)]++;
      }
    }
    w1_le_build_tables(ctx, 0);
    for (c = 0; c < 4; c++) {
      int nz = 0;
      for (i = 0; i < 256; i++) nz += ctx->counts[offsets[c] + i] != 0;
      single[g][c] = nz <= 1;
      if (!bw->p && nz > 1)
        for (i = 0; i < 256; i++)
          payload_bits += (uint64_t)(unsigned)ctx->counts[offsets[c] + i] *
                          ctx->lens[offsets[c] + i];
      w1_huff_emit(bw, ctx->lens + offsets[c], c ? 256 : 280, ctx->seq);
    }
    w1_huff_emit(bw, ctx->lens + W1_LE_OFF_D, 40, ctx->seq);
    if (bw->p && g == 0 && groups == 2) {
      memcpy(ctx->counts3, ctx->lens, W1_LE_NC);
      memcpy(ctx->counts4, ctx->codes, W1_LE_NC * sizeof(int));
    }
  }
  if (!bw->p) {
    payload_bits += (unsigned)bw->nbits;
    bw->bytes += (size_t)(payload_bits >> 3);
    bw->nbits = (int)(payload_bits & 7);
    return;
  }
  if (groups == 1 && single[0][0] && single[0][1] && single[0][2] &&
      single[0][3])
    return;   /* every pixel is 0 bits */
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      uint32_t p = pix[y * w + x];
      const uint8_t *lens;
      const int *codes;
      g = w1_le_literal_group(x, y, w, h, side);
      lens = groups == 2 && g == 0 ? (const uint8_t *)ctx->counts3 : ctx->lens;
      codes = groups == 2 && g == 0 ? ctx->counts4 : ctx->codes;
      for (c = 0; c < 4; c++) {
        int sym = offsets[c] + ((p >> shifts[c]) & 255);
        if (!single[g][c]) w1_bw_code(bw, codes[sym], lens[sym]);
      }
    }
  }
}

static W1_UNUSED int w1_le_ilog2(unsigned v) {
  int r = 0;
  while (v >>= 1) r++;
  return r;
}

static W1_UNUSED uint64_t w1_le_hcost(const int *hist);   /* exact entropy */


/* Subtract-green in place on img[0..n). Returns 1 if applied (entropy gate),
 * else leaves img untouched. Uses htmp[0..1280). */
static W1_UNUSED int w1_le_sub_green_m(uint32_t *img, int n, int *htmp,
                                       int keep16) {
  int *hr = htmp, *hg = htmp + 256, *hb = htmp + 512;
  int *hr2 = htmp + 768, *hb2 = htmp + 1024;
  int i;
  uint64_t before, after;
  for (i = 0; i < 256; i++) hr[i] = hg[i] = hb[i] = hr2[i] = hb2[i] = 0;
  for (i = 0; i < n; i++) {
    int g = (int)((img[i] >> 8) & 0xff);
    int r = (int)((img[i] >> 16) & 0xff);
    int b = (int)(img[i] & 0xff);
    hr[r]++; hg[g]++; hb[b]++;
    hr2[(r - g) & 0xff]++; hb2[(b - g) & 0xff]++;
  }
  before = w1_le_hcost(hr) + w1_le_hcost(hg) + w1_le_hcost(hb);
  after = w1_le_hcost(hr2) + w1_le_hcost(hg) + w1_le_hcost(hb2);
  /* The histogram win is not the coded win: subtracting green preserves
   * pixel equality, so the LZ parse is unchanged and the real difference is
   * only in the Huffman tables. A marginal entropy win (ramps, noisy photos)
   * can still code several times larger than raw. keep16 is the fraction of
   * the "before" cost the "after" must beat (16 = any win, 12 = 25%); a
   * negative keep16 subtracts unconditionally (green-duel forced branch). */
  if (keep16 >= 0 && (uint64_t)after * 16 >= (uint64_t)before * keep16)
    return 0;
  for (i = 0; i < n; i++) {
    int g = (int)((img[i] >> 8) & 0xff);
    int r = (int)(((img[i] >> 16) & 0xff) - g) & 0xff;
    int b = (int)((img[i] & 0xff) - g) & 0xff;
    img[i] = (img[i] & 0xff000000u) | ((uint32_t)r << 16) |
             ((uint32_t)g << 8) | (uint32_t)b;
  }
  return 1;
}

#ifndef W1_LE_SG_KEEP16
#define W1_LE_SG_KEEP16 18
#endif
static W1_UNUSED int w1_le_sub_green(uint32_t *img, int n, int *htmp) {
  return w1_le_sub_green_m(img, n, htmp, W1_LE_SG_KEEP16);
}

static W1_UNUSED int w1_le_sabs(int v) {
  v &= 0xff;
  if (v >= 128) v -= 256;
  return v < 0 ? -v : v;
}

/* S(v) = round(v * log2(v) * 1024), S(0) = 0: exact entropy table. */
static const uint32_t w1k_le_slog[257] = {
  0, 0, 2048, 4869, 8192, 11888, 15882, 20123,
  24576, 29214, 34017, 38967, 44052, 49260, 54582, 60010,
  65536, 71155, 76860, 82648, 88513, 94452, 100462, 106539,
  112680, 118883, 125145, 131463, 137836, 144263, 150740, 157266,
  163840, 170460, 177125, 183834, 190584, 197376, 204207, 211078,
  217986, 224931, 231913, 238929, 245980, 253065, 260182, 267331,
  274512, 281724, 288965, 296237, 303537, 310866, 318222, 325606,
  333017, 340454, 347917, 355406, 362919, 370458, 378020, 385606,
  393216, 400849, 408504, 416182, 423882, 431604, 439347, 447111,
  454896, 462702, 470528, 478373, 486239, 494124, 502028, 509951,
  517892, 525853, 533831, 541827, 549842, 557873, 565923, 573989,
  582072, 590172, 598289, 606422, 614572, 622737, 630919, 639116,
  647328, 655556, 663799, 672058, 680331, 688619, 696921, 705239,
  713570, 721916, 730275, 738649, 747037, 755438, 763852, 772280,
  780722, 789177, 797644, 806125, 814618, 823125, 831644, 840175,
  848719, 857275, 865843, 874424, 883016, 891620, 900237, 908864,
  917504, 926155, 934818, 943491, 952177, 960873, 969580, 978299,
  987028, 995769, 1004520, 1013282, 1022054, 1030837, 1039630, 1048434,
  1057248, 1066073, 1074908, 1083752, 1092607, 1101472, 1110347, 1119231,
  1128125, 1137029, 1145943, 1154866, 1163799, 1172742, 1181693, 1190654,
  1199625, 1208604, 1217593, 1226591, 1235598, 1244614, 1253639, 1262673,
  1271715, 1280767, 1289827, 1298896, 1307973, 1317059, 1326154, 1335257,
  1344369, 1353489, 1362617, 1371753, 1380898, 1390051, 1399213, 1408382,
  1417559, 1426745, 1435938, 1445140, 1454349, 1463566, 1472791, 1482024,
  1491264, 1500512, 1509768, 1519032, 1528303, 1537581, 1546867, 1556161,
  1565462, 1574770, 1584086, 1593409, 1602739, 1612076, 1621421, 1630773,
  1640132, 1649498, 1658871, 1668252, 1677639, 1687033, 1696434, 1705842,
  1715257, 1724679, 1734107, 1743543, 1752985, 1762434, 1771889, 1781351,
  1790820, 1800295, 1809777, 1819266, 1828760, 1838262, 1847770, 1857284,
  1866805, 1876332, 1885865, 1895405, 1904951, 1914503, 1924062, 1933627,
  1943197, 1952774, 1962358, 1971947, 1981542, 1991144, 2000751, 2010365,
  2019984, 2029609, 2039241, 2048878, 2058521, 2068170, 2077825, 2087486,
  2097152
};

/* S(v) for v > 256: log2 via floor + 8-bit mantissa from the S-table. */
static W1_UNUSED uint64_t w1_le_slog_big(unsigned v) {
  int f = w1_le_ilog2(v);
  unsigned mp = v >> (f - 7);
  uint64_t logfp = ((uint64_t)(unsigned)(f - 7) << 10) +
                   (uint64_t)(w1k_le_slog[mp] / mp);
  return (uint64_t)v * logfp;
}

static W1_UNUSED uint64_t w1_bw_bit_size(const w1_bw_t *bw) {
  return (uint64_t)bw->bytes * 8 + (unsigned)bw->nbits;
}

/* Histogram of one clipped tile, channel-major (green,red,blue,alpha). */
static W1_UNUSED void w1_le_tile_hist(const uint32_t *pix, int w,
                                      int x0, int y0, int x1, int y1, int *h) {
  int x, y;
  for (x = 0; x < 1024; x++) h[x] = 0;
  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      uint32_t p = pix[y * w + x];
      h[(p >> 8) & 255]++;
      h[256 + ((p >> 16) & 255)]++;
      h[512 + (p & 255)]++;
      h[768 + ((p >> 24) & 255)]++;
    }
  }
}

/* Shell sort tiles by key (nt <= 4096). */
static W1_UNUSED void w1_le_sort_tiles(const uint32_t *key, int *ord, int n) {
  int gap, i, j, t;
  for (gap = n / 2; gap > 0; gap /= 2)
    for (i = gap; i < n; i++) {
      t = ord[i];
      for (j = i; j >= gap && key[ord[j - gap]] > key[t]; j -= gap)
        ord[j] = ord[j - gap];
      ord[j] = t;
    }
}

/* Merge band histograms into `groups` group counts (dst is zeroed first). */
static W1_UNUSED void w1_le_grp_merge(w1_le_ctx_t *ctx, const int *bandhist,
                                      int groups, int *dst) {
  static const int offsets[5] = {0, W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A, W1_LE_OFF_D};
  int i, j, c;
  memset(dst, 0, (size_t)groups * W1_LE_NC * sizeof(int));
  for (i = 0; i < W1_LE_GRP_BANDS; i++) {
    int gg = i * groups / W1_LE_GRP_BANDS;
    const int *bh = bandhist + i * 1024;
    int *gc;
    if (gg >= groups) gg = groups - 1;
    gc = dst + gg * W1_LE_NC;
    for (c = 0; c < 4; c++)
      for (j = 0; j < 256; j++) gc[offsets[c] + j] += bh[c * 256 + j];
  }
}

/* Build the five Huffman tables of each group (counts -> ctx->glens). */
static W1_UNUSED void w1_le_grp_build(w1_le_ctx_t *ctx, const int *counts,
                                      int groups) {
  int g;
  uint8_t best[W1_LE_NG];
  for (g = 0; g < groups; g++) {
    const int *gc = counts + g * W1_LE_NC;
    uint8_t *lens = ctx->glens + g * W1_LE_NC;
    w1_huff_lengths_hdr(gc, 280, lens, ctx->htmp, ctx->seq, best);
    w1_huff_lengths_hdr(gc + W1_LE_OFF_R, 256, lens + W1_LE_OFF_R, ctx->htmp,
                        ctx->seq, best);
    w1_huff_lengths_hdr(gc + W1_LE_OFF_B, 256, lens + W1_LE_OFF_B, ctx->htmp,
                        ctx->seq, best);
    w1_huff_lengths_hdr(gc + W1_LE_OFF_A, 256, lens + W1_LE_OFF_A, ctx->htmp,
                        ctx->seq, best);
    w1_huff_lengths_hdr(gc + W1_LE_OFF_D, 40, lens + W1_LE_OFF_D, ctx->htmp,
                        ctx->seq, best);
  }
}

/* Estimated token payload bits of the current group counts/tables. */
static W1_UNUSED uint64_t w1_le_grp_payload(const w1_le_ctx_t *ctx,
                                            int groups) {
  static const int offsets[5] = {0, W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A, W1_LE_OFF_D};
  static const int sizes[5] = {280, 256, 256, 256, 40};
  uint64_t bits = 0;
  int g, c, j;
  for (g = 0; g < groups; g++) {
    const uint8_t *lens = ctx->glens + g * W1_LE_NC;
    const int *gc = ctx->gcounts + g * W1_LE_NC;
    for (c = 0; c < 5; c++) {
      int nz = 0;
      for (j = 0; j < sizes[c]; j++) nz += lens[offsets[c] + j] != 0;
      for (j = 0; j < sizes[c]; j++) {
        int extra = 0, prefix = -1;
        if (c == 0 && j >= 256) prefix = j - 256;
        else if (c == 4) prefix = j;
        if (prefix >= 4) extra = (prefix - 2) >> 1;
        bits += (uint64_t)(unsigned)gc[offsets[c] + j] *
                (unsigned)((nz > 1 ? lens[offsets[c] + j] : 0) + extra);
      }
    }
  }
  return bits;
}

/* Header + table-transmission bits of the current group tables. */
static W1_UNUSED uint64_t w1_le_grp_tablebits(w1_le_ctx_t *ctx, int groups) {
  static const int offsets[5] = {0, W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A, W1_LE_OFF_D};
  static const int sizes[5] = {280, 256, 256, 256, 40};
  w1_bw_t tb;
  int g, c;
  w1_bw_init(&tb, NULL, 0);
  for (g = 0; g < groups; g++) {
    const uint8_t *lens = ctx->glens + g * W1_LE_NC;
    for (c = 0; c < 5; c++)
      w1_huff_emit(&tb, lens + offsets[c], sizes[c], ctx->seq);
  }
  return w1_bw_bit_size(&tb);
}

/* Clustered entropy-group stream. Tiles of 2^pb pixels are ranked by their
 * residual entropy and split into equal-count bands; bands are merged into
 * `groups` meta groups whose count is chosen by estimated cost (payload +
 * tables + group-map sub-image). The meta image is a normal VP8L
 * sub-image, so any group count <= 200 is wire-compatible. */
static W1_UNUSED void w1_le_group_stream(w1_le_ctx_t *ctx,
    const uint32_t *pix, int w, int h, int level, const uint8_t *flat,
    uint32_t *map, w1_bw_t *bw, w1_le_tk_t *tk) {
  static const int offsets[5] = {0, W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A, W1_LE_OFF_D};
  static const int sizes[5] = {280, 256, 256, 256, 40};
  static const int cand[4] = {1, 2, 4, 8};
  int pb = W1_LE_GRP_PB, tw, th, nt, i, j, g, c, k, groups = 1, best_k = 1;
  int replay;
  int *bandhist = ctx->phist;
  int *ord = ctx->seq;
  int *band_of = ctx->gcodes;   /* tile -> band (free until codes are built) */
  uint64_t best_est = UINT64_MAX, payload_bits = 0;
  w1_lez_t lz;
  for (;;) {
    tw = (w + (1 << pb) - 1) >> pb;
    th = (h + (1 << pb) - 1) >> pb;
    nt = tw * th;
    if (nt <= W1_LE_GRP_MAXTILES || pb >= 9) break;
    pb++;
  }
  /* Per-tile entropy signature, sorted into equal-count bands. */
  for (i = 0; i < nt; i++) {
    int tx = i % tw, ty = i / tw;
    int x0 = tx << pb, y0 = ty << pb;
    int x1 = x0 + (1 << pb), y1 = y0 + (1 << pb);
    uint64_t s = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    w1_le_tile_hist(pix, w, x0, y0, x1, y1, ctx->counts);
    for (c = 0; c < 4; c++) s += w1_le_hcost(ctx->counts + c * 256);
    map[i] = (uint32_t)(s >> 6);
    ord[i] = i;
  }
  w1_le_sort_tiles(map, ord, nt);
  for (i = 0; i < nt; i++) {
    int band = i * W1_LE_GRP_BANDS / nt;
    if (band >= W1_LE_GRP_BANDS) band = W1_LE_GRP_BANDS - 1;
    band_of[ord[i]] = band;
  }
  memset(bandhist, 0, (size_t)W1_LE_GRP_BANDS * 1024 * sizeof(int));
  for (i = 0; i < nt; i++) {
    int tx = i % tw, ty = i / tw;
    int x0 = tx << pb, y0 = ty << pb;
    int x1 = x0 + (1 << pb), y1 = y0 + (1 << pb);
    int *bh = bandhist + band_of[i] * 1024;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    w1_le_tile_hist(pix, w, x0, y0, x1, y1, ctx->counts);
    for (j = 0; j < 1024; j++) bh[j] += ctx->counts[j];
  }
  /* Pick the group count by estimated cost. */
  for (k = 0; k < 4; k++) {
    uint64_t est;
    groups = cand[k];
    if (groups > nt || groups > W1_LE_MAXG || groups > W1_LE_GRP_BANDS)
      continue;
    w1_le_grp_merge(ctx, bandhist, groups, ctx->gcounts);
    for (g = 0; g < groups; g++) {
      int *gc = ctx->gcounts + g * W1_LE_NC;
      for (j = 256; j < 280; j++) if (!gc[j]) gc[j] = 1;
      for (j = 0; j < 40; j++) if (!gc[W1_LE_OFF_D + j]) gc[W1_LE_OFF_D + j] = 1;
    }
    w1_le_grp_build(ctx, ctx->gcounts, groups);
    est = w1_le_grp_payload(ctx, groups) + w1_le_grp_tablebits(ctx, groups);
    if (groups > 1) {
      w1_bw_t mb;
      for (i = 0; i < nt; i++)
        map[i] = (uint32_t)(band_of[i] * groups / W1_LE_GRP_BANDS) << 8;
      w1_bw_init(&mb, NULL, 0);
      w1_le_encode_tokens(ctx, map, tw, th, 1, 0, flat, &mb, NULL);
      est += w1_bw_bit_size(&mb);
    }
    if (est < best_est) { best_est = est; best_k = groups; }
  }
  groups = best_k;
  /* Support histogram: every pixel symbol of each group (so any literal the
   * parse can choose is representable), plus the length/distance prefixes
   * so the first parse can use matches. */
  w1_le_grp_merge(ctx, bandhist, groups, ctx->grcounts);
  for (g = 0; g < groups; g++) {
    int *gc = ctx->grcounts + g * W1_LE_NC;
    for (j = 256; j < 280; j++) if (!gc[j]) gc[j] = 1;
    for (j = 0; j < 40; j++) if (!gc[W1_LE_OFF_D + j]) gc[W1_LE_OFF_D + j] = 1;
  }
  for (i = 0; i < nt; i++)
    map[i] = (uint32_t)(band_of[i] * groups / W1_LE_GRP_BANDS) << 8;
  w1_bw_put(bw, 0, 1);              /* no color cache */
  w1_bw_put(bw, groups > 1, 1);     /* meta (entropy) image */
  if (groups > 1) {
    w1_bw_put(bw, (uint32_t)(pb - 2), 3);
    w1_bw_put(bw, 0, 1);
    w1_le_encode_tokens(ctx, map, tw, th, 1, 0, flat, bw, NULL);
  }
  memset(&lz, 0, sizeof(lz));
  lz.pix = pix; lz.w = w; lz.n = w * h;
  lz.ctx = ctx;
  lz.depth = w1_le_depths[level];
  lz.lg = flat; lz.lr = flat + W1_LE_OFF_R; lz.lb = flat + W1_LE_OFF_B;
  lz.la = flat + W1_LE_OFF_A; lz.ld = flat + W1_LE_OFF_D;
  lz.ngroups = groups; lz.group_shift = pb; lz.group_width = tw;
  lz.group_realcost = 1;
  lz.group_map = map;
  lz.dp = ctx->dp; lz.back = ctx->back;
  lz.opt = (ctx->dp != NULL && lz.n <= ctx->opt_cap);
  if (lz.opt && level < 9) lz.depth = w1_le_depths[level + 1];
  for (g = 0; g < groups; g++)
    lz.group_counts[g] = ctx->gcounts + g * W1_LE_NC;
  /* Iterative parse: parse under the current per-group tables, rebuild the
   * tables from the tokens actually chosen (plus the support base), repeat.
   * This removes the flat-cost parse's spurious short matches, which cost
   * far more under real tables than the literals they replace. */
  replay = w1_le_tk_match(tk, W1_TK_GROUP, pix, w, h, level, 0,
                          (int)w1_le_depths[level]) && tk->ngroups == groups;
  if (replay) {
    /* The sizing trial already parsed this stream: its final tables and
     * tokens are in the memo. */
    memcpy(ctx->glens, tk->lens, (size_t)groups * W1_LE_NC);
  } else {
    w1_le_grp_build(ctx, ctx->grcounts, groups);
    for (k = 0; k < 3; k++) {
      for (g = 0; g < groups; g++)
        lz.group_lens[g] = ctx->glens + g * W1_LE_NC;
      memset(ctx->gcounts, 0, (size_t)groups * W1_LE_NC * sizeof(int));
      w1_lz_run(&lz, 1);
      for (g = 0; g < groups; g++) {
        int *gc = ctx->gcounts + g * W1_LE_NC;
        const int *gr = ctx->grcounts + g * W1_LE_NC;
        for (j = 0; j < W1_LE_NC; j++)
          if (!gc[j] && gr[j]) gc[j] = 1;
      }
      w1_le_grp_build(ctx, ctx->gcounts, groups);
    }
  }
  for (g = 0; g < groups; g++)
    lz.group_lens[g] = ctx->glens + g * W1_LE_NC;
  /* Accounting parse under the final tables: the emit pass replays exactly
   * this parse, so the trial's estimated size equals the emitted size. */
  memset(ctx->gcounts, 0, (size_t)groups * W1_LE_NC * sizeof(int));
  if (replay) { lz.tk_in = tk->tok; lz.tk_in_n = tk->ntok; }
  else if (tk) lz.tk_out = tk->tok;
  w1_lz_run(&lz, 1);
  if (!replay && tk)
    w1_le_tk_set(tk, W1_TK_GROUP, pix, w, h, level, 0,
                 (int)w1_le_depths[level], lz.tk_n, ctx->glens, groups);
  lz.tk_out = NULL;
  lz.tk_in = (tk && tk->valid) ? tk->tok : NULL;
  lz.tk_in_n = tk ? tk->ntok : 0;
  for (g = 0; g < groups; g++) {
    const uint8_t *lens = ctx->glens + g * W1_LE_NC;
    for (c = 0; c < 5; c++) {
      int nz = 0;
      for (j = 0; j < sizes[c]; j++) nz += lens[offsets[c] + j] != 0;
      lz.group_single[g][c] = nz <= 1;
    }
  }
  if (!bw->p) {
    payload_bits = w1_le_grp_payload(ctx, groups);
    for (g = 0; g < groups; g++)
      for (c = 0; c < 5; c++)
        w1_huff_emit(bw, ctx->glens + g * W1_LE_NC + offsets[c], sizes[c],
                     ctx->seq);
    payload_bits += (unsigned)bw->nbits;
    bw->bytes += (size_t)(payload_bits >> 3);
    bw->nbits = (int)(payload_bits & 7);
    return;
  }
  for (g = 0; g < groups; g++) {
    const uint8_t *lens = ctx->glens + g * W1_LE_NC;
    int *codes = ctx->gcodes + g * W1_LE_NC;
    w1_huff_codes(lens, 280, codes);
    w1_huff_codes(lens + W1_LE_OFF_R, 256, codes + W1_LE_OFF_R);
    w1_huff_codes(lens + W1_LE_OFF_B, 256, codes + W1_LE_OFF_B);
    w1_huff_codes(lens + W1_LE_OFF_A, 256, codes + W1_LE_OFF_A);
    w1_huff_codes(lens + W1_LE_OFF_D, 40, codes + W1_LE_OFF_D);
    lz.group_lens[g] = lens;
    lz.group_codes[g] = codes;
    for (c = 0; c < 5; c++)
      w1_huff_emit(bw, lens + offsets[c], sizes[c], ctx->seq);
  }
  lz.bw = bw;
  w1_lz_run(&lz, 1);
}

/* Legacy fixed half/half 2-group stream (32x32 tiles, flat-cost parse).
 * Kept as a candidate: it beats the clustered stream on match-heavy
 * content (smooth gradients, RLE-style images) where the grouped
 * tables matter less than the flat-cost parse's long matches. */
static W1_UNUSED void w1_le_group_stream_legacy(w1_le_ctx_t *ctx,
    const uint32_t *pix, int w, int h, int level, const uint8_t *flat,
    uint32_t *map, w1_bw_t *bw, w1_le_tk_t *tk) {
  static const int offsets[5] = {0, W1_LE_OFF_R, W1_LE_OFF_B, W1_LE_OFF_A, W1_LE_OFF_D};
  static const int sizes[5] = {280, 256, 256, 256, 40};
  static const int shifts[4] = {8, 16, 0, 24};
  int tw = (w + 31) >> 5, th = (h + 31) >> 5, nt = tw * th;
  int x, y, g, c, i, pass, groups = nt > 1 ? 2 : 1;
  uint64_t payload_bits = 0;
  w1_lez_t lz;
  for (y = 0; y < th; y++) for (x = 0; x < tw; x++)
    map[y * tw + x] = (uint32_t)(groups == 2 &&
        (tw >= th ? x >= (tw + 1) / 2 : y >= (th + 1) / 2)) << 8;
  for (pass = 0; pass < 2 && groups == 2; pass++) {
    memset(ctx->counts, 0, W1_LE_NC * sizeof(int));
    memset(ctx->counts2, 0, W1_LE_NC * sizeof(int));
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
      uint32_t p = pix[y * w + x];
      int *counts = map[(y >> 5) * tw + (x >> 5)] ? ctx->counts2 : ctx->counts;
      for (c = 0; c < 4; c++) counts[offsets[c] + ((p >> shifts[c]) & 255)]++;
    }
    w1_le_build_tables(ctx, 0);
    memcpy(ctx->counts3, ctx->lens, W1_LE_NC);
    memcpy(ctx->counts, ctx->counts2, W1_LE_NC * sizeof(int));
    w1_le_build_tables(ctx, 0);
    for (y = 0; y < th; y++) for (x = 0; x < tw; x++) {
      uint64_t cost[2] = {0, 0};
      int xx, yy, x1 = (x + 1) * 32, y1 = (y + 1) * 32;
      if (x1 > w) x1 = w;
      if (y1 > h) y1 = h;
      for (yy = y * 32; yy < y1; yy++) for (xx = x * 32; xx < x1; xx++) {
        uint32_t p = pix[yy * w + xx];
        for (c = 0; c < 4; c++) {
          int sym = offsets[c] + ((p >> shifts[c]) & 255);
          int a = ((const uint8_t *)ctx->counts3)[sym], b = ctx->lens[sym];
          cost[0] += a ? a : 16;
          cost[1] += b ? b : 16;
        }
      }
      if (cost[0] != cost[1]) map[y * tw + x] = (uint32_t)(cost[1] < cost[0]) << 8;
    }
  }
  {
    int seen = 0;
    for (i = 0; i < nt; i++) seen |= 1 << (map[i] >> 8);
    if (seen != 3) {
      groups = 1;
      memset(map, 0, (size_t)nt * sizeof(*map));
    }
  }
  w1_bw_put(bw, 0, 1);
  w1_bw_put(bw, groups == 2, 1);
  if (groups == 2) {
    w1_bw_put(bw, 3, 3);
    w1_bw_put(bw, 0, 1);
    w1_le_encode_tokens(ctx, map, tw, th, 1, 0, flat, bw, NULL);
  }
  memset(&lz, 0, sizeof(lz));
  lz.pix = pix; lz.w = w; lz.n = w * h;
  lz.ctx = ctx;
  lz.depth = w1_le_depths[level];
  lz.lg = flat; lz.lr = flat + W1_LE_OFF_R; lz.lb = flat + W1_LE_OFF_B;
  lz.la = flat + W1_LE_OFF_A; lz.ld = flat + W1_LE_OFF_D;
  lz.group_map = map; lz.group_width = tw;
  lz.ngroups = groups; lz.group_shift = 5;
  lz.dp = ctx->dp; lz.back = ctx->back;
  lz.opt = (ctx->dp != NULL && lz.n <= ctx->opt_cap);
  lz.group_counts[0] = ctx->counts; lz.group_counts[1] = ctx->counts2;
  memset(ctx->counts, 0, W1_LE_NC * sizeof(int));
  memset(ctx->counts2, 0, W1_LE_NC * sizeof(int));
  /* Count pass (replayed when the sizing trial already parsed this
   * stream); the emit replays the same tokens. */
  if (w1_le_tk_match(tk, W1_TK_LEGACY, pix, w, h, level, 0,
                     (int)w1_le_depths[level])) {
    lz.tk_in = tk->tok; lz.tk_in_n = tk->ntok;
  } else if (tk) lz.tk_out = tk->tok;
  w1_lz_run(&lz, 1);
  if (lz.tk_out) {
    w1_le_tk_set(tk, W1_TK_LEGACY, pix, w, h, level, 0,
                 (int)w1_le_depths[level], lz.tk_n, ctx->lens, 1);
    lz.tk_out = NULL;
  }
  lz.tk_in = (tk && tk->valid) ? tk->tok : NULL;
  lz.tk_in_n = tk ? tk->ntok : 0;
  for (g = 0; g < groups; g++) {
    if (g) memcpy(ctx->counts, ctx->counts2, W1_LE_NC * sizeof(int));
    w1_le_build_tables(ctx, 0);
    for (c = 0; c < 5; c++) {
      int nz = 0;
      for (i = 0; i < sizes[c]; i++) nz += ctx->counts[offsets[c] + i] != 0;
      lz.group_single[g][c] = nz <= 1;
      if (!bw->p) for (i = 0; i < sizes[c]; i++) {
        int extra = 0, prefix = -1;
        if (c == 0 && i >= 256) prefix = i - 256;
        else if (c == 4) prefix = i;
        if (prefix >= 4) extra = (prefix - 2) >> 1;
        payload_bits += (uint64_t)(unsigned)ctx->counts[offsets[c] + i] *
            (unsigned)((nz > 1 ? ctx->lens[offsets[c] + i] : 0) + extra);
      }
      w1_huff_emit(bw, ctx->lens + offsets[c], sizes[c], ctx->seq);
    }
    if (bw->p && g == 0 && groups == 2) {
      memcpy(ctx->counts3, ctx->lens, W1_LE_NC);
      memcpy(ctx->counts4, ctx->codes, W1_LE_NC * sizeof(int));
    }
  }
  if (!bw->p) {
    payload_bits += (unsigned)bw->nbits;
    bw->bytes += (size_t)(payload_bits >> 3);
    bw->nbits = (int)(payload_bits & 7);
    return;
  }
  lz.group_lens[0] = groups == 2 ? (const uint8_t *)ctx->counts3 : ctx->lens;
  lz.group_lens[1] = ctx->lens;
  lz.group_codes[0] = groups == 2 ? ctx->counts4 : ctx->codes;
  lz.group_codes[1] = ctx->codes;
  lz.bw = bw;
  w1_lz_run(&lz, 1);
}


static W1_UNUSED void w1_le_stream(w1_le_ctx_t *ctx, const uint32_t *pix,
                                    int w, int h, int level, int depth,
                                    int cache_bits,
                                    const uint8_t *flat, const uint8_t *present,
                                    uint32_t *meta, w1_bw_t *bw) {
  w1_bw_t start = *bw, trial;
  uint64_t best;
  int choice = -2, side;
  /* Two memo slots: `pinned` holds the current best trial's parse (the
   * final encode replays it), the other is free for the next trial. Both
   * are scoped to this call (see w1_le_tk_t). */
  w1_le_tk_t *pinned = NULL, *tk;
  ctx->tk[0].valid = 0; ctx->tk[1].valid = 0;
  w1_bw_put(bw, cache_bits != 0, 1);
  if (cache_bits) w1_bw_put(bw, (uint32_t)cache_bits, 4);
  w1_bw_put(bw, 0, 1);
  if (level < 6) {
    /* Below level 6 the single adapted stream is final, except for a
     * one-colour image: all four channel codes are single-symbol, so the
     * literal stream costs 0 bits per pixel and beats any token stream
     * (512x512: 126 -> 32 B) without running the LZ passes at all. */
    if (w1_le_uniform(pix, w * h)) {
      *bw = start;
      w1_le_literal_stream(ctx, pix, w, h, flat, meta, -1, bw);
    } else {
      w1_le_finish(ctx, pix, w, h, level, depth, cache_bits, flat, present,
                   bw, &ctx->tk[0]);
    }
    ctx->tk[0].valid = 0;
    return;
  }
  w1_le_finish(ctx, pix, w, h, level, depth, cache_bits, flat, present, bw,
               &ctx->tk[0]);
  best = bw->err ? UINT64_MAX : w1_bw_bit_size(bw) - w1_bw_bit_size(&start);
  if (cache_bits) {
    tk = &ctx->tk[0];
    w1_bw_init(&trial, NULL, 0);
    w1_bw_put(&trial, 0, 2);
    w1_le_encode_tokens(ctx, pix, w, h, level, 0, flat, &trial, tk);
    if (w1_bw_bit_size(&trial) < best) {
      best = w1_bw_bit_size(&trial); choice = -1; pinned = tk;
    }
  }
  for (side = -1; side < 4; side++) {
    uint64_t cost;
    if (side >= 0 && best < (uint64_t)(unsigned)w * (unsigned)h) break;
    w1_bw_init(&trial, NULL, 0);
    w1_le_literal_stream(ctx, pix, w, h, flat, meta, side, &trial);
    cost = w1_bw_bit_size(&trial);
    if (cost < best) { best = cost; choice = side + 1; }
  }
  if (w * h >= 1024 && best >= 1024) {
    tk = pinned == &ctx->tk[0] ? &ctx->tk[1] : &ctx->tk[0];
    w1_bw_init(&trial, NULL, 0);
    w1_le_group_stream_legacy(ctx, pix, w, h, level, flat, meta, &trial, tk);
    if (!trial.err && w1_bw_bit_size(&trial) < best) {
      best = w1_bw_bit_size(&trial); choice = 5; pinned = tk;
    }
    tk = pinned == &ctx->tk[0] ? &ctx->tk[1] : &ctx->tk[0];
    w1_bw_init(&trial, NULL, 0);
    w1_le_group_stream(ctx, pix, w, h, level, flat, meta, &trial, tk);
    if (!trial.err && w1_bw_bit_size(&trial) < best) {
      best = w1_bw_bit_size(&trial); choice = 6; pinned = tk;
    }
  }
  if (choice != -2) {
    *bw = start;
    if (choice == -1) {
      w1_bw_put(bw, 0, 2);
      w1_le_encode_tokens(ctx, pix, w, h, level, 0, flat, bw, pinned);
    } else if (choice == 5) {
      w1_le_group_stream_legacy(ctx, pix, w, h, level, flat, meta, bw,
                                pinned);
    } else if (choice == 6) {
      w1_le_group_stream(ctx, pix, w, h, level, flat, meta, bw, pinned);
    } else {
      w1_le_literal_stream(ctx, pix, w, h, flat, meta, choice - 1, bw);
    }
  }
  ctx->tk[0].valid = 0; ctx->tk[1].valid = 0;
}

/* Shannon-ish score of a 256-bin histogram (skips zeros): used by the
 * entropy probe, whose thresholds were calibrated against this scale. */
static W1_UNUSED uint64_t w1_le_hscore(const int *hist) {
  uint64_t c = 0;
  int i;
  for (i = 0; i < 256; i++)
    if (hist[i] > 0)
      c += (uint64_t)(unsigned)hist[i] *
           (uint64_t)(unsigned)w1_le_ilog2((unsigned)hist[i]);
  return c;
}

/* Exact entropy cost of a 256-bin histogram in 1/1024 bit units:
 * S(total) - sum S(count), S(v) = round(v*log2 v*1024). Lower is better. */
static W1_UNUSED uint64_t w1_le_hcost(const int *hist) {
  uint64_t tot = 0, s;
  int i;
  for (i = 0; i < 256; i++) tot += (unsigned)hist[i];
  if (tot == 0) return 0;
  s = tot <= 256 ? w1k_le_slog[tot] : w1_le_slog_big((unsigned)tot);
  for (i = 0; i < 256; i++)
    if (hist[i])
      s -= hist[i] <= 256 ? w1k_le_slog[hist[i]]
                          : w1_le_slog_big((unsigned)hist[i]);
  return s;
}

/* Reference prediction cost bias (weight_0=1, exp_val=94): rewards zeros and
 * small values. Returns <= 0 (a bonus in fixed-point bits). */
static W1_UNUSED int64_t w1_le_pred_bias(const int *counts) {
  uint64_t bits = ((uint64_t)(unsigned)counts[0]) * 1024;
  uint64_t expv = (uint64_t)94 << 10;
  int i;
  for (i = 1; i < 16; i++) {
    bits += (expv * (uint64_t)((unsigned)counts[i] +
                               (unsigned)counts[256 - i]) + 50) / 100;
    expv = (6 * expv + 5) / 10;
  }
  return -(int64_t)((bits + 5) / 10);
}

/* Recompute one tile's residuals under mode (writer half of predict). */
static W1_UNUSED void w1_le_retile(const uint32_t *orig, uint32_t *res,
                                   int w, int h, int sb, int tx, int ty,
                                   int mode) {
  int x0 = tx << sb, y0 = ty << sb, x1 = x0 + (1 << sb), y1 = y0 + (1 << sb);
  int x, y;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      uint32_t o = orig[y * w + x], p;
      if (x == 0 && y == 0) p = 0xff000000u;
      else if (y == 0) p = orig[x - 1];
      else if (x == 0) p = orig[(y - 1) * w];
      else {
        uint32_t L = orig[y * w + x - 1], T = orig[(y - 1) * w + x];
        uint32_t TL = orig[(y - 1) * w + x - 1];
        uint32_t TR = (x + 1 < w) ? orig[(y - 1) * w + x + 1] : orig[y * w];
        p = w1_vp8l_predict(mode, L, T, TL, TR);
      }
      {
        unsigned db = ((o & 0xffu) - (p & 0xffu)) & 0xffu;
        unsigned dg = (((o >> 8) & 0xffu) - ((p >> 8) & 0xffu)) & 0xffu;
        unsigned dr = (((o >> 16) & 0xffu) - ((p >> 16) & 0xffu)) & 0xffu;
        unsigned da = (((o >> 24) & 0xffu) - ((p >> 24) & 0xffu)) & 0xffu;
        res[y * w + x] = db | (dg << 8) | (dr << 16) | (da << 24);
      }
    }
  }
}

#define W1_LE_MAXTIES 16

static W1_UNUSED void w1_le_predict(w1_le_ctx_t *ctx, const uint32_t *orig,
                                     uint32_t *res, int w, int h, int sb,
                                     uint8_t *modes, int bonus,
                                     int *tiebuf, int *ntie_out) {
  int tw = (w + (1 << sb) - 1) >> sb, th = (h + (1 << sb) - 1) >> sb;
  int tx, ty, x, y;
  int *hist = ctx->htmp;   /* 1024 ints; predict never overlaps est/green */
  /* Uniform alpha (-1 = not yet scanned): then every mode predicts that
   * alpha at every pixel but the origin, so the alpha residual histogram
   * is mode-independent and the pixel-major path skips its increments. */
  int alpha_uni = -1;
  /* Establish the touched-bins invariant (all zero): per-mode histograms
   * then track only touched bins instead of memsetting 4 KB per mode. */
  memset(hist, 0, 1024 * sizeof(int));
  for (ty = 0; ty < th; ty++) {
    for (tx = 0; tx < tw; tx++) {
      int m, bestm = 0, altm = 0;
      int64_t best = (int64_t)0x7fffffffffffffffLL;
      int64_t second = (int64_t)0x7fffffffffffffffLL;
      int x0 = tx << sb, y0 = ty << sb, x1 = x0 + (1 << sb);
      int y1 = y0 + (1 << sb);
      int left = tx ? modes[ty * tw + tx - 1] : -1;
      int top = ty ? modes[(ty - 1) * tw + tx] : -1;
      if (x1 > w) x1 = w;
      if (y1 > h) y1 = h;
      if ((x1 - x0) * (y1 - y0) >= 1024) {
        /* Pixel-major: load the four neighbours once and score all 14
         * modes into 14 dense histograms (ctx->phist). 16-bit counters
         * (28 KB, L1-resident) when the tile cannot overflow them, else
         * 32-bit. The clear and the dense scan amortise over >= 1024
         * pixels; smaller tiles keep the mode-major touched-bin loop
         * below. Same integer sums, same mode order and tie-breaks:
         * bit-identical mode choices. */
        int npx = (x1 - x0) * (y1 - y0);
        int *H = ctx->phist;
        uint16_t *H16 = (uint16_t *)ctx->phist;
        int i;
        if (alpha_uni < 0) {
          uint32_t a0 = orig[0] >> 24;
          for (i = 1; i < w * h; i++) if ((orig[i] >> 24) != a0) break;
          alpha_uni = i == w * h;
        }
#define W1_LE_PM_LOOP(HT, HB, AU)                                          \
        for (y = y0; y < y1; y++) {                                        \
          for (x = x0; x < x1; x++) {                                      \
            uint32_t o = orig[y * w + x];                                  \
            HT *hm = HB; const int au_ = AU;                               \
            if (y == 0 || x == 0) {                                        \
              uint32_t q = (x == 0 && y == 0) ? 0xff000000u                \
                         : (y == 0) ? orig[x - 1] : orig[(y - 1) * w];     \
              for (m = 0; m < 14; m++, hm += 1024) W1_LE_PM_ADD(q);        \
            } else {                                                       \
              uint32_t L = orig[y * w + x - 1], T = orig[(y - 1) * w + x]; \
              uint32_t TL = orig[(y - 1) * w + x - 1];                     \
              uint32_t TR = (x + 1 < w) ? orig[(y - 1) * w + x + 1]        \
                                        : orig[y * w];                     \
              W1_LE_PM_ADD(0xff000000u); hm += 1024;                       \
              W1_LE_PM_ADD(L); hm += 1024;                                 \
              W1_LE_PM_ADD(T); hm += 1024;                                 \
              W1_LE_PM_ADD(TR); hm += 1024;                                \
              W1_LE_PM_ADD(TL); hm += 1024;                                \
              W1_LE_PM_ADD(w1_vp8l_avg3(L, T, TR)); hm += 1024;            \
              W1_LE_PM_ADD(w1_vp8l_avg2(L, TL)); hm += 1024;               \
              W1_LE_PM_ADD(w1_vp8l_avg2(L, T)); hm += 1024;                \
              W1_LE_PM_ADD(w1_vp8l_avg2(TL, T)); hm += 1024;               \
              W1_LE_PM_ADD(w1_vp8l_avg2(T, TR)); hm += 1024;               \
              W1_LE_PM_ADD(w1_vp8l_avg4(L, TL, T, TR)); hm += 1024;        \
              W1_LE_PM_ADD(w1_vp8l_select(T, L, TL)); hm += 1024;          \
              W1_LE_PM_ADD(w1_vp8l_addsub_full(L, T, TL)); hm += 1024;     \
              W1_LE_PM_ADD(w1_vp8l_addsub_half(L, T, TL));                 \
            }                                                              \
          }                                                                \
        }
#define W1_LE_PM_ADD(Q) do {                                               \
          uint32_t q_ = (Q);                                               \
          hm[(o - q_) & 0xff]++;                                           \
          hm[256 + (((o >> 8) - (q_ >> 8)) & 0xff)]++;                     \
          hm[512 + (((o >> 16) - (q_ >> 16)) & 0xff)]++;                   \
          if (!au_) hm[768 + (((o >> 24) - (q_ >> 24)) & 0xff)]++;          \
        } while (0)
        if (npx <= 65535) {
          memset(H16, 0, 14 * 1024 * sizeof(uint16_t));
          if (alpha_uni) { W1_LE_PM_LOOP(uint16_t, H16, 1) }
          else { W1_LE_PM_LOOP(uint16_t, H16, 0) }
        } else {
          memset(H, 0, 14 * 1024 * sizeof(int));
          if (alpha_uni) { W1_LE_PM_LOOP(int, H, 1) }
          else { W1_LE_PM_LOOP(int, H, 0) }
        }
#undef W1_LE_PM_LOOP
#undef W1_LE_PM_ADD
        for (m = 0; m < 14; m++) {
          int ch;
          int64_t c = 0;
          int hc[256];
          for (ch = 0; ch < 4; ch++) {
            uint64_t s = 0;
            if (ch == 3 && alpha_uni) {
              /* Residual 0 everywhere; the origin predicts alpha 0xff. */
              int org = (x0 == 0 && y0 == 0);
              for (i = 0; i < 256; i++) hc[i] = 0;
              hc[0] = npx - org;
              if (org) hc[((orig[0] >> 24) - 0xffu) & 0xff] += 1;
            } else if (npx <= 65535)
              for (i = 0; i < 256; i++) hc[i] = H16[m * 1024 + ch * 256 + i];
            else
              for (i = 0; i < 256; i++) hc[i] = H[m * 1024 + ch * 256 + i];
            for (i = 0; i < 256; i++) {
              unsigned v = (unsigned)hc[i];
              if (v) s += v <= 256 ? w1k_le_slog[v] : w1_le_slog_big(v);
            }
            c -= (int64_t)s;
            c += w1_le_pred_bias(hc);
          }
          if (m == left) c -= (int64_t)bonus;
          if (m == top) c -= (int64_t)bonus;
          if (c < best) { second = best; best = c; altm = bestm; bestm = m; }
          else if (c < second) { second = c; altm = m; }
        }
      } else
      for (m = 0; m < 14; m++) {
        int ch, b;
        int64_t c = 0;
        uint64_t s;
        /* Touched-bin tracking: residues per tile-mode are usually sparse,
         * so scoring only touched bins beats a 4 KB memset + 4x256 scan.
         * Invariant: hist is all-zero here (cleared below after each mode,
         * and once above on entry), so 0 -> 1 transitions are first touch.
         * Arithmetic matches the dense version exactly (integer sums). */
        int touched[1024], nt = 0;
        uint64_t sc[4] = {0, 0, 0, 0};
        for (y = y0; y < y1; y++) {
          for (x = x0; x < x1; x++) {
            uint32_t o = orig[y * w + x], q;
            int b0, b1, b2, b3;
            if (x == 0 && y == 0) q = 0xff000000u;
            else if (y == 0) q = orig[x - 1];
            else if (x == 0) q = orig[(y - 1) * w];
            else {
              uint32_t L = orig[y * w + x - 1], T = orig[(y - 1) * w + x];
              uint32_t TL = orig[(y - 1) * w + x - 1];
              uint32_t TR = (x + 1 < w) ? orig[(y - 1) * w + x + 1]
                                        : orig[y * w];
              q = w1_vp8l_predict(m, L, T, TL, TR);
            }
            b0 = (int)((o - q) & 0xff);
            b1 = (int)(((o >> 8) - (q >> 8)) & 0xff);
            b2 = (int)(((o >> 16) - (q >> 16)) & 0xff);
            b3 = (int)(((o >> 24) - (q >> 24)) & 0xff);
            if (!hist[b0]) touched[nt++] = b0;
            hist[b0]++;
            if (!hist[256 + b1]) touched[nt++] = 256 + b1;
            hist[256 + b1]++;
            if (!hist[512 + b2]) touched[nt++] = 512 + b2;
            hist[512 + b2]++;
            if (!hist[768 + b3]) touched[nt++] = 768 + b3;
            hist[768 + b3]++;
          }
        }
        /* Local entropy cost (S(sum) is constant across modes, dropped)
         * plus zero/small bias, minus neighbor-mode bonus. S(0) == 0 so
         * untouched bins contribute nothing. */
        {
          int i;
          for (i = 0; i < nt; i++) {
            int idx = touched[i];
            unsigned v = (unsigned)hist[idx];
            uint64_t t = v <= 256 ? w1k_le_slog[v] : w1_le_slog_big(v);
            sc[(unsigned)idx >> 8] += t;
          }
        }
        for (ch = 0; ch < 4; ch++) {
          const int *hc = hist + ch * 256;
          s = sc[ch];
          c -= (int64_t)s;
          c += w1_le_pred_bias(hc);
        }
        if (m == left) c -= (int64_t)bonus;
        if (m == top) c -= (int64_t)bonus;
        if (c < best) { second = best; best = c; altm = bestm; bestm = m; }
        else if (c < second) { second = c; altm = m; }
        for (b = 0; b < nt; b++) hist[touched[b]] = 0;
      }
      modes[ty * tw + tx] = (uint8_t)bestm;
      /* Near-tie (runner-up within 2%): record for est resolution. */
      if (tiebuf && *ntie_out < W1_LE_MAXTIES &&
          (uint64_t)(second - best) * 50 < (uint64_t)(-best)) {
        tiebuf[*ntie_out * 2] = ty * tw + tx;
        tiebuf[*ntie_out * 2 + 1] = altm;
        (*ntie_out)++;
      }
      w1_le_retile(orig, res, w, h, sb, tx, ty, bestm);
    }
  }
}

/* Main pixel stream: transforms + cache + meta(0) + tables + tokens. */
static W1_UNUSED int w1_le_color(w1_le_ctx_t *ctx, uint32_t *img, int w,
                                 int h, int level, int cache_bits,
                                 const uint8_t *flat, uint32_t *ximg,
                                 uint64_t est_cur, uint64_t *cost_out);

/* Spatial predictor bias: 15 bits for matching a neighbor. */
#define W1_LE_PRED_BONUS (15 << 10)

/* Resolve near-tie tiles by reduced-depth est (greedy, 1% hysteresis).
 * Big tiles only (sb >= 7): small-tile flips are est noise. Flips dst
 * residuals + modes in place; modepix scratch. Caller re-ests after.
 * Tie trials are capped: each trial re-tokenizes the whole image for a
 * single-tile (< 2%) decision, so unbounded trials dominate encode time
 * on large images while barely moving the total. */
#define W1_LE_MAXTIE_TRIALS 4
static W1_UNUSED void w1_le_resolve(w1_le_ctx_t *ctx, const uint32_t *src,
                                    uint32_t *dst, int w, int h, int level,
                                    int cache_bits, int sb,
                                    const uint8_t *flat, uint8_t *modes,
                                    uint32_t *modepix,
                                    const int *ties, int nties) {
  int tw = (w + (1 << sb) - 1) >> sb, th = (h + (1 << sb) - 1) >> sb;
  int nx = tw * th, i, k, ntrials;
  uint64_t cur, ae, am;
  /* Tie trials only rank flip-vs-keep on near-identical images (a single
   * tile differs), so run them at depth 8 with greedy matching: identical
   * flips as deep trials in practice (byte-identical on photo/noise/mixed
   * at 256/512), ~5x cheaper per trial. */
  int rl = level;
  if (sb < 7 || nties <= 0) return;
  (void)level;
  ntrials = nties < W1_LE_MAXTIE_TRIALS ? nties : W1_LE_MAXTIE_TRIALS;
  for (k = 0; k < nx; k++) modepix[k] = (uint32_t)modes[k] << 8;
  cur = w1_le_estimate(ctx, dst, w, h, rl, cache_bits, flat, ctx->counts) +
        w1_le_estimate(ctx, modepix, tw, th, 1, 0, flat, ctx->counts) + 300;
  for (i = 0; i < ntrials; i++) {
    int ti = ties[i * 2], alt = ties[i * 2 + 1], tx = ti % tw, ty = ti / tw;
    int oldm = modes[ti];
    modes[ti] = (uint8_t)alt;
    modepix[ti] = (uint32_t)(unsigned)alt << 8;
    w1_le_retile(src, dst, w, h, sb, tx, ty, alt);
    ae = w1_le_estimate(ctx, dst, w, h, rl, cache_bits, flat, ctx->counts);
    am = w1_le_estimate(ctx, modepix, tw, th, 1, 0, flat, ctx->counts) + 300;
    if (101 * (ae + am) < 100 * cur) {
      cur = ae + am;
    } else {
      modes[ti] = (uint8_t)oldm;
      modepix[ti] = (uint32_t)(unsigned)oldm << 8;
      w1_le_retile(src, dst, w, h, sb, tx, ty, oldm);
    }
  }
}

/* Pick the predictor tile size: full est (residuals + mode image) for
 * sb in {3,5,7,9} on src, keep the min total. dst/modes/modepix/counts
 * are scratch. W1_LE_FORCE_SB >= 0 bypasses the search (experiments). */
#ifndef W1_LE_FORCE_SB
#define W1_LE_FORCE_SB -1
#endif
static W1_UNUSED int w1_le_pick_sb(w1_le_ctx_t *ctx, const uint32_t *src,
                                    uint32_t *dst, int w, int h, int level,
                                    int cache_bits, const uint8_t *flat,
                                    uint8_t *modes, uint32_t *modepix,
                                    int *alt) {
  static const int sbs[4] = {3, 5, 7, 9};
  int i, bestsb = 3, k, ladsb = 3;
  if (alt) *alt = -1;
#if W1_LE_FORCE_SB >= 0
  (void)ctx; (void)src; (void)dst; (void)w; (void)h; (void)level;
  (void)cache_bits; (void)flat; (void)modes; (void)modepix; (void)i;
  (void)bestsb; (void)ladsb; (void)k;
  return W1_LE_FORCE_SB;
#endif
  uint64_t best = (uint64_t)-1;
  /* Tile-size ranking only needs relative order of very different
   * tilings; depth-8 greedy estimates rank identically to deep ones
   * (byte-identical on photo/noise/mixed/alpha at 256/512) at a fraction
   * of the cost. (Branch pred/raw gates are more delicate and stay at
   * depth 32 — verified: depth-8 branch gates cost +11.7% on mixed-512.)
   * Rank at the encode depth (sl = level): shallow estimates misrank
   * small structured images (edges-128 picks sb=7 for 234B where full
   * depth picks sb=3 for 128B; photo-512 872B -> 418B), and the deeper
   * ranking is cost-neutral downstream (better tiles cheapen the search:
   * edges-512 -18%, photo-512 -9% time). At L0 sl = 0 as before. */
  int sl = level;
  /* The ladder steps by 2, so a winner that is not an endpoint leaves its
   * two neighbours untried, and those are exactly the tilings libwebp
   * reaches for: on photo_meteor it picks sb=4 where this ladder can only
   * offer 3 or 5, and webp1 lands on 5 (art_pack: lib 3, webp1 5). Those
   * two extra candidates are scored here but never returned, because the
   * estimate that ranks them is the same one the branch duels exist to
   * distrust - on edges-512 it prefers 4 over the ladder's 3 and codes
   * 140 B worse. `alt` hands the refined pick to the caller, which settles
   * it on finished streams. The lower bound stays at 3: every scratch
   * buffer here is sized for the sb=3 grid. */
  int cands[6], nc = 0, ci, ladder_n;
  for (i = 0; i < 4; i++) {
    if (i == 3 && w <= 128 && h <= 128) continue;
    cands[nc++] = sbs[i];
  }
  ladder_n = nc;
  for (ci = 0; ci < nc; ci++) {
    int sb = cands[ci];
    int tw = (w + (1 << sb) - 1) >> sb;
    int th = (h + (1 << sb) - 1) >> sb;
    uint64_t cr, mx;
    int tiebuf[W1_LE_MAXTIES * 2], nties = 0;
    w1_le_predict(ctx, src, dst, w, h, sb, modes, W1_LE_PRED_BONUS,
                  tiebuf, &nties);
    w1_le_resolve(ctx, src, dst, w, h, level, cache_bits, sb, flat, modes,
                  modepix, tiebuf, nties);
    for (k = 0; k < tw * th; k++) modepix[k] = (uint32_t)modes[k] << 8;
    mx = w1_le_estimate(ctx, modepix, tw, th, 1, 0, flat, ctx->counts) + 300;
    cr = w1_le_estimate(ctx, dst, w, h, sl, cache_bits, flat, ctx->counts);
    if (cr + mx < best) {
      best = cr + mx; bestsb = sb;
      /* Hand this tiling to the branch that follows (see sv_dst). Only a
       * ladder candidate may do so: the refined ones are not returned, so
       * their state would not match what the caller goes on to use. */
      if (ci < ladder_n && ctx->sv_dst && ctx->sv_modes) {
        memcpy(ctx->sv_dst, dst, (size_t)w * (size_t)h * 4);
        memcpy(ctx->sv_modes, modes, (size_t)tw * (size_t)th);
        ctx->sv_src = src; ctx->sv_w = w; ctx->sv_h = h; ctx->sv_sb = sb;
        ctx->sv_level = level; ctx->sv_cb = cache_bits;
        ctx->sv_sum = w1_le_sum(src, w * h);
        ctx->sv_ok = 1;
      }
    }
    if (ci + 1 == ladder_n) {               /* ladder done: refine once */
      ladsb = bestsb;
      if (!alt) break;
      if (bestsb - 1 >= 3) cands[nc++] = bestsb - 1;
      if (bestsb + 1 <= 9 && !(bestsb + 1 >= 8 && w <= 128 && h <= 128))
        cands[nc++] = bestsb + 1;
    }
  }
  if (alt && bestsb != ladsb) *alt = bestsb;
  return ladsb;
}

/* Exactly two distinct alpha values (e.g. a checkerboard). The uniform
 * predictor trial below is aimed at this structure: with a 2-valued alpha
 * the RGB residual is the only thing the mode choice can mis-cost, and
 * the per-tile spatial heuristic is blind to LZ structure there. Opaque
 * images (single alpha value) skip the trial entirely. */
static W1_UNUSED int w1_le_alpha2(const uint32_t *px, int n) {
  unsigned a0 = 0x100u, a1 = 0x100u;
  int i;
  for (i = 0; i < n; i++) {
    unsigned a = (px[i] >> 24) & 0xffu;
    if (a == a0 || a == a1) continue;
    if (a0 == 0x100u) { a0 = a; continue; }
    if (a1 == 0x100u) { a1 = a; continue; }
    return 0;
  }
  return a0 != 0x100u && a1 != 0x100u;
}

/* One L6 branch: pred search+gate on src[0..n), color iff pred. src is
 * preserved; dst is scratch; the winner is copied to out (may equal src,
 * never dst). modes/modepix/ximg/counts are scratch+out. Returns total
 * est including sub-image costs. */
static W1_UNUSED uint64_t w1_le_branch(w1_le_ctx_t *ctx, const uint32_t *src,
                                       uint32_t *dst, uint32_t *out,
                                       int w, int h, int level, int cache_bits,
                                       int sb, const uint8_t *flat,
                                       uint8_t *modes,
                                       uint32_t *modepix, uint32_t *ximg,
                                       int *use_pred, int *use_color) {
  int n = w * h, k;
  int tw = (w + (1 << sb) - 1) >> sb, th = (h + (1 << sb) - 1) >> sb;
  int tiebuf[W1_LE_MAXTIES * 2], nties = 0;
  uint64_t co, cr, penalty, total;
  /* Gate ranking uses the full LZ depth: the shallow depth-32 probe
   * mis-ranks structured images (palette indices, blocky 4-colour art)
   * badly enough to pick a stream 1.5-2.5x too large. Depth 0 is reserved
   * for pick_sb's tile-size ranking, which only needs relative order. */
  int sl = level;
  /* Uniform-mode predictor trial. The per-tile spatial heuristic scores
   * residual entropy plus a zero bias but is blind to LZ structure, so on
   * row/column-linear content it can prefer a mode whose constant residual
   * is entropy-cheap yet token-expensive (on the checker-alpha target it
   * picks TopLeft, encoding 30 literals, where Left needs 5). Try globally
   * uniform modes under the LZ-aware estimate and adopt one only when it
   * beats the searched tiling by a margin. 2-valued-alpha images try the
   * Left/Top pair; every other image tries Select then Left (Select first so
   * estimate ties keep it). The entropy score rejects Select and Left on
   * mixed content where the tile-wise search picks Top for 4058 B while
   * Select codes 3080 B and Left 1060 B at 64px. */
  int ubest = -1;
  uint64_t ubest_est = (uint64_t)-1;
  int is_a2 = w1_le_alpha2(src, n);
  /* No size cap: 512/1024 cells trail lib precisely where this trial was
   * disabled (mixed-512 28616 vs 25288, alpha-512/1024). */
  int do_uni = level >= 9 && ctx->uni_force < 0;
  /* Portfolio member: one mode everywhere, no search (the caller compares
   * the finished streams, so no estimate margin applies here). */
  if (ctx->uni_force >= 0) {
    int ux, uy;
    for (uy = 0; uy < th; uy++)
      for (ux = 0; ux < tw; ux++) {
        w1_le_retile(src, dst, w, h, sb, ux, uy, ctx->uni_force);
        modes[uy * tw + ux] = (uint8_t)ctx->uni_force;
      }
    for (k = 0; k < tw * th; k++) modepix[k] = (uint32_t)ctx->uni_force << 8;
  }
  if (do_uni) {
    static const int umodes_a2[2] = {1, 2};
    static const int umodes_u[2] = {11, 1};
    /* Small images rank every mode: the ranking seeds the portfolio of
     * full encodes in w1_vp8l_encode_full, where the estimate margin that
     * guards the adoption below cannot mislead. */
    int all = n <= W1_LE_UNI_PORTFOLIO_MAXN;
    const int *ulist = is_a2 ? umodes_a2 : umodes_u;
    int nu = all ? 14 : 2, um;
    ctx->uni_n = 0;
    for (um = 0; um < nu; um++) {
      int m = all ? um : ulist[um], ux, uy, j;
      uint64_t ue, umx;
      for (uy = 0; uy < th; uy++)
        for (ux = 0; ux < tw; ux++) {
          w1_le_retile(src, dst, w, h, sb, ux, uy, m);
          modes[uy * tw + ux] = (uint8_t)m;
        }
      for (k = 0; k < tw * th; k++) modepix[k] = (uint32_t)m << 8;
      umx = w1_le_estimate(ctx, modepix, tw, th, 1, 0, flat,
                           ctx->counts2) + 300;
      ue = w1_le_est2(ctx, dst, w, h, sl, cache_bits, flat, ctx->counts4,
                      ctx->counts);
      if (!all || m == ulist[0] || m == ulist[1]) {
        if (ue + umx < ubest_est) { ubest_est = ue + umx; ubest = m; }
      }
      /* Insertion into the ranking (ascending estimate). */
      for (j = ctx->uni_n; j > 0 && ctx->uni_est[j - 1] > ue + umx; j--) {
        ctx->uni_est[j] = ctx->uni_est[j - 1];
        ctx->uni_rank[j] = ctx->uni_rank[j - 1];
      }
      ctx->uni_est[j] = ue + umx; ctx->uni_rank[j] = m; ctx->uni_n++;
    }
  }
  if (ctx->uni_force < 0) {
    /* pick_sb already ran this exact predict + resolve (same src, same sb,
     * same depth) to choose sb; reuse its result rather than repeat it.
     * resolve re-tokenizes the whole image once per tie trial, so this is
     * the bulk of the branch's work. */
    if (ctx->sv_ok && ctx->sv_src == src && ctx->sv_w == w &&
        ctx->sv_h == h && ctx->sv_sb == sb && ctx->sv_level == level &&
        ctx->sv_cb == cache_bits && ctx->sv_sum == w1_le_sum(src, w * h)) {
      memcpy(dst, ctx->sv_dst, (size_t)w * (size_t)h * 4);
      memcpy(modes, ctx->sv_modes, (size_t)tw * (size_t)th);
    } else {
      w1_le_predict(ctx, src, dst, w, h, sb, modes, W1_LE_PRED_BONUS,
                    tiebuf, &nties);
      w1_le_resolve(ctx, src, dst, w, h, level, cache_bits, sb, flat, modes,
                    modepix, tiebuf, nties);
    }
    for (k = 0; k < tw * th; k++) modepix[k] = (uint32_t)modes[k] << 8;
  }
  penalty = w1_le_estimate(ctx, modepix, tw, th, 1, 0, flat, ctx->counts) +
            300;
  co = w1_le_estimate(ctx, src, w, h, sl, cache_bits, flat, ctx->counts2);
  cr = w1_le_estimate(ctx, dst, w, h, sl, cache_bits, flat, ctx->counts);
  if (do_uni) {
    /* cr's pass A just left its H1 lens in ctx->lens: iterate from there
     * instead of repeating that pass. */
    uint64_t cr2 = w1_le_est2_from(ctx, dst, w, h, sl, cache_bits,
                                   ctx->counts);
    if (ubest >= 0 && ubest_est + 64 < cr2 + penalty) {
      int ux, uy;
      for (uy = 0; uy < th; uy++)
        for (ux = 0; ux < tw; ux++) {
          w1_le_retile(src, dst, w, h, sb, ux, uy, ubest);
          modes[uy * tw + ux] = (uint8_t)ubest;
        }
      for (k = 0; k < tw * th; k++) modepix[k] = (uint32_t)ubest << 8;
      penalty = w1_le_estimate(ctx, modepix, tw, th, 1, 0, flat,
                               ctx->counts2) + 300;
      cr = w1_le_estimate(ctx, dst, w, h, sl, cache_bits, flat, ctx->counts);
    }
  }
  if (cr + penalty < co) {
    *use_pred = 1;
    total = cr + penalty;
#ifndef W1_NO_COLOR
    *use_color = w1_le_color(ctx, dst, w, h, level, cache_bits, flat, ximg,
                             cr, &total);
    total += penalty;
#else
    *use_color = 0;
#endif
    if (out != dst)
      for (k = 0; k < n; k++) out[k] = dst[k];
  } else {
    *use_pred = 0;
    *use_color = 0;
    for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
    total = co;
    if (out != src)
      for (k = 0; k < n; k++) out[k] = src[k];
  }
  return total;
}

/* ---- Palette+spatial trial helpers (type-3 + predictor on indices) ---- */

/* Unique ARGB of px[0..n) into tab[] (first-appearance order). Returns the
 * count, or 257 the moment it exceeds 256 (early out, tab left partial). */
static W1_UNUSED int w1_le_pal_table(const uint32_t *px, int n,
                                     uint32_t *tab) {
  int ts = 0, i;
  for (i = 0; i < n; i++) {
    uint32_t v = px[i];
    int k;
    for (k = 0; k < ts; k++) if (tab[k] == v) break;
    if (k < ts) continue;
    if (ts == 256) return 257;
    tab[ts++] = v;
  }
  return ts;
}

/* Same colour set as w1_le_pal_table (returns 257 past 256 colours) via a
 * 1024-slot open-addressing hash: O(n) instead of O(n * ts), which matters
 * at the low levels where a 100-256 colour 512x512 image would otherwise
 * spend tens of ms in the scan. Order of tab is arbitrary; callers sort. */
static W1_UNUSED int w1_le_pal_table_fast(const uint32_t *px, int n,
                                          uint32_t *tab) {
  uint32_t keys[1024];
  uint8_t used[1024];
  int ts = 0, i;
  uint32_t prev = ~px[0];
  memset(used, 0, sizeof(used));
  for (i = 0; i < n; i++) {
    uint32_t v = px[i];
    unsigned hsh;
    if (v == prev) continue;   /* runs dominate palette images */
    prev = v;
    hsh = (v * 0x9E3779B1u) >> 22;
    while (used[hsh] && keys[hsh] != v) hsh = (hsh + 1) & 1023;
    if (used[hsh]) continue;
    if (ts == 256) return 257;
    used[hsh] = 1; keys[hsh] = v;
    tab[ts++] = v;
  }
  return ts;
}

/* Packed-width bits for ts entries (bitstream map; mirrors the decoder). */
static W1_UNUSED int w1_le_pal_wb(int ts) {
  return ts <= 2 ? 3 : (ts <= 4 ? 2 : (ts <= 16 ? 1 : 0));
}

/* Sort tab[0..ts) by packed ARGB value (insertion sort; ts <= 256).
 * Like a sorted palette, this keeps consecutive delta-coded table
 * entries close and the table sub-image cheap. Deterministic; the packed
 * indices follow whatever order results (decoder-agnostic). */
static W1_UNUSED void w1_le_pal_sort(uint32_t *tab, int ts) {
  int i;
  for (i = 1; i < ts; i++) {
    uint32_t v = tab[i];
    int j = i - 1;
    while (j >= 0 && tab[j] > v) { tab[j + 1] = tab[j]; j--; }
    tab[j + 1] = v;
  }
}

/* Pack px (w x h) as table indices into dst (new_w x h): A=0xff, R=B=0,
 * G holds 8>>wb indices per pixel LSB-first, the exact lanes the type-3
 * expand reads. */
static W1_UNUSED void w1_le_pal_pack(const uint32_t *px, int w, int h,
                                     const uint32_t *tab, int ts,
                                     int wb, int new_w, uint32_t *dst) {
  int bpp = 8 >> wb, lanes = 1 << wb, x, y;
  uint32_t lv = 0;
  int li = -1;   /* last (colour, index): runs skip the search */
  for (y = 0; y < h; y++) {
    const uint32_t *row = px + (size_t)y * w;
    uint32_t *out = dst + (size_t)y * new_w;
    for (x = 0; x < w; x += lanes) {
      /* One packed word per `lanes` pixels, built in a register: no
       * read-modify-write per pixel and no pre-fill pass. */
      uint32_t acc = 0xff000000u;
      int xe = x + lanes < w ? x + lanes : w, xx;
      for (xx = x; xx < xe; xx++) {
        uint32_t v = row[xx];
        if (li < 0 || v != lv) {
          int lo = 0, hi = ts;
          while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (tab[mid] < v) lo = mid + 1; else hi = mid;
          }
          lv = v; li = lo < ts && tab[lo] == v ? lo : 0;
        }
        acc |= (uint32_t)li << (unsigned)(8 + (xx - x) * bpp);
      }
      out[x >> wb] = acc;
    }
  }
}

/* Delta-code tab[0..ts) into dst[0..ts) (mirrors the decoder undo loop). */
static W1_UNUSED void w1_le_pal_delta(const uint32_t *tab, int ts,
                                      uint32_t *dst) {
  uint32_t prev = 0;
  int i;
  for (i = 0; i < ts; i++) {
    uint32_t v = tab[i];
    unsigned a = ((v >> 24) - (prev >> 24)) & 0xffu;
    unsigned r = (((v >> 16) & 0xffu) - ((prev >> 16) & 0xffu)) & 0xffu;
    unsigned g = (((v >> 8) & 0xffu) - ((prev >> 8) & 0xffu)) & 0xffu;
    unsigned b = (((v & 0xffu) - (prev & 0xffu)) & 0xffu);
    dst[i] = (a << 24) | (r << 16) | (g << 8) | b;
    prev = v;
  }
}

/* Hopeless-search fast path probe (L6+): returns 1 when the image is
 * near-maximal-entropy, i.e. no predictor / color decorrelator can win.
 * Builds 8 single-pass histograms: R/G/B left-diffs, R/G/B top-diffs and
 * R-G / B-G decorrelation diffs. Every bin must sit below 2x the mean
 * (max*256 < count*2). Bench separation at 128/256 (max/mean): noise
 * <= 1.47x on all 8 histograms; photo/mixed/alpha trip at least one at
 * >= 3.98x (usually 16-256x), flat trips all at 256x. The 2x line has
 * ~40% margin on the noise side and ~2x on the structured side.
 * Alpha is excluded from the diffs (bench noise pins A=255; a varying
 * alpha is structure the L6 search might exploit). n < 4096 bypasses:
 * tiny images have noisy ratios and the search is trivial there anyway.
 * Cost is ~2 O(n) passes with byte ops (negligible vs the ~6 avoided
 * 14-mode predict scans plus ~15 avoided LZ estimates). Green is decided
 * exactly: the caller runs w1_le_sub_green plus the same A/B raw-estimate
 * duel the full search runs (same depth), so green is provably identical.
 * Pred/color: with uniform residuals at every offset, residual
 * est ~= raw est and the mode-image penalty (+300) always loses the gate;
 * color is only attempted under a pred win, so skipping both is exact
 * whenever the probe triggers on data the full search would also leave
 * raw (verified byte-identical via bench hashes). */
static W1_UNUSED int w1_le_uniform(const uint32_t *pix, int n) {
  int i;
  for (i = 1; i < n; i++) if (pix[i] != pix[0]) return 0;
  return 1;
}

static W1_UNUSED int w1_le_hopeless(const uint32_t *pix, int w, int h) {
  int hL[3][256], hT[3][256], hD[2][256], hR[3][256];
  int x, y, c, i;
  int64_t n = (int64_t)w * h;
  int64_t nl, nt;
  if (n < 4096) return 0;
  memset(hL, 0, sizeof hL);
  memset(hT, 0, sizeof hT);
  memset(hD, 0, sizeof hD);
  memset(hR, 0, sizeof hR);
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      uint32_t o = pix[(size_t)y * (size_t)w + (size_t)x];
      int r = (int)((o >> 16) & 0xff), g = (int)((o >> 8) & 0xff);
      int b = (int)(o & 0xff);
      hR[0][r]++; hR[1][g]++; hR[2][b]++;
      hD[0][(r - g) & 0xff]++;
      hD[1][(b - g) & 0xff]++;
      if (x > 0) {
        uint32_t L = pix[(size_t)y * (size_t)w + (size_t)x - 1];
        hL[0][(r - (int)((L >> 16) & 0xff)) & 0xff]++;
        hL[1][(g - (int)((L >> 8) & 0xff)) & 0xff]++;
        hL[2][(b - (int)(L & 0xff)) & 0xff]++;
      }
      if (y > 0) {
        uint32_t T = pix[(size_t)(y - 1) * (size_t)w + (size_t)x];
        hT[0][(r - (int)((T >> 16) & 0xff)) & 0xff]++;
        hT[1][(g - (int)((T >> 8) & 0xff)) & 0xff]++;
        hT[2][(b - (int)(T & 0xff)) & 0xff]++;
      }
    }
  }
  /* A dominant residual bin means the predictor pays off in an obvious way.
   * Not having one does NOT mean prediction is useless: real photographs
   * have broad residual histograms whose *entropy* is still far below the
   * raw channels' (13.4 vs 21.0 bits/px on a real 512 photo). The old
   * max-bin-only test sent every such photo down the raw fast path, which
   * codes ~3x worse than a plain left-delta + order-0 Huffman would. Compare
   * the predictor entropy against the raw-channel entropy instead. */
  for (c = 0; c < 3; c++) {
    uint64_t raw_c = w1_le_hscore(hR[c]);
    uint64_t bl = w1_le_hscore(hL[c]), bt = w1_le_hscore(hT[c]);
    if (w > 1 && bl * 64 < raw_c * 63) return 0;
    if (h > 1 && bt * 64 < raw_c * 63) return 0;
  }
  nl = n - h;
  nt = n - w;
  for (c = 0; c < 3; c++) {
    int ml = 0, mt = 0;
    for (i = 0; i < 256; i++) {
      if (hL[c][i] > ml) ml = hL[c][i];
      if (hT[c][i] > mt) mt = hT[c][i];
    }
    if (w > 1 && (int64_t)ml * 256 >= nl * 2) return 0;
    if (h > 1 && (int64_t)mt * 256 >= nt * 2) return 0;
  }
  for (c = 0; c < 2; c++) {
    int md = 0;
    for (i = 0; i < 256; i++) if (hD[c][i] > md) md = hD[c][i];
    if ((int64_t)md * 256 >= n * 2) return 0;
  }
  return 1;
}

#ifndef W1_LE_PAL_MAXN
#define W1_LE_PAL_MAXN (WEBP1_MAX_DIM * WEBP1_MAX_DIM)
#endif
#ifndef W1_LE_PAL_MINTS
#define W1_LE_PAL_MINTS 2   /* a single colour is cheaper as green-only LZ (flat 512: 32 vs 38 B) */
#endif
#ifndef W1_LE_MID_LEVEL
#define W1_LE_MID_LEVEL 3   /* lossless levels >= this add the spatial predictor */
#endif
static W1_UNUSED int w1_le_s8(unsigned v);
static W1_UNUSED void w1_le_ct_tile(uint32_t *img, int w, int h,
    int tx, int ty, int sb, int g2r, int g2b, int r2b, int dir);

static W1_UNUSED void w1_le_emit_image(w1_le_ctx_t *ctx,
    const uint32_t *img, int w, int h, int level, int cache_bits,
    int use_green, int use_pred, int use_color, int pred_sb,
    const uint8_t *flat, const uint8_t *modes, uint32_t *modepix,
    const uint32_t *ximg, uint32_t *scratch, w1_bw_t *bw) {
  int n = w * h, k;
  uint8_t present[1024];
  w1_le_scan_present(img, n, present);
  if (use_green) { w1_bw_put(bw, 1, 1); w1_bw_put(bw, 2, 2); }
  if (use_pred) {
    int tw = (w + (1 << pred_sb) - 1) >> pred_sb;
    int th = (h + (1 << pred_sb) - 1) >> pred_sb, i;
    w1_bw_put(bw, 1, 1);
    w1_bw_put(bw, 0, 2);   /* predictor */
    w1_bw_put(bw, (uint32_t)(pred_sb - 2), 3);
    for (i = 0; i < tw * th; i++) modepix[i] = (uint32_t)modes[i] << 8;
    w1_bw_put(bw, 0, 1);   /* sub-image: no cache */
    /* The sub-image encode clobbers ctx->counts: save/restore it. */
    for (k = 0; k < W1_LE_NC; k++) ctx->counts2[k] = ctx->counts[k];
    w1_le_encode_tokens(ctx, modepix, tw, th, 1, 0, flat, bw, NULL);
    for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
  }
  if (use_color) {
    int tw = (w + (1 << W1_CT_SB) - 1) >> W1_CT_SB;
    int th = (h + (1 << W1_CT_SB) - 1) >> W1_CT_SB;
    w1_bw_put(bw, 1, 1);
    w1_bw_put(bw, 1, 2);   /* color transform */
    w1_bw_put(bw, (uint32_t)(W1_CT_SB - 2), 3);
    w1_bw_put(bw, 0, 1);   /* sub-image: no cache */
    for (k = 0; k < W1_LE_NC; k++) ctx->counts2[k] = ctx->counts[k];
    w1_le_encode_tokens(ctx, ximg, tw, th, 1, 0, flat, bw, NULL);
    for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
  }
  w1_bw_put(bw, 0, 1);     /* end of transforms */
  w1_le_stream(ctx, img, w, h, level, (int)w1_le_depths[level], cache_bits,
               flat, present, scratch, bw);

}

static W1_UNUSED int w1_le_smooth(const uint32_t *pix, int w, int h) {
  int x, y, c;
  for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
    uint32_t p = pix[y * w + x];
    for (c = 0; c < 24; c += 8) {
      int v = (p >> c) & 255;
      if (x && w1_le_sabs(v - (int)((pix[y * w + x - 1] >> c) & 255)) > 2) return 0;
      if (y && w1_le_sabs(v - (int)((pix[(y - 1) * w + x] >> c) & 255)) > 2) return 0;
    }
  }
  return 1;
}

static W1_UNUSED int w1_le_uniform_stream(w1_le_ctx_t *ctx,
    const uint32_t *orig, uint32_t *res, uint32_t *scratch, int w, int h,
    int level, int cache_bits, int sb, int mode, int green, const uint8_t *flat,
    uint8_t *modes, uint32_t *modepix, uint32_t *ximg, w1_bw_t *bw) {
  int tx, ty, tw = (w + (1 << sb) - 1) >> sb;
  int th = (h + (1 << sb) - 1) >> sb, color;
  uint64_t cost;
  const uint32_t *src = orig;
  if (green) {
    memcpy(scratch, orig, (size_t)w * h * sizeof(*orig));
    green = w1_le_sub_green(scratch, w * h, ctx->htmp);
    src = scratch;
  }
  for (ty = 0; ty < th; ty++) for (tx = 0; tx < tw; tx++) {
    modes[ty * tw + tx] = (uint8_t)mode;
    w1_le_retile(src, res, w, h, sb, tx, ty, mode);
  }
  cost = w1_le_estimate(ctx, res, w, h, level, cache_bits, flat, ctx->counts);
  color = w1_le_color(ctx, res, w, h, level, cache_bits, flat, ximg, cost, &cost);
  w1_le_emit_image(ctx, res, w, h, level, cache_bits, green, 1,
      color, sb, flat, modes, modepix, ximg, scratch, bw);
  return green | (color << 1);
}

static W1_UNUSED int w1_le_alpha_rows(const uint32_t *orig, int w, int h) {
  size_t across = 0, down = 0, rgb_h = 0, samples = 0;
  int y, x;
  if (w < 256 || h < 256) return 0;
  for (y = 0; y + 1 < h; y += 4)
    for (x = 0; x + 16 < w; x += 16) {
      uint32_t p = orig[(size_t)y * w + x];
      uint32_t nx = orig[(size_t)y * w + x + 1];
      unsigned a = p >> 24;
      across += a != (nx >> 24);
      across += a != (orig[(size_t)y * w + x + 16] >> 24);
      down += a != (orig[(size_t)(y + 1) * w + x] >> 24);
      rgb_h += (size_t)w1_le_sabs((int)(p & 255) - (int)(nx & 255));
      rgb_h += (size_t)w1_le_sabs((int)((p >> 8) & 255) -
                                  (int)((nx >> 8) & 255));
      rgb_h += (size_t)w1_le_sabs((int)((p >> 16) & 255) -
                                  (int)((nx >> 16) & 255));
      samples++;
    }
  return samples && across * 64 < samples && down * 16 > samples &&
         rgb_h <= samples * 3;
}

static W1_UNUSED void w1_le_main(w1_le_ctx_t *ctx, const uint32_t *orig,
                                 uint32_t *res, uint32_t *predres,
                                 uint32_t *resB, uint8_t *svmodes,
                                 uint32_t *svximg,
                                 int w, int h, int level,
                                 const uint8_t *flat, uint8_t *modes,
                                 uint32_t *modepix, uint32_t *ximg,
                                 w1_bw_t *bw) {
  int n = w * h, use_pred = 0, use_green = 0, use_color = 0, k;
  int pred_sb = 3;
  int cache_bits = (int)w1_le_cachebits[level];
  /* Estimate of the chosen ARGB stream below level 6 (feeds the palette
   * trial); (uint64_t)-1 = no trial (level 6+ has its own, uniform skips). */
  uint64_t main_est = (uint64_t)-1;
  /* A cache larger than the image only inflates the green alphabet
   * (256+24+cache) and Huffman tables; clamp to image size. Fixes
   * L8/L9 regressing on tiny images (e.g. 16x16 went 414 -> 424). */
  while (cache_bits > 0 && (1 << cache_bits) > n) cache_bits--;
  const uint32_t *img = orig;
  if (level >= 6 && w1_le_alpha_rows(orig, w, h)) {
    w1_le_uniform_stream(ctx, orig, res, predres, w, h, level, cache_bits,
                         9, 1, 0, flat, modes, modepix, ximg, bw);
    return;
  }
  if (level >= 6) {
    /* Hopeless fast path: near-maximal-entropy input leaves every L6 gate
     * on a raw winner (pred/color argued in w1_le_hopeless; green decided
     * by the exact gate + an exact A/B raw-estimate duel below). Skips
     * pick_sb (4x 14-mode predicts + resolves), both pred/color branches
     * and the color search; the tail (present scan + finish at full L6
     * depth/cache) is shared, and tokenization of match-free data is
     * H1-independent (no LZ candidates pass pixel equality, ~zero
     * color-cache hits), so bytes are identical to the full search's raw
     * winner. When green applies, the two branch raw estimates (same depth
     * sl = 1 as w1_le_branch's gates) pick the same A/B winner the full
     * search would, including its strict totB < totA tie-break. */
    int fast_raw = 0;
    if (w1_le_uniform(orig, n) || w1_le_hopeless(orig, w, h)) {
      for (k = 0; k < n; k++) res[k] = orig[k];
      use_green = w1_le_sub_green(res, n, ctx->htmp);
      if (!use_green) {
        w1_le_estimate(ctx, res, w, h, level, cache_bits, flat, ctx->counts);
        img = res;
        fast_raw = 1;
      } else {
        /* Same sl as w1_le_branch for level >= 6 (depth 32). */
        uint64_t coB = w1_le_estimate(ctx, orig, w, h, 1, cache_bits, flat,
                                      ctx->counts2);
        uint64_t coA = w1_le_estimate(ctx, res, w, h, 1, cache_bits, flat,
                                      ctx->counts);
        if (coB < coA) {
          for (k = 0; k < n; k++) res[k] = orig[k];
          use_green = 0;
        }
        w1_le_estimate(ctx, res, w, h, level, cache_bits, flat, ctx->counts);
        img = res;
        fast_raw = 1;
      }
    }
    if (!fast_raw) {
    /* Mini-crunch: branch B (no green) then branch A (green); each runs
     * pred+color gates, winner by total est including sub-costs. */
    uint64_t totA, totB;
    int tw3 = (w + (1 << W1_CT_SB) - 1) >> W1_CT_SB;
    int th3 = (h + (1 << W1_CT_SB) - 1) >> W1_CT_SB;
    int nx3 = tw3 * th3;
    int twm, thm, nxm;
    int predB, colorB;
    /* Palette-only shortcut: with at most W1_LE_PAL_FORCE_TS distinct
     * colours the indexed stream wins the trial below on every measured
     * fixture, so the ARGB branches (tile-size search, two predictor /
     * colour branches: 60-400 ms at 512x512) are skipped and the palette
     * trial is compared against an infinite ARGB cost. */
    int pal_only = 0;
#ifndef W1_LE_PAL_FORCE_TS
#define W1_LE_PAL_FORCE_TS 2
#endif
    if (W1_LE_PAL_FORCE_TS > 0 && n <= W1_LE_PAL_MAXN) {
      uint32_t ptab0[256];
      int ts0 = w1_le_pal_table(orig, n, ptab0);
      pal_only = ts0 >= W1_LE_PAL_MINTS && ts0 <= W1_LE_PAL_FORCE_TS;
    }
    if (pal_only) {
      pred_sb = 3;
      twm = (w + (1 << pred_sb) - 1) >> pred_sb;
      thm = (h + (1 << pred_sb) - 1) >> pred_sb;
      nxm = twm * thm;
      predB = colorB = 0;
      totB = totA = (uint64_t)-1;
      for (k = 0; k < n; k++) res[k] = orig[k];
      use_green = 0; use_pred = 0; use_color = 0;
      w1_le_estimate(ctx, res, w, h, level, cache_bits, flat, ctx->counts);
    } else {
    pred_sb = ctx->sb_hint >= 0
                  ? ctx->sb_hint
                  : level >= 9
                      ? w1_le_pick_sb(ctx, orig, predres, w, h, level,
                                      cache_bits, flat, modes, modepix,
                                      &ctx->sb_alt)
                      : 5;
    ctx->sb_used = pred_sb;
    twm = (w + (1 << pred_sb) - 1) >> pred_sb;
    thm = (h + (1 << pred_sb) - 1) >> pred_sb;
    nxm = twm * thm;
    totA = totB = (uint64_t)-1;
    predB = colorB = 0;
    if (ctx->sg_force != 1) {
      totB = w1_le_branch(ctx, orig, predres, resB, w, h, level, cache_bits,
                          pred_sb, flat, modes, modepix, ximg, &predB,
                          &colorB);
      if (predB)
        for (k = 0; k < nxm; k++) svmodes[k] = modes[k];
      if (colorB)
        for (k = 0; k < nx3; k++) svximg[k] = ximg[k];
      for (k = 0; k < W1_LE_NC; k++) ctx->counts3[k] = ctx->counts[k];
    }
    if (ctx->sg_force != 0) {
      for (k = 0; k < n; k++) res[k] = orig[k];
      use_green = w1_le_sub_green_m(res, n, ctx->htmp,
                                    ctx->sg_force == 1 ? -1
                                                       : W1_LE_SG_KEEP16);
      totA = w1_le_branch(ctx, res, predres, res, w, h, level, cache_bits,
                          pred_sb, flat, modes, modepix, ximg,
                          &use_pred, &use_color);
    }
    }
#ifndef W1_LE_SG_MARGIN16
#define W1_LE_SG_MARGIN16 15
#endif
    /* Require subtract-green to win by a clear margin: the depth-640
     * estimates still differ from the adapted-table cost by a few percent,
     * so near-ties can flip to the wrong branch (e.g. gradient128; a raw
     * win measured 4 bytes smaller than the estimate's green pick). */
    if (!pal_only && (ctx->sg_force == 0 ||
                      (ctx->sg_force < 0 &&
                       !(totA * 16 < totB * W1_LE_SG_MARGIN16)))) {
      for (k = 0; k < n; k++) res[k] = resB[k];
      if (predB)
        for (k = 0; k < nxm; k++) modes[k] = svmodes[k];
      if (colorB)
        for (k = 0; k < nx3; k++) ximg[k] = svximg[k];
      for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts3[k];
      use_pred = predB; use_color = colorB; use_green = 0;
    }
    if (!pal_only) ctx->sg_used = use_green;
    /* Palette+spatial trial: few unique colors -> index the ORIGINAL, then
     * the predictor on the packed indices (wire order [type3, type0]; the
     * inverses run in reverse so prediction inverts first). Buffer reuse
     * only: indexed image in predres, delta table pre-estimate in resB,
     * packed-branch winner in resB (out==dst); res keeps the non-palette
     * winner untouched until a palette win discards it. Color/green stay
     * off the indexed path (R=B=0 trips the color early-out; green is
     * never applied to indices). Strict beat + 300 margin, else restore
     * and fall through bit-identical. */
    if (n <= W1_LE_PAL_MAXN) {
      uint32_t ptab[256];
      int ts = w1_le_pal_table(orig, n, ptab);
      if (ts >= W1_LE_PAL_MINTS && ts <= 256) {
        int wb;
        int new_w;
        int n_idx, cb_idx, sb_idx;
        int lvl = w1_le_idx_level(level);
        int twi = 0, thi = 0, useP = 0, useC = 0, k2, i2;
        uint64_t tbl_est, totP, totW = totB < totA ? totB : totA;
        w1_le_pal_sort(ptab, ts);
        wb = w1_le_pal_wb(ts);
        new_w = (w + (1 << wb) - 1) >> wb;
        n_idx = new_w * h; cb_idx = cache_bits;
        while (cb_idx > 0 && (1 << cb_idx) > n_idx) cb_idx--;
#ifdef W1_LE_PAL_NOCACHE
        cb_idx = 0;
#endif
        w1_le_pal_pack(orig, w, h, ptab, ts, wb, new_w, predres);
        w1_le_pal_delta(ptab, ts, resB);
        tbl_est = w1_le_estimate(ctx, resB, ts, 1, 1, 0, flat, ctx->counts2);
        if (use_pred)
          for (k2 = 0; k2 < nxm; k2++) svmodes[k2] = modes[k2];
        if (use_color)
          for (k2 = 0; k2 < nx3; k2++) svximg[k2] = ximg[k2];
        for (k2 = 0; k2 < W1_LE_NC; k2++) ctx->counts3[k2] = ctx->counts[k2];
        /* NULL: this trial is not the stream the sb duel re-encodes, so a
         * refined pick from it would be scored against the wrong image. */
        sb_idx = w1_le_pick_sb(ctx, predres, resB, new_w, h, lvl, cb_idx,
                               flat, modes, modepix, NULL);
        totP = w1_le_branch(ctx, predres, resB, resB, new_w, h, lvl, cb_idx,
                            sb_idx, flat, modes, modepix, ximg, &useP, &useC);
        totP += tbl_est + 300;

        /* No useP requirement: the branch's own pred/raw gate decides; a
         * raw-index win emits palette-only (loses on sand/pal64, where the
         * strict beat + margin keeps it rejected). useC is always 0 here
         * (R=B=0 trips the color early-out); the guard is belt-and-braces
         * since color is never emitted on this path. */
        if (!useC && (ctx->pal_force == 1 || totP < totW)) {
          /* Palette win: table, predictor block, stream on indices. */
          uint8_t presentP[1024];
          twi = (new_w + (1 << sb_idx) - 1) >> sb_idx;
          thi = (h + (1 << sb_idx) - 1) >> sb_idx;
          w1_le_pal_delta(ptab, ts, res);
          w1_bw_put(bw, 1, 1);
          w1_bw_put(bw, 3, 2);
          w1_bw_put(bw, (uint32_t)(ts - 1), 8);
          w1_bw_put(bw, 0, 1);   /* table sub-image: no cache */
          for (k2 = 0; k2 < W1_LE_NC; k2++)
            ctx->counts2[k2] = ctx->counts[k2];
          w1_le_encode_tokens(ctx, res, ts, 1, 1, 0, flat, bw, NULL);
          for (k2 = 0; k2 < W1_LE_NC; k2++)
            ctx->counts[k2] = ctx->counts2[k2];
          if (useP) {
            w1_bw_put(bw, 1, 1);
            w1_bw_put(bw, 0, 2);   /* predictor on packed indices */
            w1_bw_put(bw, (uint32_t)(sb_idx - 2), 3);
            for (i2 = 0; i2 < twi * thi; i2++)
              modepix[i2] = (uint32_t)modes[i2] << 8;
            w1_bw_put(bw, 0, 1);   /* mode sub-image: no cache */
            for (k2 = 0; k2 < W1_LE_NC; k2++)
              ctx->counts2[k2] = ctx->counts[k2];
            w1_le_encode_tokens(ctx, modepix, twi, thi, 1, 0, flat, bw, NULL);
            for (k2 = 0; k2 < W1_LE_NC; k2++)
              ctx->counts[k2] = ctx->counts2[k2];
          }
          w1_bw_put(bw, 0, 1);   /* end of transforms */
          w1_le_scan_present(resB, n_idx, presentP);
          w1_le_stream(ctx, resB, new_w, h, lvl, (int)w1_le_depths[lvl],
                       cb_idx, flat, presentP, predres, bw);
          return;
        }
        if (!useC) ctx->pal_seen = 1;
        if (use_pred)
          for (k2 = 0; k2 < nxm; k2++) modes[k2] = svmodes[k2];
        if (use_color)
          for (k2 = 0; k2 < nx3; k2++) ximg[k2] = svximg[k2];
        for (k2 = 0; k2 < W1_LE_NC; k2++) ctx->counts[k2] = ctx->counts3[k2];
      }
    }
    img = res;
    }
  } else if (w1_le_uniform(orig, n)) {
    /* One colour (levels 0-5): the raw stream is a handful of tokens, so
     * skip the predictor passes; green only where the level applies it. */
    if (level >= 1) {
      for (k = 0; k < n; k++) res[k] = orig[k];
      use_green = w1_le_sub_green(res, n, ctx->htmp);
      img = res;
    }
    w1_le_estimate(ctx, img, w, h, level, cache_bits, flat, ctx->counts);
  } else if (level >= W1_LE_MID_LEVEL) {
    /* Mid effort: subtract-green when it helps, then the per-tile spatial
     * predictor at one tile size (32x32), adopted when the LZ-aware
     * estimate says it beats the green-only image. No color transform,
     * tie resolve, branch duel or alternative streams (those are level 6). */
    int tiebuf[W1_LE_MAXTIES * 2], nties = 0, twm, thm;
    uint64_t co, cr, penalty;
    pred_sb = 5;
    twm = (w + (1 << pred_sb) - 1) >> pred_sb;
    thm = (h + (1 << pred_sb) - 1) >> pred_sb;
    for (k = 0; k < n; k++) res[k] = orig[k];
    use_green = w1_le_sub_green(res, n, ctx->htmp);
    w1_le_predict(ctx, res, predres, w, h, pred_sb, modes, W1_LE_PRED_BONUS,
                  tiebuf, &nties);
    for (k = 0; k < twm * thm; k++) modepix[k] = (uint32_t)modes[k] << 8;
    penalty = w1_le_estimate(ctx, modepix, twm, thm, 1, 0, flat, ctx->counts) +
              300;
    co = w1_le_estimate(ctx, res, w, h, level, cache_bits, flat, ctx->counts2);
    cr = w1_le_estimate(ctx, predres, w, h, level, cache_bits, flat,
                        ctx->counts);
    main_est = cr + penalty < co ? cr + penalty : co;
    if (cr + penalty < co) {
      use_pred = 1;
      img = predres;
      if (level >= 4) {
        w1_bw_t original;
        uint64_t color_cost;
        w1_bw_init(&original, NULL, 0);
        w1_le_emit_image(ctx, img, w, h, level, cache_bits, use_green,
            1, 0, pred_sb, flat, modes, modepix, ximg, NULL, &original);
        w1_le_estimate(ctx, img, w, h, level, cache_bits, flat, ctx->counts);
        use_color = w1_le_color(ctx, predres, w, h, level, cache_bits,
            flat, ximg, cr, &color_cost);
        if (use_color) {
          w1_bw_t colored;
          w1_bw_init(&colored, NULL, 0);
          w1_le_emit_image(ctx, img, w, h, level, cache_bits, use_green,
              1, 1, pred_sb, flat, modes, modepix, ximg, NULL, &colored);
          if (w1_bw_bit_size(&colored) >= w1_bw_bit_size(&original)) {
            int tx, ty, cw = (w + (1 << W1_CT_SB) - 1) >> W1_CT_SB;
            int ch = (h + (1 << W1_CT_SB) - 1) >> W1_CT_SB;
            for (ty = 0; ty < ch; ty++) for (tx = 0; tx < cw; tx++) {
              uint32_t cc = ximg[ty * cw + tx];
              w1_le_ct_tile(predres, w, h, tx, ty, W1_CT_SB,
                  w1_le_s8(cc & 255), w1_le_s8((cc >> 8) & 255),
                  w1_le_s8((cc >> 16) & 255), -1);
            }
            use_color = 0;
          }
        }
        w1_le_estimate(ctx, img, w, h, level, cache_bits, flat, ctx->counts);
      }
    } else {
      for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
      img = res;
    }
  } else {
    /* Low effort (0-2): subtract-green (1-2 only), then one fixed spatial
     * predictor for the whole image (Select, the low-effort choice) at
     * the largest tile size, adopted when the LZ-aware estimate beats the
     * unpredicted image by more than the (tiny, constant) mode image.
     * Before this, 0-2 had no predictor: photo/gradient 512 were ~400 KB
     * where the predicted stream is a few KB for one extra pixel pass. */
    int tx, ty, twm, thm;
    uint64_t co, cr;
    pred_sb = 9;
    twm = (w + (1 << pred_sb) - 1) >> pred_sb;
    thm = (h + (1 << pred_sb) - 1) >> pred_sb;
    for (k = 0; k < n; k++) res[k] = orig[k];
    use_green = level >= 1
        ? w1_le_sub_green_m(res, n, ctx->htmp,
                            w1_le_depths[level] <= 32 ? 12 : 16)
        : 0;
    if (level == 2) {
      /* Lv2 only: full 14-mode search at same sb=9 (usually 1 tile).
       * NULL tiebuf skips resolve; gate/threshold/counts unchanged.
       * Lv0/1 keep fixed Select; lv>=3 never reaches here. */
      w1_le_predict(ctx, res, predres, w, h, pred_sb, modes,
                    W1_LE_PRED_BONUS, NULL, NULL);
    } else {
      uint64_t cr11, cr12;
      for (ty = 0; ty < thm; ty++) for (tx = 0; tx < twm; tx++) {
        modes[ty * twm + tx] = 11;
        w1_le_retile(res, predres, w, h, pred_sb, tx, ty, 11);
      }
      cr11 = w1_le_estimate(ctx, predres, w, h, level, cache_bits, flat,
                            ctx->counts);
      /* mode 12 = addsub_full(L,T,TL): exact on separable-linear ramps. */
      for (ty = 0; ty < thm; ty++) for (tx = 0; tx < twm; tx++) {
        modes[ty * twm + tx] = 12;
        w1_le_retile(res, predres, w, h, pred_sb, tx, ty, 12);
      }
      cr12 = w1_le_estimate(ctx, predres, w, h, level, cache_bits, flat,
                            ctx->counts);
      if (!(cr12 * 64 < cr11 * 63)) {          /* need >1.56% to flip */
        for (ty = 0; ty < thm; ty++) for (tx = 0; tx < twm; tx++) {
          modes[ty * twm + tx] = 11;
          w1_le_retile(res, predres, w, h, pred_sb, tx, ty, 11);
        }
      }
    }
    co = w1_le_estimate(ctx, res, w, h, level, cache_bits, flat,
                        ctx->counts2);
    cr = w1_le_estimate(ctx, predres, w, h, level, cache_bits, flat,
                        ctx->counts);
    if (cr + 400 < co) {
      use_pred = 1;
      img = predres;
      main_est = cr + 400;
    } else {
      for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
      img = res;
      main_est = co;
    }
  }
  /* Palette trial (levels 0-5): <= 256 colours -> indexed image, raw
   * (no predictor) at this level's depth/cache, adopted when its estimate
   * plus the delta-coded table beats the chosen ARGB stream. The reference does
   * this at every method; without it text 512 at L0 was 5788 B vs 660. The
   * hashed colour scan is one O(n) pass and bails at 257 colours. */
  if (main_est != (uint64_t)-1 && n <= W1_LE_PAL_MAXN) {
    uint32_t ptab[256];
    int ts = w1_le_pal_table_fast(orig, n, ptab);
    if (ts >= W1_LE_PAL_MINTS && ts <= 256) {
      uint32_t dtab[256];
      uint32_t *idx = img == res ? predres : res;
      int wb = w1_le_pal_wb(ts), new_w = (w + (1 << wb) - 1) >> wb;
      int n_idx = new_w * h, cb_idx = cache_bits;
      int lvl = w1_le_idx_level(level);
      uint64_t tbl_est, pal_est;
      while (cb_idx > 0 && (1 << cb_idx) > n_idx) cb_idx--;
      w1_le_pal_sort(ptab, ts);
      w1_le_pal_pack(orig, w, h, ptab, ts, wb, new_w, idx);
      w1_le_pal_delta(ptab, ts, dtab);
      for (k = 0; k < W1_LE_NC; k++) ctx->counts3[k] = ctx->counts[k];
      tbl_est = w1_le_estimate(ctx, dtab, ts, 1, 1, 0, flat, ctx->counts2);
      pal_est = w1_le_estimate(ctx, idx, new_w, h, lvl, cb_idx, flat,
                               ctx->counts) + tbl_est + 300;
      if (pal_est < main_est) {
        uint8_t presentP[1024];
        w1_bw_put(bw, 1, 1);
        w1_bw_put(bw, 3, 2);   /* colour indexing */
        w1_bw_put(bw, (uint32_t)(ts - 1), 8);
        w1_bw_put(bw, 0, 1);   /* table sub-image: no cache */
        for (k = 0; k < W1_LE_NC; k++) ctx->counts2[k] = ctx->counts[k];
        w1_le_encode_tokens(ctx, dtab, ts, 1, 1, 0, flat, bw, NULL);
        for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
        w1_bw_put(bw, 0, 1);   /* end of transforms */
        w1_le_scan_present(idx, n_idx, presentP);
        /* meta: spare full-size buffer (the abandoned ARGB stream), never
         * NULL, so the literal-stream group map always has storage. */
        w1_le_stream(ctx, idx, new_w, h, lvl, (int)w1_le_depths[lvl],
                     cb_idx, flat, presentP,
                     idx == res ? predres : res, bw);
        return;
      }
      for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts3[k];
    }
  }
  {
    w1_bw_t start = *bw;
    w1_le_emit_image(ctx, img, w, h, level, cache_bits, use_green,
        use_pred, use_color, pred_sb, flat, modes, modepix, ximg, predres, bw);
    if (level >= 9 && n >= 65536 && !w1_le_uniform(orig, n) && w1_le_smooth(orig, w, h)) {
      static const int candidates[4] = {1, 2, 12, 13};
      uint64_t best = bw->err ? UINT64_MAX : w1_bw_bit_size(bw) - w1_bw_bit_size(&start);
      int i, green, best_mode = -1, best_green = 0;
      int last_mode = -1, last_green = 0, last_flags = 0;
      for (green = 0; green < (level >= 9 ? 2 : 1); green++)
        for (i = 0; i < (level >= 9 ? 4 : 2); i++) {
        w1_bw_t trial;
        w1_bw_init(&trial, NULL, 0);
        last_flags = w1_le_uniform_stream(ctx, orig, resB, predres, w, h, level,
            cache_bits, 5, candidates[i], green, flat, modes, modepix, ximg, &trial);
        last_mode = candidates[i]; last_green = green;
        if (!trial.err && w1_bw_bit_size(&trial) < best) {
          best = w1_bw_bit_size(&trial); best_mode = candidates[i]; best_green = green;
        }
      }
      if (best_mode >= 0) {
        *bw = start;
        if (best_mode == last_mode && best_green == last_green) {
          w1_le_estimate(ctx, resB, w, h, level, cache_bits, flat, ctx->counts);
          w1_le_emit_image(ctx, resB, w, h, level, cache_bits, last_flags & 1,
              1, last_flags >> 1, 5, flat, modes, modepix, ximg, predres, bw);
        } else {
          w1_le_uniform_stream(ctx, orig, resB, predres, w, h, level,
              cache_bits, 5, best_mode, best_green, flat, modes, modepix, ximg, bw);
        }
      }
    }
  }
}

/* Carve + init the encoder context. flat gets constant costs (chan 8,
 * length/dist 6). Returns 0 on OOM. */
static W1_UNUSED int w1_le_make_ctx(w1_bump_t *bump, int max_n,
                                    int opt_parse, w1_le_ctx_t *ctx,
                                    uint8_t **flat_out) {
  uint8_t *flat;
  int i, hbits = 12;
  if (max_n < 1 || (size_t)max_n > (size_t)-1 / 4) return 0;
  /* Hash heads: about two per position, 16 KB .. 1 MB (the fill runs once
   * per distinct pixel array, so its memset is not per pass). */
  while (hbits < 18 && (1 << hbits) < 2 * max_n) hbits++;
  ctx->cand.hbits = hbits;
  ctx->cand.nb = 1 << hbits;
  ctx->cand.list = (uint32_t *)w1_bump_alloc(
      bump, ((size_t)max_n + (size_t)ctx->cand.nb + 1) * 4, 4);
  ctx->cand.slot = (uint32_t *)w1_bump_alloc(bump, (size_t)max_n * 4, 4);
  ctx->cand.off =
      (int *)w1_bump_alloc(bump, ((size_t)ctx->cand.nb + 1) * 4, 4);
  ctx->cand.cur = (int *)w1_bump_alloc(bump, (size_t)ctx->cand.nb * 4, 4);
  ctx->cand.head = (int *)w1_bump_alloc(bump, (size_t)ctx->cand.nb * 4, 4);
  ctx->max_n = max_n;
  ctx->sv_dst = (uint32_t *)w1_bump_alloc(bump, (size_t)max_n * 4, 4);
  ctx->sv_modes = (uint8_t *)w1_bump_alloc(bump, (size_t)max_n, 1);
  ctx->sv_ok = 0; ctx->sv_src = NULL; ctx->sv_sum = 0;
  ctx->sv_w = ctx->sv_h = ctx->sv_sb = ctx->sv_level = ctx->sv_cb = -1;
  ctx->tab_lru = 0;
  for (i = 0; i < 2; i++) {
    ctx->tab[i] = (uint32_t *)w1_bump_alloc(bump, (size_t)max_n * 4, 4);
    ctx->near[i] = NULL;
    ctx->tab_valid[i] = 0;
    ctx->tab_uses[i] = 0;
    ctx->tk[i].tok = (uint32_t *)w1_bump_alloc(bump, (size_t)max_n * 4, 4);
    ctx->tk[i].lens =
        (uint8_t *)w1_bump_alloc(bump, (size_t)W1_LE_MAXG * W1_LE_NC, 1);
    ctx->tk[i].valid = 0;
    if (!ctx->tab[i] || !ctx->tk[i].tok || !ctx->tk[i].lens) return 0;
  }
  ctx->uni_force = -1; ctx->uni_n = 0;
  ctx->sg_force = -1; ctx->sg_used = -1;
  ctx->pal_force = -1; ctx->pal_seen = 0;
  ctx->sb_hint = -1; ctx->sb_used = -1; ctx->sb_alt = -1;
  ctx->counts = (int *)w1_bump_alloc(bump, (size_t)W1_LE_NC * 4, 4);
  ctx->counts2 = (int *)w1_bump_alloc(bump, (size_t)W1_LE_NC * 4, 4);
  ctx->counts3 = (int *)w1_bump_alloc(bump, (size_t)W1_LE_NC * 4, 4);
  ctx->counts4 = (int *)w1_bump_alloc(bump, (size_t)W1_LE_NC * 4, 4);
  ctx->lens = (uint8_t *)w1_bump_alloc(bump, W1_LE_NC, 1);
  ctx->codes = (int *)w1_bump_alloc(bump, (size_t)W1_LE_NC * 4, 4);
  ctx->seq = (int *)w1_bump_alloc(bump, (size_t)2 * W1_LE_NG * 4, 4);
  ctx->htmp = (int *)w1_bump_alloc(bump, (size_t)5 * W1_LE_NG * 4, 4);
  ctx->cache = (uint32_t *)w1_bump_alloc(bump, 2048 * 4, 4);
  ctx->phist = (int *)w1_bump_alloc(bump, (size_t)14 * 1024 * 4, 4);
  ctx->gcounts = (int *)w1_bump_alloc(bump, (size_t)W1_LE_MAXG * W1_LE_NC * 4, 4);
  ctx->grcounts = (int *)w1_bump_alloc(bump, (size_t)W1_LE_MAXG * W1_LE_NC * 4, 4);
  ctx->glens = (uint8_t *)w1_bump_alloc(bump, (size_t)W1_LE_MAXG * W1_LE_NC, 1);
  ctx->gcodes = (int *)w1_bump_alloc(bump, (size_t)W1_LE_MAXG * W1_LE_NC * 4, 4);
  /* Optimal-parse scratch: 8 B/px, when the caller's effort tier allows it
   * and the image is within the cap (bigger ones keep the greedy+lazy
   * tokenizer). Missing arena -> greedy. */
  ctx->dp = NULL; ctx->back = NULL; ctx->opt_cap = 0;
  if (opt_parse && max_n <= W1_LZ_OPT_MAXN) {
    ctx->dp = (uint32_t *)w1_bump_alloc(bump, ((size_t)max_n + 1) * 4, 4);
    ctx->back = (uint32_t *)w1_bump_alloc(bump, ((size_t)max_n + 1) * 4, 4);
    if (ctx->dp && ctx->back) ctx->opt_cap = max_n;
    else { ctx->dp = NULL; ctx->back = NULL; }
    if (ctx->opt_cap) {
      for (i = 0; i < 2; i++)
        ctx->near[i] = (uint32_t *)w1_bump_alloc(bump, (size_t)max_n * 4, 4);
      if (!ctx->near[0] || !ctx->near[1]) ctx->near[0] = ctx->near[1] = NULL;
    }
  }
  flat = (uint8_t *)w1_bump_alloc(bump, W1_LE_NC, 1);
  if (!ctx->cand.list || !ctx->cand.slot || !ctx->cand.off || !ctx->cand.cur ||
      !ctx->cand.head ||
      !ctx->counts || !ctx->counts2 ||
      !ctx->counts3 || !ctx->counts4 ||
      !ctx->lens ||
      !ctx->codes || !ctx->seq || !ctx->htmp || !ctx->cache ||
      !ctx->phist || !ctx->gcounts || !ctx->grcounts || !ctx->glens ||
      !ctx->gcodes || !flat)
    return 0;
  for (i = 0; i < W1_LE_NC; i++) flat[i] = 8;
  for (i = 256; i < W1_LE_NG; i++) flat[i] = 6;
  for (i = 0; i < 40; i++) flat[W1_LE_OFF_D + i] = 6;
  *flat_out = flat;
  return 1;
}

/* RGBA bytes -> ARGB u32. Returns 1 if any pixel has alpha < 255. */
static W1_UNUSED int w1_rgba_to_argb(const uint8_t *rgba, size_t stride,
                                     int w, int h, uint32_t *out) {
  int x, y, has_alpha = 0;
  if (stride == 0) stride = (size_t)w * 4;
  for (y = 0; y < h; y++) {
    const uint8_t *row = rgba + (size_t)y * stride;
    for (x = 0; x < w; x++) {
      unsigned r = row[x * 4], g = row[x * 4 + 1];
      unsigned b = row[x * 4 + 2], a = row[x * 4 + 3];
      if (a < 255) has_alpha = 1;
      out[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                       ((uint32_t)g << 8) | (uint32_t)b;
    }
  }
  return has_alpha;
}

/* Cross-run pins for w1_vp8l_encode_full's subtract-green duel. */
typedef struct {
  int sg_force;    /* in:  -1 auto, 0/1 pin the green branch off/on */
  int sg_used;     /* out: branch taken, -1 if the stream cannot depend on it */
  int sb_hint;     /* in:  -1 search, else the predictor tile size to use */
  int sb_used;     /* out: the tile size the run used, -1 if it used none */
  int sb_alt;      /* out: runner-up tile size worth a re-encode, else -1 */
  int pal_force;   /* in:  1 accepts the palette trial whenever it exists */
  int pal_seen;    /* out: a palette candidate existed and was turned down */
} w1_le_pin_t;

/* One VP8L bitstream (magic + dims + main). pix is the caller's ARGB. */
static W1_UNUSED int w1_vp8l_encode_run(const uint32_t *pix, int w, int h,
                                        int level, int has_alpha, int opt_parse,
                                        uint8_t *out, size_t out_cap,
                                        size_t *out_len, w1_bump_t *bump,
                                        int uni_force, int *uni_rank,
                                        int *uni_n, w1_le_pin_t *pin) {
  w1_le_ctx_t ctx;
  uint8_t *flat, *modes = NULL;
  uint32_t *res = NULL, *predres = NULL, *modepix = NULL, *ximg = NULL;
  uint32_t *resB = NULL, *svximg = NULL;
  uint8_t *svmodes = NULL;
  w1_bw_t bw;
  int n, tw, th;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) return 2;
  if (level < 0) level = 0;
  if (level > 9) level = 9;
  n = w * h;   /* safe: dims validated (<= 16384) */
  tw = (w + 7) >> 3; th = (h + 7) >> 3;
  if (!w1_le_make_ctx(bump, n, opt_parse, &ctx, &flat))
    return 1;
  ctx.uni_force = uni_force;
  if (pin) {
    ctx.sg_force = pin->sg_force; ctx.sb_hint = pin->sb_hint;
    ctx.pal_force = pin->pal_force;
  }
  res = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
  if (!res) return 1;
  {
    modes = (uint8_t *)w1_bump_alloc(bump, (size_t)tw * (size_t)th, 1);
    modepix = (uint32_t *)w1_bump_alloc(bump,
                                        (size_t)tw * (size_t)th * 4, 4);
    predres = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
    if (!modes || !modepix || !predres) return 1;
  }
  if (level >= 4) {
    ximg = (uint32_t *)w1_bump_alloc(bump, ((size_t)((w + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * (size_t)((h + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * 4), 4);
    if (!ximg) return 1;
  }
  if (level >= 6) {
    resB = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
    svmodes = (uint8_t *)w1_bump_alloc(bump, (size_t)tw * (size_t)th, 1);
    svximg = (uint32_t *)w1_bump_alloc(bump, ((size_t)((w + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * (size_t)((h + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * 4), 4);
    if (!modes || !modepix || !predres || !ximg || !resB || !svmodes ||
        !svximg) return 1;
  }
  w1_bw_init(&bw, out, out_cap);
  w1_bw_put(&bw, W1_VP8L_MAGIC, 8);
  w1_bw_put(&bw, (uint32_t)(w - 1), 14);
  w1_bw_put(&bw, (uint32_t)(h - 1), 14);
  w1_bw_put(&bw, has_alpha ? 1u : 0u, 1);
  w1_bw_put(&bw, 0, 3);   /* version */
  w1_le_main(&ctx, pix, res, predres, resB, svmodes, svximg, w, h, level,
               flat, modes, modepix, ximg, &bw);
  *out_len = w1_bw_flush(&bw, out);
  if (uni_rank) {
    int i;
    for (i = 0; i < ctx.uni_n; i++) uni_rank[i] = ctx.uni_rank[i];
    *uni_n = ctx.uni_n;
  }
  if (pin) {
    pin->sg_used = ctx.sg_used; pin->sb_used = ctx.sb_used;
    pin->sb_alt = ctx.sb_alt;
    pin->pal_seen = ctx.pal_seen;
  }
  return bw.err ? 3 : 0;
}

static W1_UNUSED int w1_vp8l_encode_full(const uint32_t *pix, int w, int h,
                                         int level, int has_alpha,
                                         uint8_t *out, size_t out_cap,
                                         size_t *out_len, w1_bump_t *bump) {
  uint64_t npix = (uint64_t)(unsigned)w * (uint64_t)(unsigned)h;
  uint64_t bound = (uint64_t)8 * npix + 65536 + 256;
  size_t mark = bump->used, smark, cap, len6 = 0, len8 = 0, best_len;
  uint8_t *tmp;
  int rc, best_level, opt, rank[14], nrank = 0, rank8[14], nrank8 = 0;
  w1_le_pin_t p6, p8, pg, pb, pp, pu;
  int win_sg = -1, win_sb = -1, win_pal = 0, win_sb_alt = -1;
  p6.sg_force = p8.sg_force = -1;
  p6.sb_hint = p8.sb_hint = -1;
  p6.sg_used = p8.sg_used = -1;
  p6.pal_force = p8.pal_force = -1;
  p6.pal_seen = p8.pal_seen = 0;
  p6.sb_used = p8.sb_used = -1;
  p6.sb_alt = p8.sb_alt = -1;
  if (level < 0) level = 0;
  if (level > 9) level = 9;
  if (level < 9) {
    rc = w1_vp8l_encode_run(pix, w, h, level, has_alpha, 0, out,
                            out_cap, out_len, bump, -1, NULL, NULL, NULL);
    bump->used = mark;
    return rc;
  }
  opt = level >= W1_LZ_OPT_LEVEL;
  if (bound > (uint64_t)(size_t)-1) return 2;
  cap = out_cap < (size_t)bound ? out_cap : (size_t)bound;
  tmp = (uint8_t *)w1_bump_alloc(bump, cap ? cap : 1, 1);
  if (!tmp)
    return w1_vp8l_encode_run(pix, w, h, level, has_alpha, opt, out,
                              out_cap, out_len, bump, -1, NULL, NULL,
                              NULL);
  smark = bump->used;
  rc = w1_vp8l_encode_run(pix, w, h, 6, has_alpha, opt, out, out_cap,
                          &len6, bump, -1, rank, &nrank, &p6);
  if (rc != 0) { bump->used = mark; return rc; }
  *out_len = len6;
  best_len = len6; best_level = 6;
  if (len6 <= cap) {
    memcpy(tmp, out, len6);           /* keep the level-6 payload */
    bump->used = smark;               /* reuse its scratch for level 8 */
    rc = w1_vp8l_encode_run(pix, w, h, 8, has_alpha, opt, out, out_cap,
                            &len8, bump, -1, rank8, &nrank8, &p8);
    if (rc != 0 || len8 >= len6) {    /* level 8 lost (or failed): restore */
      memcpy(out, tmp, len6);
      *out_len = len6;
      rc = 0;
    } else {
      *out_len = len8; best_len = len8; best_level = 8;
      memcpy(tmp, out, len8);
      if (nrank8) { memcpy(rank, rank8, sizeof(rank)); nrank = nrank8; }
      p6 = p8;
    }
    /* What the current winner (the level-6 or level-8 run) did, so a
     * pinned re-encode can repeat it instead of searching again. */
    win_sb = p6.sb_used; win_pal = p6.pal_seen; win_sg = p6.sg_used;
    win_sb_alt = p6.sb_alt;
    /* Subtract-green duel: w1_le_main's branch gate picks green from
     * depth-640 entropy estimates whose error is the size of the difference
     * they decide (measured over the real corpus: ratios 0.93-1.02 while the
     * truth flips inside that span), so no threshold on them is right
     * everywhere -- forcing green costs photo_meteor 2.5% and the default
     * costs photo_based 3.0%. Re-encode the winner with the branch pinned
     * the other way and keep it only when the finished stream is strictly
     * smaller: the decision becomes exact and cannot grow a file. Three
     * things keep the cost below a full extra encode: the pinned run skips
     * the branch it is pinned away from, sg_used == -1 means the stream
     * cannot depend on green at all (hopeless/uniform fast path, palette-only
     * shortcut) so no re-encode runs, and the run is handed the winning
     * pred_sb, which w1_le_pick_sb would re-derive identically because it
     * ranks tile size on the pre-green image. */
    if (p6.sg_used >= 0 && best_len <= cap) {
      size_t leng = 0;
      int rankg[14], nrankg = 0;
      pg.sg_force = p6.sg_used ? 0 : 1;
      pg.sb_hint = p6.sb_used;
      pg.sg_used = pg.sb_used = -1;
      pg.pal_force = -1; pg.pal_seen = 0;
      bump->used = smark;
      rc = w1_vp8l_encode_run(pix, w, h, best_level, has_alpha, opt, out,
                              out_cap, &leng, bump, -1, rankg, &nrankg, &pg);
      if (rc == 0 && leng < best_len && leng <= cap) {
        best_len = leng; memcpy(tmp, out, leng);
        if (nrankg) { memcpy(rank, rankg, sizeof(rank)); nrank = nrankg; }
        win_sg = pg.sg_force; win_sb = pg.sb_used; win_pal = pg.pal_seen;
      }
      memcpy(out, tmp, best_len);
      *out_len = best_len;
      rc = 0;
    }
    /* Tile-size duel, same shape again. w1_le_pick_sb walks sb in steps of
     * 2, so a winner that is not an endpoint leaves its two neighbours
     * untried, and those are the tilings libwebp reaches for: it picks 4 on
     * photo_meteor and 3 on art_pack where this ladder offers 3/5 and lands
     * on 5. Scoring the neighbours inside pick_sb and returning the better
     * one is a regression - the estimate doing the ranking is the same one
     * the duels exist to distrust, and on edges-512 it likes 4 enough to
     * code 140 B worse at every level. So pick_sb returns the ladder winner
     * unchanged and reports the refined pick as sb_alt; the re-encode here
     * settles it on finished streams (art_pack 8998 -> 8944, photo_meteor
     * 1415652 -> 1411584, edges-512 unmoved). It runs only when the two
     * disagree, and it repeats no green search. */
    if (win_sb_alt >= 0 && win_sb_alt != win_sb && best_len <= cap) {
      size_t lenb = 0;
      int rankb[14], nrankb = 0;
      pb.sg_force = win_sg; pb.sb_hint = win_sb_alt;
      pb.sg_used = pb.sb_used = -1; pb.sb_alt = -1;
      pb.pal_force = -1; pb.pal_seen = 0;
      bump->used = smark;
      rc = w1_vp8l_encode_run(pix, w, h, best_level, has_alpha, opt, out,
                              out_cap, &lenb, bump, -1, rankb, &nrankb, &pb);
      if (rc == 0 && lenb < best_len && lenb <= cap) {
        best_len = lenb; memcpy(tmp, out, lenb);
        if (nrankb) { memcpy(rank, rankb, sizeof(rank)); nrank = nrankb; }
        win_sb = win_sb_alt; win_pal = pb.pal_seen;
      }
      memcpy(out, tmp, best_len);
      *out_len = best_len;
      rc = 0;
    }
    /* Palette duel, same shape as the subtract-green one above. The
     * palette+spatial trial in w1_le_main is accepted on an estimate
     * (totP vs totW), and on the palette16 generator that estimate is wrong
     * by more than any margin can absorb: dropping the +300 bias to 0
     * changes nothing, while forcing the trial through is 22 B smaller at
     * 64x64 (2166 -> 2144), 22 B at 128, 34 B at 256 - enough to turn three
     * libwebp losses into wins. So when a candidate existed and the estimate
     * turned it down, re-encode with it forced and keep that stream only if
     * it is strictly smaller. The re-encode is handed the green branch and
     * tile size the current winner used, so it repeats none of that search,
     * and it only runs for images with at most 256 distinct colours. */
    if (win_pal && best_len <= cap) {
      size_t lenp = 0;
      int rankp[14], nrankp = 0;
      /* The palette stream is built from the original pixels, so the
       * green branch cannot change it; pinning the branch the winner
       * took just stops this run re-deciding it, and the non-palette
       * stream it would fall back to is the one we already hold. */
      pp.sg_force = win_sg; pp.sb_hint = win_sb;
      pp.sg_used = pp.sb_used = -1;
      pp.pal_force = 1; pp.pal_seen = 0;
      bump->used = smark;
      rc = w1_vp8l_encode_run(pix, w, h, best_level, has_alpha, opt, out,
                              out_cap, &lenp, bump, -1, rankp, &nrankp, &pp);
      if (rc == 0 && lenp < best_len && lenp <= cap) {
        best_len = lenp; memcpy(tmp, out, lenp);
        if (nrankp) { memcpy(rank, rankp, sizeof(rank)); nrank = nrankp; }
      }
      memcpy(out, tmp, best_len);
      *out_len = best_len;
      rc = 0;
    }
    /* Uniform-predictor portfolio: the winning level again with each of
     * the best-ranked single modes forced everywhere; the smallest
     * finished stream wins (tmp always holds the current winner). When a
     * palette candidate was turned down, the same modes are tried a second
     * time with the palette forced through: on palette16-128 neither the
     * palette duel nor the plain portfolio finds the winner, because it is
     * the combination (2180 -> 2158) that wins. The extra pass only runs
     * for images with at most 256 distinct colours. */
    if (npix <= (uint64_t)W1_LE_UNI_PORTFOLIO_MAXN && best_len <= cap) {
      int i, pf;
      for (pf = 0; pf < (win_pal ? 2 : 1); pf++) {
        pu.sg_force = pf ? win_sg : -1; pu.sb_hint = pf ? win_sb : -1;
        pu.sg_used = pu.sb_used = -1;
        pu.pal_force = pf ? 1 : -1; pu.pal_seen = 0;
        for (i = 0; i < nrank && i < W1_LE_UNI_PORTFOLIO_K; i++) {
          size_t lenu = 0;
          bump->used = smark;
          rc = w1_vp8l_encode_run(pix, w, h, best_level, has_alpha, opt, out,
                                  out_cap, &lenu, bump, rank[i], NULL, NULL,
                                  &pu);
          if (rc == 0 && lenu < best_len && lenu <= cap) {
            best_len = lenu; memcpy(tmp, out, lenu);
          }
        }
      }
      memcpy(out, tmp, best_len);
      *out_len = best_len;
      rc = 0;
    }
  }
  bump->used = mark;
  return rc;
}

/* Forward ALPH filter (mirror of w1_alph_unfilter). */
static W1_UNUSED void w1_alph_filter(int filter, const uint8_t *prev,
                                     const uint8_t *in, uint8_t *out, int w) {
  int i;
  if (filter == 0) {
    for (i = 0; i < w; i++) out[i] = in[i];
  } else if (filter == 1) {
    int pred = prev ? prev[0] : 0;
    for (i = 0; i < w; i++) { out[i] = (uint8_t)((in[i] - pred) & 0xff); pred = in[i]; }
  } else if (filter == 2) {
    if (!prev) {
      int pred = 0;
      for (i = 0; i < w; i++) { out[i] = (uint8_t)((in[i] - pred) & 0xff); pred = in[i]; }
    } else {
      for (i = 0; i < w; i++) out[i] = (uint8_t)((in[i] - prev[i]) & 0xff);
    }
  } else {
    if (!prev) {
      int pred = 0;
      for (i = 0; i < w; i++) { out[i] = (uint8_t)((in[i] - pred) & 0xff); pred = in[i]; }
    } else {
      int left = prev[0];
      for (i = 0; i < w; i++) {
        int top = prev[i], topleft = i ? prev[i - 1] : prev[0];
        out[i] = (uint8_t)((in[i] - w1_grad_pred(left, top, topleft)) & 0xff);
        left = in[i];
      }
    }
  }
}

/* ALPH chunk payload: method byte + raw deltas or VP8L (magic + main).
 * Encodes VP8L directly into out (after reserving byte 0), falls back to
 * raw if smaller. Returns 0 ok, 1 OOM, 2 bad param, 3 output full. */
static W1_UNUSED int w1_alph_encode(const uint8_t *alpha, int w, int h,
                                    int level, uint8_t *out, size_t out_cap,
                                    size_t *out_len, w1_bump_t *bump) {
  int n, f, y, best_f = 0;
  uint64_t best_l1 = (uint64_t)-1;
  uint8_t *deltas;
  uint32_t *pix;
  w1_le_ctx_t ctx;
  uint8_t *flat, *modes = NULL;
  uint32_t *res32 = NULL, *predres = NULL, *modepix = NULL, *ximg = NULL;
  uint32_t *resB = NULL, *svximg = NULL;
  uint8_t *svmodes = NULL;
  w1_bw_t bw;
  size_t vlen;
  int tw, th, i;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) return 2;
  if (!alpha || !out || !out_len) return 2;
  if (level < 0) level = 0;
  if (level > 9) level = 9;
  if (out_cap < 1) return 3;
  n = w * h;   /* safe: dims validated (<= 16384) */
  deltas = (uint8_t *)w1_bump_alloc(bump, (size_t)n, 1);
  pix = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
  if (!deltas || !pix) return 1;
  /* Pick filter by L1 of deltas (cheap proxy for size). */
  {
    uint8_t *rowbuf = (uint8_t *)w1_bump_alloc(bump, (size_t)w, 1);
    if (!rowbuf) return 1;
    for (f = 0; f < 4; f++) {
      uint64_t t = 0;
      for (y = 0; y < h; y++) {
        const uint8_t *row = alpha + (size_t)y * (size_t)w;
        w1_alph_filter(f, y ? row - w : NULL, row, rowbuf, w);
        for (i = 0; i < w; i++) t += (uint64_t)(unsigned)w1_le_sabs(rowbuf[i]);
      }
      if (t < best_l1) { best_l1 = t; best_f = f; }
    }
    for (y = 0; y < h; y++) {
      const uint8_t *row = alpha + (size_t)y * (size_t)w;
      w1_alph_filter(best_f, y ? row - w : NULL, row, deltas + (size_t)y * w, w);
    }
  }
  for (i = 0; i < n; i++) pix[i] = (uint32_t)deltas[i] << 8;
  tw = (w + 7) >> 3; th = (h + 7) >> 3;
  if (!w1_le_make_ctx(bump, n, level >= 9, &ctx, &flat))
    return 1;
  res32 = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
  if (!res32) return 1;
  {
    modes = (uint8_t *)w1_bump_alloc(bump, (size_t)tw * (size_t)th, 1);
    modepix = (uint32_t *)w1_bump_alloc(bump, (size_t)tw * (size_t)th * 4, 4);
    predres = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
    if (!modes || !modepix || !predres) return 1;
  }
  if (level >= 4) {
    ximg = (uint32_t *)w1_bump_alloc(bump, ((size_t)((w + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * (size_t)((h + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * 4), 4);
    if (!ximg) return 1;
  }
  if (level >= 6) {
    resB = (uint32_t *)w1_bump_alloc(bump, (size_t)n * 4, 4);
    svmodes = (uint8_t *)w1_bump_alloc(bump, (size_t)tw * (size_t)th, 1);
    svximg = (uint32_t *)w1_bump_alloc(bump, ((size_t)((w + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * (size_t)((h + (1 << W1_CT_SB) - 1) >> W1_CT_SB) * 4), 4);
    if (!modes || !modepix || !predres || !ximg || !resB || !svmodes ||
        !svximg) return 1;
  }
  out[0] = 0;   /* overwritten below */
  w1_bw_init(&bw, out + 1, out_cap - 1);
  /* No magic/dims/version: ALPH embeds only the VP8L main stream. */
  w1_le_main(&ctx, pix, res32, predres, resB, svmodes, svximg, w, h,
               level, flat, modes, modepix, ximg, &bw);
  vlen = w1_bw_flush(&bw, out + 1);
  if (!bw.err && vlen + 1 <= (size_t)n + 1) {
    out[0] = (uint8_t)(1 | (best_f << 2));
    *out_len = vlen + 1;
    return 0;
  }
  /* Raw fallback (also used when VP8L didn't fit). */
  if ((size_t)n + 1 > out_cap) return 3;
  out[0] = (uint8_t)(0 | (best_f << 2));
  for (i = 0; i < n; i++) out[1 + i] = deltas[i];
  *out_len = (size_t)n + 1;
  return 0;
}

/* ---- Color transform (type 1) ---- */
/* Forward: g,a unchanged; r' = r - asr(g2r*s8(g)); b' = b - asr(g2b*s8(g))
 * - asr(r2b*s8(r_orig)). Params are int8 stored B=g2r,G=g2b,R=r2b,A=0xff. */
static W1_UNUSED int w1_le_s8(unsigned v) {
  return v >= 128 ? (int)v - 256 : (int)v;
}

/* The tile arrives pre-split into contiguous per-channel arrays (gs = s8
 * green, rv = red, rs = s8 red, bv = blue, n pixels): the search calls
 * this ~70 times for one tile and the pixels never change, so the row
 * -strided ARGB gather and the field extraction happen once per tile in
 * w1_le_ct_search instead of once per candidate triple. */
typedef struct {
  unsigned hist[256];
  uint8_t touched[256];
  uint32_t idx[1 << (2 * W1_CT_SB)];
  int ntouched, nidx;
} w1_le_ct_base_t;

static W1_UNUSED int64_t w1_le_ct_cost(const int8_t *gs, const uint8_t *rv,
                                 const int8_t *rs, const uint8_t *bv,
                                 int g2b, int r2b, int g2r, int what,
                                 const unsigned *accum,
                                 const w1_le_ct_base_t *base) {
  unsigned hist[256];
  int i, ntouched = base->ntouched;
  uint8_t touched[256];
  int64_t cost = 0;
  uint64_t bias, expv = 240 * 1024;
  memcpy(hist, base->hist, sizeof(hist));
  memcpy(touched, base->touched, (size_t)ntouched);
  if (what == 0) {
    for (i = 0; i < base->nidx; i++) {
      int j = (int)base->idx[i];
      int bin = ((int)rv[j] - w1_asr(g2r * (int)gs[j], 5)) & 255;
      if (hist[bin]++ == 0) touched[ntouched++] = (uint8_t)bin;
    }
  } else {
    for (i = 0; i < base->nidx; i++) {
      int j = (int)base->idx[i];
      int bin = ((int)bv[j] - w1_asr(g2b * (int)gs[j], 5) -
                 w1_asr(r2b * (int)rs[j], 5)) & 255;
      if (hist[bin]++ == 0) touched[ntouched++] = (uint8_t)bin;
    }
  }
  for (i = 0; i < ntouched; i++) {
    int bin = touched[i];
    unsigned v = hist[bin];
    cost -= (int64_t)(v <= 256 ? w1k_le_slog[v] : w1_le_slog_big(v));
    if (v) {
      unsigned a = accum[bin], av = a + v;
      cost -= (int64_t)(av <= 256 ? w1k_le_slog[av] : w1_le_slog_big(av));
      cost += (int64_t)(a <= 256 ? w1k_le_slog[a] : w1_le_slog_big(a));
    }
  }
  bias = (uint64_t)hist[0] * 3 * 1024;
  for (i = 1; i < 16; i++) {
    bias += (expv * (hist[i] + hist[256 - i]) + 50) / 100;
    expv = (6 * expv + 5) / 10;
  }
  cost -= (int64_t)((bias + 5) / 10);
  return cost;
}

static W1_UNUSED int w1_le_ct_bonus(int p, int px, int py, int bonus) {
  int b = 0;
  if (p == 0) b += bonus;
  if (p == px) b += bonus;
  if (p == py) b += bonus;
  return b;
}

/* Search best (g2r,g2b,r2b) for one tile: greedy coarse-to-fine from 0
 * (reference color-search order: green-to-red then green-red-to-blue). px/py = accepted
 * left/top params (s8 triples) for the smoothness bonus. */
static W1_UNUSED void w1_le_ct_search(const uint32_t *img, int w, int h,
                                      int tx, int ty, int sb,
                                      const int *px, const int *py, int bonus,
                                      const unsigned *accum,
                                      int *o0, int *o1, int *o2) {
  static const int rd[6] = {32, 16, 8, 4, 2, 1};
  static const int bd[7] = {16, 16, 8, 4, 2, 2, 2};
  static const int ax[8][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0},
                               {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
  int best0 = 0, best1 = 0, best2 = 0, it, a, s;
  int64_t bestc;
  w1_le_ct_base_t red = {0}, blue = {0};
  /* Split the tile once (see w1_le_ct_cost). */
  int8_t gs[1 << (2 * W1_CT_SB)], rs[1 << (2 * W1_CT_SB)];
  uint8_t rv[1 << (2 * W1_CT_SB)], bv[1 << (2 * W1_CT_SB)];
  int x0 = tx << sb, y0 = ty << sb, x1 = x0 + (1 << sb), y1 = y0 + (1 << sb);
  int x, y, n = 0;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      uint32_t q = img[y * w + x];
      unsigned r = (q >> 16) & 0xff;
      gs[n] = (int8_t)(uint8_t)((q >> 8) & 0xff);
      rv[n] = (uint8_t)r;
      rs[n] = (int8_t)(uint8_t)r;
      bv[n] = (uint8_t)(q & 0xff);
      if (gs[n] == 0) {
        unsigned bin = rv[n];
        if (red.hist[bin]++ == 0)
          red.touched[red.ntouched++] = (uint8_t)bin;
      } else red.idx[red.nidx++] = (uint32_t)n;
      if (gs[n] == 0 && rs[n] == 0) {
        unsigned bin = bv[n];
        if (blue.hist[bin]++ == 0)
          blue.touched[blue.ntouched++] = (uint8_t)bin;
      } else blue.idx[blue.nidx++] = (uint32_t)n;
      n++;
    }
  }
  bestc = w1_le_ct_cost(gs, rv, rs, bv, 0, 0, 0, 0, accum, &red) -
              w1_le_ct_bonus(0, px[0], py[0], bonus);
  for (it = 0; it < 6; it++) {
    int d = rd[it];
    for (s = -1; s <= 1; s += 2) {
      int c = best0 + s * d;
      int64_t cc;
      if (c < -128) c = -128;
      if (c > 127) c = 127;
      if (c == best0) continue;
      cc = w1_le_ct_cost(gs, rv, rs, bv, 0, 0, c, 0, accum, &red) -
           w1_le_ct_bonus(c, px[0], py[0], bonus);
      if (cc < bestc) { bestc = cc; best0 = c; }
    }
  }
  *o0 = best0;
  bestc = w1_le_ct_cost(gs, rv, rs, bv, 0, 0, 0, 1, accum + 256, &blue) -
          w1_le_ct_bonus(0, px[1], py[1], bonus) -
          w1_le_ct_bonus(0, px[2], py[2], bonus);
  {
    /* bd repeats scales (16,16,2,2,2). A pass at scale d that moved nothing
     * proved all 8 neighbours are no better than the incumbent, and the
     * next pass at the same d would score the identical eight points, so it
     * is skipped. Exact: costs are pure functions of the candidate. */
    int prev_d = 0, prev_moved = 1;
    for (it = 0; it < 7; it++) {
      int d = bd[it], moved = 0;
      if (d != prev_d || prev_moved) {
        for (a = 0; a < 8; a++) {
          int c1 = best1 + ax[a][0] * d, c2 = best2 + ax[a][1] * d;
          int64_t cc;
          if (c1 < -128) c1 = -128;
          if (c1 > 127) c1 = 127;
          if (c2 < -128) c2 = -128;
          if (c2 > 127) c2 = 127;
          if (c1 == best1 && c2 == best2) continue;
          cc = w1_le_ct_cost(gs, rv, rs, bv, c1, c2, 0, 1, accum + 256, &blue) -
               w1_le_ct_bonus(c1, px[1], py[1], bonus) -
               w1_le_ct_bonus(c2, px[2], py[2], bonus);
          if (cc < bestc) { bestc = cc; best1 = c1; best2 = c2; moved = 1; }
        }
      }
      prev_d = d; prev_moved = moved;
      if (d == 2 && best1 == 0 && best2 == 0) break;
    }
  }
  *o1 = best1;
  *o2 = best2;
}

/* Apply (dir=+1) or invert (dir=-1) one tile with params. In-place safe. */
static W1_UNUSED void w1_le_ct_tile(uint32_t *img, int w, int h,
                                    int tx, int ty, int sb,
                                    int g2r, int g2b, int r2b, int dir) {
  int x0 = tx << sb, y0 = ty << sb, x1 = x0 + (1 << sb), y1 = y0 + (1 << sb);
  int x, y;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      uint32_t px = img[y * w + x];
      int g = w1_le_s8((px >> 8) & 0xff);
      int r = (int)((px >> 16) & 0xff);
      int b = (int)(px & 0xff);
      int dr = w1_asr(g2r * g, 5);
      int nr, nb;
      if (dir > 0) {
        nr = (r - dr) & 0xff;
        nb = (b - w1_asr(g2b * g, 5) - w1_asr(r2b * w1_le_s8((unsigned)r), 5)) & 0xff;
      } else {
        nr = (r + dr) & 0xff;
        nb = (b + w1_asr(g2b * g, 5) + w1_asr(r2b * w1_le_s8((unsigned)nr), 5)) & 0xff;
      }
      img[y * w + x] = (px & 0xff00ff00u) | ((uint32_t)nr << 16) | (uint32_t)nb;
    }
  }
}

/* Full color transform (applied last, after the other tiles). Hierarchical
 * per-tile search with zero/neighbor bonuses (no per-tile gate), then an
 * LZ-aware gate: est(colored) + est(ximg) + trees vs the cached est_cur.
 * On accept, ctx->counts holds the fresh colored counts; on reject the
 * image is inverted back and the winner counts restored. */
static W1_UNUSED int w1_le_color(w1_le_ctx_t *ctx, uint32_t *img, int w, int h,
                                 int level, int cache_bits, const uint8_t *flat,
                                 uint32_t *ximg, uint64_t est_cur,
                                 uint64_t *cost_out) {
  int sb = W1_CT_SB, tw = (w + (1 << W1_CT_SB) - 1) >> W1_CT_SB,
      th = (h + (1 << W1_CT_SB) - 1) >> W1_CT_SB, tx, ty, i, n = w * h;
  int any = 0, bonus = 3 * 1024, k;
  unsigned accum[512] = {0};
  uint64_t est_new, est_x, est_old;
  for (i = 0; i < n; i++) {
    if (img[i] & 0x00ff00ffu) break;
  }
  if (i == n) { *cost_out = est_cur; return 0; }
  for (ty = 0; ty < th; ty++) {
    for (tx = 0; tx < tw; tx++) {
      uint32_t cl = tx ? ximg[ty * tw + tx - 1] : 0xff000000u;
      uint32_t ct = ty ? ximg[(ty - 1) * tw + tx] : 0xff000000u;
      int pxl[3], pyt[3], o[3];
      pxl[0] = w1_le_s8(cl & 0xff);
      pxl[1] = w1_le_s8((cl >> 8) & 0xff);
      pxl[2] = w1_le_s8((cl >> 16) & 0xff);
      pyt[0] = w1_le_s8(ct & 0xff);
      pyt[1] = w1_le_s8((ct >> 8) & 0xff);
      pyt[2] = w1_le_s8((ct >> 16) & 0xff);
      w1_le_ct_search(img, w, h, tx, ty, sb, pxl, pyt, bonus, accum,
                      &o[0], &o[1], &o[2]);
      ximg[ty * tw + tx] = 0xff000000u |
        (((uint32_t)(o[2] & 0xff)) << 16) |
        (((uint32_t)(o[1] & 0xff)) << 8) | (uint32_t)(o[0] & 0xff);
      if (o[0] | o[1] | o[2]) any = 1;
      {
        int x, y, x1 = (tx + 1) << sb, y1 = (ty + 1) << sb;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        for (y = ty << sb; y < y1; y++) {
          for (x = tx << sb; x < x1; x++) {
            uint32_t p = img[y * w + x];
            int g = w1_le_s8((p >> 8) & 255);
            int r = (p >> 16) & 255, b = p & 255;
            accum[(r - w1_asr(o[0] * g, 5)) & 255]++;
            accum[256 + ((b - w1_asr(o[1] * g, 5) -
                          w1_asr(o[2] * w1_le_s8((unsigned)r), 5)) & 255)]++;
          }
        }
      }
    }
  }
  if (!any) { *cost_out = est_cur; return 0; }
  for (k = 0; k < W1_LE_NC; k++) ctx->counts2[k] = ctx->counts[k];
  est_old = w1_le_est2(ctx, img, w, h, level, cache_bits, flat,
                       ctx->counts4, ctx->counts);
  est_x = w1_le_est2(ctx, ximg, tw, th, 1, 0, flat,
                     ctx->counts4, ctx->counts);
  for (ty = 0; ty < th; ty++) {
    for (tx = 0; tx < tw; tx++) {
      uint32_t cc = ximg[ty * tw + tx];
      if (cc == 0xff000000u) continue;
      w1_le_ct_tile(img, w, h, tx, ty, sb, w1_le_s8(cc & 0xff),
                    w1_le_s8((cc >> 8) & 0xff),
                    w1_le_s8((cc >> 16) & 0xff), 1);
    }
  }
  est_new = w1_le_est2(ctx, img, w, h, level, cache_bits, flat,
                       ctx->counts4, ctx->counts);
  if (est_new + est_x + 300 < est_old) {
    uint64_t gain = est_old - (est_new + est_x + 300);
    *cost_out = est_cur > gain ? est_cur - gain : 0;
    return 1;
  }
  *cost_out = est_cur;
  for (ty = 0; ty < th; ty++) {
    for (tx = 0; tx < tw; tx++) {
      uint32_t cc = ximg[ty * tw + tx];
      if (cc == 0xff000000u) continue;
      w1_le_ct_tile(img, w, h, tx, ty, sb, w1_le_s8(cc & 0xff),
                    w1_le_s8((cc >> 8) & 0xff),
                    w1_le_s8((cc >> 16) & 0xff), -1);
    }
  }
  for (k = 0; k < W1_LE_NC; k++) ctx->counts[k] = ctx->counts2[k];
  return 0;
}
/* == S8: VP8 encoder (lossy analysis, RD search, token writing) == */

/* Symmetric round-half-up right shift (matches reference rounding bias). */
static W1_UNUSED int w1_vp8e_rnd_shift(int x, int s) {
  int h = 1 << (s - 1);
  return x >= 0 ? (x + h) >> s : -((h - 1 - x) >> s);
}

/* ---- Forward WHT (exact inverse of w1_vp8_wht): butterfly + >>1 ---- */
static W1_UNUSED void w1_vp8e_fwht(const int16_t *in, int16_t *out) {
  int i, a1, b1, c1, d1, a2, b2, c2, d2;
  int16_t tmp[16], *tp = tmp;
  for (i = 0; i < 4; i++) {
    a1 = in[0] + in[12]; b1 = in[4] + in[8];
    c1 = in[4] - in[8]; d1 = in[0] - in[12];
    tp[0] = (int16_t)(a1 + b1); tp[4] = (int16_t)(c1 + d1);
    tp[8] = (int16_t)(a1 - b1); tp[12] = (int16_t)(d1 - c1);
    in++; tp++;
  }
  tp = tmp;
  for (i = 0; i < 4; i++) {
    a1 = tp[0] + tp[3]; b1 = tp[1] + tp[2];
    c1 = tp[1] - tp[2]; d1 = tp[0] - tp[3];
    a2 = a1 + b1; b2 = c1 + d1; c2 = a1 - b1; d2 = d1 - c1;
    out[0] = (int16_t)w1_vp8e_rnd_shift(a2, 1);
    out[1] = (int16_t)w1_vp8e_rnd_shift(b2, 1);
    out[2] = (int16_t)w1_vp8e_rnd_shift(c2, 1);
    out[3] = (int16_t)w1_vp8e_rnd_shift(d2, 1);
    tp += 4; out += 4;
  }
}

/* ---- Forward 4x4 DCT: exact inverse of w1_vp8_idct_add ----
 * The decoder reconstructs pixels as (M*C*M^T)>>3 with
 *   M = [ 1  a  1  b ; 1  b -1 -a ; 1 -b -1  a ; 1 -a  1 -b ],
 *   a = 1 + 20091/65536 = 1.30656, b = 35468/65536 = 0.54120,
 * and M*M^T = 4*I (so M/2 is orthogonal).  Unity therefore needs the forward
 * to be A = 2*sqrt(2)*M^-1 = (sqrt2/2)*M^T on BOTH axes:
 *   A0 = (1, 1, 1, 1)/sqrt2          A1 = ( a,  b, -b, -a)/sqrt2*... (below)
 * written out with p = (p0..p3), a1 = p0+p3, b1 = p1+p2, c1 = p1-p2,
 * d1 = p0-p3:
 *   f0 = (a1 + b1) * 0.70711
 *   f1 =  d1 * 0.92388 + c1 * 0.38268
 *   f2 = (a1 - b1) * 0.70711
 *   f3 =  d1 * 0.38268 - c1 * 0.92388
 * in 16.16 fixed point (46341 = 65536/sqrt2, 60547 = 65536*cos(pi/8)*...,
 * 25080).  Consequence: a flat residual r gives DC = 8*r, matching the
 * measured d/8 per-pixel DC coupling of the decoder.  |p| <= 255 keeps every
 * intermediate inside int16 and every product inside int32. */
#define W1_E_SQ 46341    /* 65536 / sqrt(2) */
#define W1_E_C1 60547    /* 65536 * sqrt(2) * cos(pi/8)   */
#define W1_E_C2 25080    /* 65536 * sqrt(2) * cos(3pi/8) */

#define W1_E_DCT1(dst, p0, p1, p2, p3)                                    \
  do {                                                                    \
    const int w1e_a1 = (p0) + (p3), w1e_b1 = (p1) + (p2);                 \
    const int w1e_c1 = (p1) - (p2), w1e_d1 = (p0) - (p3);                 \
    (dst)[0] = w1_vp8e_rnd_shift((w1e_a1 + w1e_b1) * W1_E_SQ, 16);        \
    (dst)[1] = w1_vp8e_rnd_shift(w1e_d1 * W1_E_C1 + w1e_c1 * W1_E_C2, 16);\
    (dst)[2] = w1_vp8e_rnd_shift((w1e_a1 - w1e_b1) * W1_E_SQ, 16);        \
    (dst)[3] = w1_vp8e_rnd_shift(w1e_d1 * W1_E_C2 - w1e_c1 * W1_E_C1, 16);\
  } while (0)

static W1_UNUSED void w1_vp8e_fdct_scalar(const int16_t *in, int16_t *out) {
  int i, q[16];
  for (i = 0; i < 4; i++) W1_E_DCT1(&q[i * 4], in[i * 4], in[i * 4 + 1],
                                    in[i * 4 + 2], in[i * 4 + 3]);
  for (i = 0; i < 4; i++) {
    int t[4];
    W1_E_DCT1(t, q[i], q[4 + i], q[8 + i], q[12 + i]);
    out[i] = (int16_t)t[0];
    out[4 + i] = (int16_t)t[1];
    out[8 + i] = (int16_t)t[2];
    out[12 + i] = (int16_t)t[3];
  }
}

#ifdef W1_USE_SSE2
static W1_UNUSED void w1_vp8e_dct4(__m128i *p0, __m128i *p1,
                                  __m128i *p2, __m128i *p3) {
  __m128i a = _mm_add_epi32(*p0, *p3), b = _mm_add_epi32(*p1, *p2);
  __m128i c = _mm_sub_epi32(*p1, *p2), d = _mm_sub_epi32(*p0, *p3);
  __m128i sum = _mm_add_epi32(a, b), diff = _mm_sub_epi32(a, b);
  __m128i packed = _mm_packs_epi32(sum, diff), sq = _mm_set1_epi16(-19195);
  __m128i lo = _mm_mullo_epi16(packed, sq), hi = _mm_mulhi_epi16(packed, sq);
  __m128i dc = _mm_unpacklo_epi16(_mm_packs_epi32(d, d), _mm_packs_epi32(c, c));
  __m128i h = _mm_set1_epi32(32768);
  *p0 = _mm_add_epi32(_mm_unpacklo_epi16(lo, hi), _mm_slli_epi32(sum, 16));
  *p2 = _mm_add_epi32(_mm_unpackhi_epi16(lo, hi), _mm_slli_epi32(diff, 16));
  *p1 = _mm_add_epi32(_mm_madd_epi16(dc, _mm_set1_epi32((25080 << 16) | 60547)),
                       _mm_slli_epi32(d, 16));
  *p3 = _mm_sub_epi32(_mm_madd_epi16(dc, _mm_set1_epi32((4989 << 16) | 25080)),
                       _mm_slli_epi32(c, 16));
  *p0 = _mm_srai_epi32(_mm_add_epi32(*p0, h), 16);
  *p1 = _mm_srai_epi32(_mm_add_epi32(*p1, h), 16);
  *p2 = _mm_srai_epi32(_mm_add_epi32(*p2, h), 16);
  *p3 = _mm_srai_epi32(_mm_add_epi32(*p3, h), 16);
}
#endif
static W1_UNUSED void w1_vp8e_fdct(const int16_t *in, int16_t *out) {
#ifdef W1_USE_SSE2
  __m128i a = _mm_loadl_epi64((const __m128i *)(const void *)in);
  __m128i b = _mm_loadl_epi64((const __m128i *)(const void *)(in + 4));
  __m128i c = _mm_loadl_epi64((const __m128i *)(const void *)(in + 8));
  __m128i d = _mm_loadl_epi64((const __m128i *)(const void *)(in + 12));
  a = _mm_unpacklo_epi16(a, _mm_srai_epi16(a, 15));
  b = _mm_unpacklo_epi16(b, _mm_srai_epi16(b, 15));
  c = _mm_unpacklo_epi16(c, _mm_srai_epi16(c, 15));
  d = _mm_unpacklo_epi16(d, _mm_srai_epi16(d, 15));
  w1_vp8e_transpose(&a, &b, &c, &d);
  w1_vp8e_dct4(&a, &b, &c, &d);
  w1_vp8e_transpose(&a, &b, &c, &d);
  w1_vp8e_dct4(&a, &b, &c, &d);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_packs_epi32(a, b));
  _mm_storeu_si128((__m128i *)(void *)(out + 8), _mm_packs_epi32(c, d));
#else
  w1_vp8e_fdct_scalar(in, out);
#endif
}


/* ---- RGB -> padded YUV420 (BT.601 studio range, edge replication) ---- */
/* ---- Chroma plane refinement against the decoder's upsampler ----
 *
 * The decoder reconstructs full-resolution chroma with a fixed 4-tap
 * "fancy" upsampler S, separable as (3,1)/4 on each axis (w1_fancy_pair).
 * The 2x2 box average below is the cheapest chroma plane to derive, not the
 * one that reconstructs closest to the source. The plane that minimises
 * post-upsample squared error is the least-squares solution of
 * S x ~= c_full, i.e. the normal equations
 *
 *     (S^T S) x = S^T c_full.
 *
 * Separability makes S^T S exactly the stencil [3,10,3]/8 on each axis with
 * clamped edges (checked against the upsampler to machine precision at
 * every border and at degenerate plane sizes), and its spectrum is
 * [1/4, 4] - the 1D symbol (10 + 6 cos t)/8 squared. Two Chebyshev steps
 * with coefficients fixed from that spectrum therefore land within 0.01 dB
 * of the exact solve while needing no inner products, so unlike CG the
 * result involves no data-dependent division and stays deterministic in
 * integer arithmetic. Cost is two applications of the stencil on a
 * quarter-resolution plane.
 *
 * This lifts the colour-conversion ceiling itself, which no choice of
 * downsampling rule can do: on a 600x211 photo the best RGB PSNR any
 * box-averaged encode can reach is 27.27 dB and the refined plane reaches
 * 28.71 dB, from the same number of coded chroma samples. It is not free -
 * the refined plane carries more high-frequency detail (mean |adjacent U
 * step| 3.7 -> 8.2), which costs chroma tokens - so W1_UVLS/16 damps it
 * back toward the box plane. 0 restores the box average exactly.
 */
#ifndef W1_UVLS
#define W1_UVLS 16
#endif
/* Alpha below this codes as fully transparent: the RGB under such pixels is
 * replaced by their average (w1_vp8e_rgb_to_yuv) and excluded from the RD
 * metric (w1_vp8e_rgb_sse_row), so the two always agree on which pixels the
 * encoder is trying to reproduce.
 *
 * 1, so only a fully transparent pixel qualifies - the same rule libwebp
 * applies. The wider cutoff of 8 looks free (alpha 8 composites at 3%) and is
 * not: flattening a band to the frame average puts a hard colour edge where
 * the alpha ramp crosses it, which costs more to code than the detail it
 * removed. On the alpha_sweep generator, where 2.7% of pixels land in 1..7,
 * dropping 8 to 1 is both smaller and 13 dB better at every quality
 * (256: 1196 B / 31.06 dB -> 1132 B / 44.18 dB), and it turns a 14 dB loss
 * against libwebp into a win on bytes at matched quality. Images whose only
 * transparency is alpha == 0 - every image in the corpus - are unaffected. */
#ifndef W1_ALPHA_FLAT
#define W1_ALPHA_FLAT 1
#endif

/* Chebyshev coefficients in Q12 for the spectrum [1/4, 4]: d = 17/8,
 * c = 15/8, a0 = 1/d, b1 = (c a0)^2/2, a1 = 1/(d - b1/a0). */
#define W1_UVLS_A0 1928
#define W1_UVLS_B1 1594
#define W1_UVLS_A1 3156

/* int32 scratch the refinement needs: four cw x chh planes (x, residual,
 * search direction, stencil output) plus the four-row S^T window. The
 * spare plane holds the converted source row during the S^T assembly. */
static W1_UNUSED size_t w1_uvls_need(int mb_w, int mb_h) {
  const size_t cw = (size_t)(unsigned)mb_w * 8;
  const size_t chh = (size_t)(unsigned)mb_h * 8;
  if (W1_UVLS <= 0) return 0;
  return (4 * cw * chh + 4 * cw) * sizeof(int32_t);
}

/* Full-resolution chroma sample at column x of row[], 8.8 fixed point;
 * comp 0 = U, 1 = V. Mirrors the box path's coefficients and its
 * transparent-pixel substitution so both see the same source. */
static W1_UNUSED int w1_uvls_cval(const uint8_t *row, int w, int x, int comp,
                                  int have_avg, int ar, int ag, int ab) {
  const int sx = x < w ? x : w - 1;
  int r = row[sx * 4], g = row[sx * 4 + 1], b = row[sx * 4 + 2];
  if (have_avg && row[sx * 4 + 3] < W1_ALPHA_FLAT) { r = ar; g = ag; b = ab; }
  return comp ? (128 << 8) + ((28784 * r - 24103 * g - 4681 * b) >> 8)
              : (128 << 8) + ((-9714 * r - 19070 * g + 28784 * b) >> 8);
}

/* Horizontal half of S^T over one source row (edge-clamped), into out[cw].
 * Scaling by 1/4 is deferred to the vertical half.  The four taps overlap
 * (a sample is a centre tap for one j and a neighbour tap for the next), so
 * every source sample is converted once into cv[0..2*cw-1] and the filter
 * reads that: same values as four w1_uvls_cval calls, half the work.  The
 * SSE2 path converts four pixels per madd with the scalar tail handling the
 * right-edge clamp.  cv must hold 2*cw int32 and must not alias the
 * rotating row window (the caller lends its spare plane scratch). */
static W1_UNUSED void w1_uvls_srow(const uint8_t *rgba, size_t stride,
                                   int w, int h, int y, int cw, int comp,
                                   int have_avg, int ar, int ag, int ab,
                                   int32_t *cv, int32_t *out) {
  const uint8_t *row = rgba + (size_t)(y < h ? y : h - 1) * stride;
  const int xlim = 2 * cw;
  int j, x = 0;
#ifdef W1_USE_SSE2
  if (w >= 4) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i bias = _mm_set1_epi32(128 << 8);
    /* 16-bit lanes [cR,cG,cB,0] x2; madd sums the R/G and B/0 pairs. */
    const __m128i coef = comp
        ? _mm_set_epi16(0, -4681, -24103, 28784, 0, -4681, -24103, 28784)
        : _mm_set_epi16(0, 28784, -19070, -9714, 0, 28784, -19070, -9714);
    const __m128i avgv = _mm_set1_epi32(ar | (ag << 8) | (ab << 16));
    const __m128i amin = _mm_set1_epi32(W1_ALPHA_FLAT - 1);
    for (; x + 4 <= w; x += 4) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(row + x * 4));
      __m128i m0, m1;
      if (have_avg) {
        __m128i keep = _mm_cmpgt_epi32(_mm_srli_epi32(v, 24), amin);
        v = _mm_or_si128(_mm_and_si128(v, keep),
                         _mm_andnot_si128(keep, avgv));
      }
      m0 = _mm_madd_epi16(_mm_unpacklo_epi8(v, zero), coef);
      m1 = _mm_madd_epi16(_mm_unpackhi_epi8(v, zero), coef);
      m0 = _mm_add_epi32(m0, _mm_shuffle_epi32(m0, _MM_SHUFFLE(2, 3, 0, 1)));
      m1 = _mm_add_epi32(m1, _mm_shuffle_epi32(m1, _MM_SHUFFLE(2, 3, 0, 1)));
      m0 = _mm_shuffle_epi32(m0, _MM_SHUFFLE(2, 0, 2, 0));
      m1 = _mm_shuffle_epi32(m1, _MM_SHUFFLE(2, 0, 2, 0));
      m0 = _mm_add_epi32(_mm_srai_epi32(_mm_unpacklo_epi64(m0, m1), 8), bias);
      _mm_storeu_si128((__m128i *)(void *)(cv + x), m0);
    }
  }
#endif
  for (; x < w; x++)
    cv[x] = w1_uvls_cval(row, w, x, comp, have_avg, ar, ag, ab);
  for (; x < xlim; x++) cv[x] = cv[w - 1];
  for (j = 0; j < cw; j++) {
    const int jp = j + 1 < cw ? j + 1 : cw - 1, jm = j > 0 ? j - 1 : 0;
    out[j] = 3 * cv[2 * j] + 3 * cv[2 * j + 1] + cv[2 * jp] + cv[2 * jm + 1];
  }
}

/* dst = (S^T S) src: [3,10,3]/8 down each axis with clamped edges. The
 * vertical pass writes dst, the horizontal one runs in place over it with a
 * single saved sample. dst must not alias src. */
static W1_UNUSED void w1_uvls_apply(const int32_t *src, int32_t *dst,
                                    int cw, int chh) {
  int x, y;
  for (y = 0; y < chh; y++) {
    const int32_t *a = src + (size_t)(y > 0 ? y - 1 : 0) * cw;
    const int32_t *b = src + (size_t)y * cw;
    const int32_t *c = src + (size_t)(y + 1 < chh ? y + 1 : chh - 1) * cw;
    int32_t *o = dst + (size_t)y * cw;
#ifdef W1_USE_SSE2
    for (x = 0; x + 4 <= cw; x += 4) {
      const __m128i av = _mm_loadu_si128((const __m128i *)(const void *)(a + x));
      const __m128i bv = _mm_loadu_si128((const __m128i *)(const void *)(b + x));
      const __m128i cv = _mm_loadu_si128((const __m128i *)(const void *)(c + x));
      const __m128i a3 = _mm_add_epi32(_mm_slli_epi32(av, 1), av);
      const __m128i b10 = _mm_add_epi32(_mm_slli_epi32(bv, 3),
                                        _mm_slli_epi32(bv, 1));
      const __m128i c3 = _mm_add_epi32(_mm_slli_epi32(cv, 1), cv);
      _mm_storeu_si128((__m128i *)(void *)(o + x),
                       _mm_add_epi32(_mm_add_epi32(a3, b10), c3));
    }
    for (; x < cw; x++) o[x] = 3 * a[x] + 10 * b[x] + 3 * c[x];
#else
    for (x = 0; x < cw; x++) o[x] = 3 * a[x] + 10 * b[x] + 3 * c[x];
#endif
  }
  for (y = 0; y < chh; y++) {
    int32_t *o = dst + (size_t)y * cw;
    int32_t pm = o[0];
#ifdef W1_USE_SSE2
    if (cw > 4) {
      /* Scalar first element; the vector loop then reads the original values
       * one ahead (nx) and carries the previous original in a register. */
      {
        const int32_t cur0 = o[0];
        o[0] = (3 * pm + 10 * cur0 + 3 * o[1] + 32) >> 6;
        pm = cur0;
      }
      for (x = 1; x <= cw - 5; x += 4) {
        const __m128i cur = _mm_loadu_si128((const __m128i *)(const void *)(o + x));
        const __m128i nx = _mm_loadu_si128((const __m128i *)(const void *)(o + x + 1));
        const __m128i prev = _mm_or_si128(_mm_slli_si128(cur, 4),
                                          _mm_cvtsi32_si128(pm));
        const __m128i p3 = _mm_add_epi32(_mm_slli_epi32(prev, 1), prev);
        const __m128i c10 = _mm_add_epi32(_mm_slli_epi32(cur, 3),
                                          _mm_slli_epi32(cur, 1));
        const __m128i n3 = _mm_add_epi32(_mm_slli_epi32(nx, 1), nx);
        __m128i r = _mm_add_epi32(_mm_add_epi32(p3, c10), n3);
        r = _mm_add_epi32(r, _mm_set1_epi32(32));
        r = _mm_srai_epi32(r, 6);
        _mm_storeu_si128((__m128i *)(void *)(o + x), r);
        pm = _mm_cvtsi128_si32(_mm_srli_si128(cur, 12));
      }
      for (; x < cw; x++) {
        const int32_t cur = o[x], nx = o[x + 1 < cw ? x + 1 : cw - 1];
        o[x] = (3 * pm + 10 * cur + 3 * nx + 32) >> 6;
        pm = cur;
      }
    } else
#endif
    for (x = 0; x < cw; x++) {
      const int32_t cur = o[x], nx = o[x + 1 < cw ? x + 1 : cw - 1];
      o[x] = (3 * pm + 10 * cur + 3 * nx + 32) >> 6;   /* two /8 passes */
      pm = cur;
    }
  }
}

/* Refine one box-averaged chroma plane in place (see the header comment). */
static W1_UNUSED void w1_uvls_solve(const uint8_t *rgba, size_t stride,
                                    int w, int h, int comp, int have_avg,
                                    int ar, int ag, int ab, uint8_t *plane,
                                    int uvs, int cw, int chh, int32_t *sc) {
  const size_t n = (size_t)cw * (size_t)chh;
  int32_t *x = sc, *r = sc + n, *p = sc + 2 * n, *t = sc + 3 * n;
  int32_t *e0 = sc + 4 * n, *o0 = e0 + cw, *e1 = o0 + cw, *om = e1 + cw;
  size_t k;
  int cy, j;
  for (cy = 0; cy < chh; cy++)
    for (j = 0; j < cw; j++)
      x[(size_t)cy * cw + j] = (int32_t)plane[(size_t)cy * uvs + j] << 8;
  /* r <- (S^T c_full)/16. Each chroma row needs source rows 2cy-1, 2cy,
   * 2cy+1 and 2cy+2, so they stream through a rotating four-row window
   * instead of materialising S^T c_full at full resolution. */
  w1_uvls_srow(rgba, stride, w, h, 0, cw, comp, have_avg, ar, ag, ab, t, e0);
  w1_uvls_srow(rgba, stride, w, h, 1, cw, comp, have_avg, ar, ag, ab, t, o0);
  memcpy(om, o0, (size_t)cw * sizeof(int32_t));   /* odd[-1] clamps to odd[0] */
  for (cy = 0; cy < chh; cy++) {
    int32_t *rr = r + (size_t)cy * cw;
    if (cy + 1 < chh)
      w1_uvls_srow(rgba, stride, w, h, 2 * cy + 2, cw, comp, have_avg,
                   ar, ag, ab, t, e1);
    else memcpy(e1, e0, (size_t)cw * sizeof(int32_t));  /* even[chh] clamps */
    for (j = 0; j < cw; j++)
      rr[j] = (3 * e0[j] + 3 * o0[j] + e1[j] + om[j] + 8) >> 4;
    if (cy + 1 < chh) {
      int32_t *sw = om; om = o0; o0 = sw;   /* odd[cy] becomes odd[cy-1] */
      sw = e0; e0 = e1; e1 = sw;            /* even[cy+1] becomes even[cy] */
      w1_uvls_srow(rgba, stride, w, h, 2 * cy + 3, cw, comp, have_avg,
                   ar, ag, ab, t, o0);
    }
  }
  w1_uvls_apply(x, t, cw, chh);
  for (k = 0; k < n; k++) r[k] -= t[k];            /* r <- b - A x0 */
  memcpy(p, r, n * sizeof(int32_t));
  w1_uvls_apply(p, t, cw, chh);
  for (k = 0; k < n; k++) {
    x[k] += (int32_t)(((int64_t)W1_UVLS_A0 * p[k]) >> 12);
    r[k] -= (int32_t)(((int64_t)W1_UVLS_A0 * t[k]) >> 12);
  }
  for (k = 0; k < n; k++)
    p[k] = r[k] + (int32_t)(((int64_t)W1_UVLS_B1 * p[k]) >> 12);
  for (k = 0; k < n; k++)
    x[k] += (int32_t)(((int64_t)W1_UVLS_A1 * p[k]) >> 12);
#if W1_UVLS == 16
  /* W1_UVLS 16 makes the blend exact: 16*(x - x0) >> 4 is x - x0, so the
   * plane is just clamp255((x + 128) >> 8). */
  for (cy = 0; cy < chh; cy++)
    for (j = 0; j < cw; j++)
      plane[(size_t)cy * uvs + j] =
        (uint8_t)w1_clamp255((x[(size_t)cy * cw + j] + 128) >> 8);
#else
  for (cy = 0; cy < chh; cy++)
    for (j = 0; j < cw; j++) {
      uint8_t *d = plane + (size_t)cy * uvs + j;
      const int32_t x0 = (int32_t)*d << 8;
      const int32_t v = x0 + (int32_t)(((int64_t)W1_UVLS *
                                        (x[(size_t)cy * cw + j] - x0)) >> 4);
      *d = (uint8_t)w1_clamp255((v + 128) >> 8);
    }
#endif
}

static W1_UNUSED void w1_vp8e_rgb_to_yuv(const uint8_t *rgba, size_t stride,
                                         int w, int h, uint8_t *yp, int ys,
                                         uint8_t *up, uint8_t *vp, int uvs,
                                         int mb_w, int mb_h, int32_t *uvls) {
  int fw = mb_w * 16, fh = mb_h * 16, x, y;
  unsigned avgr = 0, avgg = 0, avgb = 0;
  size_t navg = 0;
  int have_avg;
  /* Reference transparent-area cleanup: pixels with alpha below
   * W1_ALPHA_FLAT contribute nothing to the composited image, so detail
   * under them is bits spent on nothing (transparent noise coded 24x the
   * reference size). Replace their RGB with the average colour of all such
   * pixels, which codes as a flat region. Alpha itself is preserved exactly
   * by the ALPH chunk. */
  for (y = 0; y < h; y++) {
    const uint8_t *row = rgba + (size_t)y * stride;
    for (x = 0; x < w; x++) {
      if (row[x * 4 + 3] < W1_ALPHA_FLAT) {
        avgr += row[x * 4]; avgg += row[x * 4 + 1]; avgb += row[x * 4 + 2];
        navg++;
      }
    }
  }
  have_avg = navg != 0;
  if (have_avg) {
    avgr = (unsigned)((avgr + navg / 2) / navg);
    avgg = (unsigned)((avgg + navg / 2) / navg);
    avgb = (unsigned)((avgb + navg / 2) / navg);
  }
  for (y = 0; y < fh; y++) {
    const uint8_t *row = rgba + (size_t)(y < h ? y : h - 1) * stride;
    for (x = 0; x < fw; x++) {
      int sx = x < w ? x : w - 1;
      int r = row[sx * 4], g = row[sx * 4 + 1], b = row[sx * 4 + 2];
      if (have_avg && row[sx * 4 + 3] < W1_ALPHA_FLAT) {
        r = (int)avgr; g = (int)avgg; b = (int)avgb;
      }
      yp[y * ys + x] = (uint8_t)w1_clamp255(
        16 + ((16829 * r + 33039 * g + 6416 * b + 32768) >> 16));
    }
  }
  for (y = 0; y < mb_h * 8; y++) {
    for (x = 0; x < mb_w * 8; x++) {
      int px = 2 * x, py = 2 * y, k, r = 0, g = 0, b = 0;
      for (k = 0; k < 4; k++) {
        int sx = px + (k & 1), sy = py + (k >> 1);
        const uint8_t *row;
        if (sx >= w) sx = w - 1;
        if (sy >= h) sy = h - 1;
        row = rgba + (size_t)sy * stride;
        if (have_avg && row[sx * 4 + 3] < W1_ALPHA_FLAT) {
          r += (int)avgr; g += (int)avgg; b += (int)avgb;
        } else {
          r += row[sx * 4]; g += row[sx * 4 + 1]; b += row[sx * 4 + 2];
        }
      }
      r = (r + 2) >> 2; g = (g + 2) >> 2; b = (b + 2) >> 2;
      up[y * uvs + x] = (uint8_t)w1_clamp255(
        128 + w1_vp8e_rnd_shift(-9714 * r - 19070 * g + 28784 * b, 16));
      vp[y * uvs + x] = (uint8_t)w1_clamp255(
        128 + w1_vp8e_rnd_shift(28784 * r - 24103 * g - 4681 * b, 16));
    }
  }
  /* Replace the box planes with the least-squares ones (no-op when the
   * caller passed no scratch, or when the strength is compiled out). */
  if (uvls && W1_UVLS > 0) {
    w1_uvls_solve(rgba, stride, w, h, 0, have_avg, (int)avgr, (int)avgg,
                  (int)avgb, up, uvs, mb_w * 8, mb_h * 8, uvls);
    w1_uvls_solve(rgba, stride, w, h, 1, have_avg, (int)avgr, (int)avgg,
                  (int)avgb, vp, uvs, mb_w * 8, mb_h * 8, uvls);
  }
}

/* ---- S8.2: token writer, MB coder, frame assembly ---- */

/* Extra bits of categories 0..5 (1,2,3,4,5,11 bits), MSB-first, with the
 * per-category probabilities the decoder uses (mirror of w1_vp8_token_extra). */
static W1_UNUSED void w1_vp8e_write_extra(w1_benc_t *e, int cat, int extra) {
  const uint8_t *p = w1k_vp8_pcat_ptr[cat];
  const int n = w1k_vp8_pcat_nbits[cat];
  int b;
  for (b = 0; b < n; b++)
    w1_benc_bool(e, (extra >> (n - 1 - b)) & 1, p[b]);
}

/* Flat index into a [4][8][3][11] coefficient probability table. */
#define W1_TOK_IDX(type, band, t, node)                                       \
  ((((size_t)(type) * 8 + (size_t)(band)) * 3u + (size_t)(t)) * 11u +         \
   (size_t)(node))

/* One token bool: emit with probs[idx], counting zero/one outcomes into
 * cnt (as [1056][2] ints) when cnt != NULL for the adaptation pass. */
W1_FORCEINLINE void w1_vp8e_tokbit(w1_benc_t *e, int bit, size_t idx,
                                   const uint8_t *probs, int *cnt) {
  if (cnt) cnt[idx * 2 + (bit ? 1 : 0)]++;
  if (e) w1_benc_bool(e, bit, probs[idx]);
}

/* Emit one block's tokens.  lev[] is natural order; the scan order is
 * w1k_vp8_zigzag.  probs selects the tables (defaults or adapted);
 * cnt counts outcomes for adaptation (or NULL).  Returns the scan
 * position reached, so the caller can reproduce the decoder's "has
 * coefficients" / eob bookkeeping. */
W1_FORCEINLINE int w1_vp8e_write_block(w1_benc_t *e, int type,
                                         uint8_t *left, uint8_t *above,
                                         int li, int ai, const int16_t *lev,
                                         const uint8_t *probs, int *cnt) {
  const int start = (type == 0) ? 1 : 0;
  int c = start, t = left[li] + above[ai];
  for (;;) {
    int nz = -1, k, band;
    for (k = c; k < 16; k++)
      if (lev[w1k_vp8_zigzag[k]] != 0) { nz = k; break; }
    if (nz < 0) {
      w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, w1k_vp8_bands[c], t, W1_N_EOB),
                     probs, cnt);
      break;
    }
    w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, w1k_vp8_bands[c], t, W1_N_EOB),
                   probs, cnt);
    /* Zero run: the decoder re-reads the ZERO flag with the *inherited*
     * probability pointer (same band, running ctx) for the position whose EOB
     * it just consumed, and only from the next position on does it switch to
     * &bands[c][ctx 0].  So the first zero uses the current t, the rest 0. */
    while (c < nz) {
      w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, w1k_vp8_bands[c], t, W1_N_ZERO),
                     probs, cnt);
      c++;
      t = 0;
    }
    {
      const int v = lev[w1k_vp8_zigzag[c]];
      const int a = v < 0 ? -v : v;
      /* The band/ctx pair in play is the one the zero run left us on, exactly
       * as the decoder's `pr` does: it re-points at &bands[c][0] per skipped
       * position and keeps that pointer for the value tree that follows. */
      band = w1k_vp8_bands[c];
      w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_ZERO), probs, cnt);
      if (a == 1) {
        w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, band, t, W1_N_ONE), probs, cnt);
        t = 1;
      } else {
        w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_ONE), probs, cnt);
        if (a <= 4) {
          w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, band, t, W1_N_LOW), probs, cnt);
          if (a == 2) {
            w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, band, t, W1_N_TWO),
                           probs, cnt);
          } else {
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_TWO),
                           probs, cnt);
            w1_vp8e_tokbit(e, a == 4 ? 1 : 0,
                           W1_TOK_IDX(type, band, t, W1_N_THREE), probs, cnt);
          }
          t = 2;
        } else {
          int cat, base, extra;
          w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_LOW), probs, cnt);
          if      (a <= 6)  { cat = 0; base = 5;  }
          else if (a <= 10) { cat = 1; base = 7;  }
          else if (a <= 18) { cat = 2; base = 11; }
          else if (a <= 34) { cat = 3; base = 19; }
          else if (a <= 66) { cat = 4; base = 35; }
          else              { cat = 5; base = 67; }
          extra = a - base;
          if (cat <= 1) {
            w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, band, t, W1_N_HIGHLOW),
                           probs, cnt);
            w1_vp8e_tokbit(e, cat, W1_TOK_IDX(type, band, t, W1_N_CATONE),
                           probs, cnt);
          } else if (cat <= 3) {
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_HIGHLOW),
                           probs, cnt);
            w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, band, t, W1_N_CAT34),
                           probs, cnt);
            w1_vp8e_tokbit(e, cat - 2, W1_TOK_IDX(type, band, t, W1_N_CAT3),
                           probs, cnt);
          } else if (cat == 4) {
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_HIGHLOW),
                           probs, cnt);
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_CAT34),
                           probs, cnt);
            w1_vp8e_tokbit(e, 0, W1_TOK_IDX(type, band, t, W1_N_CAT5),
                           probs, cnt);
          } else {
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_HIGHLOW),
                           probs, cnt);
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_CAT34),
                           probs, cnt);
            w1_vp8e_tokbit(e, 1, W1_TOK_IDX(type, band, t, W1_N_CAT5),
                           probs, cnt);
          }
          if (e) w1_vp8e_write_extra(e, cat, extra);
        }
        /* The decoder leaves the running context at 2 for *every* value >= 2
         * (only |v| == 1 maps to 1), which is what the following position's
         * EOB/ZERO probabilities are keyed on. */
        t = 2;
      }
      if (e) w1_benc_bool(e, v < 0 ? 1 : 0, 128);          /* sign, equiprobable */
      if (c == 15) break;
      c++;
    }
  }
  left[li] = above[ai] = (uint8_t)(c != start);
  return c;
}

/* Rate estimate (bits * 8) for one block's levels.  The a <= 66 range is
 * a step function, so keep it as a table (hot: per coefficient in the
 * RDOQ loop and in block_bits8). */
static const uint8_t w1k_vp8e_cbits8[67] = {
  8, 36, 64, 64, 64, 80, 80, 88, 88, 88, 88, 96, 96, 96, 96, 96,
  96, 96, 96, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104, 104,
  104, 104, 104, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112,
  112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112, 112,
  112, 112, 112,
};
static W1_UNUSED int w1_vp8e_coef_bits8(int a) {
  int n = 0, v;
  if (a <= 66) return (int)w1k_vp8e_cbits8[a];
  v = a - 67;
  while (v >> n) n++;
  return 120 + 8 * n;
}
/* cbits8[a] - cbits8[a-1] for a = 0..66 (index 0 unused).  Zero for most
 * magnitudes; the RDOQ difference test only needs this delta. */
static const uint8_t w1k_vp8e_cbits8d[67] = {
   0,                                    /* 0 */
  28, 28,  0,  0, 16,  0,  8,  0,  0,  0, /* 1..10 */
   8,  0,  0,  0,  0,  0,  0,  0,  8,  0, /* 11..20 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 21..30 */
   0,  0,  0,  0,  8,  0,  0,  0,  0,  0, /* 31..40 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 41..50 */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 51..60 */
   0,  0,  0,  0,  0,  0                /* 61..66 */
};
/* The two values the RDOQ coarse-q decision needs for one coefficient:
 * cbits8[a] in the high byte, cbits8[a-1] in the low one.  One 16-bit load
 * replaces the two separate lookups per coefficient; index 0 is never used
 * (a >= 1 there). */
static const uint16_t w1k_vp8e_cbits8_pair[67] = {
  0, 9224, 16420, 16448, 16448, 20544, 20560, 22608,
  22616, 22616, 22616, 24664, 24672, 24672, 24672, 24672,
  24672, 24672, 24672, 26720, 26728, 26728, 26728, 26728,
  26728, 26728, 26728, 26728, 26728, 26728, 26728, 26728,
  26728, 26728, 26728, 28776, 28784, 28784, 28784, 28784,
  28784, 28784, 28784, 28784, 28784, 28784, 28784, 28784,
  28784, 28784, 28784, 28784, 28784, 28784, 28784, 28784,
  28784, 28784, 28784, 28784, 28784, 28784, 28784, 28784,
  28784, 28784, 28784,
};
static W1_UNUSED int w1_vp8e_block_bits8(const int16_t *lev, int start) {
  int k, last = -1, bits = 24;             /* ~3 bits for the EOB decision */
  /* Hash-exact: same max-nonzero index as the forward scan, but scans from
   * the end so dense blocks exit in 1 iteration instead of 16. */
  for (k = 15; k >= start; k--) if (lev[k] != 0) { last = k; break; }
  if (last < 0) return 6;
  for (k = start; k <= last; k++) {
    const int a = w1_abs((int)lev[w1k_vp8_zigzag[k]]);
    bits += w1_vp8e_coef_bits8(a) + (a ? 8 : 0);
  }
  return bits;
}

#ifndef W1_RDOQ_NUM
#define W1_RDOQ_NUM 16
#endif
#ifndef W1_RDOQ_DEN
#define W1_RDOQ_DEN 16
#endif
/* Rate-distortion optimized quantization. Nearest rounding ignores the rate
 * side, so at q_index 0 every marginal coefficient survives and the token
 * partition balloons. For each coefficient try the next-lower magnitude and
 * keep it when distortion (weighted like the mode search: *8) plus
 * lambda * estimated rate strictly falls. Levels never grow, so this only
 * removes tokens. Only the finest quantizer (q_index 0) benefits: there the
 * marginal coefficients are reconstruction noise (measured: gradient q100
 * drops 2976->1660 bytes while PSNR rises 47.01->47.26 dB), whereas coarser
 * steps trade real quality for rate, so it is gated there. enable == 2 is
 * the coarse-q variant: only |1| levels are killed (token removal); larger
 * levels are never shrunk, which at coarse q would destroy exact structured
 * reconstructions. */
static W1_UNUSED void w1_vp8e_rdoq(int16_t *lev, const int16_t *coef,
                                   int dq_dc, int dq_ac, int skip_dc,
                                   int64_t lam, int enable) {
  int i;
  int64_t elam;
  if (!enable) return;
  elam = lam * W1_RDOQ_NUM / W1_RDOQ_DEN;
  if (elam < 1) elam = 1;
  if (enable == 2) {
    /* Coarse-q variant: only |1| levels are killed. */
    for (i = (skip_dc ? 1 : 0); i < 16; i++) {
      int L = lev[i], a, sgn, dq, c, d0, d1;
      if (L == 0) continue;
      if (L > 1 || L < -1) continue;
      dq = i ? dq_ac : dq_dc;
      c = coef[i];
      sgn = L < 0 ? -1 : 1;
      a = sgn * L;
      d0 = c - L * dq;
      d1 = d0 + sgn * dq;
      {
        int cb, cb1;
        if (a <= 66) {
          const unsigned pv = w1k_vp8e_cbits8_pair[a];
          cb = (int)(pv >> 8); cb1 = (int)(pv & 0xffu);
        } else {
          cb = w1_vp8e_coef_bits8(a);
          cb1 = w1_vp8e_coef_bits8(a - 1);
        }
        {
          const int64_t cbest = (int64_t)d0 * d0 * 8 + (int64_t)cb * elam;
          const int64_t c2 = (int64_t)d1 * d1 * 8 + (int64_t)cb1 * elam;
          const int upd = c2 < cbest;
          lev[i] = (int16_t)(L - sgn * upd);
        }
      }
    }
    return;
  }
  for (i = (skip_dc ? 1 : 0); i < 16; i++) {
    int L = lev[i], a, sgn, dq, t, delta;
    if (L == 0) continue;
    dq = i ? dq_ac : dq_dc;
    sgn = L < 0 ? -1 : 1;
    a = sgn * L;
    /* Keep L-sgn iff 8*(d1^2 - d0^2) < (cbits(a) - cbits(a-1)) * elam,
     * with d0 = c - L*dq and d1 = d0 + sgn*dq.  The left side is the
     * exact difference of the two candidates' distortions, so the test
     * needs neither square: t = sgn*d0 = sgn*c - a*dq, and
     * d1^2 - d0^2 = 2*t*dq + dq^2.  dq > 0 on every call, and the
     * rate difference is zero unless a is one of the few magnitudes
     * where cbits8 steps, so the common case is one add and a compare. */
    t = sgn * coef[i] - a * dq;
    delta = (a <= 66) ? (int)w1k_vp8e_cbits8d[a]
                      : ((((a - 67) & (a - 68)) == 0) ? 8 : 0);
    if ((int64_t)(8 * dq) * (2 * t + dq) < (int64_t)delta * elam)
      lev[i] = (int16_t)(L - sgn);
  }
}

/* Mode cost (bits * 8) from the keyframe mode trees. Exact for the fixed
 * default probs [145,156,163]: B=0@145; DC=1,0; V=1,1; H=1,1,0@163;
 * TM=1,1,1 (tree [-4,2,4,6,0,-1,-2,-3], symbols 0..4 as 0,-1..-4). */
static const uint8_t w1k_vp8e_ymode_bits8[5] = { 15, 20, 26, 32, 7 };
/* Exact for the fixed default probs [142,114,183] (tree [0,2,-1,4,-2,-3],
 * symbols 0..3 as 0,-1..-3): DC=0@142; V=1,0; H=1,1,0@183; TM=1,1,1. */
static const uint8_t w1k_vp8e_uvmode_bits8[4] = { 7, 19, 20, 31 };

/* Exact context tree cost (8x bits) of a 4x4 mode given above/left modes,
 * mirroring the decoder's bmode walk (probs slice [9], tree walk).
 * Precomputed: per-mode bit paths + prob-slice indices for
 * w1k_vp8_bmode_tree (mechanically derived; verified bit-identical to
 * w1_tree_find over all modes), and the single-bit cost table
 * ((8192 - slog[x]/x + 64)>>7, x = 1..255) replacing per-node divides.
 * Hot: called per block per tried mode in the B search. */
static const uint8_t w1k_vp8e_bmode_nb[10] = {1, 2, 3, 5, 5, 6, 6, 6, 7, 7};
static const uint8_t w1k_vp8e_bmode_bit[10][7] = {
  {0, 0, 0, 0, 0, 0, 0},
  {1, 0, 0, 0, 0, 0, 0},
  {1, 1, 0, 0, 0, 0, 0},
  {1, 1, 1, 0, 0, 0, 0},
  {1, 1, 1, 1, 0, 0, 0},
  {1, 1, 1, 0, 1, 0, 0},
  {1, 1, 1, 0, 1, 1, 0},
  {1, 1, 1, 1, 1, 0, 0},
  {1, 1, 1, 1, 1, 1, 0},
  {1, 1, 1, 1, 1, 1, 1},
};
static const uint8_t w1k_vp8e_bmode_pidx[10][7] = {
  {0, 0, 0, 0, 0, 0, 0},
  {0, 1, 0, 0, 0, 0, 0},
  {0, 1, 2, 0, 0, 0, 0},
  {0, 1, 2, 3, 4, 0, 0},
  {0, 1, 2, 3, 6, 0, 0},
  {0, 1, 2, 3, 4, 5, 0},
  {0, 1, 2, 3, 4, 5, 0},
  {0, 1, 2, 3, 6, 7, 0},
  {0, 1, 2, 3, 6, 7, 8},
  {0, 1, 2, 3, 6, 7, 8},
};
static const uint8_t w1k_vp8e_bitcost0[256] = {
   64,  64,  56,  51,  48,  45,  43,  42,  40,  39,  37,  36,  35,  34,  34,  33,
   32,  31,  31,  30,  29,  29,  28,  28,  27,  27,  26,  26,  26,  25,  25,  24,
   24,  24,  23,  23,  23,  22,  22,  22,  21,  21,  21,  21,  20,  20,  20,  20,
   19,  19,  19,  19,  18,  18,  18,  18,  18,  17,  17,  17,  17,  17,  16,  16,
   16,  16,  16,  15,  15,  15,  15,  15,  15,  14,  14,  14,  14,  14,  14,  14,
   13,  13,  13,  13,  13,  13,  13,  12,  12,  12,  12,  12,  12,  12,  12,  11,
   11,  11,  11,  11,  11,  11,  11,  11,  10,  10,  10,  10,  10,  10,  10,  10,
   10,   9,   9,   9,   9,   9,   9,   9,   9,   9,   9,   8,   8,   8,   8,   8,
    8,   8,   8,   8,   8,   8,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
    7,   7,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
    5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   4,   4,
    4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   3,   3,
    3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,   2,
    2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
    2,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
    1,   1,   1,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
static const uint8_t w1k_vp8e_bmode_cost8[100][10] = {
  {1, 36, 53, 57, 50, 64, 62, 54, 68, 65},
  {6, 14, 40, 40, 55, 57, 55, 52, 52, 46},
  {4, 28, 24, 52, 45, 57, 48, 40, 56, 51},
  {18, 20, 43, 13, 50, 37, 48, 54, 27, 31},
  {7, 25, 51, 41, 23, 51, 70, 49, 54, 29},
  {9, 33, 39, 34, 50, 19, 33, 59, 27, 35},
  {9, 34, 21, 41, 49, 32, 19, 47, 47, 39},
  {5, 33, 33, 55, 43, 62, 68, 24, 55, 44},
  {16, 32, 44, 22, 46, 22, 39, 51, 13, 37},
  {13, 25, 42, 22, 37, 45, 37, 45, 37, 16},
  {7, 13, 35, 46, 47, 52, 47, 48, 61, 65},
  {15, 8, 30, 39, 57, 54, 51, 49, 52, 43},
  {16, 14, 14, 46, 48, 52, 39, 30, 65, 65},
  {21, 20, 44, 10, 45, 49, 51, 60, 30, 26},
  {10, 20, 45, 39, 24, 59, 123, 48, 60, 18},
  {14, 25, 32, 27, 36, 22, 29, 47, 26, 31},
  {16, 25, 15, 35, 46, 37, 22, 32, 38, 38},
  {12, 20, 24, 42, 48, 59, 43, 20, 59, 32},
  {20, 23, 38, 19, 40, 33, 43, 43, 16, 29},
  {16, 23, 32, 20, 40, 38, 49, 44, 42, 15},
  {12, 17, 16, 47, 50, 48, 30, 35, 52, 68},
  {21, 13, 12, 44, 52, 58, 38, 35, 62, 48},
  {22, 20, 8, 61, 49, 65, 37, 31, 62, 70},
  {18, 26, 24, 27, 39, 36, 31, 27, 34, 24},
  {10, 24, 33, 56, 25, 104, 40, 37, 42, 19},
  {22, 28, 16, 30, 33, 21, 28, 37, 48, 35},
  {23, 30, 12, 41, 50, 37, 16, 36, 57, 47},
  {15, 31, 15, 55, 35, 47, 55, 15, 47, 45},
  {23, 32, 32, 23, 42, 27, 38, 25, 23, 23},
  {16, 34, 18, 32, 32, 41, 27, 35, 37, 21},
  {3, 27, 45, 32, 47, 61, 58, 60, 52, 49},
  {17, 9, 37, 21, 56, 42, 67, 59, 39, 36},
  {10, 16, 28, 35, 45, 45, 109, 34, 41, 39},
  {21, 23, 68, 8, 58, 45, 63, 48, 32, 28},
  {11, 19, 50, 37, 31, 106, 42, 41, 31, 20},
  {12, 26, 32, 20, 39, 29, 48, 40, 27, 32},
  {17, 19, 31, 28, 33, 37, 19, 86, 24, 42},
  {7, 23, 27, 49, 41, 121, 121, 23, 42, 50},
  {21, 23, 49, 17, 58, 39, 49, 74, 12, 33},
  {19, 22, 37, 14, 47, 35, 48, 47, 34, 25},
  {8, 19, 35, 38, 30, 47, 39, 32, 54, 45},
  {11, 18, 28, 33, 27, 44, 39, 36, 51, 33},
  {14, 18, 16, 55, 31, 47, 47, 27, 99, 35},
  {17, 34, 49, 30, 26, 42, 28, 32, 32, 15},
  {9, 36, 64, 56, 13, 51, 115, 48, 64, 21},
  {22, 26, 38, 31, 38, 31, 21, 77, 26, 18},
  {21, 39, 17, 27, 27, 27, 23, 40, 31, 29},
  {11, 31, 39, 58, 19, 58, 58, 17, 57, 32},
  {17, 34, 41, 26, 33, 23, 42, 41, 34, 15},
  {9, 36, 41, 37, 26, 52, 52, 79, 41, 16},
  {7, 33, 33, 43, 47, 23, 30, 47, 30, 55},
  {15, 16, 26, 30, 50, 30, 28, 42, 32, 42},
  {16, 20, 18, 32, 37, 23, 30, 44, 40, 45},
  {21, 23, 33, 21, 35, 25, 43, 36, 20, 29},
  {17, 23, 33, 41, 25, 105, 41, 41, 33, 13},
  {20, 35, 35, 31, 37, 12, 34, 40, 26, 36},
  {20, 33, 27, 52, 52, 20, 15, 44, 26, 36},
  {16, 27, 19, 44, 44, 27, 27, 25, 28, 44},
  {21, 53, 41, 30, 40, 17, 30, 53, 12, 34},
  {12, 27, 46, 33, 30, 33, 23, 46, 38, 20},
  {10, 24, 29, 52, 51, 31, 16, 44, 45, 69},
  {16, 15, 23, 36, 59, 38, 19, 58, 50, 36},
  {18, 20, 16, 62, 50, 38, 19, 35, 44, 63},
  {25, 24, 29, 18, 49, 31, 29, 36, 21, 23},
  {14, 28, 41, 29, 15, 42, 106, 42, 42, 26},
  {22, 32, 21, 34, 53, 18, 16, 45, 35, 99},
  {24, 40, 17, 61, 96, 38, 6, 41, 53, 43},
  {12, 29, 29, 34, 30, 42, 34, 19, 43, 35},
  {18, 32, 32, 32, 32, 21, 37, 36, 19, 32},
  {18, 25, 20, 31, 81, 32, 27, 40, 30, 25},
  {11, 23, 24, 58, 39, 55, 40, 17, 53, 66},
  {15, 21, 22, 47, 39, 46, 44, 16, 59, 39},
  {15, 24, 14, 101, 52, 57, 39, 18, 58, 66},
  {16, 34, 34, 26, 27, 43, 35, 28, 30, 19},
  {14, 37, 44, 60, 9, 47, 111, 29, 59, 28},
  {22, 23, 26, 23, 30, 23, 30, 27, 97, 33},
  {16, 42, 15, 41, 41, 32, 19, 22, 50, 42},
  {13, 48, 31, 67, 33, 54, 118, 7, 111, 47},
  {18, 43, 34, 19, 30, 21, 85, 24, 30, 30},
  {17, 36, 33, 25, 35, 44, 30, 19, 38, 19},
  {5, 31, 39, 29, 52, 42, 42, 49, 30, 50},
  {19, 14, 29, 27, 43, 35, 35, 63, 26, 31},
  {13, 26, 23, 23, 39, 29, 51, 25, 43, 43},
  {28, 27, 33, 12, 45, 49, 54, 76, 14, 32},
  {13, 40, 40, 28, 25, 41, 105, 34, 42, 17},
  {20, 34, 32, 32, 51, 21, 42, 73, 12, 26},
  {18, 32, 26, 29, 45, 22, 24, 29, 26, 45},
  {13, 31, 19, 32, 40, 40, 40, 24, 33, 41},
  {31, 37, 44, 25, 53, 33, 41, 53, 5, 42},
  {23, 28, 40, 13, 48, 27, 35, 40, 23, 23},
  {3, 29, 43, 37, 36, 60, 51, 54, 59, 41},
  {13, 13, 33, 31, 41, 59, 43, 51, 47, 25},
  {11, 20, 18, 34, 36, 38, 42, 42, 55, 47},
  {18, 24, 38, 11, 41, 52, 44, 60, 37, 24},
  {6, 33, 42, 43, 25, 51, 115, 46, 50, 22},
  {15, 29, 36, 24, 44, 22, 86, 45, 22, 24},
  {11, 28, 25, 36, 41, 36, 25, 48, 30, 32},
  {7, 29, 28, 32, 30, 116, 52, 25, 52, 39},
  {24, 23, 33, 20, 41, 31, 33, 42, 19, 26},
  {10, 37, 43, 29, 31, 51, 51, 62, 41, 15}
};
W1_FORCEINLINE int w1_vp8e_bmode_bits8(int context, int mode) {
  return w1k_vp8e_bmode_cost8[context][mode];
}

W1_FORCEINLINE int w1_vp8e_bit_cost(int bit, unsigned p) {
  if (p < 1) p = 1;
  return w1k_vp8e_bitcost0[bit ? 256 - p : p];
}
static W1_UNUSED int w1_vp8e_block_cost(int type,
                                         uint8_t *left, uint8_t *above,
                                         int li, int ai, const int16_t *lev,
                                         const uint8_t *probs) {
  const int start = (type == 0) ? 1 : 0;
  int c = start, t = left[li] + above[ai], bits = 0;
  for (;;) {
    int nz = -1, k, band;
    for (k = c; k < 16; k++)
      if (lev[w1k_vp8_zigzag[k]] != 0) { nz = k; break; }
    if (nz < 0) {
      bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, w1k_vp8_bands[c], t, W1_N_EOB)]);
      break;
    }
    bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, w1k_vp8_bands[c], t, W1_N_EOB)]);
    
    while (c < nz) {
      bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, w1k_vp8_bands[c], t, W1_N_ZERO)]);
      c++;
      t = 0;
    }
    {
      const int v = lev[w1k_vp8_zigzag[c]];
      const int a = v < 0 ? -v : v;
      
      band = w1k_vp8_bands[c];
      bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_ZERO)]);
      if (a == 1) {
        bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, band, t, W1_N_ONE)]);
        t = 1;
      } else {
        bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_ONE)]);
        if (a <= 4) {
          bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, band, t, W1_N_LOW)]);
          if (a == 2) {
            bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, band, t, W1_N_TWO)]);
          } else {
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_TWO)]);
            bits += w1_vp8e_bit_cost(a == 4 ? 1 : 0, probs[W1_TOK_IDX(type, band, t, W1_N_THREE)]);
          }
          t = 2;
        } else {
          int cat, base, extra;
          bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_LOW)]);
          if      (a <= 6)  { cat = 0; base = 5;  }
          else if (a <= 10) { cat = 1; base = 7;  }
          else if (a <= 18) { cat = 2; base = 11; }
          else if (a <= 34) { cat = 3; base = 19; }
          else if (a <= 66) { cat = 4; base = 35; }
          else              { cat = 5; base = 67; }
          extra = a - base;
          if (cat <= 1) {
            bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)]);
            bits += w1_vp8e_bit_cost(cat, probs[W1_TOK_IDX(type, band, t, W1_N_CATONE)]);
          } else if (cat <= 3) {
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)]);
            bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, band, t, W1_N_CAT34)]);
            bits += w1_vp8e_bit_cost(cat - 2, probs[W1_TOK_IDX(type, band, t, W1_N_CAT3)]);
          } else if (cat == 4) {
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)]);
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_CAT34)]);
            bits += w1_vp8e_bit_cost(0, probs[W1_TOK_IDX(type, band, t, W1_N_CAT5)]);
          } else {
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)]);
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_CAT34)]);
            bits += w1_vp8e_bit_cost(1, probs[W1_TOK_IDX(type, band, t, W1_N_CAT5)]);
          }
          {
            int j, n = w1k_vp8_pcat_nbits[cat];
            for (j = 0; j < n; j++)
              bits += w1_vp8e_bit_cost((extra >> (n - 1 - j)) & 1,
                                       w1k_vp8_pcat_ptr[cat][j]);
          }
        }
        
        t = 2;
      }
      bits += 8;          
      if (c == 15) break;
      c++;
    }
  }
  left[li] = above[ai] = (uint8_t)(c != start);
  return bits;
}

/* Any nonzero level from `skip_dc` on: the value written into the token
 * left/above context maps, exactly like w1_vp8e_write_block's final flag. */
static W1_UNUSED int w1_vp8e_lev_any(const int16_t *lev, int skip_dc) {
  int i;
  for (i = skip_dc ? 1 : 0; i < 16; i++) if (lev[i]) return 1;
  return 0;
}

/* Reconstruction distortion (x8) of natural-order levels against the DCT
 * coefficients: the guard that keeps the trellis from outspending the
 * heuristic winner's distortion budget. */
static W1_UNUSED int64_t w1_vp8e_dist8(const int16_t *coef, const int16_t *lev,
                                       int dq_dc, int dq_ac, int skip_dc) {
  int64_t s = 0;
  int i;
  for (i = skip_dc ? 1 : 0; i < 16; i++) {
    const int dq = i ? dq_ac : dq_dc;
    const int64_t d = (int64_t)coef[i] - (int64_t)lev[i] * dq;
    s += 8 * d * d;
  }
  return s;
}

/* Exact token bits (x8) of one nonzero value: ZERO=1, the magnitude tree
 * walk and the sign, under precomputed per-node costs c0/c1.  Mirrors
 * w1_vp8e_block_cost's value branch; *tnext receives the running context
 * the next position sees (1 for |v|==1, else 2). */
static W1_UNUSED int w1_vp8e_val_cost8(const int16_t *c0, const int16_t *c1,
                                       int type, int band, int t, int a,
                                       int *tnext) {
  int bits = c1[W1_TOK_IDX(type, band, t, W1_N_ZERO)];
  if (a == 1) {
    bits += c0[W1_TOK_IDX(type, band, t, W1_N_ONE)];
    if (tnext) *tnext = 1;
  } else {
    bits += c1[W1_TOK_IDX(type, band, t, W1_N_ONE)];
    if (a <= 4) {
      bits += c0[W1_TOK_IDX(type, band, t, W1_N_LOW)];
      if (a == 2) {
        bits += c0[W1_TOK_IDX(type, band, t, W1_N_TWO)];
      } else {
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_TWO)];
        bits += (a == 4) ? c1[W1_TOK_IDX(type, band, t, W1_N_THREE)]
                         : c0[W1_TOK_IDX(type, band, t, W1_N_THREE)];
      }
    } else {
      int cat, base, extra;
      bits += c1[W1_TOK_IDX(type, band, t, W1_N_LOW)];
      if      (a <= 6)  { cat = 0; base = 5;  }
      else if (a <= 10) { cat = 1; base = 7;  }
      else if (a <= 18) { cat = 2; base = 11; }
      else if (a <= 34) { cat = 3; base = 19; }
      else if (a <= 66) { cat = 4; base = 35; }
      else              { cat = 5; base = 67; }
      extra = a - base;
      if (cat <= 1) {
        bits += c0[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)];
        bits += (cat == 0) ? c0[W1_TOK_IDX(type, band, t, W1_N_CATONE)]
                           : c1[W1_TOK_IDX(type, band, t, W1_N_CATONE)];
      } else if (cat <= 3) {
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)];
        bits += c0[W1_TOK_IDX(type, band, t, W1_N_CAT34)];
        bits += (cat == 2) ? c0[W1_TOK_IDX(type, band, t, W1_N_CAT3)]
                           : c1[W1_TOK_IDX(type, band, t, W1_N_CAT3)];
      } else if (cat == 4) {
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)];
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_CAT34)];
        bits += c0[W1_TOK_IDX(type, band, t, W1_N_CAT5)];
      } else {
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_HIGHLOW)];
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_CAT34)];
        bits += c1[W1_TOK_IDX(type, band, t, W1_N_CAT5)];
      }
      {
        int j, n = w1k_vp8_pcat_nbits[cat];
        for (j = 0; j < n; j++)
          bits += w1_vp8e_bit_cost((extra >> (n - 1 - j)) & 1,
                                   w1k_vp8_pcat_ptr[cat][j]);
      }
    }
    if (tnext) *tnext = 2;
  }
  return bits + 8;
}

/* Per-block level trellis: DP over (scan position, running token context
 * t in 0..2).  Each coefficient may keep the rounded level, drop to one of
 * W1_TRELLIS_RED shallower levels, or be skipped by a zero run; the EOB
 * flags between coefficients and the all-zero tail are part of the walk,
 * and every transition is priced with the real token costs c0/c1 and the
 * reconstruction distortion, so the winners minimize the mode search's own
 * objective (distortion*8 + lam*bits) over level reductions.  lev[] holds
 * the rounded candidates on entry (natural order) and the winners on
 * exit. */
#ifndef W1_TRELLIS_RED
#define W1_TRELLIS_RED 3
#endif
/* The trellis only ever removes tokens, so its distortion weight bounds how
 * much decoded quality it may spend.  The polish tries progressively calmer
 * rate weights per block and keeps the first result inside the block's
 * distortion allowance; W1_TRELLIS_DIST_DEN sets that allowance as a
 * fraction of the heuristic winner's own DCT distortion. */
#ifndef W1_TRELLIS_DIST_DEN
#define W1_TRELLIS_DIST_DEN 4
#endif
static W1_UNUSED void w1_vp8e_trellis(const int16_t *c0, const int16_t *c1,
                                      int type, const int16_t *coef,
                                      int dq_dc, int dq_ac, int skip_dc,
                                      int t0, int64_t lam, int16_t *lev) {
  const int start = skip_dc ? 1 : 0;
  int64_t any[17][3], nz[17][3];
  int16_t val[16][3][2];
  int8_t mode[16][3][2], nextc[16][3][2];
  int64_t zd8[17];
  int c, t, j, i;
  if (!w1_vp8e_lev_any(lev, skip_dc)) return;
  zd8[16] = 0;
  for (c = 15; c >= start; c--) {
    const int64_t x = coef[w1k_vp8_zigzag[c]];
    zd8[c] = zd8[c + 1] + 8 * x * x;
  }
  for (c = 15; c >= start; c--) {
    const int nat = w1k_vp8_zigzag[c], band = w1k_vp8_bands[c];
    const int dq = (c == 0) ? dq_dc : dq_ac;
    const int64_t x = coef[nat];
    int L = lev[nat];
    int cand[W1_TRELLIS_RED + 1], nc = 0;
    int64_t cdist[W1_TRELLIS_RED + 1];
    if (L < 0) L = -L;
    for (j = 0; j <= W1_TRELLIS_RED; j++) {
      int a = L - j, sv;
      if (a <= 0) break;
      sv = x < 0 ? -a : a;
      cand[nc] = a;
      { int64_t d = x - (int64_t)sv * dq; cdist[nc] = 8 * d * d; }
      nc++;
    }
    for (t = 0; t < 3; t++) {
      const size_t ie = W1_TOK_IDX(type, band, t, W1_N_EOB);
      const size_t iz = W1_TOK_IDX(type, band, t, W1_N_ZERO);
      int64_t bany = lam * (int64_t)c0[ie] + zd8[c];
      /* nz[c][t]: at least one nonzero at positions c..15.  The all-zero
       * tail is only legal for `any`, never for a zero-run continuation. */
      int64_t bnz = (int64_t)1 << 62;
      mode[c][t][0] = 0; nextc[c][t][0] = 0; val[c][t][0] = 0;
      mode[c][t][1] = 0; nextc[c][t][1] = 0; val[c][t][1] = 0;
      if (c < 15) {
        int64_t z = lam * (int64_t)(c1[ie] + c0[iz]) + 8 * x * x +
                    nz[c + 1][0];
        if (z < bany) { bany = z; mode[c][t][0] = 1; }
        bnz = z; mode[c][t][1] = 1;
      }
      for (j = 0; j < nc; j++) {
        int a = cand[j], nt = 2;
        int64_t q = lam * (int64_t)(c1[ie] +
                        w1_vp8e_val_cost8(c0, c1, type, band, t, a, &nt)) +
                    cdist[j] + (c < 15 ? any[c + 1][nt] : 0);
        if (q < bany) {
          bany = q; mode[c][t][0] = 2;
          val[c][t][0] = (int16_t)(x < 0 ? -a : a);
          nextc[c][t][0] = (int8_t)nt;
        }
        if (q < bnz) {
          bnz = q; mode[c][t][1] = 2;
          val[c][t][1] = (int16_t)(x < 0 ? -a : a);
          nextc[c][t][1] = (int8_t)nt;
        }
      }
      any[c][t] = bany;
      nz[c][t] = bnz;
    }
    if (c == start) break;
  }
  for (i = 0; i < 16; i++) lev[i] = 0;
  {
    int t = t0, need_nz = 0;
    for (c = start; c <= 15; c++) {
      const int md = mode[c][t][need_nz];
      if (md == 0) break;
      if (md == 2) lev[w1k_vp8_zigzag[c]] = val[c][t][need_nz];
      t = nextc[c][t][need_nz];
      need_nz = (md == 1);
    }
  }
}

typedef struct {
  unsigned reciprocal;
  int dq, round, limit;
} w1_vp8e_quant_t;

/* ---- Per-MB coder state ---- */
typedef struct {
  w1_vp8d_t d;                 /* reused decoder state (predictors + filter) */
  int dq[6];                   /* Y1DC,Y1AC,UVDC,UVAC,Y2DC,Y2AC steps */
  int64_t lam;                 /* RD weight: SSE units per estimated bit */
  const uint8_t *org_y, *org_u, *org_v;
  int mb_w, mb_h, w, h;
  const uint8_t *rd_probs;
  const w1_vp8e_quant_t *qp;
  int max_lev;                 /* level clamp so |level*dq| fits int16 */
  int rdoq_on;                 /* RDOQ mode: 1 = full, 2 = |1|-only, 0 = off */
  int polish_on;               /* run the trellis polish on winner MBs */
  const uint8_t *tc_probs;     /* probs the c0/c1 token-cost tables mirror */
  int16_t *tc0, *tc1;          /* per-token-node bit costs (x8), bit 0/1 */
} w1_vp8e_t;

/* Per-node bit costs for the token trees under `probs`: the trellis needs
 * the price of every tree node many times per block, so pay one bit_cost
 * pass per frame/pass instead. */
static W1_UNUSED void w1_vp8e_tokcost_fill(int16_t *c0, int16_t *c1,
                                           const uint8_t *probs) {
  int i;
  for (i = 0; i < 1056; i++) {
    c0[i] = (int16_t)w1_vp8e_bit_cost(0, probs[i]);
    c1[i] = (int16_t)w1_vp8e_bit_cost(1, probs[i]);
  }
}
static W1_UNUSED void w1_vp8e_tc_ensure(w1_vp8e_t *en) {
  const uint8_t *p = en->rd_probs ? en->rd_probs : w1k_vp8_coef_dflt;
  if (en->tc_probs != p) {
    w1_vp8e_tokcost_fill(en->tc0, en->tc1, p);
    en->tc_probs = p;
  }
}

/* Quantize + clamp one block's coefficients. Rounding (deadzone width)
 * is W1Q_ROUND/16 of a step (8 = round-half-up, the long-standing
 * default); smaller keeps more small coefficients (better fidelity at
 * coarse q, at a token cost the RD model prices). */
#ifndef W1Q_ROUND
#define W1Q_ROUND 8
#endif
static W1_UNUSED void w1_vp8e_quant_blk_scalar(const int16_t *c, int dq_dc, int dq_ac,
                                       int skip_dc, int max_lev,
                                       int16_t *lev) {
  int i;
  for (i = (skip_dc ? 1 : 0); i < 16; i++) {
    int dq = i ? dq_ac : dq_dc;
    int v = c[i], a = v < 0 ? -v : v;
    int l = (a + ((dq * W1Q_ROUND) >> 4)) / dq;
    int ml = max_lev < 32767 / dq ? max_lev : 32767 / dq;
    if (l > ml) l = ml;
    lev[i] = (int16_t)(v < 0 ? -l : l);
  }
  if (skip_dc) lev[0] = 0;
}

static W1_UNUSED void w1_vp8e_quant_pre(const int16_t *c, const w1_vp8e_quant_t *dc,
                                       const w1_vp8e_quant_t *ac, int skip_dc,
                                       int16_t *lev) {
#ifdef W1_USE_SSE2
  const unsigned recip = ac->reciprocal;
  const int dq_ac = ac->dq;
  const int ml = ac->limit;
  const __m128i rv = _mm_set1_epi32((int)recip);
  const __m128i dv16 = _mm_set1_epi16((short)dq_ac), zero = _mm_setzero_si128();
  const __m128i round = _mm_set1_epi32(ac->round);
  const __m128i lim = _mm_set1_epi32(ml), dm1 = _mm_set1_epi32(dq_ac - 1);
  int i;
  for (i = 0; i < 16; i += 8) {
    /* Two coefficient quads per iteration: one 16-byte load, one 16-byte
     * store, the two reciprocal pipelines issue independently.  Each quad
     * sees exactly the operations the 4-wide loop ran. */
    const __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(c + i));
    const __m128i sgn16 = _mm_srai_epi16(v, 15);
    __m128i x0 = _mm_unpacklo_epi16(v, sgn16);
    __m128i x1 = _mm_unpackhi_epi16(v, sgn16);
    __m128i sign0 = _mm_srai_epi32(x0, 31), sign1 = _mm_srai_epi32(x1, 31);
    __m128i n0 = _mm_add_epi32(_mm_sub_epi32(_mm_xor_si128(x0, sign0), sign0), round);
    __m128i n1 = _mm_add_epi32(_mm_sub_epi32(_mm_xor_si128(x1, sign1), sign1), round);
    __m128i a0 = _mm_srli_epi64(_mm_mul_epu32(n0, rv), 32);
    __m128i b0 = _mm_srli_epi64(_mm_mul_epu32(_mm_srli_si128(n0, 4), rv), 32);
    __m128i a1 = _mm_srli_epi64(_mm_mul_epu32(n1, rv), 32);
    __m128i b1 = _mm_srli_epi64(_mm_mul_epu32(_mm_srli_si128(n1, 4), rv), 32);
    __m128i q0 = _mm_unpacklo_epi64(_mm_unpacklo_epi32(a0, b0),
                                    _mm_unpackhi_epi32(a0, b0));
    __m128i q1 = _mm_unpacklo_epi64(_mm_unpacklo_epi32(a1, b1),
                                    _mm_unpackhi_epi32(a1, b1));
    { /* q*dq <= n < 2^16 and dq >= 4 (table min) so q fits 16 bits and the
       * low-16 product is exact - no 32-bit multiply needed. */
      __m128i prod0 = _mm_unpacklo_epi16(
          _mm_mullo_epi16(_mm_packs_epi32(q0, q0), dv16), zero);
      __m128i prod1 = _mm_unpacklo_epi16(
          _mm_mullo_epi16(_mm_packs_epi32(q1, q1), dv16), zero);
      q0 = _mm_sub_epi32(q0, _mm_cmpgt_epi32(_mm_sub_epi32(n0, prod0), dm1));
      q1 = _mm_sub_epi32(q1, _mm_cmpgt_epi32(_mm_sub_epi32(n1, prod1), dm1));
    }
    {
      __m128i over0 = _mm_cmpgt_epi32(q0, lim), over1 = _mm_cmpgt_epi32(q1, lim);
      q0 = _mm_or_si128(_mm_and_si128(over0, lim), _mm_andnot_si128(over0, q0));
      q1 = _mm_or_si128(_mm_and_si128(over1, lim), _mm_andnot_si128(over1, q1));
    }
    q0 = _mm_sub_epi32(_mm_xor_si128(q0, sign0), sign0);
    q1 = _mm_sub_epi32(_mm_xor_si128(q1, sign1), sign1);
    _mm_storeu_si128((__m128i *)(void *)(lev + i), _mm_packs_epi32(q0, q1));
  }
  if (skip_dc) lev[0] = 0;
  else {
    int v = c[0], a = v < 0 ? -v : v;
    unsigned n = (unsigned)(a + dc->round);
    int l = (int)(((uint64_t)n * dc->reciprocal) >> 32);
    int m = dc->limit;
    l += n - (unsigned)(l * dc->dq) >= (unsigned)dc->dq;
    if (l > m) l = m;
    lev[0] = (int16_t)(v < 0 ? -l : l);
  }
#else
  int i;
  for (i = skip_dc ? 1 : 0; i < 16; i++) {
    const w1_vp8e_quant_t *p = i ? ac : dc;
    int v = c[i], a = v < 0 ? -v : v;
    int l = (a + p->round) / p->dq;
    if (l > p->limit) l = p->limit;
    lev[i] = (int16_t)(v < 0 ? -l : l);
  }
  if (skip_dc) lev[0] = 0;
#endif
}

static W1_UNUSED void w1_vp8e_quant_plan(w1_vp8e_quant_t *p, int dq, int max_lev) {
  p->dq = dq; p->reciprocal = 0xffffffffu / (unsigned)dq;
  p->round = (dq * W1Q_ROUND) >> 4;
  p->limit = max_lev < 32767 / dq ? max_lev : 32767 / dq;
}
static W1_UNUSED void w1_vp8e_quant_blk(const int16_t *c, int dq_dc, int dq_ac,
                                       int skip_dc, int max_lev, int16_t *lev) {
  w1_vp8e_quant_t dc, ac;
  w1_vp8e_quant_plan(&dc, dq_dc, max_lev); w1_vp8e_quant_plan(&ac, dq_ac, max_lev);
  w1_vp8e_quant_pre(c, &dc, &ac, skip_dc, lev);
}

/* Residual of one 4x4 block: org - pred (pred already in the plane).
 * Returns 1 when every residual is zero (the forward DCT is then zero and
 * fdct+quant+rdoq can be skipped); the OR test rides along the compute. */
static W1_UNUSED int w1_vp8e_residue(const uint8_t *org, int ostride,
                                     const uint8_t *pred, int pstride,
                                     int16_t *res) {
#ifdef W1_USE_SSE2
  const __m128i zero = _mm_setzero_si128();
  __m128i acc = zero;
  int r;
  for (r = 0; r < 4; r += 2) {
    uint32_t o0, o1, p0, p1;
    __m128i o, q, d;
    memcpy(&o0, org + r * ostride, 4); memcpy(&o1, org + (r + 1) * ostride, 4);
    memcpy(&p0, pred + r * pstride, 4); memcpy(&p1, pred + (r + 1) * pstride, 4);
    o = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)o0), _mm_cvtsi32_si128((int)o1));
    q = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)p0), _mm_cvtsi32_si128((int)p1));
    d = _mm_sub_epi16(_mm_unpacklo_epi8(o, zero), _mm_unpacklo_epi8(q, zero));
    acc = _mm_or_si128(acc, d);
    _mm_storeu_si128((__m128i *)(void *)(res + r * 4), d);
  }
  return _mm_movemask_epi8(_mm_cmpeq_epi16(acc, zero)) == 0xffff;
#else
  int r, k, nz = 0;
  for (r = 0; r < 4; r++)
    for (k = 0; k < 4; k++) {
      const int16_t d = (int16_t)((int)org[r * ostride + k] -
                                  (int)pred[r * pstride + k]);
      res[r * 4 + k] = d;
      nz |= d;
    }
  return nz == 0;
#endif
}

/* Dequantize 16 natural-order levels: slot 0 by dq_dc, the rest by dq_ac.
 * Products fit int16 by the level clamp in w1_vp8e_quant_plan, and both
 * paths keep the low 16 bits, so the SIMD form is bit-exact. */
static W1_UNUSED void w1_vp8e_dequant(const int16_t *lev, int dq_dc, int dq_ac,
                                     int16_t *out) {
#ifdef W1_USE_SSE2
  const __m128i ac = _mm_set1_epi16((short)dq_ac);
  const __m128i lo = _mm_insert_epi16(ac, dq_dc, 0);
  _mm_storeu_si128((__m128i *)(void *)out,
      _mm_mullo_epi16(_mm_loadu_si128((const __m128i *)(const void *)lev), lo));
  _mm_storeu_si128((__m128i *)(void *)(out + 8),
      _mm_mullo_epi16(_mm_loadu_si128((const __m128i *)(const void *)(lev + 8)), ac));
#else
  int i;
  out[0] = (int16_t)(lev[0] * dq_dc);
  for (i = 1; i < 16; i++) out[i] = (int16_t)(lev[i] * dq_ac);
#endif
}

static W1_UNUSED uint64_t w1_vp8e_sse4(const uint8_t *a, int as,
                                      const uint8_t *b, int bs) {
#ifdef W1_USE_SSE2
  __m128i sum = _mm_setzero_si128(), zero = _mm_setzero_si128();
  int r;
  for (r = 0; r < 4; r++) {
    uint32_t av, bv;
    __m128i d;
    memcpy(&av, a + r * as, 4); memcpy(&bv, b + r * bs, 4);
    d = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)av), zero),
                       _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)bv), zero));
    sum = _mm_add_epi32(sum, _mm_madd_epi16(d, d));
  }
  sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 4));
  return (uint32_t)_mm_cvtsi128_si32(sum);
#else
  int r, k;
  uint64_t s = 0;
  for (r = 0; r < 4; r++)
    for (k = 0; k < 4; k++) {
      const int d = (int)a[r * as + k] - (int)b[r * bs + k];
      s += (uint64_t)(d * d);
    }
  return s;
#endif
}

/* Try one luma prediction mode: predict, transform, quantize, reconstruct.
 * Levels (and the Y2 block) land in lev[25][16]; returns the RD cost.
 * sse_out (nullable) gets the raw SSE for gating decisions. */
static W1_UNUSED uint64_t w1_vp8e_luma_try(w1_vp8e_t *en, int mbx, int mby,
                                           int ymode, int16_t (*lev)[16],
                                           uint64_t *sse_out,
                                           const uint8_t *cbab,
                                           const uint8_t *cblf, int ccorner,
                                           uint64_t limit) {
  const int ys = en->d.y_stride;
  uint8_t *yp = en->d.plane_y + mby * 16 * ys + mbx * 16;
  const uint8_t *op = en->org_y + mby * 16 * ys + mbx * 16;
  uint8_t bab[20], blf[16];
  int16_t dcs[16], y2[16], y2dq[16], recdc[16];
  int64_t sse = 0;
  int bits = w1k_vp8e_ymode_bits8[ymode];
  int k;
  uint8_t lf[9], ab[9];
  if (en->rd_probs) {
    memcpy(lf, en->d.left_tok, 9);
    memcpy(ab, en->d.above_tok + 9 * mbx, 9);
  }

  /* 16x16 prediction (shared with the decoder, hence identical). */
  {
    int corner;
    if (cbab) { memcpy(bab, cbab, 20); memcpy(blf, cblf, 16); corner = ccorner; }
    else w1_vp8_luma_edges(&en->d, mbx, mby, ymode, bab, blf, &corner);
    if (ymode == W1_VP8_DC) w1_pred_dc_n(yp, ys, bab, blf, 16);
    else if (ymode == W1_VP8_V) w1_pred_v_n(yp, ys, bab, 16);
    else if (ymode == W1_VP8_H) w1_pred_h_n(yp, ys, blf, 16);
    else w1_pred_tm_n(yp, ys, bab, blf, corner, 16);
  }
  /* per-4x4 transform + quantize (DC collected for the Y2 block) */
  for (k = 0; k < 16; k++) {
    const int bx = (k & 3) * 4, by = (k >> 2) * 4;
    int16_t res[16], coef[16];
    if (w1_vp8e_residue(op + by * ys + bx, ys, yp + by * ys + bx, ys, res)) {
      memset(lev[k], 0, 16 * sizeof(int16_t));
      dcs[k] = 0;
    } else {
      w1_vp8e_fdct(res, coef);
      w1_vp8e_quant_pre(coef, en->qp, en->qp + 1, 1, lev[k]);
      w1_vp8e_rdoq(lev[k], coef, en->dq[0], en->dq[1], 1, en->lam, en->rdoq_on);
      dcs[k] = coef[0];
    }
    lev[24][k] = 0;                 /* placeholder; Y2 filled below */
  }
  /* Y2 (second-order luma DC) block */
  w1_vp8e_fwht(dcs, y2);
  w1_vp8e_quant_pre(y2, en->qp + 4, en->qp + 5, 0, lev[24]);
  w1_vp8e_rdoq(lev[24], y2, en->dq[4], en->dq[5], 0, en->lam, en->rdoq_on);
  w1_vp8e_dequant(lev[24], en->dq[4], en->dq[5], y2dq);
  w1_vp8_wht(y2dq, recdc);
  if (en->rd_probs) bits += w1_vp8e_block_cost(1, lf, ab, 8, 8, lev[24], en->rd_probs);
  /* reconstruct + distortion + rate */
  for (k = 0; k < 16; k++) {
    const int bx = (k & 3) * 4, by = (k >> 2) * 4;
    uint8_t *dp = yp + by * ys + bx;
    int16_t dq[16];
    w1_vp8e_dequant(lev[k], 0, en->dq[1], dq);   /* lev[k][0] is 0 */
    dq[0] = recdc[k];
    w1_vp8_block_residue(dp, ys, dq);
    sse += (int64_t)w1_vp8e_sse4(op + by * ys + bx, ys, dp, ys);
    bits += en->rd_probs ? w1_vp8e_block_cost(0, lf, ab,
      w1k_vp8_ctx_left[k], w1k_vp8_ctx_above[k], lev[k], en->rd_probs)
      : w1_vp8e_block_bits8(lev[k], 1);
    if ((uint64_t)sse * 8 + (uint64_t)bits * (uint64_t)en->lam >= limit)
      return limit;
  }
  if (!en->rd_probs) bits += w1_vp8e_block_bits8(lev[24], 0);
  if (sse_out) *sse_out = (uint64_t)sse;
  return (uint64_t)sse * 8 + (uint64_t)bits * (uint64_t)en->lam;
}

/* Greedy 4x4 (B_PRED) search for one MB: per block in raster order, best
 * of the 10 modes by RD (same SSE + lam*bits units as luma_try, with exact
 * context mode costs). Winners commit into rec in decoder recon order.
 * Contexts are only READ (above/left state); nothing is written, so a
 * losing trial leaves no trace and needs no save/restore. bmodes[] and
 * lev[0..15] are filled (no Y2 block). */
static W1_UNUSED uint64_t w1_vp8e_b_try(w1_vp8e_t *en, int mbx, int mby,
                                        int16_t (*lev)[16], uint8_t *bmodes,
                                        uint64_t limit) {
  const int ys = en->d.y_stride;
  uint8_t *yp = en->d.plane_y + mby * 16 * ys + mbx * 16;
  const uint8_t *op = en->org_y + mby * 16 * ys + mbx * 16;
  uint8_t bab[20], blf[16];
  int corner, bi;
  uint64_t sse = 0;
  int bits = w1k_vp8e_ymode_bits8[W1_VP8_B];
  uint8_t tl[9], ta[9];
  if (en->rd_probs) {
    memcpy(tl, en->d.left_tok, 9);
    memcpy(ta, en->d.above_tok + 9 * mbx, 9);
  }
  w1_vp8_luma_edges(&en->d, mbx, mby, W1_VP8_B, bab, blf, &corner);
  for (bi = 0; bi < 16; bi++) {
    int bx = bi & 3, by = bi >> 2;
    int ya = en->d.above_ym[mbx], yl = en->d.left_ym;
    int a = (bi < 4) ? (ya == W1_VP8_B ? en->d.above_brow[4 * mbx + bi]
                                       : w1k_vp8_bmode_from_ymode[ya])
                     : bmodes[bi - 4];
    int l = (!(bi & 3)) ? (yl == W1_VP8_B ? en->d.left_rcol[bi >> 2]
                                          : w1k_vp8_bmode_from_ymode[yl])
                        : bmodes[bi - 1];
    uint8_t *dp = yp + by * 4 * ys + bx * 4;
    const uint8_t *sp = op + by * 4 * ys + bx * 4;
    uint8_t ab[9], lf[5];
    int bestm = 0, m, r;
    uint64_t bests = 0;
    int bestb = 0;
    uint64_t bestc = ~(uint64_t)0;
    int16_t bestlev[16];
    uint8_t bestrc[16];
    w1_vp8_block_neighbors(&en->d, mbx, mby, bx, by, bab, blf, corner,
                           ab, lf);
    for (m = 0; m < 10; m++) {
      uint8_t pr[16], rc[16];
      int16_t res[16], coef[16], qlev[16], dq[16];
      uint64_t s;
      int bb, c8;
      c8 = w1_vp8e_bmode_bits8(a * 10 + l, m);
      if ((uint64_t)c8 * (uint64_t)en->lam >= bestc) continue;
      switch (m) {
      case 0: w1_pred_bdc(pr, 4, ab, lf); break;
      case 1: w1_pred_btm(pr, 4, ab, lf); break;
      case 2: w1_pred_bve(pr, 4, ab, lf); break;
      case 3: w1_pred_bhe(pr, 4, ab, lf); break;
      case 4: w1_pred_bld(pr, 4, ab, lf); break;
      case 5: w1_pred_brd(pr, 4, ab, lf); break;
      case 6: w1_pred_bvr(pr, 4, ab, lf); break;
      case 7: w1_pred_bvl(pr, 4, ab, lf); break;
      case 8: w1_pred_bhd(pr, 4, ab, lf); break;
      default: w1_pred_bhu(pr, 4, ab, lf); break;
      }
      if (w1_vp8e_residue(sp, ys, pr, 4, res)) {
        /* Zero residual: reconstruction is the prediction itself. */
        memset(qlev, 0, 16 * sizeof(int16_t));
        memcpy(rc, pr, 16);
        s = w1_vp8e_sse4(sp, ys, rc, 4);
      } else {
        w1_vp8e_fdct(res, coef);
        w1_vp8e_quant_pre(coef, en->qp, en->qp + 1, 0, qlev);
        w1_vp8e_rdoq(qlev, coef, en->dq[0], en->dq[1], 0, en->lam, en->rdoq_on);
        w1_vp8e_dequant(qlev, en->dq[0], en->dq[1], dq);
        memcpy(rc, pr, 16);
        w1_vp8_block_residue(rc, 4, dq);
        s = w1_vp8e_sse4(sp, ys, rc, 4);
      }
      if (en->rd_probs) {
        uint8_t lf0 = tl[w1k_vp8_ctx_left[bi]], ab0 = ta[w1k_vp8_ctx_above[bi]];
        bb = w1_vp8e_block_cost(3, &lf0, &ab0, 0, 0, qlev, en->rd_probs);
      } else bb = w1_vp8e_block_bits8(qlev, 0);
      {
        uint64_t c = s * 8 + (uint64_t)(bb + c8) * (uint64_t)en->lam;
        if (c < bestc) {
          bestc = c; bestm = m; bests = s; bestb = bb + c8;
          memcpy(bestlev, qlev, sizeof(bestlev));
          memcpy(bestrc, rc, sizeof(bestrc));
        }
      }
    }
    if (en->rd_probs) (void)w1_vp8e_block_cost(3, tl, ta,
      w1k_vp8_ctx_left[bi], w1k_vp8_ctx_above[bi], bestlev, en->rd_probs);
    bmodes[bi] = (uint8_t)bestm;
    memcpy(lev[bi], bestlev, sizeof(bestlev));
    for (r = 0; r < 4; r++) memcpy(dp + r * ys, bestrc + r * 4, 4);
    sse += bests;
    bits += bestb;
    if (sse * 8 + (uint64_t)bits * (uint64_t)en->lam >= limit) return limit;
  }
  return sse * 8 + (uint64_t)bits * (uint64_t)en->lam;
}

/* Emit the 16 bmode trees for one B MB, deriving a/l contexts exactly
 * like the decoder's mb_modes walk (pre-commit above/left state plus
 * already-chosen modes), then commit the bottom row and right column
 * into the decoder state (mirrors the decoder's read-then-update). */
static W1_UNUSED void w1_vp8e_write_bmodes(w1_benc_t *e, w1_vp8d_t *b, int col,
                                           const uint8_t *bmodes) {
  int ya = b->above_ym[col], yl = b->left_ym, i;
  for (i = 0; i < 16; i++) {
    int a = (i < 4) ? (ya == W1_VP8_B ? b->above_brow[4 * col + i]
                                      : w1k_vp8_bmode_from_ymode[ya])
                    : bmodes[i - 4];
    int l = (!(i & 3)) ? (yl == W1_VP8_B ? b->left_rcol[i >> 2]
                                         : w1k_vp8_bmode_from_ymode[yl])
                       : bmodes[i - 1];
    w1_benc_tree(e, w1k_vp8_bmode_tree, &w1k_vp8_kf_bmode[a * 90 + l * 9],
                 bmodes[i]);
  }
  for (i = 0; i < 4; i++) {
    b->above_brow[4 * col + i] = bmodes[12 + i];
    b->left_rcol[i] = bmodes[3 + 4 * i];
  }
}

/* Try one chroma mode; lev slots 16..23. */
static W1_UNUSED uint64_t w1_vp8e_chroma_try(w1_vp8e_t *en, int mbx, int mby,
                                            int ymode, int uvmode,
                                            int16_t (*lev)[16], uint64_t limit) {
  const int uvs = en->d.uv_stride;
  uint8_t *up = en->d.plane_u + mby * 8 * uvs + mbx * 8;
  uint8_t *vp = en->d.plane_v + mby * 8 * uvs + mbx * 8;
  const uint8_t *ou = en->org_u + mby * 8 * uvs + mbx * 8;
  const uint8_t *ov = en->org_v + mby * 8 * uvs + mbx * 8;
  int64_t sse = 0;
  int bits = w1k_vp8e_uvmode_bits8[uvmode];
  int64_t lam = en->lam * (int64_t)en->dq[3] * en->dq[3] /
                ((int64_t)en->dq[1] * en->dq[1]);
  int b, i;
  if (lam < 1) lam = 1;
  uint8_t lf[9], ab[9];
  if (en->rd_probs) {
    memcpy(lf, en->d.left_tok, 9);
    memcpy(ab, en->d.above_tok + 9 * mbx, 9);
  }
  {
    uint8_t bm[16];
    for (i = 0; i < 16; i++) bm[i] = 0;
    (void)ymode;
    w1_vp8_mb_predict(&en->d, mbx, mby, W1_VP8_B, uvmode, bm);
  }
  for (b = 0; b < 8; b++) {
    const int pl = b >> 2, j = b & 3;
    const int bx = (j & 1) * 4, by = (j >> 1) * 4;
    uint8_t *dp = (pl ? vp : up) + by * uvs + bx;
    const uint8_t *sp = (pl ? ov : ou) + by * uvs + bx;
    int16_t res[16], coef[16];
    if (w1_vp8e_residue(sp, uvs, dp, uvs, res)) {
      /* Zero residual reconstructs to the prediction: same SSE as the
       * dequant/block_residue round trip would have produced. */
      memset(lev[16 + b], 0, 16 * sizeof(int16_t));
      sse += (int64_t)w1_vp8e_sse4(sp, uvs, dp, uvs);
    } else {
      w1_vp8e_fdct(res, coef);
      w1_vp8e_quant_pre(coef, en->qp + 2, en->qp + 3, 0, lev[16 + b]);
      w1_vp8e_rdoq(lev[16 + b], coef, en->dq[2], en->dq[3], 0, lam, en->rdoq_on);
      {
        int16_t dq[16];
        w1_vp8e_dequant(lev[16 + b], en->dq[2], en->dq[3], dq);
        w1_vp8_block_residue(dp, uvs, dq);
        sse += (int64_t)w1_vp8e_sse4(sp, uvs, dp, uvs);
      }
    }
    bits += en->rd_probs ? w1_vp8e_block_cost(2, lf, ab,
      w1k_vp8_ctx_left[16 + b], w1k_vp8_ctx_above[16 + b], lev[16 + b], en->rd_probs)
      : w1_vp8e_block_bits8(lev[16 + b], 0);
    if ((uint64_t)sse * 8 + (uint64_t)bits * (uint64_t)lam >= limit)
      return limit;
  }
  return (uint64_t)sse * 8 + (uint64_t)bits * (uint64_t)lam;
}

/* Reconstruct one MB from its (already quantized) levels -- mirrors
 * w1_vp8_mb_recon for the non-B case. */
/* Chroma half of MB reconstruction (shared by full recon + B-mode recommit). */
static W1_UNUSED void w1_vp8e_recon_chroma_blocks(w1_vp8e_t *en, uint8_t *up,
                                                  uint8_t *vp,
                                                  int16_t (*lev)[16]) {
  const int uvs = en->d.uv_stride;
  int16_t dq[16];
  int k;
  for (k = 0; k < 8; k++) {
    const int pl = k >> 2, j = k & 3;
    uint8_t *dp = (pl ? vp : up) + (j >> 1) * 4 * uvs + (j & 1) * 4;
    w1_vp8e_dequant(lev[16 + k], en->dq[2], en->dq[3], dq);
    w1_vp8_block_residue(dp, uvs, dq);
  }
}

static W1_UNUSED void w1_vp8e_recon_mb(w1_vp8e_t *en, int mbx, int mby,
                                       int ymode, int uvmode,
                                       int16_t (*lev)[16]) {
  const int ys = en->d.y_stride, uvs = en->d.uv_stride;
  uint8_t *yp = en->d.plane_y + mby * 16 * ys + mbx * 16;
  uint8_t *up = en->d.plane_u + mby * 8 * uvs + mbx * 8;
  uint8_t *vp = en->d.plane_v + mby * 8 * uvs + mbx * 8;
  int16_t y2dq[16], recdc[16], dq[16];
  int i, k;
  uint8_t bm[16];
  for (i = 0; i < 16; i++) bm[i] = 0;
  w1_vp8_mb_predict(&en->d, mbx, mby, ymode, uvmode, bm);
  w1_vp8e_dequant(lev[24], en->dq[4], en->dq[5], y2dq);
  w1_vp8_wht(y2dq, recdc);
  for (k = 0; k < 16; k++) {
    uint8_t *dp = yp + (k >> 2) * 4 * ys + (k & 3) * 4;
    w1_vp8e_dequant(lev[k], 0, en->dq[1], dq);   /* lev[k][0] is 0 */
    dq[0] = recdc[k];
    w1_vp8_block_residue(dp, ys, dq);
  }
  w1_vp8e_recon_chroma_blocks(en, up, vp, lev);
}

/* Recommit chroma for a B-mode win: luma is already B-committed by the
 * search, but the chroma loop ran before B was decided and left the TM
 * candidate's version. Predicts chroma (luma skipped via ymode B) and
 * adds the winner residues. */
static W1_UNUSED void w1_vp8e_recon_chroma(w1_vp8e_t *en, int mbx, int mby,
                                           int uvmode, int16_t (*lev)[16]) {
  const int uvs = en->d.uv_stride;
  uint8_t *up = en->d.plane_u + mby * 8 * uvs + mbx * 8;
  uint8_t *vp = en->d.plane_v + mby * 8 * uvs + mbx * 8;
  int i;
  uint8_t bm[16];
  for (i = 0; i < 16; i++) bm[i] = 0;
  w1_vp8_mb_predict(&en->d, mbx, mby, W1_VP8_B, uvmode, bm);
  w1_vp8e_recon_chroma_blocks(en, up, vp, lev);
}

/* Trellis one block under the winner's distortion budget: try progressively
 * calmer rate weights and take the first result whose DCT distortion stays
 * inside the block's allowance (the caller's whole-frame SSE guard bounds
 * the aggregate).  lev[] keeps its greedy winner when nothing fits. */
static W1_UNUSED void w1_vp8e_polish_blk(w1_vp8e_t *en, int type,
                                         const int16_t *coef,
                                         const w1_vp8e_quant_t *dc,
                                         const w1_vp8e_quant_t *ac,
                                         int dq_dc, int dq_ac, int skip_dc,
                                         int t0, int64_t gd, int16_t *lev) {
  static const int scales[5] = { 16, 8, 4, 2, 1 };
  const int64_t cap = gd + gd / W1_TRELLIS_DIST_DEN + 64;
  int16_t cand[16];
  int s;
  for (s = 0; s < 5; s++) {
    w1_vp8e_quant_pre(coef, dc, ac, skip_dc, cand);
    w1_vp8e_trellis(en->tc0, en->tc1, type, coef, dq_dc, dq_ac, skip_dc,
                    t0, en->lam * scales[s], cand);
    if (w1_vp8e_dist8(coef, cand, dq_dc, dq_ac, skip_dc) <= cap) {
      memcpy(lev, cand, 16 * sizeof(int16_t));
      return;
    }
  }
}

/* Coefficient polish for one MB: the mode search keeps its own fast path,
 * then this re-decides every emitted block's levels with the exact token
 * trellis under the MB's entry contexts (a local copy, so the pending token
 * write still walks the live context maps).  B mode already committed its
 * luma plane during the search, so its polished blocks are re-predicted and
 * re-applied in decoder order here; 16x16 modes leave the plane to the
 * caller's recon pass.  The winners land in lev[] for emission. */
static W1_UNUSED void w1_vp8e_polish_mb(w1_vp8e_t *en, int mbx, int mby,
                                        int ymode, int uvmode,
                                        const uint8_t *bmodes,
                                        int16_t (*lev)[16]) {
  const int ys = en->d.y_stride, uvs = en->d.uv_stride;
  uint8_t *yp = en->d.plane_y + mby * 16 * ys + mbx * 16;
  uint8_t *up = en->d.plane_u + mby * 8 * uvs + mbx * 8;
  uint8_t *vp = en->d.plane_v + mby * 8 * uvs + mbx * 8;
  const uint8_t *op = en->org_y + mby * 16 * ys + mbx * 16;
  const uint8_t *ou = en->org_u + mby * 8 * uvs + mbx * 8;
  const uint8_t *ov = en->org_v + mby * 8 * uvs + mbx * 8;
  uint8_t lf[9], ab[9], bm[16];
  int k;
  w1_vp8e_tc_ensure(en);
  memcpy(lf, en->d.left_tok, 9);
  memcpy(ab, en->d.above_tok + 9 * mbx, 9);
  for (k = 0; k < 16; k++) bm[k] = 0;

  if (ymode == W1_VP8_B) {
    uint8_t bab[20], blf[16];
    int corner, bi;
    w1_vp8_mb_predict(&en->d, mbx, mby, W1_VP8_B, uvmode, bm);
    w1_vp8_luma_edges(&en->d, mbx, mby, W1_VP8_B, bab, blf, &corner);
    for (bi = 0; bi < 16; bi++) {
      const int bx = bi & 3, by = bi >> 2;
      const int li = w1k_vp8_ctx_left[bi], ai = w1k_vp8_ctx_above[bi];
      uint8_t *dp = yp + by * 4 * ys + bx * 4;
      const uint8_t *sp = op + by * 4 * ys + bx * 4;
      int16_t res[16], coef[16], dq[16], gl[16];
      int64_t gd = 0;
      w1_vp8_predict_b4(&en->d, mbx, mby, bi, bmodes[bi], bab, blf, corner);
      memcpy(gl, lev[bi], sizeof(gl));
      if (w1_vp8e_residue(sp, ys, dp, ys, res)) {
        memset(lev[bi], 0, 16 * sizeof(int16_t));
      } else {
        w1_vp8e_fdct(res, coef);
        gd = w1_vp8e_dist8(coef, gl, en->dq[0], en->dq[1], 0);
        w1_vp8e_polish_blk(en, 3, coef, en->qp, en->qp + 1,
                           en->dq[0], en->dq[1], 0, lf[li] + ab[ai], gd,
                           lev[bi]);
      }
      w1_vp8e_dequant(lev[bi], en->dq[0], en->dq[1], dq);
      w1_vp8_block_residue(dp, ys, dq);
      lf[li] = ab[ai] = (uint8_t)w1_vp8e_lev_any(lev[bi], 0);
    }
  } else {
    int16_t dcs[16], y2[16], bcoef[16][16];
    w1_vp8_mb_predict(&en->d, mbx, mby, ymode, uvmode, bm);
    for (k = 0; k < 16; k++) {
      const int bx = (k & 3) * 4, by = (k >> 2) * 4;
      const int li = w1k_vp8_ctx_left[k], ai = w1k_vp8_ctx_above[k];
      int16_t res[16], gl[16];
      int64_t gd;
      memcpy(gl, lev[k], sizeof(gl));
      if (w1_vp8e_residue(op + by * ys + bx, ys, yp + by * ys + bx, ys, res)) {
        memset(lev[k], 0, 16 * sizeof(int16_t));
        memset(bcoef[k], 0, 16 * sizeof(int16_t));
        dcs[k] = 0;
      } else {
        w1_vp8e_fdct(res, bcoef[k]);
        gd = w1_vp8e_dist8(bcoef[k], gl, en->dq[0], en->dq[1], 1);
        w1_vp8e_polish_blk(en, 0, bcoef[k], en->qp, en->qp + 1,
                           en->dq[0], en->dq[1], 1, lf[li] + ab[ai], gd,
                           lev[k]);
        dcs[k] = bcoef[k][0];
      }
      lf[li] = ab[ai] = (uint8_t)w1_vp8e_lev_any(lev[k], 1);
    }
    w1_vp8e_fwht(dcs, y2);
    {
      int16_t gl[16];
      int64_t gd;
      memcpy(gl, lev[24], sizeof(gl));
      gd = w1_vp8e_dist8(y2, gl, en->dq[4], en->dq[5], 0);
      w1_vp8e_polish_blk(en, 1, y2, en->qp + 4, en->qp + 5,
                         en->dq[4], en->dq[5], 0, lf[8] + ab[8], gd, lev[24]);
    }
    lf[8] = ab[8] = (uint8_t)w1_vp8e_lev_any(lev[24], 0);
  }
  for (k = 0; k < 8; k++) {
    const int pl = k >> 2, j = k & 3;
    const int bx = (j & 1) * 4, by = (j >> 1) * 4;
    const int li = w1k_vp8_ctx_left[16 + k], ai = w1k_vp8_ctx_above[16 + k];
    const uint8_t *sp = (pl ? ov : ou) + by * uvs + bx;
    uint8_t *dp = (pl ? vp : up) + by * uvs + bx;
    int16_t res[16], coef[16], gl[16];
    int64_t gd;
    memcpy(gl, lev[16 + k], sizeof(gl));
    if (w1_vp8e_residue(sp, uvs, dp, uvs, res)) {
      memset(lev[16 + k], 0, 16 * sizeof(int16_t));
    } else {
      w1_vp8e_fdct(res, coef);
      gd = w1_vp8e_dist8(coef, gl, en->dq[2], en->dq[3], 0);
      w1_vp8e_polish_blk(en, 2, coef, en->qp + 2, en->qp + 3,
                         en->dq[2], en->dq[3], 0, lf[li] + ab[ai], gd,
                         lev[16 + k]);
    }
    lf[li] = ab[ai] = (uint8_t)w1_vp8e_lev_any(lev[16 + k], 0);
  }
}

/* ---- Keyframe control partition + two-pass coef adaptation ----
 * Pass 1 tokenizes with the default tables and counts node outcomes into
 * cnt ([1056][2] zero/one ints). Each node then gets its maximum-likelihood
 * prob if the bits saved beat the update cost (flag under w1k_vp8_coef_upd
 * plus 8 value bits, with margin). The RD bit model is prob-independent
 * (fixed 8x costs), so pass-1 decisions stay frozen; pass 2 only re-emits
 * tokens under the adapted tables. Bit costs in 1/1024-bit units via the
 * S-table (S(v) = round(v*log2(v)*1024), S(0) = 0). */

/* -n*log2(p/256), in 1/1024-bit units. p in [1,255]. */
static W1_UNUSED uint64_t w1_vp8e_logcost(unsigned n, unsigned p) {
  uint64_t lp;
  if (!n) return 0;
  if (p < 1) p = 1; else if (p > 255) p = 255;
  /* log2(p/256)*1024 = S(p)/p - 8192 */
  lp = (uint64_t)8192 - (uint64_t)w1k_le_slog[p] / p;
  return (uint64_t)n * lp;
}

/* Fill newp (complete 1056 table, defaults overwritten where adapted) and
 * cupd (0 = keep default, else new prob). Returns net savings in
 * 1/1024-bit units. */
static W1_UNUSED int64_t w1_vp8e_adapt_probs(const int *cnt,
                                             const uint8_t *oldp,
                                             uint8_t *newp, uint8_t *cupd) {
  int i;
  int64_t net = 0;
  memcpy(newp, oldp, 1056);
  memset(cupd, 0, 1056);
  for (i = 0; i < 1056; i++) {
    unsigned z = (unsigned)cnt[i * 2], o = (unsigned)cnt[i * 2 + 1];
    unsigned tot = z + o, np, op = oldp[i], up;
    int64_t save, dcost;
    if (!tot) continue;
    np = (z * 256 + tot / 2) / tot;
    if (np < 1) np = 1; else if (np > 255) np = 255;
    if (np == op) continue;
    save = (int64_t)(w1_vp8e_logcost(z, op) +
                     w1_vp8e_logcost(o, 256 - op)) -
           (int64_t)(w1_vp8e_logcost(z, np) +
                     w1_vp8e_logcost(o, 256 - np));
    /* Switching the update flag 0 -> 1 costs the delta plus 8 value bits. */
    up = w1k_vp8_coef_upd[i];
    dcost = (int64_t)(w1_vp8e_logcost(1, 256 - up) -
                      w1_vp8e_logcost(1, up)) + (int64_t)8 * 1024;
    if (save > dcost) {
      cupd[i] = (uint8_t)np;
      newp[i] = (uint8_t)np;
      net += save - dcost;
    }
  }
  return net;
}

/* Pass 2 pays a second tokenize; require it to earn its keep (units are
 * 1/1024 bit, matching w1_vp8e_adapt_probs). Overridable for tuning. */
#ifndef W1_ADAPT_THRESH_UBITS
#define W1_ADAPT_THRESH_UBITS ((int64_t)16 * 1024)
#endif

static W1_UNUSED void w1_vp8e_write_header(w1_benc_t *e, int q, int nparts_pow2,
                                           int use_simple, int level,
                                           int sharpness,
                                           const uint8_t *cupd,
                                           int seg_on, int seg_abs,
                                           const int8_t *seg_q,
                                           const int8_t *seg_lf,
                                           const uint8_t *seg_probs, int uv_delta,
                                           int skip_on, int skip_prob) {
  int t, bb, cc, nn, i;
  w1_benc_uint(e, 0, 2);                       /* colorspace + clamp format */
  w1_benc_bool(e, seg_on, 128);                /* segmentation              */
  if (seg_on) {
    /* Mirror of w1_vp8_parse_header's segment block. */
    w1_benc_bool(e, 1, 128);                   /* update map                */
    w1_benc_bool(e, 1, 128);                   /* update data               */
    w1_benc_bool(e, seg_abs, 128);
    for (i = 0; i < 4; i++) w1_benc_maybe_int(e, seg_q[i], 7);
    for (i = 0; i < 4; i++) w1_benc_maybe_int(e, seg_lf[i], 6);
    for (i = 0; i < 3; i++) {
      int upd = seg_probs[i] != 255;
      w1_benc_bool(e, upd, 128);
      if (upd) w1_benc_uint(e, seg_probs[i], 8);
    }
  }
  w1_benc_bool(e, use_simple, 128);            /* filter type               */
  w1_benc_uint(e, level, 6);
  w1_benc_uint(e, sharpness, 3);
  w1_benc_bool(e, 0, 128);                     /* no quant deltas           */
  w1_benc_uint(e, nparts_pow2, 2);
  w1_benc_uint(e, q, 7);
  w1_benc_bool(e, 0, 128);
  w1_benc_bool(e, 0, 128);
  w1_benc_bool(e, 0, 128);
  w1_benc_maybe_int(e, uv_delta, 4);
  w1_benc_maybe_int(e, uv_delta, 4);
  w1_benc_bool(e, 0, 128);
  for (t = 0; t < 4; t++)
    for (bb = 0; bb < 8; bb++)
      for (cc = 0; cc < 3; cc++)
        for (nn = 0; nn < 11; nn++) {
          /* (t,bb,cc,nn) order matches the decoder's parse loop. */
          size_t idx = (size_t)(((t * 8) + bb) * 33 + cc * 11 + nn);
          int upd = cupd && cupd[idx];
          w1_benc_bool(e, upd, w1k_vp8_coef_upd[idx]);
          if (upd) w1_benc_uint(e, cupd[idx], 8);
        }
  /* Macroblock skip signalling (RFC 6386 19.3): per-MB flags follow the
   * segment id, before the prediction modes.  skip_prob is the bool table
   * probability of "not skipped" (0 = no skip signalling). */
  w1_benc_bool(e, skip_on, 128);
  if (skip_on) w1_benc_uint(e, skip_prob, 8);
}

static W1_UNUSED size_t w1_uvls_need(int mb_w, int mb_h);

static W1_UNUSED size_t w1_vp8e_work_worst(int mb_w, int mb_h) {
  const size_t y = (size_t)mb_w * 16 * ((size_t)mb_h * 16);
  const size_t uv = (size_t)mb_w * 8 * ((size_t)mb_h * 8);
  const size_t n = (size_t)mb_w * (size_t)mb_h;
  /* 4*n: seg/ymode/eob/uvmode maps. +16*n: B-mode subblock modes.
   * +800*n: frozen pass-1 winner levels (25x16 int16) for the pass-2
   * re-emit (avoids recomputing identical transforms under identical
   * per-segment quant). +10560: adaptation counts (2112 ints) +
   * adapted tables/updates. +4224: per-node token cost tables (c0/c1)
   * for the coefficient trellis. */
  return 2 * (y + 2 * uv) + 820 * n + 15 * (size_t)(mb_w + 1) + 128 + 10560
       + 4224 + w1_uvls_need(mb_w, mb_h) + 16;
}

/* Deblock level fitted to the reference q_index->level mapping (its files
 * use normal type, sharpness 0): middle of its content-adaptive range.
 * (q_index-14)/2 threads (26,8),(31,8-9),(63,24),(88,37),(114,50);
 * small smooth images take more (up to 63) and textured less — that
 * per-image adaptation (SNS/segments) is future work. */
static W1_UNUSED int w1_vp8_filter_level_for_q(int q) {
  int l = q < 14 ? 0 : (q - 14) / 2;
  return l > 63 ? 63 : l;
}

/* Min 16x16-predict residual SSE of one MB against org neighbors (edge
 * replicated). Classification-only; never touches the bitstream. */
static W1_UNUSED uint64_t w1_vp8e_mb_predcost(const uint8_t *org, int ostride,
                                             int w, int h, int mbx, int mby) {
  int x0 = mbx * 16, y0 = mby * 16, x1 = x0 + 16, y1 = y0 + 16, m, x, y;
  uint8_t above[16], left[16], pred[16 * 16];
  uint64_t best = ~(uint64_t)0;
  int corner;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  for (x = x0; x < x1; x++)
    above[x - x0] = y0 > 0 ? org[(y0 - 1) * ostride + x] : org[x];
  for (y = y0; y < y1; y++)
    left[y - y0] = x0 > 0 ? org[y * ostride + x0 - 1] : org[y * ostride + x0];
  for (x = x1; x < x0 + 16; x++) above[x - x0] = above[x1 - x0 - 1];
  for (y = y1; y < y0 + 16; y++) left[y - y0] = left[y1 - y0 - 1];
  corner = (x0 > 0 && y0 > 0) ? org[(y0 - 1) * ostride + x0 - 1]
         : (x0 > 0)          ? org[y0 * ostride + x0 - 1]
         : (y0 > 0)          ? org[(y0 - 1) * ostride + x0]
                             : org[0];
  for (m = 0; m < 4; m++) {
    uint64_t s = 0;
    if (m == 0) w1_pred_dc_n(pred, 16, above, left, 16);
    else if (m == 1) w1_pred_v_n(pred, 16, above, 16);
    else if (m == 2) w1_pred_h_n(pred, 16, left, 16);
    else w1_pred_tm_n(pred, 16, above, left, corner, 16);
    for (y = y0; y < y1; y++)
      for (x = x0; x < x1; x++) {
        int d = (int)org[y * ostride + x] -
                (int)pred[(y - y0) * 16 + (x - x0)];
        s += (uint64_t)(d * d);
      }
    if (s < best) best = s;
  }
  return best;
}

/* Smooth/complex split by min-predict residual against an absolute,
 * quantizer-scaled threshold T: seg 0 holds MBs whose best-16x16 residue
 * is within a few base-quant steps (safe to coarsen), seg 1 the rest at
 * base quant. Single pass over org; the caller gates on the smooth
 * fraction. Pass 1 then decides every MB under its segment's quant, so
 * the RD model budgets the split. */
static W1_UNUSED void w1_vp8e_classify_abs(const uint8_t *org, int ostride,
                                           int w, int h, int mb_w, int mb_h,
                                           uint64_t thresh, uint8_t *seg,
                                           uint64_t counts[2]) {
  int mbx, mby;
  counts[0] = counts[1] = 0;
  for (mby = 0; mby < mb_h; mby++)
    for (mbx = 0; mbx < mb_w; mbx++) {
      int idx = mby * mb_w + mbx, s;
      uint64_t c = w1_vp8e_mb_predcost(org, ostride, w, h, mbx, mby);
      s = (c <= thresh) ? 0 : 1;
      seg[idx] = (uint8_t)s;
      counts[s]++;
    }
}

/* Re-emit the control + token partitions from the frozen pass-1 winners
 * (modes in en->d.mb_ymode/mb_seg, levels in bestm, submodes in bmod).
 * skip_on adds macroblock skip flags; skipped (all-zero) MBs emit no
 * coefficient tokens and reset their token contexts exactly like the
 * decoder does.  Both benches are re-initialised over their existing
 * buffers. */
static W1_UNUSED void w1_vp8e_emit_frozen(w1_vp8e_t *en, w1_benc_t *ctl,
                                          w1_benc_t *tok, int seg_on,
                                          const int8_t *seg_q,
                                          const int8_t *seg_lf,
                                          const uint8_t *seg_probs, int q,
                                          int level, int sharpness, int uv_delta,
                                          int skip_on, int skip_prob,
                                          const uint8_t *cupd,
                                          const uint8_t *emit_probs, int mb_w,
                                          int mb_h,                                           const uint8_t *uvm,
                                          const uint8_t *bmod,
                                          const int16_t *bestm,
                                          size_t tok_cap_limit) {
  uint8_t *ctx = en->d.above_tok;
  int row, col, k;
  w1_benc_init(ctl, ctl->buf, (size_t)(ctl->end - ctl->buf));
  w1_vp8e_write_header(ctl, q, 0, 0, level, sharpness, cupd, seg_on, 0,
                       seg_q, seg_lf, seg_probs, uv_delta, skip_on, skip_prob);
  if (ctl->err) return;
  /* A caller-supplied cap (the sweep adoption budget) stops the token
   * partition early: once it alone passes the cap the final length must
   * exceed it, so the candidate can never be adopted.  Zero disables. */
  {
    size_t cap = (size_t)(tok->end - tok->buf);
    if (tok_cap_limit && tok_cap_limit < cap) cap = tok_cap_limit;
    w1_benc_init(tok, tok->buf, cap);
  }
  memset(ctx, 0, 9 * (size_t)(mb_w + 1) + 5 * (size_t)(mb_w + 1));
  memset(en->d.left_rcol, 0, 4);   /* B-mode contexts replay from zero */
  for (row = 0; row < mb_h; row++) {
    memset(en->d.left_tok, 0, 9);
    en->d.left_ym = 0;
    for (col = 0; col < mb_w; col++) {
      const int idx = row * mb_w + col;
      int ymode = en->d.mb_ymode[idx], uvmode = uvm[idx];
      int is_b = (ymode == W1_VP8_B);
      int skip = skip_on && !en->d.mb_eob[idx];
      uint8_t *ab = ctx + 9 * col;
      uint8_t *lf = en->d.left_tok;
      const int16_t (*lev)[16] = (const int16_t (*)[16])(bestm + (size_t)idx * 400);
      if (seg_on)
        w1_benc_tree(ctl, w1k_vp8_seg_tree, en->d.seg_tree_probs,
                     en->d.mb_seg[idx]);
      if (skip_on) w1_benc_bool(ctl, skip, skip_prob);
      w1_benc_tree(ctl, w1k_vp8_kf_ymode_tree, w1k_vp8_kf_ymode_prob, ymode);
      if (is_b)
        w1_vp8e_write_bmodes(ctl, &en->d, col, bmod + (size_t)idx * 16);
      w1_benc_tree(ctl, w1k_vp8_uv_mode_tree, w1k_vp8_kf_uv_prob, uvmode);
      en->d.above_ym[col] = (uint8_t)ymode;
      en->d.left_ym = (uint8_t)ymode;
      if (skip) {
        for (k = 0; k < 8; k++) lf[k] = ab[k] = 0;
        if (!is_b) lf[8] = ab[8] = 0;
      } else if (is_b) {
        for (k = 0; k < 16; k++)
          (void)w1_vp8e_write_block(tok, 3, lf, ab, w1k_vp8_ctx_left[k],
                                    w1k_vp8_ctx_above[k], lev[k], emit_probs,
                                    NULL);
      } else {
        (void)w1_vp8e_write_block(tok, 1, lf, ab, 8, 8, lev[24], emit_probs,
                                  NULL);
        for (k = 0; k < 16; k++)
          (void)w1_vp8e_write_block(tok, 0, lf, ab, w1k_vp8_ctx_left[k],
                                    w1k_vp8_ctx_above[k], lev[k], emit_probs,
                                    NULL);
      }
      if (!skip) {
        for (k = 16; k < 24; k++)
          (void)w1_vp8e_write_block(tok, 2, lf, ab, w1k_vp8_ctx_left[k],
                                    w1k_vp8_ctx_above[k], lev[k], emit_probs,
                                    NULL);
      }
      if (ctl->err || tok->err) return;
    }
  }
}

/* Encode one VP8 intra key frame (RFC 6386) into dst, which receives the
 * complete VP8 chunk payload (3-byte tag + 7-byte header + partitions).
 * Returns 0 ok, 1 work too small, 2 bad params, 3 output too small.
 *
 * The control partition is built in a small work-buffer scratch (its size is
 * bounded by construction: at most one byte per coded decision) and then
 * shifted in front of the token partition, which is written straight into dst.
 */
/* Sum of squared RGB differences over one row of w RGBA pixels (alpha
 * ignored). Per-lane 32-bit sums are safe for any row: w <= 16384 and each
 * pixel adds at most 3 * 255^2. */
/* RGB distortion of one reconstructed row against the source row.
 * Pixels with alpha < W1_ALPHA_FLAT are skipped, because w1_vp8e_rgb_to_yuv
 * has already replaced their colour with the average of all such pixels: the
 * encoder is not trying to reproduce what is under them, so counting the
 * difference measures a choice that was made on purpose. Counting it is not a small
 * bias - on a 56%-transparent frame it is a constant the quantizer cannot
 * move (SSE 5.58e7 at q_index 9 against 5.84e7 at 78), so the sweep reads
 * every finer quantizer as bits spent for nothing and collapses to the
 * coarsest one, taking the visible pixels down with it. */
static W1_UNUSED uint64_t w1_vp8e_rgb_sse_row(const uint8_t *sp,
                                             const uint8_t *rp, int w) {
  uint64_t s = 0;
  int xx = 0;
#ifdef W1_USE_SSE2
  {
    const __m128i zero = _mm_setzero_si128();
    const __m128i rgb = _mm_set1_epi32(0x00ffffff);
    const __m128i amin = _mm_set1_epi32(W1_ALPHA_FLAT - 1);
    __m128i acc = _mm_setzero_si128();
    for (; xx + 4 <= w; xx += 4) {
      __m128i sv = _mm_loadu_si128((const __m128i *)(const void *)(sp + xx * 4));
      /* keep = 0xffffffff where the source alpha byte is >= the cutoff */
      __m128i keep = _mm_cmpgt_epi32(_mm_srli_epi32(sv, 24), amin);
      __m128i msk = _mm_and_si128(rgb, keep);
      __m128i a = _mm_and_si128(sv, msk);
      __m128i b = _mm_and_si128(_mm_loadu_si128((const __m128i *)(const void *)(rp + xx * 4)), msk);
      __m128i lo = _mm_sub_epi16(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero));
      __m128i hi = _mm_sub_epi16(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero));
      acc = _mm_add_epi32(acc, _mm_madd_epi16(lo, lo));
      acc = _mm_add_epi32(acc, _mm_madd_epi16(hi, hi));
    }
    acc = _mm_add_epi32(acc, _mm_srli_si128(acc, 8));
    acc = _mm_add_epi32(acc, _mm_srli_si128(acc, 4));
    s = (uint32_t)_mm_cvtsi128_si32(acc);
  }
#endif
  for (; xx < w; xx++) {
    int dr, dg, db;
    if (sp[xx * 4 + 3] < W1_ALPHA_FLAT) continue;
    dr = (int)sp[xx * 4] - (int)rp[xx * 4];
    dg = (int)sp[xx * 4 + 1] - (int)rp[xx * 4 + 1];
    db = (int)sp[xx * 4 + 2] - (int)rp[xx * 4 + 2];
    s += (uint64_t)(dr * dr + dg * dg + db * db);
  }
  return s;
}

typedef struct {
  const uint8_t *probs;
  int force_b, lambda_num, uv_delta;
  int polish;                 /* exact-cost trellis on the winner levels */
  int no_lf_bump;             /* suppress the smooth-frame deblock bump */
} w1_vp8e_rd_t;

/* Defined with the q-map helpers; used by the sweep adoption rule. */
static W1_UNUSED int w1_frame_is_flat(const uint8_t *rgba, size_t stride,
                                      int w, int h);

static W1_UNUSED int w1_vp8_encode_frame_core(const uint8_t *rgba, size_t stride,
                                         int w, int h, int q, int level,
                                         int sharpness, uint8_t *dst,
                                         size_t dst_cap, size_t *out_len,
                                         uint8_t *work, size_t work_cap,
                                         uint64_t *sse_out,
                                         uint64_t *unfiltered_sse_out,
                                         const uint8_t *prepared,
                                         const uint64_t *predcost,
                                         const w1_vp8e_rd_t *rd,
                                         size_t tok_cap_limit) {
  const int mb_w = (w + 15) / 16, mb_h = (h + 15) / 16;
  const size_t y_sz = (size_t)mb_w * 16 * ((size_t)mb_h * 16);
  const size_t uv_sz = (size_t)mb_w * 8 * ((size_t)mb_h * 8);
  const size_t n_mb = (size_t)mb_w * (size_t)mb_h;
  /* Control headroom covers worst-case prob updates (~2.5 KB). */
  const size_t ctl_cap = 1094 + 32 * n_mb + 16 + 4096;
  const size_t need = w1_vp8e_work_worst(mb_w, mb_h) + ctl_cap;
  w1_vp8e_t en;
  w1_vp8e_quant_t plans[4][6];
  w1_bump_t bump;
  w1_benc_t ctl, tok;
  uint8_t *rec_y, *rec_u, *rec_v, *ctx, *seg, *ymb, *eo, *uvm, *ctl_buf;
  uint8_t *bmod;
  int16_t *bestm;   /* frozen pass-1 winner levels, 25x16 per MB */
  int32_t *uvls_sc;
  int *tok_counts;
  uint8_t *adapt_probs, *adapt_upd;
  int64_t adapt_net;
  int seg_on = 0, defer_tokens = 0;
  const int uv_delta = rd ? rd->uv_delta : 0;
  int8_t seg_q[4] = {0, 0, 0, 0}, seg_lf[4] = {0, 0, 0, 0};
  uint8_t seg_probs[3] = {255, 255, 255};
  uint32_t tag;  size_t ctl_sz, tok_sz, tok_cap, tok_src_off = 0;
  int row, col;
  uint8_t *rec_rgba = NULL;

  if (!rgba || !dst || !out_len) return 2;
  *out_len = 0;
  if (w < 1 || h < 1 || w > 16383 || h > 16383) return 2;
  if (dst_cap < 12) return 3;
  if (q < 0) q = 0; else if (q > 127) q = 127;
  if (level < 0) level = 0; else if (level > 63) level = 63;
  if (sharpness < 0) sharpness = 0; else if (sharpness > 7) sharpness = 7;
  if (!work || work_cap < need) return 1;
  if (sse_out) {
    size_t off = (need + 15u) & ~(size_t)15;
    if (work_cap >= off + (size_t)w * (size_t)h * 4 + 16)
      rec_rgba = work + off;
    else
      return 1;
  }

  w1_bump_init(&bump, work, need);
  rec_y = (uint8_t *)w1_bump_alloc(&bump, y_sz, 1);
  rec_u = (uint8_t *)w1_bump_alloc(&bump, uv_sz, 1);
  rec_v = (uint8_t *)w1_bump_alloc(&bump, uv_sz, 1);
  en.org_y = (uint8_t *)w1_bump_alloc(&bump, y_sz, 1);
  en.org_u = (uint8_t *)w1_bump_alloc(&bump, uv_sz, 1);
  en.org_v = (uint8_t *)w1_bump_alloc(&bump, uv_sz, 1);
  seg = (uint8_t *)w1_bump_alloc(&bump, n_mb, 1);
  ymb = (uint8_t *)w1_bump_alloc(&bump, n_mb, 1);
  eo = (uint8_t *)w1_bump_alloc(&bump, n_mb, 1);
  uvm = (uint8_t *)w1_bump_alloc(&bump, n_mb, 1);
  bmod = (uint8_t *)w1_bump_alloc(&bump, n_mb * 16, 1);
  bestm = (int16_t *)w1_bump_alloc(&bump, n_mb * 25 * 16 * sizeof(int16_t), 2);
  ctx = (uint8_t *)w1_bump_alloc(&bump,
            9 * (size_t)(mb_w + 1) + 5 * (size_t)(mb_w + 1), 1);
  ctl_buf = (uint8_t *)w1_bump_alloc(&bump, ctl_cap, 1);
  tok_counts = (int *)w1_bump_alloc(&bump, (size_t)2112 * sizeof(int), 4);
  adapt_probs = (uint8_t *)w1_bump_alloc(&bump, 1056, 1);
  adapt_upd = (uint8_t *)w1_bump_alloc(&bump, 1056, 1);
  en.tc0 = (int16_t *)w1_bump_alloc(&bump, 1056 * sizeof(int16_t), 2);
  en.tc1 = (int16_t *)w1_bump_alloc(&bump, 1056 * sizeof(int16_t), 2);
  uvls_sc = W1_UVLS > 0
          ? (int32_t *)w1_bump_alloc(&bump, w1_uvls_need(mb_w, mb_h), 4)
          : NULL;
  if ((W1_UVLS > 0 && !uvls_sc) ||
      !rec_y || !rec_u || !rec_v || !en.org_y || !en.org_u || !en.org_v ||
      !seg || !ymb || !eo || !uvm || !bmod || !bestm || !ctx || !ctl_buf ||
      !tok_counts || !adapt_probs || !adapt_upd || !en.tc0 || !en.tc1)
    return 1;
  {
    int i;
    for (i = 0; i < 2112; i++) tok_counts[i] = 0;
    memset(bmod, 0, n_mb * 16);
  }

  memset(rec_y, 0, y_sz);
  memset(rec_u, 0, uv_sz);
  memset(rec_v, 0, uv_sz);
  memset(ctx, 0, 9 * (size_t)(mb_w + 1) + 5 * (size_t)(mb_w + 1));
  en.rd_probs = rd ? rd->probs : NULL;
  en.tc_probs = NULL;
  en.polish_on = rd ? rd->polish : 0;
  if (prepared) {
    en.org_y = prepared; en.org_u = prepared + y_sz;
    en.org_v = prepared + y_sz + uv_sz;
  } else w1_vp8e_rgb_to_yuv(rgba, stride, w, h, (uint8_t *)en.org_y, mb_w * 16,
                     (uint8_t *)en.org_u, (uint8_t *)en.org_v, mb_w * 8,
                     mb_w, mb_h, uvls_sc);

  /* seg[] pre-pass default (uniform); the complexity split below may
   * enable two segments (smooth coarser, rest base). */
  memset(seg, 0, n_mb);
  seg_on = 0;

  memset(&en.d, 0, sizeof(en.d));
  en.d.mb_w = mb_w; en.d.mb_h = mb_h;
  en.d.use_simple = 0; en.d.level = level; en.d.sharpness = sharpness;
  en.d.nparts = 1; en.d.q_index = q;
  en.d.uvdc_d = en.d.uvac_d = uv_delta;
  en.d.seg_on = seg_on; en.d.seg_upd_map = seg_on; en.d.seg_abs = 0;
  {
    int i;
    for (i = 0; i < 4; i++) {
      en.d.seg_q[i] = seg_q[i];
      en.d.seg_lf[i] = seg_lf[i];
      if (i < 3) en.d.seg_tree_probs[i] = seg_probs[i];
    }
  }
  en.d.plane_y = rec_y; en.d.plane_u = rec_u; en.d.plane_v = rec_v;
  en.d.y_stride = mb_w * 16; en.d.uv_stride = mb_w * 8;
  en.d.above_tok = ctx;
  en.d.above_ym = ctx + 9 * (size_t)(mb_w + 1);
  en.d.above_brow = en.d.above_ym + (size_t)(mb_w + 1);
  en.d.mb_seg = seg; en.d.mb_ymode = ymb; en.d.mb_eob = eo;
  w1_vp8_dequant(&en.d);                    /* reuse the decoder's tables */
  { int i; for (i = 0; i < 6; i++) en.dq[i] = en.d.dqf[0][i]; }
  /* Complexity split (needs base dq for the absolute threshold): MBs
   * whose best-16x16 residue is within ~1 base-quant step go seg 0 at
   * coarser quant (+Ds), the rest stay base. Enabled only with 8+ MBs on
   * each side (map bits must earn their keep). */
  {
    uint64_t counts[2], p0, thresh;
    int i, pc_textured = 0;
#ifndef W1_SEG_DSMOOTH
#define W1_SEG_DSMOOTH 24
#endif
    thresh = (uint64_t)(unsigned)en.dq[1] * (uint64_t)(unsigned)en.dq[1];
    thresh *= 256;
    if (predcost) {
      size_t j;
      counts[0] = counts[1] = 0;
      for (j = 0; j < n_mb; j++) {
        seg[j] = (uint8_t)(predcost[j] > thresh);
        counts[seg[j]]++;
        if (predcost[j] > (uint64_t)256 * 32 * 32) pc_textured = 1;
      }
    } else w1_vp8e_classify_abs(en.org_y, mb_w * 16, w, h, mb_w, mb_h, thresh, seg,
                                counts);
    /* RDOQ engages at fine quantizers (q<=36 ~ q75 parity): marginal |1|s
     * are half-step noise even on textured frames; the lam test below
     * keeps only RD-winning kills. Coarse quantizers keep it off on smooth
     * ramps (gradient: -1.3..-2.7 dB for -14..-28 B), but textured frames
     * with a sub-quantizer 16x16 residue (pc_textured) still win at coarse q
     * (photo -11% at -0.03 dB). At coarse q the lam weight is large enough
     * that shrinking |2|+ levels destroys exact structured reconstructions
     * (checker q60e6: -16 dB at equal bytes), so the coarse extension only
     * removes |1| tokens and never shrinks a larger level. */
    en.rdoq_on = ((q <= 36) || (counts[1] != 0)) ? 1 : (pc_textured ? 2 : 0);
    defer_tokens = counts[1] != 0;
    /* W1_SEG_SPLIT=1 restores the two-segment split (smooth MBs at
     * q+W1_SEG_DSMOOTH). Measured off by default: the split is a step
     * function of q (engages near q_index 58) that made the size/SSE
     * landscape non-monotone, so the quantizer sweep mostly spent its far
     * candidates undoing it. Without it, matched-PSNR size is 0.978x on the
     * 120-point parity grid and 0.955x on photo/text 128 (deltas 6, 12,
     * -8, -16 all measured within 0.01 of each other, none better). */
#ifndef W1_SEG_SPLIT
#define W1_SEG_SPLIT 0
#endif
    if (W1_SEG_SPLIT && counts[0] >= 8 && counts[1] >= 1) {
      seg_q[0] = W1_SEG_DSMOOTH;
      en.d.seg_q[0] = W1_SEG_DSMOOTH;
      p0 = (256 * counts[0]) / ((uint64_t)mb_w * (uint64_t)mb_h);
      if (p0 < 1) p0 = 1; else if (p0 > 255) p0 = 255;
      seg_probs[0] = (uint8_t)p0;
      en.d.seg_tree_probs[0] = (uint8_t)p0;
      seg_on = 1;
      en.d.seg_on = 1;
      en.d.seg_upd_map = 1;
      w1_vp8_dequant(&en.d);              /* rebuild with the seg delta */
      for (i = 0; i < 6; i++) en.dq[i] = en.d.dqf[0][i];
    }
    /* Fully-smooth frames (all MBs under the classifier threshold) leave
     * visible block edges that the fixed (q-14)/2 filter mapping
     * under-filters (e.g. 128x128 gradient q80: filt 5 -> 40.06 dB vs
     * filt 20 -> 42.47 dB at identical bytes). Bump the deblock level
     * when the classifier already estimates full smoothness; textured
     * frames (counts[1] > 0) keep the mapped level bit-identically.
     * Gated on n_mb >= 16 so tiny (e.g. 32x32, 4 MBs) headers keep the
     * mapped level bit-identically (test_lossy_filter_header). */
    if (counts[1] == 0 && counts[0] == (uint64_t)n_mb && n_mb >= 16 &&
        !(rd && rd->no_lf_bump) && level < 20) {
      level = 20;
      en.d.level = 20;
    }
  }
  /* RD weight for mode choice, in SSE units per (estimated) bit: one
   * quantisation level is worth ~dq*dq/16 of block SSE, which measured best
   * (and within noise of every other setting) against the reference. Pass 1
   * re-derives this per MB from its segment's own AC step below. */
#ifndef W1_LAM_DIV
#define W1_LAM_DIV 32
#endif
  en.lam = ((int64_t)en.dq[1] * (int64_t)en.dq[1]) / W1_LAM_DIV;
  if (en.lam < 1) en.lam = 1;
  en.max_lev = 2047;
  {
    int si, qi;
    for (si = 0; si < 4; si++) for (qi = 0; qi < 6; qi++)
      w1_vp8e_quant_plan(&plans[si][qi], en.d.dqf[si][qi], en.max_lev);
  }
  en.mb_w = mb_w; en.mb_h = mb_h; en.w = w; en.h = h;
  memcpy(en.d.coef_probs, w1k_vp8_coef_dflt, 1056);

  w1_benc_init(&ctl, ctl_buf, ctl_cap);
  w1_vp8e_write_header(&ctl, q, 0, 0, level, sharpness, NULL,
                       seg_on, 0, seg_q, seg_lf, seg_probs, uv_delta, 0, 0);
  if (ctl.err) return 3;

  tok_cap = dst_cap - 10 - ctl_cap;
  if (tok_cap < 2) return 3;
  w1_benc_init(&tok, dst + 10, tok_cap);

  for (row = 0; row < mb_h; row++) {
    memset(en.d.left_tok, 0, 9);
    en.d.left_ym = 0;
    for (col = 0; col < mb_w; col++) {
      int16_t lev[25][16], best[25][16];
      uint64_t bestc = 0, bestu = 0;
      uint64_t sse4[4] = {0, 0, 0, 0};
      uint8_t bbmodes[16];
      uint32_t eob = 0;
      int ymode = W1_VP8_DC, uvmode = W1_VP8_DC;
      int ym, um, k, i, cfin;
      const int idx = row * mb_w + col;

      /* Per-segment quant (+lambda) for this MB: pass 1 budgets every
       * decision under the segment's own tables (seg[] preassigned by
       * the complexity classifier). */
      for (i = 0; i < 6; i++) en.dq[i] = en.d.dqf[seg[idx]][i];
      en.qp = plans[seg[idx]];
      en.lam = ((int64_t)en.dq[1] * (int64_t)en.dq[1]) / W1_LAM_DIV;
      if (rd) en.lam = en.lam * rd->lambda_num / 8;
      if (en.lam < 1) en.lam = 1;
      memset(lev, 0, sizeof(lev));
      memset(best, 0, sizeof(best));
      /* Interior MBs: the 16x16 border pixels do not depend on ymode, so
       * build them once and share across the four mode trials (bit-exact:
       * only the frame-edge branches below change with ymode). */
      {
        uint8_t cab[20], cbf[16];
        int ccorner = 0;
        const uint8_t *pab = NULL, *pbf = NULL;
        if (col > 0 && row > 0) {
          w1_vp8_luma_edges(&en.d, col, row, W1_VP8_DC, cab, cbf, &ccorner);
          pab = cab; pbf = cbf;
        }
        for (ym = 0; ym <= 3; ym++) {
          uint64_t c = w1_vp8e_luma_try(&en, col, row, ym, lev, &sse4[ym],
                                        pab, pbf, ccorner,
                                        ym ? bestc : ~(uint64_t)0);
          if (ym == 0 || c < bestc) {
            bestc = c; ymode = ym;
            /* Hash-exact: 2D arrays are contiguous; memcpy is bit-identical
             * to the nested loops and uses wide stores. */
            memcpy(best, lev, (size_t)16 * 16 * sizeof(int16_t));
            memcpy(best[24], lev[24], (size_t)16 * sizeof(int16_t));
          }
        }
      }
      for (um = 0; um <= 3; um++) {
        uint64_t c = w1_vp8e_chroma_try(&en, col, row, ymode, um, lev,
                                        um ? bestu : ~(uint64_t)0);
        if (um == 0 || c < bestu) {
          bestu = c; uvmode = um;
          /* Hash-exact: rows 16..23 are contiguous (8*16 int16). */
          memcpy(&best[16][0], &lev[16][0], (size_t)8 * 16 * sizeof(int16_t));
        }
      }
      /* B_PRED trial gate: textured (seg1) MBs with a non-trivial
       * post-quant residual (avg |err| > 0.5; excludes flat/perfect where
       * bmode overhead can only lose). The old org-domain fine-structure
       * test excluded iid-noise MBs, but 4x4 prediction pays there too;
       * the measured RD compare below keeps B only when it wins. seg0
       * (sub-quantizer 16x16 residue) still skips the trial. */
      {
        uint64_t sseB = sse4[ymode];
#ifdef W1_B_ALWAYS
        if (1) {
#elif defined(W1_B_OFF)
        if (0) {
#else
        if (en.rd_probs || (seg[idx] && sseB > (uint64_t)64)) {
#endif
          uint64_t bcost;
          int16_t blev[16][16];
          memset(blev, 0, sizeof(blev));
          bcost = w1_vp8e_b_try(&en, col, row, blev, bbmodes,
                                rd && rd->force_b ? ~(uint64_t)0 : bestc);
          if ((rd && rd->force_b) || bcost < bestc) {
            bestc = bcost; ymode = W1_VP8_B;
            for (k = 0; k < 16; k++)
              for (i = 0; i < 16; i++) best[k][i] = blev[k][i];
            for (i = 0; i < 16; i++) best[24][i] = 0;
            memcpy(bmod + (size_t)idx * 16, bbmodes, 16);
            /* en.d contexts untouched by search; the winner commits via
             * write_bmodes at emission time below. */
          }
        }
      }

      /* RD-optimal coefficient polish: the mode search above kept its
       * heuristic, this re-decides the winner's levels under the exact
       * token-cost model before any token is written. */
      if (en.polish_on)
        w1_vp8e_polish_mb(&en, col, row, ymode, uvmode, bbmodes, best);

      /* segment id, then modes -> control partition (decoder reads seg
       * first when seg_upd_map is set). */
      if (seg_on)
        w1_benc_tree(&ctl, w1k_vp8_seg_tree, en.d.seg_tree_probs,
                     seg[idx]);
      w1_benc_tree(&ctl, w1k_vp8_kf_ymode_tree, w1k_vp8_kf_ymode_prob, ymode);
      /* B sub-modes (commits brow/rcol) come between ymode and uvmode,
       * mirroring the decoder's read order. */
      if (ymode == W1_VP8_B)
        w1_vp8e_write_bmodes(&ctl, &en.d, col, bbmodes);
      w1_benc_tree(&ctl, w1k_vp8_uv_mode_tree, w1k_vp8_kf_uv_prob, uvmode);
      en.d.above_ym[col] = (uint8_t)ymode;
      en.d.left_ym = (uint8_t)ymode;

      {
        uint8_t *ab = en.d.above_tok + 9 * col;
        uint8_t *lf = en.d.left_tok;
        int is_b = (ymode == W1_VP8_B);
        if (!is_b) {
          cfin = w1_vp8e_write_block(defer_tokens ? NULL : &tok, 1, lf, ab, 8, 8, best[24],
                                     w1k_vp8_coef_dflt, tok_counts);
          if (cfin > 1) eob |= 1u << 24;
          if (cfin != 0) eob |= 1u << 31;
        }
        for (k = 0; k < 16; k++) {
          cfin = w1_vp8e_write_block(defer_tokens ? NULL : &tok, is_b ? 3 : 0, lf, ab,
                                     w1k_vp8_ctx_left[k],
                                     w1k_vp8_ctx_above[k], best[k],
                                     w1k_vp8_coef_dflt, tok_counts);
          if (cfin > 1) eob |= 1u << k;
          if (is_b ? (cfin != 0) : (cfin != 1)) eob |= 1u << 31;
        }
        for (k = 16; k < 24; k++) {
          cfin = w1_vp8e_write_block(defer_tokens ? NULL : &tok, 2, lf, ab,
                                     w1k_vp8_ctx_left[k],
                                     w1k_vp8_ctx_above[k], best[k],
                                     w1k_vp8_coef_dflt, tok_counts);
          if (cfin > 1) eob |= 1u << k;
          if (cfin != 0) eob |= 1u << 31;
        }
      }
      /* uvmode/ymode/levels for the pass-2 re-emit live in uvm[]/ymb[]/
       * bestm[] (seg[] was preassigned by the classifier and never
       * changes). Freezing winner levels avoids recomputing identical
       * transforms under identical per-segment quant in pass 2. */
      uvm[idx] = (uint8_t)uvmode;
      memcpy(bestm + (size_t)idx * 400, best, sizeof(best));
      en.d.mb_ymode[idx] = (uint8_t)ymode;
      en.d.mb_eob[idx] = (uint8_t)(eob != 0);

      /* Closed-loop reconstruction (identical to the decoder's). B mode
       * is already committed by its search; other modes recon here. */
      if (ymode != W1_VP8_B)
        w1_vp8e_recon_mb(&en, col, row, ymode, uvmode, best);
      else
        w1_vp8e_recon_chroma(&en, col, row, uvmode, best);
      if (ctl.err || tok.err) return 3;
    }
  }

  adapt_net = w1_vp8e_adapt_probs(tok_counts, w1k_vp8_coef_dflt,
                                  adapt_probs, adapt_upd);
  {
    size_t n_zero = 0, j;
    for (j = 0; j < n_mb; j++) if (!en.d.mb_eob[j]) n_zero++;
    if (defer_tokens || adapt_net > W1_ADAPT_THRESH_UBITS || n_zero > 0) {
      const int use_adapt = adapt_net > W1_ADAPT_THRESH_UBITS;
      const uint8_t *emit_probs = use_adapt ? adapt_probs : w1k_vp8_coef_dflt;
      const uint8_t *cupd = use_adapt ? adapt_upd : NULL;
      int skip_on = 0, skip_prob = 255;
      size_t off_total;
      /* A candidate whose token partition alone passes the sweep adoption
       * budget can never be adopted; cap the token writer for that trial.
       * Zero MBs disable the cap: skip-on can shrink the tokens below it. */
      const size_t trial_cap = n_zero > 0 ? 0 : tok_cap_limit;
      if (n_zero > 0) {
        int p = (int)(((uint64_t)(n_mb - n_zero) * 256 + n_mb / 2) / n_mb);
        if (p < 1) p = 1; else if (p > 255) p = 255;
        skip_prob = p;
      }
      /* Trial A: skip signalling off. */
      w1_vp8e_emit_frozen(&en, &ctl, &tok, seg_on, seg_q, seg_lf, seg_probs,
                          q, level, sharpness, uv_delta, 0, 255, cupd,
                          emit_probs, mb_w, mb_h, uvm, bmod, bestm, trial_cap);
      if (ctl.err || tok.err) return 3;
      ctl_sz = w1_benc_stop(&ctl);
      tok_sz = w1_benc_stop(&tok);
      off_total = ctl_sz + tok_sz;
      /* Trial B: skip signalling on, only when some MB is all-zero.  The
       * flags cost one bool per MB, so adopt only when the elided tokens
       * pay for them.  Trial B is written to the token partition's unused
       * tail (after A's stream, whose worst case is the whole partition),
       * and A's control partition is parked in the byte range the final
       * assembly reserves for the control partition, so when B loses,
       * A's finished stream is still in place and the historical rebuild
       * (a third full-frame emit of the same inputs) is skipped.  When B
       * does not fit the tail the old sequence runs unchanged (a tail
       * overflow only proves B >= the room left), and a B that wins from
       * the tail is moved down by the final memmove as the old stream was. */
      if (n_zero > 0) {
        size_t c2, t2, room = tok_cap - tok_sz;
        int b_done = 0;
        if (room > 0) {
          w1_benc_t tokb;
          const size_t a_len = tok_sz;
          memcpy(dst + 10 + tok_cap, ctl_buf, ctl_sz);
          w1_benc_init(&tokb, dst + 10 + a_len, room);
          w1_vp8e_emit_frozen(&en, &ctl, &tokb, seg_on, seg_q, seg_lf, seg_probs,
                              q, level, sharpness, uv_delta, 1, skip_prob, cupd,
                              emit_probs, mb_w, mb_h, uvm, bmod, bestm, 0);
          c2 = w1_benc_stop(&ctl);
          t2 = w1_benc_stop(&tokb);
          if (!ctl.err && !tokb.err) {
            if (c2 + t2 < off_total) {
              skip_on = 1;
              tok_src_off = a_len;
              ctl_sz = c2;
              tok_sz = t2;
            } else {
              memcpy(ctl_buf, dst + 10 + tok_cap, ctl_sz);  /* A's ctl back */
            }
            b_done = 1;
          }
        }
        if (!b_done) {
          /* B did not fit the tail: write it over A's stream with the full
           * cap, exactly as before, and rebuild A when it loses. */
          w1_benc_init(&tok, dst + 10, tok_cap);
          w1_vp8e_emit_frozen(&en, &ctl, &tok, seg_on, seg_q, seg_lf, seg_probs,
                              q, level, sharpness, uv_delta, 1, skip_prob, cupd,
                              emit_probs, mb_w, mb_h, uvm, bmod, bestm, 0);
          if (ctl.err || tok.err) return 3;
          c2 = w1_benc_stop(&ctl);
          t2 = w1_benc_stop(&tok);
          if (c2 + t2 < off_total) {
            skip_on = 1;
            ctl_sz = c2;
            tok_sz = t2;
          } else {
            w1_vp8e_emit_frozen(&en, &ctl, &tok, seg_on, seg_q, seg_lf, seg_probs,
                                q, level, sharpness, uv_delta, 0, 255, cupd,
                                emit_probs, mb_w, mb_h, uvm, bmod, bestm, trial_cap);
            if (ctl.err || tok.err) return 3;
            ctl_sz = w1_benc_stop(&ctl);
            tok_sz = w1_benc_stop(&tok);
          }
        }
      }
    } else {
      ctl_sz = w1_benc_stop(&ctl);
      tok_sz = w1_benc_stop(&tok);
    }
  }
  if (ctl.err || tok.err || ctl_sz + 2 > ctl_cap) return 3;
  if (ctl_sz > 0x7ffffu) return 3;              /* 19-bit partition field */
  if (10 + ctl_sz + tok_sz > dst_cap) return 3;

  /* Shift the token partition up to make room for the control partition.
   * The tokens may start offset into the partition when trial B won from
   * the tail (tok_src_off); memmove handles the overlap. */
  memmove(dst + 10 + ctl_sz, dst + 10 + tok_src_off, tok_sz);
  memcpy(dst + 10, ctl_buf, ctl_sz);

  dst[3] = 0x9d; dst[4] = 0x01; dst[5] = 0x2a;
  dst[6] = (uint8_t)(w & 0xff); dst[7] = (uint8_t)((w >> 8) & 0x3f);
  dst[8] = (uint8_t)(h & 0xff); dst[9] = (uint8_t)((h >> 8) & 0x3f);
  tag = (uint32_t)(1u << 4) | ((uint32_t)ctl_sz << 5);   /* key, shown, size */
  dst[0] = (uint8_t)(tag & 0xff);
  dst[1] = (uint8_t)((tag >> 8) & 0xff);
  dst[2] = (uint8_t)((tag >> 16) & 0xff);
  *out_len = 10 + ctl_sz + tok_sz;
  if (sse_out && rec_rgba) {
    w1_vp8_frame_t fr;
    uint64_t s;
    int yy, pass;
    for (pass = unfiltered_sse_out ? 0 : 1; pass < 2; pass++) {
      s = 0;
      if (pass && en.d.level) {
        for (row = 0; row < mb_h; row++)
          for (col = 0; col < mb_w; col++) w1_vp8_filter_mb(&en.d, col, row);
      }
      fr.y = rec_y; fr.u = rec_u; fr.v = rec_v;
      fr.y_stride = en.d.y_stride; fr.uv_stride = en.d.uv_stride;
      fr.w = w; fr.h = h;
      w1_vp8_yuv_to_rgba(&fr, rec_rgba);
      for (yy = 0; yy < h; yy++)
        s += w1_vp8e_rgb_sse_row(rgba + (size_t)yy * stride,
                                 rec_rgba + (size_t)yy * (size_t)w * 4, w);
      if (pass) *sse_out = s;
      else *unfiltered_sse_out = s;
    }
  } else if (sse_out) {
    *sse_out = 0;
  }
  return 0;
}

static W1_UNUSED int w1_vp8_encode_frame(const uint8_t *rgba, size_t stride,
                                         int w, int h, int q, int level,
                                         int sharpness, uint8_t *dst,
                                         size_t dst_cap, size_t *out_len,
                                         uint8_t *work, size_t work_cap,
                                         uint64_t *sse_out,
                                         uint64_t *unfiltered_sse_out) {
  return w1_vp8_encode_frame_core(rgba, stride, w, h, q, level, sharpness,
    dst, dst_cap, out_len, work, work_cap, sse_out, unfiltered_sse_out,
    NULL, NULL, NULL, 0);
}
/* == S9: Mux + encode API (RIFF assembly, work bounds, public encoders) == */

/* --- Fixed-buffer mux writer (sticky overflow flag -> OUTPUT_FULL) --- */
typedef struct {
  uint8_t *base;
  size_t cap;
  size_t pos;
  int err;
} w1_mux_t;

static W1_UNUSED void w1_mux_init(w1_mux_t *m, uint8_t *base, size_t cap) {
  m->base = base; m->cap = cap; m->pos = 0; m->err = 0;
}

/* Reserve n bytes, return their offset (cap+1 on overflow, sticky err). */
static W1_UNUSED size_t w1_mux_reserve(w1_mux_t *m, size_t n) {
  size_t off = m->pos;
  if (m->err) return m->cap + 1;
  if (n > m->cap - off) { m->err = 1; return m->cap + 1; }
  m->pos = off + n;
  return off;
}

static W1_UNUSED void w1_mux_u8(w1_mux_t *m, uint8_t v) {
  size_t o = w1_mux_reserve(m, 1);
  if (o <= m->cap) m->base[o] = v;
}

static W1_UNUSED void w1_mux_u16(w1_mux_t *m, uint16_t v) {
  w1_mux_u8(m, (uint8_t)(v & 0xff));
  w1_mux_u8(m, (uint8_t)((v >> 8) & 0xff));
}

static W1_UNUSED void w1_mux_u24(w1_mux_t *m, uint32_t v) {
  size_t o = w1_mux_reserve(m, 3);
  if (o <= m->cap) {
    m->base[o] = (uint8_t)v;
    m->base[o + 1] = (uint8_t)(v >> 8);
    m->base[o + 2] = (uint8_t)(v >> 16);
  }
}

static W1_UNUSED void w1_mux_u32(w1_mux_t *m, uint32_t v) {
  size_t o = w1_mux_reserve(m, 4);
  if (o <= m->cap) {
    m->base[o] = (uint8_t)v;
    m->base[o + 1] = (uint8_t)(v >> 8);
    m->base[o + 2] = (uint8_t)(v >> 16);
    m->base[o + 3] = (uint8_t)(v >> 24);
  }
}

static W1_UNUSED void w1_mux_bytes(w1_mux_t *m, const uint8_t *p, size_t n) {
  if (n == 0) return;
  {
    size_t o = w1_mux_reserve(m, n);
    if (o <= m->cap) memcpy(m->base + o, p, n);
  }
}

static W1_UNUSED void w1_mux_patch_u32(w1_mux_t *m, size_t at, uint32_t v) {
  if (m->err || at > m->cap || m->cap - at < 4) { m->err = 1; return; }
  m->base[at] = (uint8_t)v;
  m->base[at + 1] = (uint8_t)(v >> 8);
  m->base[at + 2] = (uint8_t)(v >> 16);
  m->base[at + 3] = (uint8_t)(v >> 24);
}

/* RIFF + size placeholder + WEBP. */
static W1_UNUSED void w1_mux_riff_begin(w1_mux_t *m) {
  w1_mux_bytes(m, (const uint8_t *)"RIFF", 4);
  w1_mux_u32(m, 0);
  w1_mux_bytes(m, (const uint8_t *)"WEBP", 4);
}

static W1_UNUSED void w1_mux_riff_end(w1_mux_t *m) {
  if (m->err || m->pos < 8 || m->pos - 8 > 0xffffffffu) {
    m->err = 1; return;
  }
  w1_mux_patch_u32(m, 4, (uint32_t)(m->pos - 8));
}

/* Chunk header (FourCC + size placeholder); returns payload offset. */
static W1_UNUSED size_t w1_mux_chunk_begin(w1_mux_t *m, const char *fcc) {
  w1_mux_bytes(m, (const uint8_t *)fcc, 4);
  w1_mux_u32(m, 0);
  return m->pos;
}

/* Fill chunk size (payload bytes since begin) + pad to even. */
static W1_UNUSED void w1_mux_chunk_end(w1_mux_t *m, size_t pay_off) {
  if (m->err || pay_off > m->pos || m->pos - pay_off > 0xffffffffu) {
    m->err = 1; return;
  }
  w1_mux_patch_u32(m, pay_off - 4, (uint32_t)(m->pos - pay_off));
  if ((m->pos - pay_off) & 1) w1_mux_u8(m, 0);
}

static W1_UNUSED void w1_mux_chunk_bytes(w1_mux_t *m, const char *fcc,
                                         const uint8_t *p, size_t n) {
  size_t o = w1_mux_chunk_begin(m, fcc);
  w1_mux_bytes(m, p, n);
  w1_mux_chunk_end(m, o);
}

/* Full VP8X chunk (mirror of the part-50 parser). */
static W1_UNUSED void w1_mux_vp8x(w1_mux_t *m, int w, int h, uint8_t flags) {
  size_t o = w1_mux_chunk_begin(m, "VP8X");
  w1_mux_u8(m, flags);
  w1_mux_u8(m, 0); w1_mux_u8(m, 0); w1_mux_u8(m, 0);   /* reserved */
  w1_mux_u24(m, (uint32_t)(w - 1));
  w1_mux_u24(m, (uint32_t)(h - 1));
  w1_mux_chunk_end(m, o);
}

/* Sum of metadata payload lengths (0 for NULL opts). Saturates. */
static W1_UNUSED size_t w1_enc_meta_len(const webp1_encode_opts_t *o) {
  uint64_t t = 0;
  if (!o) return 0;
  t = (uint64_t)o->iccp_len + (uint64_t)o->exif_len + (uint64_t)o->xmp_len;
  if (t > (uint64_t)(size_t)-1) return (size_t)-1;
  return (size_t)t;
}

/* VP8L worst-case work (level-independent: covers L9): ARGB + full ctx +
 * pixel buffers + worst-case sub-image buffers + alignment slack. Terms
 * mirror part 60 exactly (same defines); tw/th match w1_vp8l_encode_full. */
static W1_UNUSED size_t w1_vp8l_work_worst(int w, int h) {
  uint64_t npix = (uint64_t)(unsigned)w * (uint64_t)(unsigned)h;
  uint64_t tw = (uint64_t)((w + 7) >> 3), th = (uint64_t)((h + 7) >> 3);
  uint64_t t;
  t = (uint64_t)32 * npix;                       /* argb+res+pred+resB + 2x(match table+token memo) */
  t += (uint64_t)5 * npix;                       /* sv_dst + sv_modes (saved tiling) */
  t += (uint64_t)38 * tw * th;                   /* modes/modepix/ximg/saves (color tiles 4x finer) */
  t += (uint64_t)8 * npix;                       /* candidate list + slots */
  t += (uint64_t)4 * ((4u << 18) + 4);
  t += (uint64_t)2 * (uint64_t)W1_LE_MAXG * W1_LE_NC;  /* memo lens */
  t += (uint64_t)4 * (uint64_t)W1_LE_NC * 4;     /* counts x4 */
  t += (uint64_t)W1_LE_NC;                       /* lens */
  t += (uint64_t)W1_LE_NC * 4;                   /* codes */
  t += (uint64_t)2 * (uint64_t)W1_LE_NG * 4;     /* seq */
  t += (uint64_t)5 * (uint64_t)W1_LE_NG * 4;     /* htmp */
  t += (uint64_t)2048 * 4;                       /* cache */
  t += (uint64_t)14 * 1024 * 4;                  /* phist */
  if (npix <= (uint64_t)W1_LZ_OPT_MAXN) t += (uint64_t)16 * npix + 8; /* dp+back+2 near */
  t += (uint64_t)W1_LE_NC;                       /* flat */
  t += (uint64_t)2 * (uint64_t)W1_LE_MAXG * W1_LE_NC * 4;  /* gcounts+grcounts */
  t += (uint64_t)W1_LE_MAXG * W1_LE_NC * 4;      /* gcodes */
  t += (uint64_t)W1_LE_MAXG * W1_LE_NC;          /* glens */
  t += (uint64_t)8 * npix + 65536 + 256;         /* portfolio second payload */
  t += 4096;                                     /* bump alignment slack */
  if (t > (uint64_t)(size_t)-1) return (size_t)-1;
  return (size_t)t;
}

/* ALPH's own scratch: the VP8L main-stream bound plus the per-pixel delta
 * row and filter-trial arrays it allocates on top (measured: <= 1 B/px at
 * level 9, 2 B/px here is comfortably safe). */
static W1_UNUSED size_t w1_alph_work_need(int w, int h) {
  uint64_t t = w1_vp8l_work_worst(w, h)
             + 2 * (uint64_t)(unsigned)w * (unsigned)h + (uint64_t)(unsigned)w + 1024;
  if (t > (uint64_t)(size_t)-1) return (size_t)-1;
  return (size_t)t;
}

/* Scratch needed by the lossy path: alpha plane + ALPH scratch for it,
 * plus the VP8 encoder's planes, per-MB arrays and control-partition buffer.
 * `webp1_encode_lossy` carves exactly these three regions (the VP8 one is
 * anchored at the end), so this bound is met by construction. */
static W1_UNUSED size_t w1_enc_lossy_work_need(int w, int h) {
  const int mb_w = (w + 15) / 16, mb_h = (h + 15) / 16;
  const uint64_t n_mb = (uint64_t)(unsigned)mb_w * (unsigned)mb_h;
  uint64_t t = (uint64_t)(unsigned)w * (unsigned)h + 16       /* alpha plane */
             + w1_alph_work_need(w, h)                        /* ALPH/VP8L */
             + w1_vp8e_work_worst(mb_w, mb_h)                 /* VP8 planes */
             + 1094 + 32 * n_mb + 64 + 4096                   /* ctl + adapt */
             + (uint64_t)(unsigned)w * (unsigned)h * 4 + 64;  /* RGB score scratch */
  if (t > (uint64_t)(size_t)-1) return (size_t)-1;
  return (size_t)t;
}

/* ---- Shared per-frame payload writers (used by the still encoders and by
 * every animation frame, so buffer carving and bounds stay in one place). ---- */

#ifdef W1_SWEEP_TRACE
#include <stdio.h>
#endif
static W1_UNUSED void w1_lossy_effort(int effort, int *span, int *adapt,
                                      int *refine, int *ninit) {
  if (effort < 0) effort = 0; else if (effort > 9) effort = 9;
  *span = effort == 9 ? 128 : 0;
  *adapt = 0;
  *refine = effort == 9 ? 2 : 0;
  *ninit = effort == 9 ? 3 : 1;
}
/* Lossy: ALPH chunk when the frame carries transparency, then the VP8 chunk.
 * Sets *has_alpha_out for the caller's VP8X flags. Returns WEBP1_OK or an
 * error; *need is set on NO_MEMORY. */
static W1_UNUSED int w1_enc_lossy_payload(w1_mux_t *m, const uint8_t *rgba,
                                          size_t stride, int w, int h, int q,
                                          int filt_level, int sharpness,
                                          int alph_level, int effort,
                                          int *has_alpha_out,
                                          uint8_t *work, size_t work_cap,
                                          size_t *need) {
  const size_t aplane_sz = (size_t)(unsigned)w * (unsigned)h;
  int sweep_span, sweep_adapt, refine, ninit;
  w1_lossy_effort(effort, &sweep_span, &sweep_adapt, &refine, &ninit);
  const int large_frame = (uint64_t)(unsigned)w * (unsigned)h >= 1024u * 1024u;
  if (large_frame && effort >= 9) {
    sweep_span = 64;
    refine = 0;
  }
  const size_t wb = w1_enc_lossy_work_need(w, h);
  uint8_t *alpha = NULL, *vp8_work, *base;
  uint8_t *prepared = NULL;
  uint64_t *predcost = NULL;
  int chroma_act = -1;   /* mean |adjacent U/V step|, -1 = unknown */
  int chroma_range = 0;  /* max U/V span over the frame */
  size_t alen = 0, elen = 0, vp8_sz, apay, vpay;
  int has_alpha = 0, x, y, rc = 0;
  w1_bump_t bump_a;

  for (y = 0; y < h && !has_alpha; y++) {
    const uint8_t *row = rgba + (size_t)y * stride;
    for (x = 0; x < w; x++) if (row[x * 4 + 3] != 255) { has_alpha = 1; break; }
  }
  if (has_alpha_out) *has_alpha_out = has_alpha;

  {
    const int mb_w = (w + 15) / 16, mb_h = (h + 15) / 16;
    const size_t n_mb = (size_t)mb_w * (size_t)mb_h;
    const size_t vp8_need =
        w1_vp8e_work_worst(mb_w, mb_h) + 1094u + 32u * n_mb + 16u + 4096u +
        ((size_t)(unsigned)w * (unsigned)h * 4 + 64);   /* RGB score scratch */
    uint8_t *const endg = work + work_cap;
    uint8_t *lim;
    if (vp8_need + 32u > work_cap) {
      if (need) *need = wb;
      return WEBP1_ERR_NO_MEMORY;
    }
    lim = (uint8_t *)(void *)((uintptr_t)(endg - vp8_need) & ~(uintptr_t)15);
    base = work;
    if (has_alpha) {
      alpha = base;
      if ((size_t)(lim - base) < aplane_sz) {
        if (need) *need = wb;
        return WEBP1_ERR_NO_MEMORY;
      }
      base += aplane_sz;
      for (y = 0; y < h; y++) {
        const uint8_t *row = rgba + (size_t)y * stride;
        for (x = 0; x < w; x++)
          alpha[(size_t)y * (unsigned)w + x] = row[x * 4 + 3];
      }
      base = (uint8_t *)(void *)(((uintptr_t)base + 15u) & ~(uintptr_t)15);
    }
    if (base >= lim) {
      if (need) *need = wb;
      return WEBP1_ERR_NO_MEMORY;
    }
    w1_bump_init(&bump_a, base, (size_t)(lim - base));
    vp8_work = lim;
    vp8_sz = (size_t)(endg - lim);
  }

  if (has_alpha) {
    apay = w1_mux_chunk_begin(m, "ALPH");
    if (!m->err) {
      rc = w1_alph_encode(alpha, w, h, alph_level, m->base + m->pos,
                          m->cap - m->pos, &alen, &bump_a);
      if (rc == 0) m->pos += alen;
    }
    w1_mux_chunk_end(m, apay);
    if (rc == 1) { if (need) *need = wb; return WEBP1_ERR_NO_MEMORY; }
    if (rc == 2) return WEBP1_ERR_BAD_PARAM;
    if (rc == 3 || m->err) return WEBP1_ERR_OUTPUT_FULL;
  }
  {
    int mw = (w + 15) / 16, mh = (h + 15) / 16, r, c;
    size_t nm = (size_t)mw * mh, ys = nm * 256, us = nm * 64;
    base = (uint8_t *)(void *)(((uintptr_t)base + 7u) & ~(uintptr_t)7);
    /* The refinement scratch is required here, not optional: taking the
     * prepared path without it would hand the encoder box-averaged planes
     * where the non-prepared path produces refined ones, making the output
     * depend on how much work memory happened to be free. */
    const size_t pre_need = ys + 2 * us + nm * sizeof(uint64_t) + 8 +
                            w1_uvls_need(mw, mh);
    if (base <= vp8_work && (size_t)(vp8_work - base) >= pre_need) {
      int32_t *uvls_sc;
      prepared = base;
      predcost = (uint64_t *)(void *)(base + ys + 2 * us);
      uvls_sc = (int32_t *)(void *)(((uintptr_t)(base + ys + 2 * us +
                                     nm * sizeof(uint64_t)) + 7u) &
                                    ~(uintptr_t)7);
      w1_vp8e_rgb_to_yuv(rgba, stride, w, h, prepared, mw * 16,
                          prepared + ys, prepared + ys + us, mw * 8, mw, mh,
                          W1_UVLS > 0 ? uvls_sc : NULL);
      for (r = 0; r < mh; r++) for (c = 0; c < mw; c++)
        predcost[r * mw + c] = w1_vp8e_mb_predcost(prepared, mw * 16, w, h, c, r);
      /* Chroma activity: mean |adjacent U/V step| (scale-invariant). Text is
       * near-grey (low), photo carries chroma noise (high), gradient is
       * smooth (low). Drives the UV quant delta so it need not be searched. */
      {
        int uvs = mw * 8, cw = mw * 8, ch = mh * 8, yy;
        uint64_t acc = 0; size_t cnt = 0;
        const uint8_t *U = prepared + ys, *V = prepared + ys + us;
        int umin = 255, umax = 0, vmin = 255, vmax = 0;
        for (yy = 0; yy < ch; yy++) {
          const uint8_t *ur = U + (size_t)yy * uvs, *vr = V + (size_t)yy * uvs;
          int xx;
          for (xx = 1; xx < cw; xx++) {
            int du = (int)ur[xx] - (int)ur[xx - 1];
            int dv = (int)vr[xx] - (int)vr[xx - 1];
            acc += (uint64_t)(du < 0 ? -du : du) + (uint64_t)(dv < 0 ? -dv : dv);
            cnt++;
          }
          for (xx = 0; xx < cw; xx++) {
            int u = ur[xx], v = vr[xx];
            if (u < umin) umin = u; if (u > umax) umax = u;
            if (v < vmin) vmin = v; if (v > vmax) vmax = v;
          }
        }
        chroma_act = cnt ? (int)(acc / cnt) : 0;
        /* Chroma range captures smooth ramps that the adjacent-step
         * activity misses (a ramp has a tiny step but a large span). */
        chroma_range = (umax - umin) > (vmax - vmin) ? umax - umin
                                                     : vmax - vmin;
      }
    }
  }
  if (large_frame && !has_alpha && q >= 20 && q <= 40 &&
      chroma_act >= 0 && chroma_act <= 6 &&
      chroma_range >= 64) {
    sweep_span = 0;
    refine = 0;
    ninit = 1;
  }
  vpay = w1_mux_chunk_begin(m, "VP8 ");
  if (!m->err) {
    /* Per-frame loop-filter search: the fitted level with the caller's
     * sharpness is the baseline; a bounded candidate set of levels, plus
     * sharpness at the strong-filter point, is evaluated with a full
     * sweep+RDO run each.  The winner is the shortest payload whose decoded
     * (loop-filtered) SSE is no worse than the baseline's; a cell with no
     * such candidate keeps the baseline.  Candidates are tried only when
     * the content pre-pass is available and the frame is big enough for the
     * level field to be worth a probe (>=16 MBs keeps the 32x32 header pin
     * in test_lossy_filter_header on the fitted mapping).
     *
     * Round-2 pruning: on the fixed grid only levels 16-20 ever produced a
     * strict win (edges 128 at q30/q75; every other probe level - 0, /2,
     * x2, 40, 63 - won nowhere), so the candidate set dropped fitted/2,
     * 40 and the level-20 sharpness probes: 15 sweep+RDO runs per frame
     * became at most 9 at identical grid bytes.  The winner's payload is
     * already in place when it was the last candidate evaluated, so the
     * final re-emit is skipped in that case.
     *
     * Round-3 content gate: all fixed-grid wins were on grey structured
     * frames (edges), and the search is pure overhead on smooth (gradient/
     * flat) and chroma-noisy (photo/mixed) content - so it only runs when
     * the frame class is "textured, low chroma" (the same split that drives
     * the UV delta).  The content scans happen once per frame, not per
     * attempt. */
    int textured = 0;
    const int is_flat = w1_frame_is_flat(rgba, stride, w, h);
    /* Keeping the baseline payload lets the winner be restored by a copy
     * instead of a third full encode.  The upper half of the sweep's
     * scratch gap is free here: the block that uses that gap is gated on
     * `textured`, and the duel below is gated on its negation.  An attempt
     * whose payload reaches past the split invalidates the copy, and the
     * re-emit takes over - so the copy is an optimisation, never a
     * correctness assumption. */
    uint8_t *kept = NULL;
    size_t kept_cap = 0, kept_len = 0;
    if (predcost) {
      size_t j, nmb = (size_t)((w + 15) / 16) * (size_t)((h + 15) / 16);
      for (j = 0; j < nmb; j++)
        if (predcost[j] > (uint64_t)256 * 32 * 32) { textured = 1; break; }
      if (!textured && !is_flat && nmb >= 16) {
        uint8_t *gap = (uint8_t *)(void *)(predcost + nmb);
        if (vp8_work > gap) {
          kept_cap = (size_t)(vp8_work - gap) / 2;
          kept = gap + kept_cap;
        }
      }
    }
    int att, n_att = 0;
    int att_level[16], att_sharp[16], att_nobump[16];
    int win_level = filt_level, win_sharp = sharpness, win_nobump = 0;
    int last_ok = 0, last_level = filt_level, last_sharp = sharpness;
    int last_nobump = 0, win_att = 0;
    size_t win_len = 0;
    uint64_t floor_sse = 0;
    {
      int n_mb = ((w + 15) / 16) * ((h + 15) / 16);
      att_level[n_att] = filt_level; att_sharp[n_att] = sharpness;
      att_nobump[n_att] = 0; n_att++;
      /* Smooth-frame deblock duel.  A frame whose every MB is under the
       * complexity threshold gets its mapped level forced up to 20 inside
       * the core, because the mapped (q-14)/2 under-filters the block edges
       * quantisation leaves in a gentle ramp (gradient 128 q80: 40.06 ->
       * 42.47 dB at identical bytes).  Near-lossless, that same bump is
       * pure damage: gray_ramp is exactly reconstructed by TM_PRED, there
       * are no edges to hide, and level 20 smears the result (64 q100:
       * 50.87 dB filtered vs 53.02 dB with the mapped level, same bytes).
       * The two cases are indistinguishable before the frame is coded -
       * predcost is ~0 for both - so this settles it by measurement: one
       * extra run with the bump suppressed, adopted only when it is a
       * strict improvement on one axis and no worse on the other.  The
       * level is a header field, so the usual outcome is identical bytes
       * at better SSE. */
      if (effort >= 9 && !large_frame && predcost && n_mb >= 16 && !textured && !is_flat) {
        att_level[n_att] = filt_level; att_sharp[n_att] = sharpness;
        att_nobump[n_att] = 1; n_att++;
      }
      /* Automatic max-effort tool: measured -86B on the whole LY grid for
       * +4.2x total encode time (11x gated cells, several 0B wins), so it
       * only runs at effort 9 (the "best" tier). */
      if (effort >= 9 && !large_frame && predcost && n_mb >= 16 && textured &&
          !is_flat && chroma_act <= 6) {
        int want[5], nw = 0, i, j;
        want[nw++] = 0;
        want[nw++] = filt_level * 2;
        want[nw++] = 16;
        want[nw++] = 20;
        want[nw++] = 63;
        for (i = 0; i < nw; i++) {
          int L = want[i], dup = 0;
          if (L < 0) L = 0; else if (L > 63) L = 63;
          for (j = 0; j < n_att; j++)
            if (att_level[j] == L && att_sharp[j] == 0) { dup = 1; break; }
          if (!dup && n_att < 9) {
            att_level[n_att] = L; att_sharp[n_att] = 0;
            att_nobump[n_att] = 0; n_att++;
          }
        }
        for (i = 1; i <= 3 && n_att < 9; i++) {
          att_level[n_att] = 16; att_sharp[n_att] = i;
          att_nobump[n_att] = 0; n_att++;
        }
      }
    }
    for (att = 0; att <= n_att && rc == 0; att++) {
    const int lev = att < n_att ? att_level[att] : win_level;
    const int shp = att < n_att ? att_sharp[att] : win_sharp;
    const int nbump = att < n_att ? att_nobump[att] : win_nobump;
    if (att == n_att && last_ok && win_level == last_level &&
        win_sharp == last_sharp && win_nobump == last_nobump) {
      elen = win_len;                          /* payload already in place */
      m->pos += elen;
      break;
    }
    if (att == n_att && win_att == 0 && kept_len == win_len && win_len > 0) {
      memcpy(m->base + m->pos, kept, win_len);   /* saved, not re-encoded */
      elen = win_len;
      m->pos += elen;
      break;
    }
    size_t att_len = 0;
    uint64_t att_sse = 0;
    static const int offs[3] = { 8, -8, 0 };
    uint64_t sses[3] = {0, 0, 0}, unfiltered_sses[3] = {0, 0, 0};
    const int score_init = ninit > 1 || sweep_span >= 16 || refine;
    /* (textured / is_flat are per-frame, computed once before the search;
     * textured drives the SSE slack, is_flat the min-SSE budget below.) */
    w1_vp8e_rd_t rduv;
    rduv.probs = NULL;
    rduv.force_b = 0;
    rduv.lambda_num = 8;
    rduv.polish = 0;
    rduv.no_lf_bump = nbump;
    /* UV delta from content (replaces textured/not binary): textured with
     * chroma noise (photo) -> coarser +6; textured near-grey (text) -> finer
     * -4; smooth ramp (gradient) -> -2; constant -> 0. */
    {
      int uv_def;
      if (textured) uv_def = (chroma_act > 6) ? 15 : 0;
      else if (is_flat) uv_def = 0;
      else {
        uv_def = -2 - (chroma_range >> 6);
        if (uv_def < -15) uv_def = -15;
      }
      if (!has_alpha && large_frame && q >= 20 && q <= 40 &&
          chroma_act <= 6 &&
          chroma_range >= 64) uv_def = -13;
      rduv.uv_delta = uv_def;
    }
    size_t lens[3];
    uint8_t tried[128] = {0};
    int qs[3], i, bi = 2;
    for (i = 3 - ninit; i < 3; i++) {
      int qc = q + offs[i];
      size_t clen = 0;
      if (qc < 0) qc = 0; else if (qc > 127) qc = 127;
      qs[i] = qc;
      tried[qc] = 1;
      rc = w1_vp8_encode_frame_core(rgba, stride, w, h, qc,
                               lev, shp,
                               m->base + m->pos, m->cap - m->pos, &clen,
                               vp8_work, vp8_sz, score_init ? &sses[i] : NULL,
                               score_init && ninit > 1 ? &unfiltered_sses[i] : NULL,
                               prepared, predcost, &rduv, 0);
      if (rc) break;
      lens[i] = clen;
#ifdef W1_SWEEP_TRACE
      fprintf(stderr, "SWEEP %d %u %llu\n", qc, (unsigned)clen, (unsigned long long)sses[i]);
#endif
    }
    if (rc == 0) {
      int bestq, lastq = q;
      uint64_t bestsse;
      size_t bestlen;
      int fin_refined = 0, fin_q, fin_delta = 0, fin_have = 0;
      uint8_t fin_probs[1056];
      for (i = 3 - ninit; i < 3; i++)
        if (unfiltered_sses[i] * 32 <= unfiltered_sses[2] * 31 &&
            lens[i] < lens[bi]) bi = i;
      bestq = qs[bi]; bestsse = sses[bi]; bestlen = lens[bi];
      fin_q = bestq;
#ifdef W1_SWEEP_TRACE
      fprintf(stderr, "START %d %u %llu bi %d\n", bestq, (unsigned)bestlen, (unsigned long long)bestsse, bi);
#endif
      /* Audit #2: the init q+-8 comparison uses unfiltered SSE and a
       * different candidate set than the sweep, so letting an offset winner
       * suppress the whole sweep can miss the best RD point. Track the init
       * winner but always give the sweep a chance; it only adopts within
       * budget and the SSE cap. */
      /* Content gate for the surplus-aware short pick (tracker below) and
       * for letting the RDO refinement stage run on the point it picks:
       * textured low-chroma frames. q is the q_index: >=70 is the original
       * coarse regime; the 20..29 and 32..55 mid bands are measured
       * extensions (totals drop, no cell grows >1%). q 30/31 (public q65)
       * and 56..69 (public q15/q20) regressed cells and stay out. */
      const int spare_db = textured && chroma_act <= 6 &&
                           (q >= 70 || (q >= 20 && q <= 29) ||
                            (q >= 32 && q <= 55));
      if (sweep_span > 0) {
        int span, step, off;
        int improved = 0;
#ifndef W1_SWEEP_SSE_SLACK
#define W1_SWEEP_SSE_SLACK 300  /* +1.14 dB distortion slack (per 1000) */
#endif
#ifndef W1_SWEEP_SSE_SLACK_CHROMA
#define W1_SWEEP_SSE_SLACK_CHROMA 400  /* colour-noise frames only */
#endif
#ifndef W1_SWEEP_SLACK_Q
#define W1_SWEEP_SLACK_Q 32    /* q_index below which the slack tapers to 0 */
#endif
        /* Bench criterion is "bytes <= lib AND psnr >= lib-0.5", so allow
         * adopting a shorter, slightly-coarser stream: SSE may rise up to
         * the slack above the base-q encode, but never beyond.  Content
         * split (chroma_act = mean |adjacent U/V step|, >6 = photo/mixed
         * colour noise): noise-like frames have a flat RD curve where the
         * wider budget buys 2-7x the bytes of the structured cap - measured
         * on the fixed grid vs R1: 300 -> -13.1 kB on mixed+photo for
         * -0.3 dB, 400 -> -7.5 kB more for -0.28 dB (no cell grows, all
         * other kinds byte-identical); grey structured frames
         * (text/edges/checker) keep the tighter one. */
        /* Taper the slack to zero at the finest quantizers. The slack
         * exists so a coarser, shorter encode can win at low rates, where
         * webp1's rate-distortion curve sits above the reference's. Applied
         * flat it also fires when the caller asked for high quality, and
         * there it walks away from the very operating points the request
         * was for: that, not any coding deficit, is what capped the encoder
         * at 25.45 dB on a 600x211 photo while the reference reached
         * 27.24 dB. Below q_index W1_SWEEP_SLACK_Q the budget scales with
         * q, so q_index 0 spends nothing and the coarse regime is
         * untouched. */
        const int sse_slack0 = large_frame ? 100 :
            (chroma_act > 6) ? W1_SWEEP_SSE_SLACK_CHROMA : W1_SWEEP_SSE_SLACK;
        const int sse_slack = q < W1_SWEEP_SLACK_Q
            ? (int)((long)sse_slack0 * q / W1_SWEEP_SLACK_Q) : sse_slack0;
        /* Smooth non-flat frames (ramps): bytes are a staircase in q and the
         * ratchet can lock the sweep onto the base plateau before a coarser
         * step is reached (gradient q90: q18 is 36% shorter at +0.42 dB, but
         * the early q8 adoption tightens dyn_cap first). They get the slack
         * in sse_cap (seed for dyn_cap) and pick the shortest candidate
         * within the guard-band floor instead of the ratchet. Flat frames
         * keep the exact cap (budget rule governs them). */
        const int smooth_frame = !textured && !is_flat;
        const uint64_t sse_cap = (textured || smooth_frame)
            ? sses[2] + sses[2] * (uint64_t)sse_slack / 1000
            : sses[2];
        /* Quality memory: the cap above is anchored to the BASE SSE, so a
         * walk that finds a much-better point (e.g. an exact-SSE trial on
         * plateau content) may then adopt arbitrarily worse points back up
         * to the base-anchored cap - shorter always wins and quality is
         * forgotten (checkerboard q65: exact points found AND abandoned,
         * final q109 at 48.7dB vs libwebp 59.4dB). dyn_cap ratchets down to
         * the best SSE seen so far (plus slack, never loosened), so later
         * adoptions stay within slack of the best quality found, not the
         * base quality. No-op when nothing better than base appears. */
        uint64_t dyn_cap = sse_cap, best_anchor = bestsse;
        /* e6 textured coarse-q: webp1 lands 27-50% under reference bytes, so
         * spend a bounded slice of that headroom on strictly-lower SSE.
         * Anchored to the base length so adoptions cannot ratchet. */
        size_t budget = (effort >= 6 && effort <= 8 && q >= 52 &&
                         w >= 128 && h >= 128)
                        ? bestlen + (bestlen >> 4)   /* +6.25% */
                        : bestlen;
        /* Constant-colour frames: bytes barely move with q, so allow a
         * longer (finer) point when it strictly lowers SSE (min-SSE first).
         * Gradient is excluded: min-SSE-first opened 69 gradient size gaps.
         * Flat byte counts stay well under the reference, so no size gap appears. */
        const int flat_frame = !textured && is_flat;
        if (flat_frame) budget = bestlen + (bestlen >> 1) + 16;
        /* Surplus-aware short pick: the adoption rule above keeps the best
         * decoded SSE within the cap (quality memory), so on textured
         * low-chroma frames it walks past strictly shorter points that sit
         * inside the same distortion budget - e.g. text-128 q10 ends at a
         * 1308 B quality point while q56 yields 1180 B at 0.95x the base
         * SSE (better than the base encode itself). Track the shortest
         * candidate that passes the budget and the cap, and prefer it: the
         * bytes drop while decoded SSE never leaves base+slack, the same
         * budget the current pick already certified.
         *
         * Scoped to the coarse quantizer regime (q_index >= 70, public
         * quality <= ~11): there libwebp's rate-quality on structured grey
         * content collapses while webp1 keeps a large PSNR surplus (text q10
         * +3.4..+7.0 dB, checker1 +40..+69 dB, measured on the fixed grid),
         * so the spare dB can be traded for bytes. At finer quantizers the
         * margins are tight (text-128 q50 +0.73 dB, edges-128 q50 -1.16 dB)
         * and a shorter pick risks the lib-0.5 criterion; frames there keep
         * the quality-greedy choice. */
        size_t short_len = 0;
        uint64_t short_sse = 0;
        int short_q = 0;
        /* Monotone fences (textured frames). Bytes rise as q_index falls
         * and decoded SSE rises as it climbs, while every bound a candidate
         * is judged by only tightens (budget is fixed; dyn_cap only falls).
         * So a finer probe over budget condemns every finer q_index, and a
         * coarser probe well past the loosest cap it could still meet
         * condemns every coarser one. Only the step-4 grid sets fences:
         * next to the base the landscape is noisy (text-64 q90: q8 694 B
         * over budget, q6 635 B the winner), and the fill-in passes still
         * explore everything between base and fence. The SSE fence keeps a
         * margin for the same noise (edges-64 q10: q91 at 1.20x the cap,
         * q99 back inside and adopted). Smooth ramps stay unfenced: their
         * byte staircase moved 13 of 30 cells. Measured byte-identical on
         * the 105-cell e6 grid at 1.24x encode speed. */
#ifndef W1_SWEEP_FENCE_SLACK
#define W1_SWEEP_FENCE_SLACK 250   /* SSE fence margin (per 1000 of cap) */
#endif
        int lo_dead = -1, hi_dead = 128;
        for (span = 16; span <= sweep_span && rc == 0; span *= 2) {
          if (sweep_adapt && span > sweep_adapt && !improved) break;
          improved = 0;
          for (step = large_frame ? 8 : 4;
               step >= (large_frame ? 8 : 1) && rc == 0; step /= 2) {
            for (off = step; off <= span && rc == 0; off += step) {
              int sign;
              for (sign = 1; sign >= (large_frame ? 1 : -1); sign -= 2) {
                int qc = q + sign * off;
                uint64_t csse;
                size_t clen = 0;
                if (qc < 0 || qc > 127 || tried[qc]) continue;
                if (qc <= lo_dead || qc >= hi_dead) continue;
                tried[qc] = 1;
                rc = w1_vp8_encode_frame_core(rgba, stride, w, h, qc,
                                         lev, shp,
                                         m->base + m->pos, m->cap - m->pos,
                                         &clen, vp8_work, vp8_sz, &csse, NULL, prepared, predcost, &rduv,
                                         budget);
                lastq = qc;
                if (textured && step == 4) {
                  const uint64_t hcap = spare_db ? sse_cap : dyn_cap;
                  if (qc < q && qc > lo_dead &&
                      (rc == 3 || (rc == 0 && clen > budget)))
                    lo_dead = qc;
                  if (qc > q && qc < hi_dead && rc == 0 &&
                      csse > hcap + hcap * W1_SWEEP_FENCE_SLACK / 1000)
                    hi_dead = qc;
                }
                if (rc == 3) { rc = 0; continue; }
                if (rc) break;
#ifdef W1_SWEEP_TRACE
                fprintf(stderr, "SWEEP %d %u %llu\n", qc, (unsigned)clen, (unsigned long long)csse);
#endif
                if (spare_db && clen <= budget && csse <= sse_cap &&
                    (short_len == 0 || clen < short_len ||
                     (clen == short_len && csse < short_sse))) {
                  short_q = qc; short_len = clen; short_sse = csse;
                }
                if (clen <= budget &&
                    (flat_frame ? (csse < bestsse)
                     : smooth_frame ? (csse <= sse_cap && clen < bestlen)
                     : (csse <= dyn_cap &&
                        (clen < bestlen || csse < bestsse)))) {
                  bestq = qc; bestsse = csse; bestlen = clen; improved = 1;
                  if (textured && csse < best_anchor) {
                    uint64_t nc;
                    best_anchor = csse;
                    nc = csse + csse * (uint64_t)sse_slack / 1000;
                    if (nc < dyn_cap) dyn_cap = nc;
                  }
#ifdef W1_SWEEP_TRACE
                  fprintf(stderr, "ADOPT %d %u %llu\n", qc, (unsigned)clen, (unsigned long long)csse);
#endif
                }
              }
            }
          }
        }
        if (short_len && (short_len < bestlen ||
                          (short_len == bestlen && short_sse < bestsse))) {
          bestq = short_q; bestlen = short_len; bestsse = short_sse;
        }
      }
      if (rc == 0 && bestq != lastq) {
        rc = w1_vp8_encode_frame_core(rgba, stride, w, h, bestq,
                                 lev, shp,
                                 m->base + m->pos, m->cap - m->pos, &bestlen,
                                 vp8_work, vp8_sz, NULL, NULL, prepared, predcost, &rduv,
                                 0);
      }
#ifndef W1_NO_RDO_REFINEMENT
      /* The short pick lands on a coarser point than the historical
       * quality-greedy winner; give it the same RDO refinement the e4+ tiers
       * get (the block only accepts streams at or below the picked point's
       * SSE, so it can only shave bytes, never quality). At e0 the spare
       * path skips picks already >3dB below the base SSE (under 2x):
       * checker1-128 q10 picks at 0.002x base where the sweep already found
       * the floor and the full search spends ~1.7s for 0 bytes, while text
       * (0.94x) and edges (1.03x) do gain. The e4+ rule is unchanged. */
      if (rc == 0 &&
          (refine || (sweep_span && spare_db && bestsse <= 2 * sses[2])) && predcost) {
        const size_t nm = (size_t)((w + 15) / 16) * ((h + 15) / 16);
        uint8_t *saved = (uint8_t *)(void *)(predcost + nm);
        size_t j;
        int textured = 0;
        for (j = 0; j < nm; j++)
          if (predcost[j] > (uint64_t)256 * 32 * 32) textured = 1;
        if (textured && (size_t)((kept ? kept : vp8_work) - saved) >= bestlen) {
          w1_vp8d_t header;
          w1_bool_t ctl;
          uint8_t probs[1056];
          uint8_t *payload = m->base + m->pos;
          const uint64_t target = bestsse;
          w1_vp8e_rd_t rd;
          size_t clen;
          int pass, delta, center = bestq, best_delta = 0, fine_center = 0;
          struct { int q, delta; size_t len; uint64_t sse; } cache[128];
          int cache_n = 0;
          memcpy(saved, payload, bestlen);
          rd.probs = w1k_vp8_coef_dflt; rd.force_b = 1;
          rd.lambda_num = 8; rd.uv_delta = 0; rd.polish = 0;
          rd.no_lf_bump = nbump;
          if (w1_vp8_encode_frame_core(rgba, stride, w, h, center,
                lev, shp, payload, m->cap - m->pos, &clen,
                vp8_work, vp8_sz, NULL, NULL, prepared, predcost, &rd, 0) == 0) {
            unsigned tag = (unsigned)payload[0] | ((unsigned)payload[1] << 8) |
                           ((unsigned)payload[2] << 16);
            memset(&header, 0, sizeof(header));
            w1_bool_init(&ctl, payload + 10, tag >> 5);
            (void)w1_vp8_parse_header(&ctl, &header);
            memcpy(probs, header.coef_probs, sizeof(probs));
            memcpy(fin_probs, probs, sizeof(fin_probs));
            fin_have = 1;
            rd.probs = probs; rd.force_b = 0; rd.lambda_num = 4;
            rd.polish = 0;
            for (pass = 0; pass < (large_frame ? 1 : 2); pass++) {
              if (pass) fine_center = best_delta;
#ifndef W1_RDO_UV_COARSE
#define W1_RDO_UV_COARSE 12
#endif
#ifndef W1_RDO_TRIALS
#define W1_RDO_TRIALS 5
#endif
              /* Spare frames refine from the short pick, not from a
               * quality-greedy winner, so spend a finer q step on them at
               * every effort: delta/3 offsets become +-1..+-4 instead of
               * only +-2,+-4 (text-128 q10e0: 1200 -> 1124 B at +1.9 dB).
               * Non-spare frames keep the historical step. */
              const int dstep = large_frame ? 12 :
                  pass ? 4 : ((refine && !spare_db) ? 6 : 3);
              for (delta = pass ? fine_center - 2 : -W1_RDO_UV_COARSE;
                   delta <= (pass ? fine_center + 2 : W1_RDO_UV_COARSE); delta += dstep) {
                int trial, qc = refine == 1 ? center : center - delta / 3, lo = -1, hi = 128;
                if (delta < -15 || delta > 15) continue;
                rd.uv_delta = delta;
                for (trial = 0; trial < (large_frame ? 4 :
                                         refine == 1 ? 1 : W1_RDO_TRIALS); trial++) {
                  uint64_t csse;
                  int cr, next;
                  if (qc < 0 || qc > 127) break;
                  int ci;
                  for (ci = 0; ci < cache_n; ci++)
                    if (cache[ci].q == qc && cache[ci].delta == delta) break;
                  if (ci < cache_n) {
                    clen = cache[ci].len; csse = cache[ci].sse;
                  } else {
                    cr = w1_vp8_encode_frame_core(rgba, stride, w, h, qc,
                      lev, shp, payload, m->cap - m->pos, &clen,
                      vp8_work, vp8_sz, &csse, NULL, prepared, predcost, &rd, 0);
                    if (cr) break;
                    if (cache_n < 128) {
                      cache[cache_n].q = qc; cache[cache_n].delta = delta;
                      cache[cache_n].len = clen; cache[cache_n].sse = csse;
                      cache_n++;
                    }
                  }
                  if (csse <= target) {
                    lo = qc;
                    /* Spare frames accept a strict byte win only: their
                     * target already spent the distortion surplus, and an
                     * equal-size/internal-SSE swap decoded 0.5 dB worse on
                     * edges-64 q10 (internal SSE misses that regression). */
                    if (clen < bestlen ||
                        (clen == bestlen && csse < bestsse && !spare_db)) {
                      bestsse = csse; bestlen = clen; best_delta = delta;
                      fin_refined = 1; fin_q = qc; fin_delta = delta;
                      memcpy(saved, payload, bestlen);
                    }
                  } else hi = qc;
                  next = lo >= 0 && hi < 128 ? (lo + hi) / 2 : qc + (csse <= target ? 2 : -2);
                  if (next == qc) break;
                  qc = next;
                }
              }
            }
          }
          memcpy(payload, saved, bestlen);
        }
      }
#endif
      /* Exact-cost coefficient polish, final step: the sweep and refinement
       * above ran the baseline heuristic, so their operating-point choices
       * are untouched; only the winning payload is re-encoded with the
       * trellis and adopted when it is strictly shorter inside a small
       * quality guard (<= 10% SSE, ~0.41 dB). */
      if (rc == 0 && refine && prepared && predcost) {
        const size_t nm = (size_t)((w + 15) / 16) * ((h + 15) / 16);
        uint8_t *scratch = (uint8_t *)(void *)(predcost + nm);
        size_t scratch_cap = (size_t)((kept ? kept : vp8_work) - scratch);
        uint8_t *payload = m->base + m->pos;
        if (scratch_cap >= bestlen && bestlen > 0) {
          uint64_t psse = 0;
          size_t plen = 0;
          int ok;
          memcpy(scratch, payload, bestlen);
          if (fin_refined) {
            w1_vp8e_rd_t rd;
            rd.probs = fin_have ? fin_probs : w1k_vp8_coef_dflt;
            rd.force_b = 0; rd.lambda_num = 4; rd.uv_delta = fin_delta;
            rd.polish = 1; rd.no_lf_bump = nbump;
            ok = w1_vp8_encode_frame_core(rgba, stride, w, h, fin_q, lev, shp,
                   payload, m->cap - m->pos, &plen, vp8_work, vp8_sz, &psse,
                   NULL, prepared, predcost, &rd, 0) == 0;
          } else {
            rduv.polish = 1;
            ok = w1_vp8_encode_frame_core(rgba, stride, w, h, bestq, lev, shp,
                   payload, m->cap - m->pos, &plen, vp8_work, vp8_sz, &psse,
                   NULL, prepared, predcost, &rduv, 0) == 0;
            rduv.polish = 0;
          }
          if (ok && plen + 1 < bestlen && psse <= bestsse + bestsse / 10) {
            bestlen = plen;
            bestsse = psse;
          } else {
            memcpy(payload, scratch, bestlen);
          }
        }
      }
      att_len = bestlen;
      att_sse = bestsse;
    }
    if (rc != 0) {
      if (att == 0 || att == n_att) break;     /* baseline / emit failure */
      rc = 0;                                   /* candidate too big: skip */
      last_ok = 0;
      continue;
    }
    if (att < n_att && kept) {
      if (att_len > kept_cap) kept_len = 0;    /* this payload crossed it */
      else if (att == 0) { memcpy(kept, m->base + m->pos, att_len);
                           kept_len = att_len; }
    }
    if (att == 0) {
      floor_sse = att_sse;
      win_len = att_len;
    } else if (att < n_att) {
      /* Clauses 1-2 are strict Pareto: better on one axis, no worse on the
       * other.  Clause 2 is what usually settles the smooth-frame duel,
       * since the deblock level does not move the payload length.
       *
       * Clause 3 is for the cells where it does move: with the bump in
       * place the filter caps the reachable PSNR, so the sweep correctly
       * stops spending bits and lands short and blurry (gray_ramp 64 q90:
       * 160 B / 47.75 dB against 172 B / 51.63 dB unbumped).  The exchange
       * rate is the sweep's own - it already gives up 10% of SSE for a byte
       * win (psse <= bestsse + bestsse/10 above), so this spends at most 10%
       * of the bytes for an SSE win of at least the same 10%.  Restricted
       * to the duel: the effort-9 level search keeps its bytes-first rule. */
      if ((att_len < win_len && att_sse <= floor_sse) ||
          (att_len <= win_len && att_sse < floor_sse) ||
          (nbump && att_len <= win_len + win_len / 10 &&
           att_sse * 10 <= floor_sse * 9)) {
        win_len = att_len;
        win_level = lev;
        win_sharp = shp;
        win_nobump = nbump;
        win_att = att;
        if (att_sse < floor_sse) floor_sse = att_sse;
      }
    } else {
      elen = att_len;                          /* winner, re-emitted last */
      m->pos += elen;
    }
    if (att < n_att) {
      last_ok = 1; last_level = lev; last_sharp = shp; last_nobump = nbump;
    }
    }
  }
  w1_mux_chunk_end(m, vpay);
  if (rc == 1) { if (need) *need = wb; return WEBP1_ERR_NO_MEMORY; }
  if (rc == 2) return WEBP1_ERR_BAD_PARAM;
  if (rc == 3 || m->err) return WEBP1_ERR_OUTPUT_FULL;
  return WEBP1_OK;
}

/* Lossless: a single VP8L chunk carrying color and alpha together. */
static W1_UNUSED int w1_enc_lossless_payload(w1_mux_t *m, const uint8_t *rgba,
                                              size_t stride, int w, int h,
                                              int level, int *has_alpha_out,
                                              uint8_t *work, size_t work_cap,
                                              size_t *need) {
  const int n = w * h;   /* safe: caller validated the dimensions */
  uint8_t *argb8; uint32_t *argb;
  int has_alpha, rc = 0;
  size_t vpay, elen = 0;
  w1_bump_t bump;
  w1_bump_init(&bump, work, work_cap);
  argb8 = (uint8_t *)w1_bump_alloc(&bump, (size_t)n * 4, 4);
  if (!argb8) {
    if (need) *need = webp1_encode_work_bound(w, h, 1);
    return WEBP1_ERR_NO_MEMORY;
  }
  argb = (uint32_t *)(void *)argb8;
  has_alpha = w1_rgba_to_argb(rgba, stride, w, h, argb);
  if (has_alpha_out) *has_alpha_out = has_alpha;
  vpay = w1_mux_chunk_begin(m, "VP8L");
  if (!m->err) {
    rc = w1_vp8l_encode_full(argb, w, h, level, has_alpha, m->base + m->pos,
                             m->cap - m->pos, &elen, &bump);
    if (rc == 0) m->pos += elen;
  }
  w1_mux_chunk_end(m, vpay);
  if (rc == 1) { if (need) *need = webp1_encode_work_bound(w, h, 1); return WEBP1_ERR_NO_MEMORY; }
  if (rc == 2) return WEBP1_ERR_BAD_PARAM;
  if (rc == 3 || m->err) return WEBP1_ERR_OUTPUT_FULL;
  return WEBP1_OK;
}

static W1_UNUSED size_t webp1_encode_work_bound(int w, int h, int lossless) {
  uint64_t t;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) return 0;
  if (!lossless) return w1_enc_lossy_work_need(w, h);
  t = w1_vp8l_work_worst(w, h);
  if (t > (uint64_t)(size_t)-1) return (size_t)-1;
  return (size_t)t;
}

/* VP8L payload true upper bound: every token consumes >= 1 px and costs
 * <= 60 bits (literal: 4 x 15-bit codes; match: 15+0 + 15+18), plus
 * sub-stream tables (~30 KB worst) and container+metadata. */
static W1_UNUSED size_t webp1_encode_bound(int w, int h, int lossless,
                                           const webp1_encode_opts_t *o) {
  uint64_t npix, t;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) return 0;
  npix = (uint64_t)(unsigned)w * (uint64_t)(unsigned)h;
  t = (lossless ? (uint64_t)8 : (uint64_t)12) * npix;
  t += (lossless ? 65536 : 131072) + 256;
  t += w1_enc_meta_len(o);
  if (t > (uint64_t)(size_t)-1) return 0;   /* rgba_size precedent */
  return (size_t)t;
}

/* Shared still-image input validation. Returns WEBP1_OK or the error;
 * sets *stride_out (0 resolved to tight). */
static W1_UNUSED int w1_enc_check_still(const uint8_t *rgba, int w, int h,
                                        size_t stride, const uint8_t *out,
                                        const size_t *out_len,
                                        size_t *stride_out) {
  if (!rgba || !out || !out_len) return WEBP1_ERR_BAD_PARAM;
  if (w < 1 || h < 1) return WEBP1_ERR_BAD_PARAM;
  if (w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) return WEBP1_ERR_TOO_LARGE;
  if (stride == 0) stride = (size_t)w * 4;
  else if (stride < (size_t)w * 4) return WEBP1_ERR_BAD_PARAM;
  if (h > 1 && stride > ((size_t)-1 - (size_t)w * 4) / (size_t)(h - 1))
    return WEBP1_ERR_TOO_LARGE;
  *stride_out = stride;
  return WEBP1_OK;
}

static W1_UNUSED int w1_enc_check_meta(const webp1_encode_opts_t *o) {
  if (!o) return WEBP1_OK;
  if ((!o->iccp && o->iccp_len) || (!o->exif && o->exif_len) ||
      (!o->xmp && o->xmp_len))
    return WEBP1_ERR_BAD_PARAM;
  return WEBP1_OK;
}

static W1_UNUSED int webp1_encode_lossless(const uint8_t *rgba, int w, int h,
                                           size_t stride,
                                           const webp1_encode_opts_t *o,
                                           uint8_t *out, size_t out_cap,
                                           size_t *out_len, uint8_t *work,
                                           size_t work_cap, size_t *need) {
  webp1_encode_opts_t dfl;
  uint8_t *argb8;
  uint32_t *argb;
  int n, level, has_alpha, rc;
  size_t wb, vpay;
  w1_bump_t bump;
  w1_mux_t m;
  int use_vp8x;
  rc = w1_enc_check_still(rgba, w, h, stride, out, out_len, &stride);
  if (rc) return rc;
  rc = w1_enc_check_meta(o);
  if (rc) return rc;
  if (!o) { webp1_encode_opts_init(&dfl); o = &dfl; }
  level = o->lossless_level;
  if (level < 0) level = 0;
  if (level > 9) level = 9;
  wb = webp1_encode_work_bound(w, h, 1);
  if (!work || work_cap < wb) {
    if (need) *need = wb;
    return WEBP1_ERR_NO_MEMORY;
  }
  n = w * h;   /* safe: dims validated */
  w1_bump_init(&bump, work, work_cap);
  argb8 = (uint8_t *)w1_bump_alloc(&bump, (size_t)n * 4, 4);
  if (!argb8) {
    if (need) *need = wb;
    return WEBP1_ERR_NO_MEMORY;
  }
  argb = (uint32_t *)(void *)argb8;
  has_alpha = w1_rgba_to_argb(rgba, stride, w, h, argb);
  use_vp8x = (o->iccp_len > 0 || o->exif_len > 0 || o->xmp_len > 0);
  w1_mux_init(&m, out, out_cap);
  w1_mux_riff_begin(&m);
  if (use_vp8x) {
    uint8_t flags = 0;
    if (o->iccp_len) flags |= (uint8_t)W1_FLAG_ICC;
    if (has_alpha) flags |= (uint8_t)W1_FLAG_ALPHA;
    if (o->exif_len) flags |= (uint8_t)W1_FLAG_EXIF;
    if (o->xmp_len) flags |= (uint8_t)W1_FLAG_XMP;
    w1_mux_vp8x(&m, w, h, flags);
    if (o->iccp_len) w1_mux_chunk_bytes(&m, "ICCP", o->iccp, o->iccp_len);
  }
  {
    size_t elen = 0;
    vpay = w1_mux_chunk_begin(&m, "VP8L");
    if (!m.err) {
      rc = w1_vp8l_encode_full(argb, w, h, level, has_alpha,
                               out + m.pos, out_cap - m.pos, &elen, &bump);
      if (rc == 0) m.pos += elen;
    }
    w1_mux_chunk_end(&m, vpay);
    if (rc == 1) {
      if (need) *need = wb;
      return WEBP1_ERR_NO_MEMORY;
    }
    if (rc == 2) return WEBP1_ERR_BAD_PARAM;
    if (rc == 3 || m.err) return WEBP1_ERR_OUTPUT_FULL;
  }
  if (use_vp8x) {
    if (o->exif_len) w1_mux_chunk_bytes(&m, "EXIF", o->exif, o->exif_len);
    if (o->xmp_len) w1_mux_chunk_bytes(&m, "XMP ", o->xmp, o->xmp_len);
  }
  w1_mux_riff_end(&m);
  if (m.err) return WEBP1_ERR_OUTPUT_FULL;
  *out_len = m.pos;
  return WEBP1_OK;
}

/* Flat-frame gate for the quality -> q_index table below: an exactly
 * uniform RGB frame quantizes to a single DC value, where the finer
 * reference-parity q_index strictly cuts DC error at ~same bytes. On any
 * non-uniform frame the legacy linear map is kept, so textured operating
 * points, q75 files, and the test_lossy_filter_header pins stay
 * bit-identical (a global switch measurably regresses textured points,
 * e.g. gradient q75 +16B/-2.28dB, mixed q75 +34B/-1.16dB, and fails 3
 * filter-level pins). Alpha is coded separately (lossless) and excluded.
 * Early-out: non-uniform frames return on the first differing pixel. */
static W1_UNUSED int w1_frame_is_flat(const uint8_t *rgba, size_t stride,
                                      int w, int h) {
  uint8_t r0, g0, b0;
  int x, y;
  if (!rgba || w < 1 || h < 1) return 0;
  r0 = rgba[0]; g0 = rgba[1]; b0 = rgba[2];
  for (y = 0; y < h; y++) {
    const uint8_t *row = rgba + (size_t)y * stride;
    for (x = 0; x < w; x++) {
      const uint8_t *p = row + (size_t)x * 4;
      if (p[0] != r0 || p[1] != g0 || p[2] != b0) return 0;
    }
  }
  return 1;
}


/* Reference base quality -> q_index curve: q = round(127*(1 - cbrt(lc)))
 * with the lc(Q) piecewise (lc = 2Q-1 above Q=0.75, else 0.5*Q,
 * both in 1/100 units), tabulated per integer quality 0..100. This is the
 * base quantizer the reference uses; the old linear map ran 20-40
 * q-indices coarser below Q50, the root of most lossy PSNR gaps. */
static W1_UNUSED int w1_quality_to_q_index(int quality) {
  static const uint8_t qi[101] = {
    127,103, 96, 92, 89, 86, 83, 81, 79, 77, 75, 73, 72, 70, 69, 68,
     66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51,
     51, 50, 49, 48, 48, 47, 46, 45, 45, 44, 43, 43, 42, 41, 41, 40,
     40, 39, 38, 38, 37, 37, 36, 36, 35, 35, 34, 33, 33, 32, 32, 31,
     31, 30, 30, 29, 29, 28, 28, 28, 27, 27, 26, 26, 24, 23, 22, 21,
     19, 18, 17, 16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,
      3,  2,  1,  0,  0
  };
  if (quality < 0) quality = 75;
  if (quality > 100) quality = 100;
  return (int)qi[quality];
}


static W1_UNUSED int webp1_encode_lossy(const uint8_t *rgba, int w, int h,
                                        size_t stride,
                                        const webp1_encode_opts_t *o,
                                        uint8_t *out, size_t out_cap,
                                        size_t *out_len, uint8_t *work,
                                        size_t work_cap, size_t *need) {
  webp1_encode_opts_t dfl;
  size_t wb;
  int q, alph_level, has_alpha = 0, rc, use_vp8x, x, y;
  w1_mux_t m;

  rc = w1_enc_check_still(rgba, w, h, stride, out, out_len, &stride);
  if (rc) return rc;
  rc = w1_enc_check_meta(o);
  if (rc) return rc;
  if (!o) { webp1_encode_opts_init(&dfl); o = &dfl; }
  /* ALPH is stored losslessly, so the lossless level drives it. */
  alph_level = o->lossless_level;
  if (alph_level < 0) alph_level = 0;
  if (alph_level > 9) alph_level = 9;
  /* Reference base quality->q_index curve for every frame. */
  q = w1_quality_to_q_index(o->quality);
  if (q < 0) q = 0; else if (q > 127) q = 127;

  wb = webp1_encode_work_bound(w, h, 0);
  if (!work || work_cap < wb) {
    if (need) *need = wb;
    return WEBP1_ERR_NO_MEMORY;
  }
  for (y = 0; y < h && !has_alpha; y++) {
    const uint8_t *row = rgba + (size_t)y * stride;
    for (x = 0; x < w; x++) if (row[x * 4 + 3] != 255) { has_alpha = 1; break; }
  }

  w1_mux_init(&m, out, out_cap);
  w1_mux_riff_begin(&m);
  use_vp8x = has_alpha || o->iccp_len > 0 || o->exif_len > 0 || o->xmp_len > 0;
  if (use_vp8x) {
    uint8_t flags = 0;
    if (o->iccp_len) flags |= (uint8_t)W1_FLAG_ICC;
    if (has_alpha) flags |= (uint8_t)W1_FLAG_ALPHA;
    if (o->exif_len) flags |= (uint8_t)W1_FLAG_EXIF;
    if (o->xmp_len) flags |= (uint8_t)W1_FLAG_XMP;
    w1_mux_vp8x(&m, w, h, flags);
    if (o->iccp_len) w1_mux_chunk_bytes(&m, "ICCP", o->iccp, o->iccp_len);
  }
  rc = w1_enc_lossy_payload(&m, rgba, stride, w, h, q,
                            w1_vp8_filter_level_for_q(q), 0, alph_level,
                            alph_level, NULL, work, work_cap, need);
  if (rc == 0) {
    if (use_vp8x) {
      if (o->exif_len) w1_mux_chunk_bytes(&m, "EXIF", o->exif, o->exif_len);
      if (o->xmp_len) w1_mux_chunk_bytes(&m, "XMP ", o->xmp, o->xmp_len);
    }
    w1_mux_riff_end(&m);
    if (m.err) return WEBP1_ERR_OUTPUT_FULL;
    *out_len = m.pos;
  }
  return rc;
}

/* Worst-case container size for an animation of n_frames full-canvas frames:
 * RIFF+VP8X+ANIM headers, per-frame ANMF header + sub-chunk headers + payload
 * (VP8L, or ALPH + VP8), plus metadata. */
static W1_UNUSED size_t webp1_encode_anim_bound(int n_frames, int w, int h,
                                                 int lossless,
                                                 const webp1_encode_opts_t *o) {
  uint64_t npix, per, t;
  if (n_frames < 1 || w < 1 || h < 1) return 0;
  if (w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM) return 0;
  if (n_frames > WEBP1_MAX_ANIM_FRAMES) return 0;
  npix = (uint64_t)(unsigned)w * (uint64_t)(unsigned)h;
  /* VP8L worst case 8 B/px + tables; lossy VP8 12 B/px + ALPH 1 B/px. */
  per = (lossless ? (uint64_t)8 * npix + 65536
                  : (uint64_t)13 * npix + 131072 + 64)
        + 8 + 16 + 8;
  t = 12 + 20 + 14 + per * (uint64_t)n_frames + w1_enc_meta_len(o);
  if (t > (uint64_t)(size_t)-1) return 0;
  return (size_t)t;
}

static W1_UNUSED int webp1_encode_anim(const webp1_anim_in_t *frames,
                                       int n_frames, int w, int h,
                                       int loop_count, uint32_t bg_color,
                                       int lossless,
                                       const webp1_encode_opts_t *o,
                                       uint8_t *out, size_t out_cap,
                                       size_t *out_len, uint8_t *work,
                                       size_t work_cap, size_t *need) {
  webp1_encode_opts_t dfl;
  size_t wb;
  int q = 0, level = 0, i, rc, has_alpha = 0, k, x, y;
  w1_mux_t m;

  if (!frames || n_frames < 1 || !out || !out_len) return WEBP1_ERR_BAD_PARAM;
  if (w < 1 || h < 1 || w > WEBP1_MAX_DIM || h > WEBP1_MAX_DIM)
    return WEBP1_ERR_TOO_LARGE;
  if (n_frames > WEBP1_MAX_ANIM_FRAMES) return WEBP1_ERR_TOO_LARGE;
  if (loop_count < 0 || loop_count > 0xffff) return WEBP1_ERR_BAD_PARAM;
  rc = w1_enc_check_meta(o);
  if (rc) return rc;
  for (i = 0; i < n_frames; i++) {
    size_t st = frames[i].stride ? frames[i].stride : (size_t)(unsigned)w * 4;
    if (!frames[i].rgba) return WEBP1_ERR_BAD_PARAM;
    if (frames[i].duration_ms < 0) return WEBP1_ERR_BAD_PARAM;
    if (st < (size_t)(unsigned)w * 4) return WEBP1_ERR_BAD_PARAM;
    if (h > 1 && st > ((size_t)-1 - (size_t)(unsigned)w * 4) / (size_t)(h - 1))
      return WEBP1_ERR_TOO_LARGE;
    if (frames[i].duration_ms > 0xffffff) return WEBP1_ERR_TOO_LARGE;
  }
  if (!o) { webp1_encode_opts_init(&dfl); o = &dfl; }
  level = o->lossless_level;
  if (level < 0) level = 0;
  if (level > 9) level = 9;
  /* Reference base quality->q_index curve (same as stills). */
  q = w1_quality_to_q_index(o->quality);
  if (q < 0) q = 0; else if (q > 127) q = 127;

  wb = webp1_encode_work_bound(w, h, lossless);
  if (!work || work_cap < wb) {
    if (need) *need = wb;
    return WEBP1_ERR_NO_MEMORY;
  }
  /* Any frame with transparency needs the VP8X alpha flag. */
  for (k = 0; k < n_frames && !has_alpha; k++) {
    const uint8_t *f = frames[k].rgba;
    size_t st = frames[k].stride ? frames[k].stride : (size_t)(unsigned)w * 4;
    for (y = 0; y < h && !has_alpha; y++) {
      const uint8_t *row = f + (size_t)y * st;
      for (x = 0; x < w; x++) if (row[x * 4 + 3] != 255) { has_alpha = 1; break; }
    }
  }

  w1_mux_init(&m, out, out_cap);
  w1_mux_riff_begin(&m);
  {
    uint8_t flags = (uint8_t)W1_FLAG_ANIM;
    if (o->iccp_len) flags |= (uint8_t)W1_FLAG_ICC;
    if (has_alpha) flags |= (uint8_t)W1_FLAG_ALPHA;
    if (o->exif_len) flags |= (uint8_t)W1_FLAG_EXIF;
    if (o->xmp_len) flags |= (uint8_t)W1_FLAG_XMP;
    w1_mux_vp8x(&m, w, h, flags);
    if (o->iccp_len) w1_mux_chunk_bytes(&m, "ICCP", o->iccp, o->iccp_len);
  }
  {
    size_t o7 = w1_mux_chunk_begin(&m, "ANIM");
    w1_mux_u32(&m, bg_color);
    w1_mux_u16(&m, (uint16_t)loop_count);
    w1_mux_chunk_end(&m, o7);
  }
  rc = WEBP1_OK;
  for (i = 0; i < n_frames; i++) {
    const uint8_t *f = frames[i].rgba;
    size_t st = frames[i].stride ? frames[i].stride : (size_t)(unsigned)w * 4;
    size_t oa;
    oa = w1_mux_chunk_begin(&m, "ANMF");
    if (m.err) { rc = WEBP1_ERR_OUTPUT_FULL; break; }
    w1_mux_u24(&m, 0);                       /* x / 2 (full-canvas frame)  */
    w1_mux_u24(&m, 0);                       /* y / 2                      */
    w1_mux_u24(&m, (uint32_t)(w - 1));       /* width  - 1                 */
    w1_mux_u24(&m, (uint32_t)(h - 1));       /* height - 1                 */
    w1_mux_u24(&m, (uint32_t)frames[i].duration_ms);
    /* dispose = none, and bit 1 set = "do not blend" so each full-canvas
     * frame replaces the canvas: compositing then reproduces the source
     * frame exactly, transparent pixels included. */
    w1_mux_u8(&m, 0x02);
    rc = lossless
         ? w1_enc_lossless_payload(&m, f, st, w, h, level, NULL, work, work_cap, need)
         : w1_enc_lossy_payload(&m, f, st, w, h, q,
                                w1_vp8_filter_level_for_q(q), 0, level, level, NULL,
                                work, work_cap, need);
    w1_mux_chunk_end(&m, oa);
    if (rc) break;
  }
  if (rc == 0) {
    if (o->exif_len) w1_mux_chunk_bytes(&m, "EXIF", o->exif, o->exif_len);
    if (o->xmp_len) w1_mux_chunk_bytes(&m, "XMP ", o->xmp, o->xmp_len);
    w1_mux_riff_end(&m);
    if (m.err) return WEBP1_ERR_OUTPUT_FULL;
    *out_len = m.pos;
  }
  return rc;
}

#endif /* WEBP1_IMPLEMENTATION */
