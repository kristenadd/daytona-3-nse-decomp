#pragma once
// Guest addresses the host code uses directly, per edition. The translator has
// its own tables (tools/x86tr: OVERRIDES, SITE_OVERRIDES, COOKIE_CHECKS); this
// is the runtime side. The 2017 values were found from the 2019 ones by masked
// instruction signatures (the instructions that use each address, matched in
// the 2017 build) and checked by disassembly; the emulated Model 2 RAM sits
// 0x14180 lower in 2017.
#include <cstdint>

#include "system/edition.hpp"

namespace dcusa::recomp {

struct GameAddresses {
    dcusa::Edition edition;
    std::uint32_t crt_thread_entry;   // _threadstartex: [param + 0x54] is the procedure
    std::uint32_t crt_fpatan_tail;    // atan2's fpatan tail, reached by the CRT dispatcher's jmp
    std::uint32_t cri_sound_server;   // the CRI sound server thread procedure
    std::uint32_t boot_state;         // u32, the game's boot / mode state
    std::uint32_t shell_credits;      // u8 at +7: credits as the shell last set them
    std::uint32_t race_timer;         // u32, the HUD's race timer in 1/64 s
    std::uint32_t pad_slot;           // the input profile's slot (pad debug log)
    std::uint32_t jvs_inputs;         // emulated Model 2 RAM: JVS input bytes (pad debug log)
    std::uint32_t jvs_gear;           // emulated Model 2 RAM: gear (pad debug log)
};

inline constexpr GameAddresses kGame2019{dcusa::Edition::Nse2019,
                                         0xC215BBu, 0xC5030Cu, 0x59E660u, 0x1735608u, 0x15278F8u,
                                         0x19C98ECu, 0x15964A0u, 0x19C87F8u, 0x19C880Cu};
// race_timer: 0x19C98EC less the Model 2 RAM shift; no instruction names it
// directly, so it is not checked against the HUD yet.
inline constexpr GameAddresses kGame2017{dcusa::Edition::Dcu2017,
                                         0xC1BD9Bu, 0xC4AAFCu, 0x599870u, 0x1721470u, 0x15158F8u,
                                         0x19B576Cu, 0x1584220u, 0x19B4678u, 0x19B468Cu};

// The loaded game's addresses (GuestProcess::load sets them; 2019 before that).
inline const GameAddresses* g_game_addresses = &kGame2019;
inline const GameAddresses& game_addresses() { return *g_game_addresses; }
inline void set_game_edition(dcusa::Edition e) {
    g_game_addresses = e == dcusa::Edition::Dcu2017 ? &kGame2017 : &kGame2019;
}

}
