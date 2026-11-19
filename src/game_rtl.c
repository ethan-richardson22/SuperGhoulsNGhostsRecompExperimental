/*
 * Super Ghouls N Ghosts — per-game runtime glue.
 *
 * THIS FILE IS THE PORT. Everything else the scaffold produced is layout,
 * build wiring, and packaging; this is where the actual work happens.
 *
 * The framework does not, and cannot, drive an arbitrary SNES game on its
 * own. Two responsibilities always land on the game host:
 *
 *   1. Deciding what "one frame" means for THIS title, and returning from
 *      run_frame() at that boundary. A real ROM's reset vector never
 *      returns — it enters a main loop that waits on vblank — so somebody
 *      has to choose the yield point.
 *   2. Delivering NMI/IRQ at the hardware edge (see
 *      snesrecomp/docs/FRAME_MODEL_HOSTS.md).
 *
 * A new port starts on the framework's beam-aligned driver
 * (runner/src/beam_frame_driver.h): one frame is one PPU field, cut at the
 * beam's vblank edge; interrupts -- raster splits and a coprocessor's IRQ
 * line -- are taken where the beam latches them; parked time still runs the
 * beam, APU and coprocessors; and the field is rasterized from the raster
 * journal with real HDMA. It is the framework's, so a fix to it reaches this
 * port on a submodule pull.
 *
 * This file used to hold its own copy of a simpler driver: frames measured
 * from wherever the CPU last stopped, NMI delivered at an arbitrary beam
 * position, the field drawn with end-of-frame register state. A game that
 * uploads in vblank between INIDISP=$80 and $0F flickered black, and one
 * that chains raster splits fell apart. Replace these calls only for what
 * the driver does not do; MetalWarriorsSNESRecomp's src/mw_rtl.c and
 * GundamWingEndlessDuelSNESRecomp's src/game_rtl.c are developed examples of
 * title-specific drivers.
 */

#include "game_rtl.h"
#include "game_ws.h"

#include "beam_frame_driver.h"
#include "common_rtl.h"
#include "mod_runtime.h"
#include "snes/interp_bridge.h"
#include "snes/snes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern CpuState g_cpu;

/* ── Lag reduction (the "Lag reduction" mod on the launcher's Mods page) ──
 *
 * The game slows down when its per-frame logic takes longer than one field
 * of 65816 time. The main loop ($01:B1xx) increments $0379 ("frame ready")
 * after the logic and the object tasks have run, then suspends until the next
 * NMI. The NMI tests $0379: zero means the logic is still running, so it skips
 * the frame's OAM/scroll commit and the game repeats a frame (slowdown).
 *
 * At the field boundary, before the NMI is delivered, a gameplay frame that
 * is not ready gets extra CPU time with the beam held: the logic runs to its
 * end "between" two fields, and the CPU clock is then rewound to where it
 * was. The NMI then finds the frame ready, the beam, raster IRQ and HDMA
 * splits stay on their real lines, and only the game's slowdown disappears.
 *
 * Gameplay is recognised by NMI being on and the HUD mode byte $032E being
 * set (the NMI handler uses it to put BG3 in HUD mode above the status-bar
 * split after line 40). Loading screens run with NMI off and the title, intro and map
 * screens leave $032E clear, so their timing (and the SPC upload handshakes)
 * is untouched. */

#define SGNG_READY_FLAG       0x0379u
#define SGNG_HUD_MODE         0x032Eu
#define SGNG_MASTER_PER_LINE  1364ull
#define SGNG_MASTER_PER_FIELD (262ull * SGNG_MASTER_PER_LINE)
/* At most this many extra fields of CPU time per frame. A frame needing more
 * still lags, by the remainder only. */
#define SGNG_LAG_BUDGET_FIELDS 3ull
/* Slice length while finishing a frame, so the loop notices the ready flag
 * soon after the main loop sets it. */
#define SGNG_LAG_SLICE_MASTER (16ull * SGNG_MASTER_PER_LINE)

static bool s_lag_removal;        /* set by the sgng.lag_reduction plugin */
static unsigned s_frames_finished;  /* lag frames removed (diagnostics) */

static void EnableLagReduction(void) { s_lag_removal = true; }
static void ResetEnhancements(void) { s_lag_removal = false; }

/* Features of the built-in "sgng.enhancements" package
 * (mods/preloaded/packages). The runtime runs the reset first on every
 * launch, then the plugins of the features the player enabled. */
