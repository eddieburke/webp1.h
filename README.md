# webp1.h

A single header C99 WebP encoder and decoder. It uses only the C standard library, allocates no heap memory internally, and accepts caller owned output and work buffers.

Copy `webp1.h` into your project. Define `WEBP1_IMPLEMENTATION` in a translation unit that uses the implementation:

```c
#include <stdlib.h>
#define WEBP1_IMPLEMENTATION
#include "webp1.h"
```

All public functions have internal linkage, so the header can be included in more than one translation unit. Pixel buffers use 8 bit, row major, top down RGBA with straight alpha.

## Encode

```c
webp1_encode_opts_t opts;
webp1_encode_opts_init(&opts);
opts.quality = 75;
opts.lossless_level = 6;

size_t output_cap = webp1_encode_bound(width, height, 0, &opts);
size_t work_cap = webp1_encode_work_bound(width, height, 0);
size_t output_len = 0;
size_t needed = 0;

uint8_t *output = malloc(output_cap);
uint8_t *work = malloc(work_cap);
int rc = webp1_encode_lossy(rgba, width, height, width * 4,
                            &opts, output, output_cap, &output_len,
                            work, work_cap, &needed);
```

Use `webp1_encode_lossless` with `lossless = 1` in the two bound functions for VP8L. A zero stride means tightly packed RGBA. Check that both allocations succeeded before calling the encoder. On `WEBP1_ERR_NO_MEMORY`, `needed` reports the recommended work buffer size.

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

The header also provides animation encoding and decoding, frame inspection, and metadata chunk access. See the public declarations near the top of `webp1.h` for their signatures.

## Limits

Dimensions are at most 16,384 per side; lossy VP8 encoding is limited to 16,383 per side by its bitstream field. The decoder accepts WebP stills and animations with VP8 key frames or VP8L. It rejects VP8 interframes. Error codes are named by `webp1_error_name`.
