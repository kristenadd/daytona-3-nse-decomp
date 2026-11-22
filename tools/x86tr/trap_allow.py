"""Reviewed traps.

guest_cpuid reports SSE2 and nothing newer: leaf 1 ECX is 0, so the SSE4.2
bit (20) and the AVX / OSXSAVE bits (28 and 27) stay clear. Two initializers
are the only things that turn those bits into a dispatch, and both read this
cpuid.

The CRT __isa_available global (2019: 0x2132544, 2017: 0x211D8F4) is written
only by the CRT initializer (2019: 0xC285FF, 2017: 0xC22DEF). It
stores 1 when IsProcessorFeaturePresent(10) says SSE2 is present, 2 only when
ECX bit 20 is set, and 3 only when ECX bit 28 is set. The value is therefore
0 or 1. Every `cmp [__isa_available], 2` below takes the scalar loop, including
the two copies that compare the register the global was just loaded into.

The game's own table (2019: 0x1EC5D20 filled by 0x8B78F0, 2017: 0x1EB10D8
filled by 0x8B21F0) stores one byte per feature. Lookup is
byte[table + index + 4] via 2019 0x8B6FB0 / 2017 0x8B18B0. Index 0xA is the
byte at table+0xE, set only when AVX and OSXSAVE are both set. The ymm helpers
are called only after that lookup returns true.

pblendvb is gated by a byte copied out of the same table during static init
(2019: 0x115AAD0 copies table+0xE, the AVX byte, to 0x1EC5D1D; 2017: 0x11496A0
copies table+0xB, the SSE4.2 byte, to 0x1EB10D5). Both source bytes stay 0, so
the je takes the scalar loop.

int 0x29 is __fastfail. Each site calls IsProcessorFeaturePresent(23) first
(2019: 0xC98942, 2017: 0xC93142) and skips the interrupt when that returns 0.
It is a crash path either way, so it stays a trap.

fnsave / frstor are the CRT x87 exception helper (2019: 0xC3946D, 2017:
0xC33C5D), reached only after a special value fails the status check. Left as
a trap.

translate.py fails if a trap is missing from this list or a listed address is
no longer trapped.
"""

