/* Streaming ABI test for libksf.
 *
 * Links against the real libz for inflate(); run under
 * LD_PRELOAD=build/libksf.so so every deflate* call reaches the shim. Covers
 * the entry points Python's zlib module does not expose directly:
 *
 *   deflateReset        reuse a stream object
 *   deflateParams       change level/strategy mid-stream
 *   deflateBound        output bound
 *   deflatePending      buffered output query
 *   deflateInit2_       version / size / argument validation
 *   deflateSetDictionary preset dictionary, DICTID, raw and gzip rules
 *   deflateGetDictionary window reporting
 *   deflateReset/Keep   dictionary lifetime
 *   zalloc / zfree      caller-supplied allocators
 *   interleaved streams independent state for concurrent objects
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SRC_LEN 300000
#define DST_LEN 700000
#define DICT_LEN 20000

static unsigned char src[SRC_LEN];
static unsigned char dst[DST_LEN];
static unsigned char back[SRC_LEN];
static unsigned char dict[DICT_LEN];

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

static int inflate_all(const unsigned char *in, uLong in_len,
                       unsigned char *out, uLong *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) != Z_OK) {
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = out;
    zs.avail_out = (uInt)*out_len;
    int rc = inflate(&zs, Z_FINISH);
    *out_len = zs.total_out;
    inflateEnd(&zs);
    return rc;
}

/* Inflate a zlib-wrapped stream, supplying the preset dictionary when the
 * stream asks for it. Reports whether Z_NEED_DICT was seen and the DICTID it
 * carried. */
static int inflate_all_dict(const unsigned char *in, uLong in_len,
                            unsigned char *out, uLong *out_len, int *needed,
                            uLong *dictid, const unsigned char *d,
                            uInt dlen) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) != Z_OK) {
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = out;
    zs.avail_out = (uInt)*out_len;
    *needed = 0;
    *dictid = 0;
    int rc = inflate(&zs, Z_FINISH);
    if (rc == Z_NEED_DICT) {
        *needed = 1;
        *dictid = zs.adler;
        rc = inflateSetDictionary(&zs, d, dlen);
        if (rc == Z_OK) {
            rc = inflate(&zs, Z_FINISH);
        }
    }
    *out_len = zs.total_out;
    inflateEnd(&zs);
    return rc;
}

/* Inflate a raw stream with an explicit window. */
static int inflate_all_raw_dict(const unsigned char *in, uLong in_len,
                                unsigned char *out, uLong *out_len,
                                const unsigned char *window, uInt window_len) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, -15) != Z_OK) {
        return -1;
    }
    if (window_len != 0 &&
        inflateSetDictionary(&zs, window, window_len) != Z_OK) {
        inflateEnd(&zs);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = out;
    zs.avail_out = (uInt)*out_len;
    int rc = inflate(&zs, Z_FINISH);
    *out_len = zs.total_out;
    inflateEnd(&zs);
    return rc;
}

/* Deflate src[0..len) into dst, feeding in chunks of `step` bytes and
 * returning the total output size, or 0 on failure. */
static uLong deflate_buffer(int level, int wbits, uInt step,
                            unsigned char *out, uLong out_cap) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, level, Z_DEFLATED, wbits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return 0;
    }
    zs.next_out = out;
    zs.avail_out = (uInt)out_cap;
    uLong consumed = 0;
    int rc = Z_OK;
    while (consumed < SRC_LEN) {
        uInt n = (uInt)(SRC_LEN - consumed);
        if (n > step) {
            n = step;
        }
        zs.next_in = src + consumed;
        zs.avail_in = n;
        while (zs.avail_in != 0) {
            rc = deflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK) {
                deflateEnd(&zs);
                return 0;
            }
        }
        consumed += n;
    }
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    uLong produced = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        return 0;
    }
    return produced;
}

