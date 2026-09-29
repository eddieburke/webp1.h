# webp1.h

webp1.h is a WebP encoder and decoder in a single C99 header. It needs only `<stdint.h>`, `<stddef.h>` and `<string.h>`. It never allocates from the heap, starts no threads and keeps no mutable globals. The caller supplies the output buffer and a scratch buffer. For the same input and options, the output is byte-for-byte the same.

Copy `webp1.h` into your project. Define `WEBP1_IMPLEMENTATION` before including it wherever you call the library:

```c
#include <stdlib.h>
#define WEBP1_IMPLEMENTATION
#include "webp1.h"
```

Every function is `static`, so you can include the header, with or without the implementation, in more than one translation unit. Pixels are 8-bit RGBA with straight (non-premultiplied) alpha, row-major, top row first.

The two encoders are separate functions with separate trade-offs, so each has its own section below:

- **Lossless (VP8L):** every pixel, including fully transparent ones, decodes exactly.
- **Lossy (VP8):** smaller files at a chosen quality. Alpha is still stored losslessly.

## Buffers and errors (both encoders)

Each encoder takes a caller-owned output buffer and a caller-owned work buffer, both sized by the bound functions.

- Pass `lossless = 1` to the bound functions for lossless and `0` for lossy.
- A stride of 0 means the rows are tightly packed (`width * 4`).
- If a call returns `WEBP1_ERR_NO_MEMORY`, the `needed` output holds the recommended work buffer size. Retry with a buffer at least that large.
- `webp1_error_name` gives the name of each error code.

## Lossless encoding

```c
webp1_encode_opts_t opts;
webp1_encode_opts_init(&opts);
opts.lossless_level = 6;           /* effort 0..9, default 6 */

size_t output_cap = webp1_encode_bound(width, height, 1, &opts);
size_t work_cap = webp1_encode_work_bound(width, height, 1);
size_t output_len = 0, needed = 0;
uint8_t *output = malloc(output_cap);
uint8_t *work = malloc(work_cap);
int rc = WEBP1_ERR_NO_MEMORY;
if (output && work)
    rc = webp1_encode_lossless(rgba, width, height, 0, &opts,
                               output, output_cap, &output_len,
                               work, work_cap, &needed);
```

`lossless_level` sets how hard the encoder searches predictors, colour transforms, palettes and LZ77 matches. `quality` has no effect on lossless encoding.

- Levels 0 to 8 are the practical range.
- Level 9 adds an optimal parse and takes seconds to tens of seconds on megapixel photos.

### Lossless compared with libwebp

**Real images.** The reference is the smallest file libwebp produces over every method (0 to 6) and every quality (0 to 100). Every webp1 file round-trips exactly. Below 1 means webp1 is smaller.

| image | level 6 | level 9 |
|---|---:|---:|
| natural photo, 512x512 | 1.013 | 1.010 |
| photographic composite, 1497x823 | 1.080 | 1.062 |
| pixel art and sprite sheets (6 images) | 1.011 to 1.074 | 0.911 to 1.035 |
| all 8 images, total bytes | 1.066 | 1.050 |

**Synthetic 1024x1024 images.** There are 16 generators (photo-like, gradients, text, palettes, stripes, checkerboards, noise and alpha), each encoded at levels 0 to 9. The reference is the best of libwebp methods 0, 4 and 6.

- **Size:** webp1 is 0.74x the size (geometric mean). Only 1 of the 160 cells is larger: the alpha image at level 0, 452 B against 448 B.
- **Encoding speed:** 1.8x faster than libwebp method 6. It is slower on four image types:
  - stripes: 0.37x;
  - 16-colour palette: 0.39x;
  - text: 0.53x;
  - 1-pixel checkerboard: 0.65x.
- **Decoding speed:** 3.2x faster.

**Encoding speed on the real images.** Level 6 against libwebp lossless method 6, on the 9 real images cropped to 512x512. The median image is 1.2x faster.

- Fastest: 2.6x faster.
- Slowest: 0.23x, on a graphic with text.
- Three upscaled pixel-art crops come in at 0.36x to 0.94x.