ALLOW = {
    2019: {
        0x008F903D: 'pmulld',
        0x008F90FD: 'pmulld',
        0x008FED30: 'pabsd',
        0x00901927: 'pabsd',
        0x00902698: 'pabsd',
        0x0093BA51: 'pabsd',
        0x0093C0EC: 'pmaxsd',
        0x0093C4AC: 'pminsd',
        0x008B8E52: 'pblendvb',
        0x008B8F29: 'pblendvb',
        0x00C377C6: 'int',
        0x00C3957D: 'x87 fnsave (DD /6)',
        0x00C39589: 'x87 frstor (DD /4)',
        0x00C42F1E: 'int',
        0x00C1E2D4: 'int',
        0x00C1E3D9: 'int',
        0x00C76E0D: 'pcmpistri',
        0x00C76E25: 'pcmpistri',
        0x009A028D: 'pmulld',
        0x00936A3C: 'pmulld',
        0x00A1D3D9: 'vbroadcastss',
        0x00A1D3DE: 'vbroadcastss',
        0x00A1D3E4: 'vbroadcastss',
        0x00A1D3EA: 'vbroadcastss',
        0x00A1D453: 'vmulps',
        0x00A1D457: 'vmulps',
        0x00A1D45F: 'vaddps',
        0x00A1D463: 'vmulps',
        0x00A1D46B: 'vaddps',
        0x00A1D46F: 'vmulps',
        0x00A1D477: 'vaddps',
        0x00A1D47B: 'vmulps',
        0x00A1D480: 'vmulps',
        0x00A1D488: 'vaddps',
        0x00A1D48C: 'vmulps',
        0x00A1D490: 'vaddps',
        0x00A1D494: 'vmulps',
        0x00A1D499: 'vaddps',
        0x00A1D49D: 'vmovups',
        0x00A1D4A8: 'vmovups',
        0x00A1D513: 'vmulps',
        0x00A1D517: 'vmulps',
        0x00A1D51F: 'vaddps',
        0x00A1D523: 'vmulps',
        0x00A1D52B: 'vaddps',
        0x00A1D52F: 'vmulps',
        0x00A1D537: 'vaddps',
        0x00A1D53B: 'vmulps',
        0x00A1D540: 'vmulps',
        0x00A1D548: 'vaddps',
        0x00A1D54C: 'vmulps',
        0x00A1D550: 'vaddps',
        0x00A1D554: 'vmulps',
        0x00A1D559: 'vaddps',
        0x00A1D55D: 'vmovups',
        0x00A1D568: 'vmovups',
        0x00A1D5A1: 'vbroadcastss',
        0x00A1D5A6: 'vbroadcastss',
        0x00A1D5F0: 'vmulps',
        0x00A1D5F5: 'vmulps',
        0x00A1D5FA: 'vmulps',
        0x00A1D5FF: 'vmulps',
        0x00A1D606: 'vaddps',
        0x00A1D60A: 'vmovups',
        0x00A1D60E: 'vaddps',
        0x00A1D612: 'vmovups',
        0x00A1D660: 'vmulps',
        0x00A1D665: 'vmulps',
        0x00A1D66A: 'vmulps',
        0x00A1D66F: 'vmulps',
        0x00A1D676: 'vaddps',
        0x00A1D67A: 'vmovups',
        0x00A1D67E: 'vaddps',
        0x00A1D682: 'vmovups',
    },
    2017: {
        0x008F393D: 'pmulld',
        0x008F39FD: 'pmulld',
        0x008F4A85: 'pmulld',
        0x008F9630: 'pabsd',
        0x008FC227: 'pabsd',
        0x008FCF98: 'pabsd',
        0x00C715FD: 'pcmpistri',
        0x00C71615: 'pcmpistri',
        0x00936351: 'pabsd',
        0x009369EC: 'pmaxsd',
        0x00936DAC: 'pminsd',
        0x008B3752: 'pblendvb',
        0x008B3829: 'pblendvb',
        0x0099AB8D: 'pmulld',
        0x00C31FB6: 'int',
        0x00C33D6D: 'x87 fnsave (DD /6)',
        0x00C33D79: 'x87 frstor (DD /4)',
        0x00C3D70E: 'int',
        0x0093133C: 'pmulld',
        0x00A17CD9: 'vbroadcastss',
        0x00A17CDE: 'vbroadcastss',
        0x00A17CE4: 'vbroadcastss',
        0x00A17CEA: 'vbroadcastss',
        0x00A17D53: 'vmulps',
        0x00A17D57: 'vmulps',
        0x00A17D5F: 'vaddps',
        0x00A17D63: 'vmulps',
        0x00A17D6B: 'vaddps',
        0x00A17D6F: 'vmulps',
        0x00A17D77: 'vaddps',
        0x00A17D7B: 'vmulps',
        0x00A17D80: 'vmulps',
        0x00A17D88: 'vaddps',
        0x00A17D8C: 'vmulps',
        0x00A17D90: 'vaddps',
        0x00A17D94: 'vmulps',
        0x00A17D99: 'vaddps',
        0x00A17D9D: 'vmovups',
        0x00A17DA8: 'vmovups',
        0x00A17E13: 'vmulps',
        0x00A17E17: 'vmulps',
        0x00A17E1F: 'vaddps',
        0x00A17E23: 'vmulps',
        0x00A17E2B: 'vaddps',
        0x00A17E2F: 'vmulps',
        0x00A17E37: 'vaddps',
        0x00A17E3B: 'vmulps',
        0x00A17E40: 'vmulps',
        0x00A17E48: 'vaddps',
        0x00A17E4C: 'vmulps',
        0x00A17E50: 'vaddps',
        0x00A17E54: 'vmulps',
        0x00A17E59: 'vaddps',
        0x00A17E5D: 'vmovups',
        0x00A17E68: 'vmovups',
        0x00A17EA1: 'vbroadcastss',
        0x00A17EA6: 'vbroadcastss',
        0x00A17EF0: 'vmulps',
        0x00A17EF5: 'vmulps',
        0x00A17EFA: 'vmulps',
        0x00A17EFF: 'vmulps',
        0x00A17F06: 'vaddps',
        0x00A17F0A: 'vmovups',
        0x00A17F0E: 'vaddps',
        0x00A17F12: 'vmovups',
        0x00A17F60: 'vmulps',
        0x00A17F65: 'vmulps',
        0x00A17F6A: 'vmulps',
        0x00A17F6F: 'vmulps',
        0x00A17F76: 'vaddps',
        0x00A17F7A: 'vmovups',
        0x00A17F7E: 'vaddps',
        0x00A17F82: 'vmovups',
        0x00C18AB4: 'int',
        0x00C18BB9: 'int',
    },
}


def check(edition, trapped):
    """trapped is (address, reason) pairs. Returns error lines; empty means the list matches."""
    allowed = ALLOW.get(edition)
    if allowed is None:
        return ['no trap allow-list for edition %s' % edition]
    got = {}
    errors = []
    for va, reason in trapped:
        prev = got.get(va)
        if prev is not None and prev != reason:
            errors.append('%08X trapped as %s and %s' % (va, prev, reason))
        got[va] = reason
    for va, reason in sorted(got.items()):
        if va not in allowed:
            errors.append('trap %08X (%s) is not on the allow-list' % (va, reason))
        elif allowed[va] != reason:
            errors.append('trap %08X is %s, allow-list says %s' % (va, reason, allowed[va]))
    for va, reason in sorted(allowed.items()):
        if va not in got:
            errors.append('allow-list %08X (%s) was not trapped' % (va, reason))
    return errors