static void test_roundtrip_chunks(void) {
    const uInt steps[] = {1, 2, 3, 7, 4096, 65536, SRC_LEN};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        uLong n = deflate_buffer(6, 15, steps[i], dst, DST_LEN);
        check(n != 0, "deflate with chunked input");
        if (n == 0) {
            continue;
        }
        uLong m = SRC_LEN;
        int rc = inflate_all(dst, n, back, &m);
        check(rc == Z_STREAM_END, "inflate reaches Z_STREAM_END");
        check(m == SRC_LEN && memcmp(back, src, SRC_LEN) == 0,
              "chunked round-trip matches");
    }
}

static void test_reset(void) {
    z_stream a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    check(deflateInit(&a, 6) == Z_OK, "deflateInit a");
    check(deflateInit(&b, 6) == Z_OK, "deflateInit b");

    /* Fresh stream writes into the first half of dst. */
    b.next_in = src;
    b.avail_in = SRC_LEN;
    b.next_out = dst;
    b.avail_out = DST_LEN / 2;
    int rc = deflate(&b, Z_FINISH);
    check(rc == Z_STREAM_END, "fresh stream finish");
    const uLong fresh = b.total_out;

    /* A used stream is finished once, reset, and finished again into the
     * second half; the two runs must agree. */
    a.next_in = src;
    a.avail_in = SRC_LEN;
    a.next_out = dst + DST_LEN / 2;
    a.avail_out = DST_LEN / 2;
    rc = deflate(&a, Z_FINISH);
    check(rc == Z_STREAM_END, "first finish");
    check(deflateReset(&a) == Z_OK, "deflateReset");

    a.next_in = src;
    a.avail_in = SRC_LEN;
    a.next_out = dst + DST_LEN / 2;
    a.avail_out = DST_LEN / 2;
    rc = deflate(&a, Z_FINISH);
    check(rc == Z_STREAM_END, "reset stream finish");
    check(a.total_out == fresh, "reset reproduces the same size");
    check(memcmp(dst, dst + DST_LEN / 2, fresh) == 0,
          "reset reproduces the same bytes as a fresh stream");

    deflateEnd(&a);
    deflateEnd(&b);
}

static void test_params_and_bound(void) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    check(deflateInit(&zs, 1) == Z_OK, "deflateInit level 1");
    check(deflateBound(&zs, SRC_LEN) >= SRC_LEN, "deflateBound covers input");

    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    zs.next_in = src;
    zs.avail_in = SRC_LEN / 2;
    check(deflate(&zs, Z_NO_FLUSH) == Z_OK, "first half");
    check(deflateParams(&zs, 9, Z_DEFAULT_STRATEGY) == Z_OK, "deflateParams");
    zs.next_in = src + SRC_LEN / 2;
    zs.avail_in = SRC_LEN - SRC_LEN / 2;
    int rc;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "finish after deflateParams");
    uLong n = zs.total_out;

    unsigned pending = 0;
    int bits = 0;
    check(deflatePending(&zs, &pending, &bits) == Z_OK, "deflatePending");
    check(pending == 0, "no pending output after Z_STREAM_END");
    deflateEnd(&zs);

    uLong m = SRC_LEN;
    rc = inflate_all(dst, n, back, &m);
    check(rc == Z_STREAM_END && m == SRC_LEN &&
             memcmp(back, src, SRC_LEN) == 0,
          "deflateParams round-trip");
}

