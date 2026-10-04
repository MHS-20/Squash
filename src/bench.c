/* `squash bench`: compression ratio and speed of every codec, next to the
 * system's reference tools when they are installed.
 *
 * Our codecs are timed in memory (best of several runs, each run long
 * enough to time). Reference tools are timed as processes reading a file
 * and writing to a file, so their figures include process start-up and
 * I/O; on multi-megabyte inputs that overhead is small but not zero. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "codec.h"

extern char **environ;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Levels to measure: each codec's fastest, default and strongest. */
static int pick_levels(const sq_codec *c, int all, int *lv)
{
    int k = 0;
    if (!all) {
        lv[k++] = 0;  /* the default */
        return k;
    }
    lv[k++] = c->min_level;
    if (c->max_level > c->min_level)
        lv[k++] = c->max_level;
    lv[k++] = 0;
    return k;
}

typedef struct {
    double in, out, ctime, dtime;
} totals;

/* Repeats `fn` until about 0.2 s have passed; returns the best time. */
#define TIME_BEST(var, stmt)                                       \
    do {                                                           \
        double start_ = now(), best_ = 1e30;                       \
        int reps_ = 0;                                             \
        do {                                                       \
            double t0_ = now();                                    \
            stmt;                                                  \
            double t_ = now() - t0_;                               \
            best_ = t_ < best_ ? t_ : best_;                       \
            reps_++;                                               \
        } while (now() - start_ < 0.2 && reps_ < 20);              \
        var = best_;                                               \
    } while (0)

static void row(const char *who, const char *level, double in, double out, double ct, double dt)
{
    printf("  %-22s %-6s %8.3f %10.2f %10.2f\n", who, level, out > 0 ? in / out : 0,
           ct > 0 ? in / ct / 1e6 : 0, dt > 0 ? in / dt / 1e6 : 0);
}

static int bench_ours(const uint8_t *data, size_t n, int all, totals *tot)
{
    int bad = 0;
    for (size_t i = 0; i < sq_codec_count; i++) {
        const sq_codec *c = &sq_codecs[i];
        int lv[4], nl = pick_levels(c, all, lv);
        for (int k = 0; k < nl; k++) {
            sq_buf z, d;
            sq_buf_init(&z);
            sq_buf_init(&d);
            double ct, dt;
            int err = 0;
            TIME_BEST(ct, (z.len = 0, err |= c->compress(data, n, lv[k], &z)));
            TIME_BEST(dt, (d.len = 0, err |= c->decompress(z.data, z.len, &d)));
            int ok = !err && d.len == n && (n == 0 || memcmp(d.data, data, n) == 0);
            char level[16];
            snprintf(level, sizeof level, lv[k] ? "%d" : "def", lv[k]);
            row(c->name, ok ? level : "FAIL", (double)n, (double)z.len, ct, dt);
            bad |= !ok;
            size_t t = i * 4 + (size_t)k;
            if (t < 64) {
                tot[t].in += (double)n;
                tot[t].out += (double)z.len;
                tot[t].ctime += ct;
                tot[t].dtime += dt;
            }
            sq_buf_free(&z);
            sq_buf_free(&d);
        }
    }
    return bad;
}

/* Runs argv with stdin and stdout redirected; returns elapsed seconds or
 * a negative value on failure. */
static double run(char *const argv[], const char *in, const char *out)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, in, O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    double t0 = now();
    pid_t pid;
    int status = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    if (rc == 0)
        waitpid(pid, &status, 0);
    posix_spawn_file_actions_destroy(&fa);
    double t = now() - t0;
    /* compress(1) exits 2 when its output is larger; still a result. */
    return rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) <= 2 ? t : -1;
}

static const struct {
    const char *name, *level;
    const char *comp[6], *decomp[6];  /* NULL-terminated argv */
} tools[] = {
    {"gzip", "-6", {"gzip", "-6", "-c"}, {"gzip", "-dc"}},
    {"gzip", "-9", {"gzip", "-9", "-c"}, {"gzip", "-dc"}},
    {"bzip2", "-9", {"bzip2", "-9", "-c"}, {"bzip2", "-dc"}},
    {"lz4", "-1", {"lz4", "-1", "-c"}, {"lz4", "-dc"}},
    {"lz4", "-9", {"lz4", "-9", "-c"}, {"lz4", "-dc"}},
    {"xz", "-6", {"xz", "-6", "-T1", "-c"}, {"xz", "-dc"}},
    {"zstd", "-3", {"zstd", "-q", "-3", "-c"}, {"zstd", "-q", "-dc"}},
    {"zstd", "-19", {"zstd", "-q", "-19", "-c"}, {"zstd", "-q", "-dc"}},
    {"compress", "-b16", {"compress", "-c"}, {"compress", "-dc"}},
};

static totals tool_tot[16];

