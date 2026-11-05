#include <stdio.h>
#include "hcan_timing.h"
int main(void)
{
    unsigned long clk, rate, sp; int data;
    while (scanf("%lu %lu %lu %d", &clk, &rate, &sp, &data) == 4) {
        HcanTiming_t t;
        if (Hcan_ComputeTiming((uint32_t)clk, (uint32_t)rate, (uint32_t)sp, data != 0, &t))
            printf("%u %u %u %u\n", t.prescaler, t.seg1, t.seg2, t.sjw);
        else
            printf("none\n");
    }
    return 0;
}