static void test_dictionary(void) {
    /* Baseline without a dictionary, for the ratio check below. */
    const uLong plain = deflate_buffer(6, 15, SRC_LEN, dst, DST_LEN);
    check(plain != 0, "plain deflate for dictionary comparison");

    /* --- zlib wrapper: DICTID in the header, Z_NEED_DICT on inflate --- */
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    check(deflateInit(&zs, 6) == Z_OK, "dictionary init");
    check(deflateSetDictionary(&zs, dict, DICT_LEN) == Z_OK,
          "deflateSetDictionary");
    check(zs.adler == adler32(adler32(0L, Z_NULL, 0), dict, DICT_LEN),
          "strm->adler reports the dictionary checksum");

    unsigned char window[32768];
    uInt window_len = 0;
    check(deflateGetDictionary(&zs, window, &window_len) == Z_OK,
          "deflateGetDictionary after set");
    check(window_len == DICT_LEN && memcmp(window, dict, DICT_LEN) == 0,
          "deflateGetDictionary returns the dictionary");

    zs.next_in = src;
    zs.avail_in = SRC_LEN / 2;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    check(deflate(&zs, Z_NO_FLUSH) == Z_OK, "dictionary first half");

    /* The reported window is the dictionary followed by input consumed but
     * not yet compressed, capped at 32 KiB. */
    window_len = 0;
    check(deflateGetDictionary(&zs, window, &window_len) == Z_OK,
          "deflateGetDictionary after input");
    check(window_len == 32768, "deflateGetDictionary caps at 32 KiB");
    {
        const uInt tail_len = 4096;
        check(memcmp(window + window_len - tail_len,
                     src + SRC_LEN / 2 - tail_len, tail_len) == 0,
              "deflateGetDictionary ends with recent input");
    }

    zs.next_in = src + SRC_LEN / 2;
    zs.avail_in = SRC_LEN - SRC_LEN / 2;
    int rc;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "dictionary finish");
    uLong n = zs.total_out;
    deflateEnd(&zs);

    uLong m = SRC_LEN;
    int needed = 0;
    uLong dictid = 0;
    rc = inflate_all_dict(dst, n, back, &m, &needed, &dictid, dict, DICT_LEN);
    check(needed, "zlib stream reports Z_NEED_DICT");
    check(dictid == adler32(adler32(0L, Z_NULL, 0), dict, DICT_LEN),
          "DICTID matches the dictionary checksum");
    check(rc == Z_STREAM_END && m == SRC_LEN &&
             memcmp(back, src, SRC_LEN) == 0,
          "zlib dictionary round-trip");
    check(n < plain, "dictionary improves compression");

    /* --- a dictionary larger than the window: DICTID covers all of it, the
     * engine keeps only the tail --- */
    static unsigned char big[40000];
    for (int i = 0; i < 40000; ++i) {
        big[i] = (unsigned char)((i * 7 + 3) & 0x3f);
    }
    memset(&zs, 0, sizeof zs);
    check(deflateInit(&zs, 6) == Z_OK, "big dictionary init");
    check(deflateSetDictionary(&zs, big, 40000) == Z_OK, "big dictionary set");
    window_len = 0;
    check(deflateGetDictionary(&zs, window, &window_len) == Z_OK,
          "big dictionary get");
    check(window_len == 32768 &&
             memcmp(window, big + 40000 - 32768, 32768) == 0,
          "big dictionary keeps only the tail");
    zs.next_in = src;
    zs.avail_in = SRC_LEN;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "big dictionary finish");
    const uLong big_n = zs.total_out;
    deflateEnd(&zs);

    m = SRC_LEN;
    needed = 0;
    dictid = 0;
    rc = inflate_all_dict(dst, big_n, back, &m, &needed, &dictid, big, 40000);
    check(needed, "big dictionary reports Z_NEED_DICT");
    check(dictid == adler32(adler32(0L, Z_NULL, 0), big, 40000),
          "big DICTID covers the whole dictionary");
    check(rc == Z_STREAM_END && m == SRC_LEN &&
             memcmp(back, src, SRC_LEN) == 0,
          "big dictionary round-trip");

    /* --- raw deflate: no DICTID, caller supplies the dictionary --- */
    memset(&zs, 0, sizeof zs);
    check(deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK,
          "raw dictionary init");
    check(deflateSetDictionary(&zs, dict, DICT_LEN) == Z_OK,
          "raw deflateSetDictionary");
    zs.next_in = src;
    zs.avail_in = SRC_LEN;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "raw dictionary finish");
    const uLong raw_n = zs.total_out;
    deflateEnd(&zs);

    m = SRC_LEN;
    rc = inflate_all_raw_dict(dst, raw_n, back, &m, dict, DICT_LEN);
    check(rc == Z_STREAM_END && m == SRC_LEN &&
             memcmp(back, src, SRC_LEN) == 0,
          "raw dictionary round-trip");

    /* --- gzip has no DICTID field and rejects dictionaries --- */
    memset(&zs, 0, sizeof zs);
    check(deflateInit2(&zs, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK,
          "gzip dictionary init");
    check(deflateSetDictionary(&zs, dict, DICT_LEN) == Z_STREAM_ERROR,
          "gzip rejects deflateSetDictionary");
    deflateEnd(&zs);

    /* --- zlib wrapper: too late once input is pending --- */
    memset(&zs, 0, sizeof zs);
    check(deflateInit(&zs, 6) == Z_OK, "late dictionary init");
    zs.next_in = src;
    zs.avail_in = 1000;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    check(deflate(&zs, Z_NO_FLUSH) == Z_OK, "late dictionary input");
    check(deflateSetDictionary(&zs, dict, DICT_LEN) == Z_STREAM_ERROR,
          "zlib rejects a late dictionary");
    deflateEnd(&zs);

    /* --- deflateReset drops the dictionary --- */
    memset(&zs, 0, sizeof zs);
    check(deflateInit(&zs, 6) == Z_OK, "reset dictionary init");
    check(deflateSetDictionary(&zs, dict, DICT_LEN) == Z_OK,
          "reset dictionary set");
    check(deflateReset(&zs) == Z_OK, "dictionary reset");
    zs.next_in = src;
    zs.avail_in = SRC_LEN;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "reset dictionary finish");
    const uLong reset_n = zs.total_out;
    deflateEnd(&zs);

    m = SRC_LEN;
    rc = inflate_all(dst, reset_n, back, &m);
    check(rc == Z_STREAM_END && m == SRC_LEN &&
             memcmp(back, src, SRC_LEN) == 0,
          "reset stream needs no dictionary");

    /* --- deflateResetKeep preserves the raw window --- */
    memset(&zs, 0, sizeof zs);
    check(deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK,
          "keep dictionary init");
    check(deflateSetDictionary(&zs, dict, DICT_LEN) == Z_OK,
          "keep dictionary set");
    zs.next_in = src;
    zs.avail_in = SRC_LEN;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "keep first finish");

    window_len = 0;
    check(deflateGetDictionary(&zs, window, &window_len) == Z_OK,
          "keep get dictionary");
    check(window_len == 32768, "keep window is full");
    check(deflateResetKeep(&zs) == Z_OK, "deflateResetKeep");
    zs.next_in = src;
    zs.avail_in = SRC_LEN;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "keep second finish");
    const uLong keep_n = zs.total_out;
    deflateEnd(&zs);

    m = SRC_LEN;
    rc = inflate_all_raw_dict(dst, keep_n, back, &m, window, window_len);
    check(rc == Z_STREAM_END && m == SRC_LEN &&
             memcmp(back, src, SRC_LEN) == 0,
          "deflateResetKeep round-trip with the saved window");
}