static void bench_tools(const char *path, double n, const char *tmp)
{
    char zpath[512], dpath[512];
    snprintf(zpath, sizeof zpath, "%s/z", tmp);
    snprintf(dpath, sizeof dpath, "%s/d", tmp);
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        double ct = run((char *const *)tools[i].comp, path, zpath);
        if (ct < 0)
            continue;  /* not installed */
        double dt = run((char *const *)tools[i].decomp, zpath, dpath);
        struct stat st;
        double out = stat(zpath, &st) == 0 ? (double)st.st_size : 0;
        char who[64];
        snprintf(who, sizeof who, "[system] %s", tools[i].name);
        row(who, tools[i].level, n, out, ct, dt);
        tool_tot[i].in += n;
        tool_tot[i].out += out;
        tool_tot[i].ctime += ct;
        tool_tot[i].dtime += dt;
    }
}

/* Built-in inputs for when no files are given. */
static uint8_t *generate(int which, size_t n, const char **name)
{
    uint8_t *p = malloc(n);
    if (!p)
        return NULL;
    static const char *words[] = {"compression ", "the ", "of ", "a ", "entropy ",
                                  "window\n", "match ", "literal ", "Huffman, ", "is "};
    uint64_t x = 88172645463325252ull;
    for (size_t i = 0; i < n;) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        if (which == 0) {
            *name = "generated-text";
            for (const char *w = words[x % 10]; *w && i < n; w++)
                p[i++] = (uint8_t)*w;
        } else if (which == 1) {
            *name = "generated-binary";
            p[i] = (uint8_t)(i % 4 == 0 ? i >> 2 : i % 4 == 1 ? i >> 12 : x >> 60);
            i++;
        } else {
            *name = "generated-random";
            p[i++] = (uint8_t)x;
        }
    }
    return p;
}

int bench_main(int argc, char **argv)
{
    int all = 0, first = 1;
    if (argc > 1 && strcmp(argv[1], "-a") == 0)
        all = 1, first = 2;
    char tmp[] = "/tmp/squash-bench-XXXXXX";
    if (!mkdtemp(tmp)) {
        perror("squash bench");
        return 1;
    }
    static totals tot[64];
    int bad = 0, nfiles = argc - first, ninputs = nfiles ? nfiles : 3;
    printf("  %-22s %-6s %8s %10s %10s\n", "codec", "level", "ratio", "comp MB/s", "dec MB/s");
    for (int f = 0; f < ninputs; f++) {
        const char *name = nfiles ? argv[first + f] : NULL;
        uint8_t *data = NULL;
        size_t n = 0;
        char path[600];
        if (name) {
            FILE *fp = fopen(name, "rb");
            sq_buf b;
            sq_buf_init(&b);
            int ok = fp != NULL;
            while (ok) {
                if (sq_buf_reserve(&b, 1 << 20)) {
                    ok = 0;
                    break;
                }
                size_t got = fread(b.data + b.len, 1, b.cap - b.len, fp);
                b.len += got;
                if (!got)
                    break;
            }
            if (fp)
                fclose(fp);
            if (!ok) {
                fprintf(stderr, "squash bench: %s: %s\n", name, strerror(errno));
                sq_buf_free(&b);
                bad = 1;
                continue;
            }
            data = b.data;
            n = b.len;
            snprintf(path, sizeof path, "%s", name);
        } else {
            n = 4u << 20;
            data = generate(f, n, &name);
            snprintf(path, sizeof path, "%s/input", tmp);
            FILE *fp = fopen(path, "wb");
            if (fp) {
                fwrite(data, 1, n, fp);
                fclose(fp);
            }
        }
        printf("%s (%zu bytes)\n", name, n);
        bad |= bench_ours(data, n, all, tot);
        bench_tools(path, (double)n, tmp);
        free(data);
    }
    if (ninputs > 1) {
        printf("total\n");
        for (size_t i = 0; i < sq_codec_count; i++) {
            int lv[4], nl = pick_levels(&sq_codecs[i], all, lv);
            for (int k = 0; k < nl; k++) {
                const totals *t = &tot[i * 4 + (size_t)k];
                char level[16];
                snprintf(level, sizeof level, lv[k] ? "%d" : "def", lv[k]);
                row(sq_codecs[i].name, level, t->in, t->out, t->ctime, t->dtime);
            }
        }
        for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
            const totals *t = &tool_tot[i];
            char who[64];
            snprintf(who, sizeof who, "[system] %s", tools[i].name);
            if (t->in > 0)
                row(who, tools[i].level, t->in, t->out, t->ctime, t->dtime);
        }
    }
    char z[600];
    snprintf(z, sizeof z, "%s/z", tmp);
    remove(z);
    snprintf(z, sizeof z, "%s/d", tmp);
    remove(z);
    snprintf(z, sizeof z, "%s/input", tmp);
    remove(z);
    rmdir(tmp);
    return bad;
}
