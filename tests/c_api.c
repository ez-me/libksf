/* C API test for include/ksflate.h.
 *
 * Compiled as C, linked against build/libksflate.a, and decompressed with the
 * system zlib. Covers:
 *
 *   - a complete raw-deflate stream
 *   - literal-only mode
 *   - a preset dictionary (inflateSetDictionary)
 *   - chained non-final/final segments with history
 *   - empty input, both final and non-final
 *   - argument validation and ksflate_free(NULL)
 */
#include "ksflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SRC_LEN 200000
#define DICT_LEN 32768
#define BODY_LEN 100000

static unsigned char src[SRC_LEN];
static unsigned char dict[DICT_LEN];
static unsigned char body[BODY_LEN];
static unsigned char dst[SRC_LEN + 65536];
static unsigned char back[SRC_LEN];

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

/* Inflate a raw deflate stream, optionally with a preset dictionary. */
static int inflate_raw(const unsigned char *in, size_t in_len,
                       unsigned char *out, size_t *out_len,
                       const unsigned char *d, size_t dlen) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, -15) != Z_OK) {
        return -1;
    }
    if (dlen != 0 && inflateSetDictionary(&zs, d, (uInt)dlen) != Z_OK) {
        inflateEnd(&zs);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = out;
    zs.avail_out = (uInt)*out_len;
    const int rc = inflate(&zs, Z_FINISH);
    *out_len = zs.total_out;
    inflateEnd(&zs);
    return rc == Z_STREAM_END ? 0 : -1;
}

static void test_roundtrip(void) {
    size_t n = 0;
    unsigned char *packed =
        ksflate_compress(src, SRC_LEN, NULL, 0, 0, 1, &n);
    check(packed != NULL && n != 0, "compress returns data");
    if (packed == NULL) {
        return;
    }
    size_t m = SRC_LEN;
    check(inflate_raw(packed, n, back, &m, NULL, 0) == 0 && m == SRC_LEN &&
              memcmp(back, src, SRC_LEN) == 0,
          "raw round-trip");
    ksflate_free(packed);
}

static void test_literal_only(void) {
    size_t n = 0;
    unsigned char *packed =
        ksflate_compress(src, SRC_LEN, NULL, 0, 1, 1, &n);
    check(packed != NULL && n != 0, "literal-only returns data");
    if (packed == NULL) {
        return;
    }
    size_t m = SRC_LEN;
    check(inflate_raw(packed, n, back, &m, NULL, 0) == 0 && m == SRC_LEN &&
              memcmp(back, src, SRC_LEN) == 0,
          "literal-only round-trip");
    ksflate_free(packed);
}

static void test_dictionary(void) {
    size_t plain_n = 0, primed_n = 0;
    unsigned char *plain =
        ksflate_compress(body, BODY_LEN, NULL, 0, 0, 1, &plain_n);
    unsigned char *primed =
        ksflate_compress(body, BODY_LEN, dict, DICT_LEN, 0, 1, &primed_n);
    check(plain != NULL && primed != NULL, "dictionary compress");
    if (plain == NULL || primed == NULL) {
        ksflate_free(plain);
        ksflate_free(primed);
        return;
    }
    size_t m = BODY_LEN;
    check(inflate_raw(primed, primed_n, back, &m, dict, DICT_LEN) == 0 &&
              m == BODY_LEN && memcmp(back, body, BODY_LEN) == 0,
          "dictionary round-trip");
    check(primed_n < plain_n, "dictionary improves compression");
    ksflate_free(plain);
    ksflate_free(primed);
}

static void test_chained_segments(void) {
    const size_t half = SRC_LEN / 2;
    size_t n1 = 0, n2 = 0;
    unsigned char *first =
        ksflate_compress(src, half, NULL, 0, 0, 0, &n1);
    unsigned char *second = ksflate_compress(src + half, SRC_LEN - half,
                                             src + half - KSFLATE_WINDOW_SIZE,
                                             KSFLATE_WINDOW_SIZE, 0, 1, &n2);
    check(first != NULL && second != NULL, "chained compress");
    if (first == NULL || second == NULL) {
        ksflate_free(first);
        ksflate_free(second);
        return;
    }
    check(n1 + n2 <= sizeof dst, "chained output fits");
    if (n1 + n2 <= sizeof dst) {
        memcpy(dst, first, n1);
        memcpy(dst + n1, second, n2);
        size_t m = SRC_LEN;
        check(inflate_raw(dst, n1 + n2, back, &m, NULL, 0) == 0 &&
                  m == SRC_LEN && memcmp(back, src, SRC_LEN) == 0,
              "chained round-trip");
    }
    ksflate_free(first);
    ksflate_free(second);
}

static void test_empty(void) {
    size_t n = 0;
    unsigned char *final = ksflate_compress(NULL, 0, NULL, 0, 0, 1, &n);
    check(final != NULL && n == 2 && final[0] == 0x03 && final[1] == 0x00,
          "empty final segment is two bytes");
    if (final != NULL) {
        size_t m = 0;
        check(inflate_raw(final, n, back, &m, NULL, 0) == 0 && m == 0,
              "empty final segment inflates");
    }
    ksflate_free(final);

    size_t n1 = 0, n2 = 0;
    unsigned char *a = ksflate_compress(NULL, 0, NULL, 0, 0, 0, &n1);
    unsigned char *b = ksflate_compress(NULL, 0, NULL, 0, 0, 1, &n2);
    check(a != NULL && b != NULL && n1 + n2 <= sizeof dst,
          "empty chained segments");
    if (a != NULL && b != NULL && n1 + n2 <= sizeof dst) {
        memcpy(dst, a, n1);
        memcpy(dst + n1, b, n2);
        size_t m = 0;
        check(inflate_raw(dst, n1 + n2, back, &m, NULL, 0) == 0 && m == 0,
              "empty chained segments inflate");
    }
    ksflate_free(a);
    ksflate_free(b);
}

static void test_invalid(void) {
    check(ksflate_compress(src, SRC_LEN, NULL, 0, 0, 1, NULL) == NULL,
          "NULL out_len rejected");
    ksflate_free(NULL); /* must be a no-op */
}

static void fill_data(void) {
    unsigned state = 0x12345678u;
    for (size_t i = 0; i < SRC_LEN; ++i) {
        state = state * 1664525u + 1013904223u;
        src[i] = (i % 17 == 0) ? (unsigned char)(state >> 24)
                               : (unsigned char)((i / 7) & 0x3f);
    }
    for (size_t i = 0; i < DICT_LEN; ++i) {
        dict[i] = (unsigned char)((i * 13 + (i >> 5)) & 0x3f);
    }
    for (size_t i = 0; i < BODY_LEN; ++i) {
        body[i] = (i < 60000) ? dict[i % DICT_LEN]
                              : (unsigned char)((i / 3) & 0x1f);
    }
}

int main(void) {
    fill_data();

    test_roundtrip();
    test_literal_only();
    test_dictionary();
    test_chained_segments();
    test_empty();
    test_invalid();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("c api ok\n");
    return 0;
}
