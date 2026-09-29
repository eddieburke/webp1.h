# webp1.h

webp1.h is a WebP encoder and decoder in a single C99 header. It needs only `<stdint.h>`, `<stddef.h>` and `<string.h>`. It never allocates from the heap, starts no threads and keeps no mutable globals. The caller supplies the output buffer and a scratch buffer. For the same input and options, the output is byte-for-byte the same.

Copy `webp1.h` into your project. Define `WEBP1_IMPLEMENTATION` before including it wherever you call the library:

```c
#include <stdlib.h>
#define WEBP1_IMPLEMENTATION
#include "webp1.h"
```

Every function is `static`, so you can include the header, with or without the implementation, in more than one translation unit. Pixels are 8-bit RGBA with straight (non-premultiplied) alpha, row-major, top row first.

## Encode

```c
webp1_encode_opts_t opts;
webp1_encode_opts_init(&opts);   /* quality 75, level 6 */
opts.quality = 75;               /* lossy only: 0..100 */
opts.lossless_level = 6;         /* effort 0..9, used by BOTH encoders */

size_t output_cap = webp1_encode_bound(width, height, 0, &opts);
size_t work_cap = webp1_encode_work_bound(width, height, 0);
size_t output_len = 0;
size_t needed = 0;

uint8_t *output = malloc(output_cap);
uint8_t *work = malloc(work_cap);
int rc = WEBP1_ERR_NO_MEMORY;
if (output && work)
    rc = webp1_encode_lossy(rgba, width, height, width * 4,
                            &opts, output, output_cap, &output_len,
                            work, work_cap, &needed);
```

For lossless (VP8L), call `webp1_encode_lossless` and pass `lossless = 1` to both bound functions. A stride of 0 means the rows are tightly packed (`width * 4`). If the call returns `WEBP1_ERR_NO_MEMORY`, `needed` holds the recommended work buffer size. Retry with a buffer at least that large.

Despite its name, `lossless_level` is the effort setting for both encoders.

- Lossless: higher levels search more predictor, transform and LZ77 options. Level 9 adds an optimal parse and takes seconds to tens of seconds on megapixel photos. Levels 0 to 8 are the practical range.
- Lossy: higher levels widen the quantizer and rate-distortion search. Extra search only changes the output when it finds a better result, so neighbouring levels often produce identical bytes.

## Decode

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

The header also covers:

- animation encoding (`webp1_encode_anim`);
- animation decoding (`webp1_anim_decode`, `webp1_frame_info`);
- EXIF, XMP and ICC profile writing (`webp1_encode_opts_t`);
- chunk lookup (`webp1_find_chunk`).

The public declarations near the top of `webp1.h` give the full signatures.

## Compared with libwebp

These numbers were measured on 2026-09-29 against libwebp 1.6.0 through Pillow 12.3.0, on a single Windows x64 machine with an MSVC `/O2` build. Byte ratios are webp1 size divided by libwebp size, so below 1 means webp1 is smaller. Speed ratios are libwebp time divided by webp1 time, so above 1 means webp1 is faster. The real-image set is small, and only one image in it is a natural photograph, so treat these as indications rather than guarantees.

**Lossless, real images.** The reference is the smallest file libwebp produces over every method (0 to 6) and every quality (0 to 100). Every row round-trips exactly.

| image | level 6 | level 9 |
|---|---:|---:|
| natural photo, 512x512 | 1.013 | 1.010 |
| photographic composite, 1497x823 | 1.080 | 1.062 |
| pixel art and sprite sheets (6 images) | 1.011 to 1.074 | 0.911 to 1.035 |
| all 8 images, total bytes | 1.066 | 1.050 |

**Lossless, synthetic 1024x1024 images.** There are 16 generators (photo-like, gradients, text, palettes, stripes, checkerboards, noise and alpha), each encoded at levels 0 to 9. The reference is the best of libwebp methods 0, 4 and 6.

- **Size:** webp1 is 0.74x the size (geometric mean). Only 1 of the 160 cells is larger: the alpha image at level 0, 452 B against 448 B.
- **Encoding:** 1.8x faster, measured against libwebp method 6. It is slower on four image types:
  - stripes: 0.37x;
  - 16-colour palette: 0.39x;
  - text: 0.53x;
  - 1-pixel checkerboard: 0.65x.
- **Decoding:** 3.2x faster.

**Lossy, real images at equal quality.** These are the 9 real images cropped to 512x512, encoded at qualities 5 to 100 with webp1 at level 6 and libwebp at method 6. The numbers are bytes at equal PSNR (Bjøntegaard delta over the overlapping PSNR range).

- Across the set, webp1 is 0.915x the size (geometric mean).
- It is smaller on 7 of the 8 images with an overlapping range, at 0.86x to 0.97x.
- It is 3.4% larger on the one natural photograph.
- One tile sheet is coded exactly by both encoders: 512 B from webp1 against 570 B from libwebp.

The same quality setting does not give the same PSNR in both encoders. For example, the natural photograph at quality 75 is 20798 B at 37.13 dB from webp1 against 18292 B at 36.61 dB from libwebp. Compare the two at matched PSNR, not at matched quality numbers.

## Limits

Images can be at most 16,384 pixels per side. Lossy (VP8) encoding is limited to 16,383 per side by its bitstream field; use lossless above that. The decoder accepts WebP stills and animations whose frames are VP8 key frames or VP8L. It rejects VP8 interframes. `webp1_error_name` gives the name of each error code.
