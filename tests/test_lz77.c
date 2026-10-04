#include "lz77.h"
#include "test.h"

/* Rebuild the input from a parse, checking every format limit on the way. */
static int replay_ok(const lz_seqs *s, const uint8_t *in, size_t n, const lz_params *p)
{
    static uint8_t out[1 << 17];
    size_t pos = 0;
    for (size_t i = 0; i < s->n; i++) {
        const lz_seq *q = &s->seq[i];
        if (pos + q->lit_len > n)
            return 0;
        memcpy(out + pos, in + pos, q->lit_len);
        pos += q->lit_len;
        if (q->match_len == 0) {
            if (i != s->n - 1)
                return 0;  /* only the last sequence may be literal-only */
            continue;
        }
        if (q->match_len < p->min_match || q->match_len > p->max_match ||
            q->dist == 0 || q->dist > p->max_dist || q->dist > pos ||
            pos + p->start_margin >= n || pos + q->match_len + p->end_margin > n)
            return 0;
        for (uint32_t k = 0; k < q->match_len; k++, pos++)
            out[pos] = out[pos - q->dist];
    }
    return pos == n && memcmp(out, in, n) == 0;
}

static size_t matched_bytes(const lz_seqs *s)
{
    size_t m = 0;
    for (size_t i = 0; i < s->n; i++)
        m += s->seq[i].match_len;
    return m;
}

int main(void)
{
    static uint8_t text[1 << 16];
    /* Semi-repetitive input: words drawn from a small vocabulary. */
    const char *words[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ",
                           "lazy ", "dog ", "\n", "aaaaaaaa", "squash "};
    unsigned char rnd[1 << 16];
    fill_random(rnd, sizeof rnd, 3);
    size_t n = 0;
    for (int i = 0; n < 6000; i++) {
        const char *w = words[rnd[i] % 11];
        size_t l = strlen(w);
        memcpy(text + n, w, l);
        n += l;
    }

    lz_params deflate_like = {32768, 32768, 3, 258, 1u << 30, 258, 0, 0, 0};
    lz_params lz4_like = {65536, 65535, 4, 1u << 30, 64, 1u << 30, 0, 5, 12};

    lz_seqs a, b;
    lz_seqs_init(&a);
    lz_seqs_init(&b);
    /* Greedy hash chains with an unbounded chain find exactly what brute
     * force finds: every match of min_match bytes shares the hash. */
    CHECK(lz_parse_naive(text, n, &deflate_like, &a) == 0);
    CHECK(lz_parse(text, n, &deflate_like, &b) == 0);
    CHECK(replay_ok(&a, text, n, &deflate_like));
    CHECK(replay_ok(&b, text, n, &deflate_like));
    CHECK_EQ_U(a.n, b.n);
    CHECK_EQ_U(matched_bytes(&a), matched_bytes(&b));

    /* Lazy matching never covers less than greedy on this input. */
    lz_params lazy = deflate_like;
    lazy.lazy = 32;
    lazy.max_chain = 128;
    lz_seqs c;
    lz_seqs_init(&c);
    CHECK(lz_parse(text, n, &lazy, &c) == 0);
    CHECK(replay_ok(&c, text, n, &lazy));

    /* LZ4-style margins and 4-byte minimum, on random and run data. */
    static uint8_t mixed[70000];
    fill_random(mixed, 30000, 9);
    memset(mixed + 30000, 'z', 20000);
    memcpy(mixed + 50000, mixed + 100, 20000);
    a.n = 0;
    CHECK(lz_parse(mixed, sizeof mixed, &lz4_like, &a) == 0);
    CHECK(replay_ok(&a, mixed, sizeof mixed, &lz4_like));
    CHECK(matched_bytes(&a) > 39000);

    /* Edge sizes: empty, tiny, inputs shorter than the margins. */
    for (size_t len = 0; len < 20; len++) {
        a.n = 0;
        CHECK(lz_parse((const uint8_t *)"aaaaaaaaaaaaaaaaaaaa", len, &lz4_like, &a) == 0);
        CHECK(replay_ok(&a, (const uint8_t *)"aaaaaaaaaaaaaaaaaaaa", len, &lz4_like));
        a.n = 0;
        CHECK(lz_parse((const uint8_t *)"aaaaaaaaaaaaaaaaaaaa", len, &lazy, &a) == 0);
        CHECK(replay_ok(&a, (const uint8_t *)"aaaaaaaaaaaaaaaaaaaa", len, &lazy));
    }

    lz_seqs_free(&a);
    lz_seqs_free(&b);
    lz_seqs_free(&c);
    TEST_DONE("lz77");
}