SNES_MOD_CONSTRUCTOR(register_lag_reduction)
{
    snes_mod_register_reset_callback(ResetEnhancements);
    snes_mod_register_activation_plugin("sgng.lag_reduction", EnableLagReduction);
}

/* SGNG_LAG_REMOVAL=0/1 overrides the mod, for A/B runs. */
static bool LagReductionEnabled(void)
{
    static int env = -2;
    if (env == -2) {
        const char *v = getenv("SGNG_LAG_REMOVAL");
        env = (v && *v) ? (*v != '0') : -1;
        fprintf(stderr, "[sgng] lag reduction %s%s\n",
                (env >= 0 ? env : s_lag_removal) ? "on" : "off",
                env >= 0 ? " (SGNG_LAG_REMOVAL)" : "");
    }
    return env >= 0 ? env != 0 : s_lag_removal;
}

static int InGameplay(void)
{
    return g_snes->nmiEnabled && g_ram[SGNG_HUD_MODE] != 0;
}

static void FinishLaggedFrame(void)
{
    uint32_t pc;
    bool wai;

    if (g_ram[SGNG_READY_FLAG] || !InGameplay())
        return;
    snes_beam_frame_driver_get_position(&pc, &wai);
    if (!pc || wai)
        return;

    const uint64_t start = g_cpu.master_cycles;
    const uint64_t limit = start + SGNG_LAG_BUDGET_FIELDS * SGNG_MASTER_PER_FIELD;

    snes_beam_hold(1);
    while (!g_ram[SGNG_READY_FLAG] && g_cpu.master_cycles < limit) {
        uint64_t deadline = g_cpu.master_cycles + SGNG_LAG_SLICE_MASTER;
        if (deadline > limit) deadline = limit;
        interp_bridge_set_master_deadline(deadline);
        (void)interp_bridge_run_until_quiescent(&g_cpu, pc);
        interp_bridge_set_master_deadline(0);
        uint32_t resume = interp_bridge_lle_resume_pc();
        if (resume) pc = resume;
        if (interp_bridge_lle_took_wai()) {
            wai = true;
            break;
        }
    }
    snes_beam_hold(0);

    /* The extra time was never on the beam's timeline: give it back. */
    g_cpu.master_cycles = start;
    g_cpu.coprocessor_master_cycles = start;
    snes_beam_frame_driver_set_position(pc, wai);
    if (g_ram[SGNG_READY_FLAG])
        s_frames_finished++;
}

void GameRunOneFrame(void)
{
    snes_beam_frame_driver_run_frame();
    if (LagReductionEnabled())
        FinishLaggedFrame();

    /* SGNG_LAG_LOG=1: one line per field with the ready flag the NMI will
     * see (00 = a lag frame) and the running count of frames finished. */
    static int log = -1;
    if (log < 0) log = getenv("SGNG_LAG_LOG") != NULL;
    if (log)
        fprintf(stderr, "[sgng-lag] f=%d ready=%02X gameplay=%d finished=%u\n",
                snes_frame_counter, g_ram[SGNG_READY_FLAG], InGameplay(),
                s_frames_finished);
}

void GameDrawPpuFrame(void)
{
    SgngWsDrawPpuFrame();
}

void GameSessionReset(void)
{
    /* Rematch / soft-return: clear anything that must not survive a new
     * session. recomp-ai-rules/NETPLAY.md §3 — sticky state that "has always
     * been fine" is the usual desync culprit, because single-player never
     * re-enters the boot path twice in one process. */
    snes_beam_frame_driver_reset();
    SgngWsReset();
}

const RtlGameInfo kGameInfo = {
    .title = "superghoulsnghosts",
    .initialize = NULL,
    .run_frame = &GameRunOneFrame,
    .draw_ppu_frame = &GameDrawPpuFrame,
    .save_name_prefix = "save",
    .session_reset = &GameSessionReset,
    .hardware_reset = &GameSessionReset,
    .state_save_extra = &snes_beam_frame_driver_state_save,
    .state_load_extra = &snes_beam_frame_driver_state_load,
    .on_state_loaded = &snes_beam_frame_driver_state_loaded,
    .snapshot_guard_identity = &snes_beam_frame_driver_state_identity,
    .snapshot_preflight = &snes_beam_frame_driver_state_preflight,
};
