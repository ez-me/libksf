/* ABI smoke test for libksf.
 *
 * The binary links against the real libz for uncompress(); at runtime it is
 * run under LD_PRELOAD=build/libksf.so so compress2()/compressBound() resolve
 * to the shim. This checks the return codes and that stock libz can inflate
 * what the shim produced.
 */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void) {
    static unsigned char src[100000];
    static unsigned char dst[200000];
    static unsigned char back[100000];

    for (size_t i = 0; i < sizeof src; ++i) {
        src[i] = (unsigned char)((i * 7 + (i >> 3)) & 0xff);
    }

    /* A too-small destination must report Z_BUF_ERROR, not crash. */
    uLongf n = 16;
    int rc = compress2(dst, &n, src, sizeof src, 6);
    if (rc != Z_BUF_ERROR) {
        printf("too-small dest: expected Z_BUF_ERROR, got %d\n", rc);
        return 1;
    }

    /* compressBound must cover the actual output. */
    const uLong bound = compressBound(sizeof src);
    if (bound < sizeof src) {
        printf("compressBound too small: %lu\n", (unsigned long)bound);
        return 1;
    }

    n = sizeof dst;
    rc = compress2(dst, &n, src, sizeof src, 9);
    if (rc != Z_OK) {
        printf("compress2: expected Z_OK, got %d\n", rc);
        return 1;
    }
    if ((uLong)n > bound) {
        printf("output %lu exceeds bound %lu\n", (unsigned long)n,
               (unsigned long)bound);
        return 1;
    }

    uLongf m = sizeof back;
    rc = uncompress(back, &m, dst, n);
    if (rc != Z_OK || m != sizeof src || memcmp(back, src, m) != 0) {
        printf("round-trip failed: rc=%d size=%lu\n", rc, (unsigned long)m);
        return 1;
    }

    /* Level 0 must also round-trip and stay close to the input size. */
    n = sizeof dst;
    rc = compress2(dst, &n, src, sizeof src, 0);
    if (rc != Z_OK || n > sizeof src + 1024) {
        printf("level 0 failed: rc=%d size=%lu\n", rc, (unsigned long)n);
        return 1;
    }
    m = sizeof back;
    rc = uncompress(back, &m, dst, n);
    if (rc != Z_OK || m != sizeof src || memcmp(back, src, m) != 0) {
        printf("level 0 round-trip failed\n");
        return 1;
    }

    printf("abi ok: %lu -> %lu bytes\n", (unsigned long)sizeof src,
           (unsigned long)n);
    return 0;
}
