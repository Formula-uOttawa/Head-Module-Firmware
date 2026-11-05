"""FDCAN bit timing (mirror of Common/Src/hcan_timing.c) and achievable-rate checks."""

from __future__ import annotations


def compute_timing(kernel_hz: int, bitrate: int, sample_permille: int, data_phase: bool):
    """(prescaler, seg1, seg2, sjw) or None if the rate cannot be generated exactly from kernel_hz."""
    max_presc, max_s1, max_s2 = (32, 32, 16) if data_phase else (512, 256, 128)
    if bitrate <= 0 or kernel_hz <= 0:
        return None
    for presc in range(1, max_presc + 1):
        if kernel_hz % (presc * bitrate):
            continue
        tq = kernel_hz // (presc * bitrate)
        if tq < 4 or tq > 1 + max_s1 + max_s2:
            continue
        s2 = (tq * (1000 - sample_permille) + 500) // 1000
        s2 = max(1, min(s2, max_s2))
        if tq - 1 <= s2:
            continue
        s1 = tq - 1 - s2
        if s1 > max_s1:
            s1 = max_s1
            if tq - 1 - s1 > max_s2:
                continue
            s2 = tq - 1 - s1
        return presc, s1, s2, min(s2, 16 if data_phase else 128)
    return None


def achievable(kernel_hz: int, rate: int, data_phase: bool) -> bool:
    return compute_timing(kernel_hz, rate, 750 if data_phase else 800, data_phase) is not None


COMMON_RATES = [125_000, 250_000, 320_000, 500_000, 800_000, 1_000_000, 1_600_000, 2_000_000, 3_200_000,
                4_000_000, 5_000_000, 6_400_000, 8_000_000]


def nearby(kernel_hz: int, rate: int, data_phase: bool, n: int = 3) -> list[int]:
    """Common rates the clock can hit exactly, closest to `rate` first."""
    ok = [r for r in COMMON_RATES if achievable(kernel_hz, r, data_phase)]
    return sorted(ok, key=lambda r: abs(r - rate))[:n]


def fmt(rate: int) -> str:
    return f"{rate / 1e6:g} Mbit/s" if rate >= 1_000_000 else f"{rate / 1e3:g} kbit/s"