## Lossy encoding

```c
webp1_encode_opts_t opts;
webp1_encode_opts_init(&opts);
opts.quality = 75;                 /* 0..100, default 75 */
opts.lossless_level = 6;           /* lossy effort 0..9, default 6 */

size_t output_cap = webp1_encode_bound(width, height, 0, &opts);
size_t work_cap = webp1_encode_work_bound(width, height, 0);
size_t output_len = 0, needed = 0;
uint8_t *output = malloc(output_cap);
uint8_t *work = malloc(work_cap);
int rc = WEBP1_ERR_NO_MEMORY;
if (output && work)
    rc = webp1_encode_lossy(rgba, width, height, 0, &opts,
                            output, output_cap, &output_len,
                            work, work_cap, &needed);
```

`quality` sets the quantizer. There is no separate lossy effort field: the lossy encoder reads its effort from `lossless_level` too.

- Higher effort widens the quantizer and rate-distortion search.
- Extra search only changes the output when it finds a better result, so neighbouring efforts often produce identical bytes.
- Lossy output is limited to 16,383 pixels per side.

### Lossy compared with libwebp

The test set is the 9 real images cropped to 512x512, encoded at qualities 5 to 100 with webp1 at effort 6 and libwebp at method 6.

**Size at equal quality.** Sizes are compared at equal PSNR (Bjøntegaard delta over the PSNR range both encoders cover). Below 1 means webp1 is smaller.

- Across the set, webp1 is 0.915x the size (geometric mean).
- It is smaller on 7 of the 8 images with an overlapping range, at 0.86x to 0.97x.
- It is 3.4% larger on the one natural photograph.
- One tile sheet is coded exactly by both encoders: 512 B from webp1 against 570 B from libwebp.

The same quality number does not give the same PSNR in both encoders. For example, the natural photograph at quality 75 is 20798 B at 37.13 dB from webp1 against 18292 B at 36.61 dB from libwebp. Compare the two at matched PSNR, not at matched quality numbers.

**Encoding speed at quality 75.** The results are mixed:

- **Similar:** the natural photograph, at 0.96x.
- **Faster:** three images, at 1.6x to 6.3x, and the exactly-coded tile sheet, which libwebp takes over a second to encode.
- **Slower:** four graphics and pixel-art crops, at 0.38x to 0.48x.

## Decoding

```c
webp1_info_t info;
int rc = webp1_info(data, data_len, &info);
if (rc == WEBP1_OK) {
    size_t rgba_cap = webp1_rgba_size(info.width, info.height);
    size_t work_cap = webp1_decode_work_bound(&info);
    uint8_t *rgba = malloc(rgba_cap);
    uint8_t *work = malloc(work_cap);
    size_t needed = 0;
    int width = 0, height = 0;
    if (rgba && work)
        rc = webp1_decode_rgba(data, data_len, rgba, rgba_cap,
                               work, work_cap, &needed, &width, &height);
}
```

The decoder handles both formats through the same call. `info.is_lossless` reports which one a still image uses. It accepts WebP stills and animations whose frames are VP8 key frames or VP8L, and it rejects VP8 interframes. Images can be at most 16,384 pixels per side.

## Animation and metadata

- **Animation encoding:** `webp1_encode_anim`. The `lossless` argument selects the encoder for every frame.
- **Animation decoding:** `webp1_anim_decode` and `webp1_frame_info`.
- **Metadata:** EXIF, XMP and ICC profiles are written from `webp1_encode_opts_t` and found with `webp1_find_chunk`.

The public declarations near the top of `webp1.h` give the full signatures.

## How the comparisons were measured

- **Date and versions:** 2026-09-29, against libwebp 1.6.0 through Pillow 12.3.0.
- **Machine and build:** a single Windows x64 machine, MSVC `/O2` build.
- **Size ratios:** webp1 bytes divided by libwebp bytes.
- **Speed ratios:** libwebp time divided by webp1 time, single-threaded, fastest of 3 runs.

The real-image set is small, and only one image in it is a natural photograph. Treat these numbers as indications, not guarantees.
