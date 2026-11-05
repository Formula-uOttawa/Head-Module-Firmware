#include "hcan_timing.h"

bool Hcan_ComputeTiming(uint32_t kernel_hz, uint32_t bitrate, uint32_t sample_permille, bool data_phase, HcanTiming_t *out)
{
    /* FDCAN limits (RM0399): nominal NBRP 1-512, NTSEG1 1-256, NTSEG2 1-128, NSJW 1-128;
       data DBRP 1-32, DTSEG1 1-32, DTSEG2 1-16, DSJW 1-16 */
    const uint32_t max_presc = data_phase ? 32 : 512, max_s1 = data_phase ? 32 : 256, max_s2 = data_phase ? 16 : 128;
    const uint32_t max_sjw = data_phase ? 16u : 128u;
    if (bitrate == 0 || kernel_hz == 0 || bitrate > kernel_hz / 4u || sample_permille > 1000u || !out)
        return false; /* at least 4 time quanta per bit are needed */
    for (uint32_t presc = 1; presc <= max_presc; presc++) {
        const uint64_t denom = (uint64_t)presc * bitrate; /* 64-bit: presc * bitrate must not wrap */
        if (kernel_hz % denom)
            continue;
        uint32_t tq = (uint32_t)(kernel_hz / denom); /* time quanta per bit, including the sync segment */
        if (tq < 4 || tq > 1 + max_s1 + max_s2)
            continue;
        uint32_t s2 = (tq * (1000 - sample_permille) + 500) / 1000;
        if (s2 < 1) s2 = 1;
        if (s2 > max_s2) s2 = max_s2;
        if (tq - 1 <= s2)
            continue;
        uint32_t s1 = tq - 1 - s2;
        if (s1 > max_s1) {
            s1 = max_s1;
            if (tq - 1 - s1 > max_s2)
                continue;
            s2 = tq - 1 - s1;
        }
        out->prescaler = presc;
        out->seg1 = s1;
        out->seg2 = s2;
        out->sjw = s2 < max_sjw ? s2 : max_sjw;
        return true;
    }
    return false;
}
