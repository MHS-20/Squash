/* libFuzzer target for every decoder: the first input byte picks the
 * codec, the rest is the compressed stream. Decoders must reject or decode
 * anything without crashing; run under ASan/UBSan via `make fuzz`. */
#include <stddef.h>
#include <stdint.h>

#include "codec.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const sq_codec *c = &sq_codecs[data[0] % sq_codec_count];
    sq_buf out;
    sq_buf_init(&out);
    c->decompress(data + 1, size - 1, &out);
    sq_buf_free(&out);
    return 0;
}
