/* squash: command-line front end over the codec registry. Whole files are
 * read into memory, which matches the whole-buffer library API. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "codec.h"

int bench_main(int argc, char **argv);

static void usage(FILE *f)
{
    fprintf(f,
            "usage: squash [-f FORMAT] [-d] [-l LEVEL] [-c] [-F] [-D DICT] [IN [OUT]]\n"
            "       squash bench [-a] [FILES...]\n"
            "  -f FORMAT  format to write (required to compress); detected\n"
            "             from the magic number when decompressing\n"
            "  -d         decompress\n"
            "  -l LEVEL   compression level (format specific; default 0 = format default)\n"
            "  -c         write to standard output\n"
            "  -F         overwrite an existing output file\n"
            "  -D DICT    zstd dictionary (raw or from `zstd --train`), decompression only\n"
            "  -L         list formats\n"
            "With no IN, read standard input. With IN but no OUT, write IN plus the\n"
            "format's suffix (or IN minus it when decompressing). IN is kept.\n"
            "bench reports ratio and MB/s for each codec at its default level and for\n"
            "installed reference tools; -a adds container variants, fastest and\n"
            "strongest levels. Without FILES it uses generated data.\n");
}

static int read_all(FILE *f, sq_buf *b)
{
    for (;;) {
        if (sq_buf_reserve(b, 1 << 16))
            return -1;
        size_t got = fread(b->data + b->len, 1, b->cap - b->len, f);
        b->len += got;
        if (got == 0)
            return ferror(f) ? -1 : 0;
    }
}

static char *derive_name(const char *in, const sq_codec *c, int decompress)
{
    size_t n = strlen(in), e = strlen(c->ext);
    char *out = malloc(n + e + 5);
    if (!out)
        return NULL;
    if (decompress && n > e && strcmp(in + n - e, c->ext) == 0) {
        memcpy(out, in, n - e);
        out[n - e] = 0;
    } else {
        sprintf(out, "%s%s", in, decompress ? ".out" : c->ext);
    }
    return out;
}

static void list_formats(void)
{
    for (size_t i = 0; i < sq_codec_count; i++) {
        const sq_codec *c = &sq_codecs[i];
        printf("%-10s %-10s levels %d-%d%s\n", c->name, c->ext, c->min_level, c->max_level,
               c->magic ? "" : "  (raw: no magic, give -f to decompress)");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "bench") == 0)
        return bench_main(argc - 1, argv + 1);
    const char *fmt = NULL, *dict_name = NULL;
    int decompress = 0, level = 0, to_stdout = 0, force = 0, opt;
    while ((opt = getopt(argc, argv, "f:dl:cFD:Lh")) != -1) {
        switch (opt) {
        case 'f': fmt = optarg; break;
        case 'd': decompress = 1; break;
        case 'l': level = atoi(optarg); break;
        case 'c': to_stdout = 1; break;
        case 'F': force = 1; break;
        case 'D': dict_name = optarg; break;
        case 'L': list_formats(); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (argc - optind > 2) {
        usage(stderr);
        return 2;
    }
    const char *in_name = optind < argc ? argv[optind] : NULL;
    const char *out_name = optind + 1 < argc ? argv[optind + 1] : NULL;
    if (in_name && strcmp(in_name, "-") == 0)
        in_name = NULL;

    const sq_codec *codec = fmt ? sq_codec_find(fmt) : NULL;
    if (fmt && !codec) {
        fprintf(stderr, "squash: unknown format '%s' (try -L)\n", fmt);
        return 2;
    }
    if (!codec && !decompress) {
        fprintf(stderr, "squash: -f FORMAT is required to compress\n");
        return 2;
    }

    sq_buf in, out;
    sq_buf_init(&in);
    sq_buf_init(&out);
    FILE *fi = in_name ? fopen(in_name, "rb") : stdin;
    if (!fi || read_all(fi, &in)) {
        fprintf(stderr, "squash: %s: %s\n", in_name ? in_name : "stdin", strerror(errno));
        return 1;
    }
    if (fi != stdin)
        fclose(fi);

    if (!codec && !(codec = sq_codec_detect(in.data, in.len))) {
        fprintf(stderr, "squash: cannot detect the format; give -f\n");
        return 1;
    }
    if (level && (level < codec->min_level || level > codec->max_level)) {
        fprintf(stderr, "squash: %s levels are %d-%d\n", codec->name, codec->min_level,
                codec->max_level);
        return 2;
    }
    sq_buf dict;
    sq_buf_init(&dict);
    if (dict_name) {
        FILE *fd = fopen(dict_name, "rb");
        if (!decompress || strcmp(codec->name, "zstd") != 0) {
            fprintf(stderr, "squash: -D only applies to zstd decompression\n");
            return 2;
        }
        if (!fd || read_all(fd, &dict)) {
            fprintf(stderr, "squash: %s: %s\n", dict_name, strerror(errno));
            return 1;
        }
        fclose(fd);
    }
    int err = dict_name ? sq_zstd_decompress_dict(in.data, in.len, dict.data, dict.len, &out)
              : decompress ? codec->decompress(in.data, in.len, &out)
                           : codec->compress(in.data, in.len, level, &out);
    sq_buf_free(&dict);
    if (err) {
        fprintf(stderr, "squash: %s: %s\n", codec->name, sq_strerror(err));
        return 1;
    }

    char *derived = NULL;
    if (!out_name && !to_stdout && in_name)
        out_name = derived = derive_name(in_name, codec, decompress);
    FILE *fo = stdout;
    if (out_name) {
        if (!force && access(out_name, F_OK) == 0) {
            fprintf(stderr, "squash: %s exists (use -F to overwrite)\n", out_name);
            return 1;
        }
        fo = fopen(out_name, "wb");
    }
    if (!fo || fwrite(out.data, 1, out.len, fo) != out.len || fflush(fo)) {
        fprintf(stderr, "squash: %s: %s\n", out_name ? out_name : "stdout", strerror(errno));
        return 1;
    }
    if (fo != stdout)
        fclose(fo);
    free(derived);
    sq_buf_free(&in);
    sq_buf_free(&out);
    return 0;
}