static void test_interleaved(void) {
    z_stream a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    check(deflateInit(&a, 1) == Z_OK, "interleaved init a");
    check(deflateInit(&b, 1) == Z_OK, "interleaved init b");

    static unsigned char out_a[DST_LEN], out_b[DST_LEN];
    a.next_out = out_a;
    a.avail_out = DST_LEN;
    b.next_out = out_b;
    b.avail_out = DST_LEN;

    uLong pos = 0;
    const uLong step = 10000;
    while (pos < SRC_LEN) {
        uLong n = SRC_LEN - pos;
        if (n > step) {
            n = (uLong)step;
        }
        a.next_in = src + pos;
        a.avail_in = (uInt)n;
        b.next_in = src + pos;
        b.avail_in = (uInt)n;
        while (a.avail_in != 0) {
            check(deflate(&a, Z_NO_FLUSH) == Z_OK, "interleaved a");
        }
        while (b.avail_in != 0) {
            check(deflate(&b, Z_NO_FLUSH) == Z_OK, "interleaved b");
        }
        pos += n;
    }
    int ra, rb;
    do {
        ra = deflate(&a, Z_FINISH);
    } while (ra == Z_OK && a.avail_out != 0);
    do {
        rb = deflate(&b, Z_FINISH);
    } while (rb == Z_OK && b.avail_out != 0);
    check(ra == Z_STREAM_END && rb == Z_STREAM_END, "interleaved finish");
    check(a.total_out == b.total_out, "interleaved same size");
    check(memcmp(out_a, out_b, a.total_out) == 0, "interleaved same bytes");

    uLong m = SRC_LEN;
    check(inflate_all(out_a, a.total_out, back, &m) == Z_STREAM_END &&
             m == SRC_LEN && memcmp(back, src, SRC_LEN) == 0,
          "interleaved round-trip");
    deflateEnd(&a);
    deflateEnd(&b);
}

