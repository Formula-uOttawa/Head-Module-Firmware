/* Unit test of hcanlink.c: fragmenting a message into CAN-FD frames and reassembling it, with every kind of damage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hcanlink.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t data[64]; uint8_t len, seg; } Frag_t;

static int split(uint8_t type, uint8_t seq, const uint8_t *pl, size_t n, Frag_t *out)
{
    HclTx_t t;
    if (!HclTx_Begin(&t, type, seq, pl, n)) return -1;
    int k = 0;
    while (HclTx_Next(&t, out[k].data, &out[k].len, &out[k].seg)) k++;
    return k;
}

static int is_fd_len(unsigned n) { static const unsigned l[] = { 8, 12, 16, 20, 24, 32, 48, 64 }; for (unsigned i = 0; i < 8; i++) if (l[i] == n) return 1; return 0; }

int main(void)
{
    uint8_t pl[300];
    for (int i = 0; i < 300; i++) pl[i] = (uint8_t)(i * 7 + 3);
    Frag_t f[HCL_MAX_FRAGMENTS + 2];
    HclRx_t rx;
    HclRx_Init(&rx);

    /* every payload size: fragments are valid CAN-FD lengths, the last carries the flag, the message comes back intact */
    for (size_t n = 0; n <= HCL_MAX_BODY - 4; n++) {
        int k = split(0x04, 0x55, pl, n, f);
        CHECK(k > 0 && k <= (int)HCL_MAX_FRAGMENTS, "n=%zu fragments %d", n, k);
        int got = 0;
        for (int i = 0; i < k; i++) {
            CHECK(is_fd_len(f[i].len), "n=%zu frag %d len %u", n, i, f[i].len);
            CHECK((f[i].seg & HCL_SEG_IDX_MASK) == i && ((f[i].seg & HCL_SEG_LAST) != 0) == (i == k - 1), "flags n=%zu i=%d seg %02X", n, i, f[i].seg);
            int r = HclRx_Feed(&rx, f[i].seg, f[i].data, f[i].len);
            CHECK(r == (i == k - 1 ? 1 : 0), "n=%zu frag %d result %d", n, i, r);
            got = r;
        }
        if (got == 1) {
            const uint8_t *m = HclRx_Msg(&rx);
            CHECK(rx.msg_len == n + 2 && m[0] == 0x04 && m[1] == 0x55 && memcmp(m + 2, pl, n) == 0, "payload mismatch n=%zu", n);
        }
    }
    CHECK(rx.crc_fail == 0 && rx.seq_err == 0 && rx.len_err == 0, "clean run counted errors");
    uint32_t ok_before = rx.ok;

    /* too big for one message */
    HclTx_t t;
    CHECK(!HclTx_Begin(&t, 1, 1, pl, HCL_MAX_BODY - 3), "oversize payload accepted");

    /* damage: 252 bytes = 5 fragments (the largest message) */
    int k = split(0x03, 9, pl, 252, f);
    CHECK(k == 5, "fragments %d", k);
    /* a lost fragment (1) */
    HclRx_Reset(&rx);
    CHECK(HclRx_Feed(&rx, f[0].seg, f[0].data, f[0].len) == 0, "f0");
    CHECK(HclRx_Feed(&rx, f[2].seg, f[2].data, f[2].len) == -1 && rx.seq_err == 1, "gap not detected");
    CHECK(HclRx_Feed(&rx, f[3].seg, f[3].data, f[3].len) == -1, "fragment after a dropped message must not resume it");
    /* repeated fragment */
    HclRx_Reset(&rx);
    (void)HclRx_Feed(&rx, f[0].seg, f[0].data, f[0].len);
    (void)HclRx_Feed(&rx, f[1].seg, f[1].data, f[1].len);
    CHECK(HclRx_Feed(&rx, f[1].seg, f[1].data, f[1].len) == -1, "duplicate fragment accepted");
    /* a new message (fragment 0) interrupts an unfinished one and is received whole */
    HclRx_Reset(&rx);
    (void)HclRx_Feed(&rx, f[0].seg, f[0].data, f[0].len);
    (void)HclRx_Feed(&rx, f[1].seg, f[1].data, f[1].len);
    int r = 0;
    for (int i = 0; i < k; i++) r = HclRx_Feed(&rx, f[i].seg, f[i].data, f[i].len);
    CHECK(r == 1 && memcmp(HclRx_Msg(&rx) + 2, pl, 252) == 0, "restart at fragment 0");
    /* corrupted payload byte -> CRC */
    uint32_t crc_before = rx.crc_fail;
    f[2].data[5] ^= 0x40;
    for (int i = 0; i < k; i++) r = HclRx_Feed(&rx, f[i].seg, f[i].data, f[i].len);
    CHECK(r == -1 && rx.crc_fail == crc_before + 1, "crc error not detected (r=%d)", r);
    f[2].data[5] ^= 0x40;
    /* a continuation without a start */
    HclRx_Reset(&rx);
    CHECK(HclRx_Feed(&rx, f[1].seg, f[1].data, f[1].len) == -1, "continuation without start");
    /* last flag too early / missing */
    HclRx_Reset(&rx);
    (void)HclRx_Feed(&rx, f[0].seg, f[0].data, f[0].len);
    CHECK(HclRx_Feed(&rx, (uint8_t)(f[1].seg | HCL_SEG_LAST), f[1].data, f[1].len) == -1, "early last flag");
    HclRx_Reset(&rx);
    for (int i = 0; i < k - 1; i++) (void)HclRx_Feed(&rx, f[i].seg, f[i].data, f[i].len);
    CHECK(HclRx_Feed(&rx, (uint8_t)(f[k - 1].seg & ~HCL_SEG_LAST), f[k - 1].data, f[k - 1].len) == -1, "missing last flag");
    /* absurd lengths in fragment 0 */
    uint8_t bad[8] = { 0xFF, 0xFF, 1, 2, 3, 4, 5, 6 };
    CHECK(HclRx_Feed(&rx, 0x80, bad, 8) == -1 && rx.len_err > 0, "oversize body length accepted");
    bad[0] = 2; bad[1] = 0;
    CHECK(HclRx_Feed(&rx, 0x80, bad, 8) == -1, "body shorter than type+seq+crc accepted");
    CHECK(HclRx_Feed(&rx, 0x80, bad, 1) == -1, "fragment 0 shorter than the length prefix accepted");

    /* Unget: a refused fragment is produced again, the stream is unchanged */
    HclTx_Begin(&t, 0x10, 1, pl, 200);
    uint8_t d1[64], d2[64], l1, l2, s1, s2;
    HclTx_Next(&t, d1, &l1, &s1);
    HclTx_Next(&t, d2, &l2, &s2);
    HclTx_Unget(&t);
    uint8_t d3[64], l3, s3;
    HclTx_Next(&t, d3, &l3, &s3);
    CHECK(l3 == l2 && s3 == s2 && memcmp(d2, d3, l2) == 0, "unget did not repeat the fragment");
    CHECK(rx.ok >= ok_before, "ok counter");

    /* identifier helpers */
    CHECK(HCL_ID(HCL_TYPE_CONFIG, 5, 0x83) == ((1u << 27) | (5u << 24) | 0x83u), "id layout");
    CHECK(HCL_ID_TYPE(HCL_ID(HCL_TYPE_ACK, 3, 1)) == HCL_TYPE_ACK && HCL_ID_HUB(HCL_ID(HCL_TYPE_ACK, 3, 1)) == 3 && HCL_ID_SEG(HCL_ID(HCL_TYPE_ACK, 3, 0x81)) == 0x81, "id fields");
    CHECK((HCL_ID(HCL_TYPE_CONFIG, 0, 0) & HCL_FILTER_MASK) == HCL_ID(HCL_TYPE_CONFIG, 0, 0), "filter id inside its mask");

    printf(g_fail ? "SIM FAILED (%d)\n" : "SIM OK\n", g_fail);
    return g_fail ? 1 : 0;
}