static long alloc_count;
static long free_count;

static voidpf counting_alloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    ++alloc_count;
    return malloc((size_t)items * size);
}

static void counting_free(voidpf opaque, voidpf address) {
    (void)opaque;
    ++free_count;
    free(address);
}

static void test_custom_allocator(void) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    zs.zalloc = counting_alloc;
    zs.zfree = counting_free;
    check(deflateInit(&zs, 6) == Z_OK, "custom allocator init");
    zs.next_in = src;
    zs.avail_in = SRC_LEN;
    zs.next_out = dst;
    zs.avail_out = DST_LEN;
    int rc;
    do {
        rc = deflate(&zs, Z_FINISH);
    } while (rc == Z_OK && zs.avail_out != 0);
    check(rc == Z_STREAM_END, "custom allocator finish");
    check(deflateEnd(&zs) == Z_OK, "custom allocator end");
    check(alloc_count > 0, "custom zalloc was used");
    check(free_count == alloc_count, "custom zfree balanced zalloc");
}

static void test_init_errors(void) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    check(deflateInit_(&zs, 6, "2.0", (int)sizeof(z_stream)) ==
             Z_VERSION_ERROR,
          "wrong major version rejected");
    check(deflateInit_(&zs, 6, ZLIB_VERSION, (int)sizeof(z_stream) + 1) ==
             Z_VERSION_ERROR,
          "wrong stream size rejected");
    check(deflateInit2(&zs, 6, Z_DEFLATED, 15, 8, 99) == Z_STREAM_ERROR,
          "bad strategy rejected");
    check(deflateInit2(&zs, 10, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) ==
             Z_STREAM_ERROR,
          "bad level rejected");
    check(deflateInit2(&zs, 6, Z_DEFLATED, 99, 8, Z_DEFAULT_STRATEGY) ==
             Z_STREAM_ERROR,
          "bad windowBits rejected");
}

static void fill_dictionary(void) {
    for (int i = 0; i < DICT_LEN; ++i) {
        dict[i] = (unsigned char)((i * 13 + (i >> 5)) & 0x3f);
    }
}

int main(void) {
    for (int i = 0; i < SRC_LEN; ++i) {
        src[i] = (unsigned char)((i * 7 + (i >> 3)) & 0xff);
    }
    /* Make it compressible in stretches so both branches are exercised. */
    for (int i = 0; i < SRC_LEN; i += 4096) {
        memset(src + i, 0, 2048 < SRC_LEN - i ? 2048 : SRC_LEN - i);
    }
    fill_dictionary();
    /* Reuse the dictionary in the body so priming measurably helps. */
    for (int i = 0; i < 60000; ++i) {
        src[i] = dict[i % DICT_LEN];
    }

    test_roundtrip_chunks();
    test_reset();
    test_params_and_bound();
    test_dictionary();
    test_interleaved();
    test_custom_allocator();
    test_init_errors();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("stream abi ok\n");
    return 0;
}
