/*
 * Super Ghouls N Ghosts — widescreen enhancement (tier 1: backgrounds + HUD).
 *
 * The "Widescreen (16:9)" mod on the launcher's Mods page renders a
 * 342x224 field: the authentic 256 columns plus 43 per side, which is 16:9 at
 * the SNES's 4:3 display aspect. Presentation only -- the game, its camera,
 * object spawning and culling are untouched, so the 4:3 picture inside the
 * frame is the original one, and widescreen off draws exactly as before.
 *
 * What this ROM does, measured:
 *
 *   Gameplay is Mode 1. BG1 is a 64x64 map at $0000 (terrain), BG2 64x32 at
 *   $1000 (foreground props; the water on the ship stage), BG3 64x32 at
 *   $1800 (far parallax, half speed). The game streams them as whole map
 *   columns (VMAIN increment 32) and whole map rows (increment 1).
 *
 *   The NMI loads BG1 scroll from $19BD/$19C1, which the camera code fills
 *   from $15DD/$15E1: the world camera X/Y. BG2 and BG3 follow per-stage
 *   parallax instead.
 *
 *   Lines 1..40 are the status bar. An H-blank write after line 40 switches
 *   BGMODE $09 -> $01 and moves BG3 from the HUD map ($5C00) to the parallax
 *   map. So BG3 is split: on lines < 41 the PLAYER/lives, weapon and TIME
 *   groups are anchored to the frame edges and centre; from line 41 down BG3
 *   widens like the other layers.
 *
 * Margin content. The game only streams map cells a few pixels past the 4:3
 * edge, so the margins are built from two sources, per tile:
 *
 *   1. VRAM, where the cell is known to hold the tile the margin shows.
 *      Every map cell is tagged with the world tile it holds: the native view
 *      tags what it shows each frame, the game's uploads tag what they write
 *      (OnVramWrite), and a tag also records the entry, so a save-state load
 *      or rollback that changes VRAM behind the hook is not trusted.
 *   2. Otherwise the stage's own map (LevelTile), the same four-level data
 *      the game's column builder reads, which covers any position at once.
 *      Outside the stage (left of Stage 1's start) the margin stays empty.
 *
 *   A VRAM tile that the map does not have was drawn by the game at run time.
 *   It is extended only on rows where the native view shows such drawing too
 *   (Stage 1's heaving ground, the ship's water); a leftover the game parked
 *   off screen (the ship stage's solid water column) gives way to the map.
 *
 * BG2 goes through ws_shadow's world provider. BG1 and BG3 are drawn by a
 * line enhancer (ws_shadow serves only BG1/BG2, and with an enhancer the PPU
 * leaves BG1's margins to it): same sources, the PPU's own pixel and priority
 * values, the layer's hardware window, BG3 only below the status bar.
 *
 * Only live gameplay widens: NMI on and the HUD mode byte $032E set (the
 * test the lag-removal code uses), and every line of the previous field in
 * Mode 1. Title, intro, map and loading screens draw the native 256 columns
 * centred in the wide frame.
 *
 * Sprites are untouched: the game still culls objects at its native screen
 * edges, and draws the few it places just off screen into the margins.
 */

#include "game_ws.h"

#include "beam_frame_driver.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "display_aspect.h"
#include "mod_runtime.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/ws_shadow.h"
#include "widescreen.h"

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#define SGNG_HUD_MODE   0x032Eu  /* nonzero: gameplay HUD split active */
/* The camera clamp at $01:C699: BG1's camera X stays in [min, max]. The
 * minimum ratchets forward as the player advances; the maximum is the end
 * of the stage (Stage 1: $1300, the castle). */
#define SGNG_CAMERA_MIN_X 0x1A7Du
#define SGNG_CAMERA_MAX_X 0x19E6u
/* The gameplay test must fail this many fields in a row before the picture
 * goes back to 4:3, so a field with NMI briefly off or the HUD byte briefly
 * clear does not flash the margins black and snap the HUD to the middle. */
#define SGNG_WIDE_HOLD_FIELDS 10
#define SGNG_HUD_LINES  41       /* HUD on scanlines 1..40 */
/* Native x thresholds that split the status bar into its three groups:
 * PLAYER/score/lives [0,72), weapon box [72,184), TIME [184,256). The centre
 * group is wider than the weapon box because the gold armour's magic charge
 * bar flanks it: magic_bar_handling ($01:C8E4) writes it to HUD map columns
 * 9..14 and 17..22 (native x 72..183), so a narrower group tore it apart. */
#define SGNG_HUD_LEFT_END    72
#define SGNG_HUD_RIGHT_START 184

/* Each layer's scroll engine keeps its state in its own direct page:
 * camera X/Y (the layer's own pixel position in its level map) at +$3B/+$3F,
 * the offset of its screen table at +$42, and the ROM banks of the three
 * pointer levels below the screen table at +$46/+$49/+$4C. BG1's camera is
 * the world camera ($15DD/$15E1). */
static const uint16_t kLayerDp[3] = {0x15A2, 0x16F8, 0x184E};
#define LDP_CAMERA_X   0x3B
#define LDP_CAMERA_Y   0x3F
#define LDP_SCREENS    0x42
#define LDP_BANK_SCREEN 0x46
#define LDP_BANK_BLOCK  0x49
#define LDP_BANK_META   0x4C
/* Screen table: one 16-bit ROM pointer per 256x256 screen, 32 per row. */
#define SGNG_SCREEN_TABLE 0xF700u   /* $7E:F700 */
#define SGNG_SCREEN_ROWS  12        /* per layer ($300 bytes) */
#define SGNG_SCREEN_EMPTY 0x8000u   /* unused slots */

enum {
    kLayers = 3,           /* BG1, BG2, BG3 (below the HUD) */
    kShadowLayers = 2,     /* ws_shadow serves BG1/BG2 */
    kMapCells = 64 * 64,
};

typedef struct LayerTags {
    /* World tile each map cell holds; tag_valid says the pair is meaningful.
     * tag_entry is the map entry the tag was taken from: VRAM can change
     * without a write this file sees (a save-state load, rewind, run-ahead's
     * rollback), and a cell that no longer holds that entry is not trusted. */
    int32_t tag_x[kMapCells], tag_y[kMapCells];
    uint16_t tag_entry[kMapCells];
    uint8_t tag_valid[kMapCells];
    /* Written while the layer had no view yet (a stage loading): placed
     * around the first view once there is one. */
    uint8_t tag_pending[kMapCells];
    bool any_pending;
    /* The game's own window: which world column each map column, and which
     * world row each map row, was last loaded for. */
    int32_t col_world[64], row_world[64];
    uint8_t col_set[64], row_set[64];
    /* Map layout, from the PPU registers the field renders with. */
    uint16_t map_base;         /* word address */
    uint8_t cols, rows;        /* 32 or 64 */
    uint8_t shift;             /* 3: 8x8 tiles, 4: 16x16 */
    /* Unwrapped world origin of the native view, in pixels. */
    int32_t world_x, world_y;
    uint16_t last_h, last_v;   /* the 10-bit scroll registers */
    bool have_last;
    /* Rows of the native view where the game shows tiles that differ from
     * the stage's map (terrain or water it draws at run time), by world row
     * - dyn_row0. */
    int32_t dyn_row0;
    uint8_t dyn_rows[40];
    uint16_t blank_entry;      /* map entry of an all-transparent tile */
    bool have_blank;
} LayerTags;

/* The registers a layer's playfield renders with. */
typedef struct LayerRegs {
    uint16_t map_base;
    uint8_t cols, rows, shift;
    uint16_t h, v;
    bool valid;
} LayerRegs;

static LayerTags s_layers[kLayers];
static uint16_t LayerCamX(int i);
static uint16_t LayerCamY(int i);
static bool LevelTile(int i, int32_t px, int32_t py, uint16_t *entry);
static bool s_enabled;  /* set by the sgng.widescreen plugin */
/* BG3 below the split, as the line observer saw it on the last field. */
static LayerRegs s_bg3_seen;
static bool s_hook_installed;
static bool s_was_wide;
static int s_gate_misses;

/* Set by the line observer: some line of the field just drawn was not Mode 1.
 * Read on the next field. */
static bool s_unsupported_mode;

static LayerRegs RegsFromPpu(const Ppu *ppu, int i)
{
    LayerRegs r;
    r.map_base = (uint16_t)PPU_bgTilemapAdr(ppu, i);
    r.cols = PPU_bgTilemapWider(ppu, i) ? 64 : 32;
    r.rows = PPU_bgTilemapHigher(ppu, i) ? 64 : 32;
    r.shift = PPU_bigTiles(ppu, i) ? 4 : 3;
    r.h = ppu->hScroll[i] & 0x3ff;
    r.v = ppu->vScroll[i] & 0x3ff;
    r.valid = true;
    return r;
}

static void ObserveLine(const Ppu *ppu, unsigned line, void *context)
{
    (void)context;
    if ((ppu->bgmode & 7) != 1)
        s_unsupported_mode = true;
    /* BG3 is the HUD map until the H-blank split, so the registers before
     * the field show that; the playfield's are taken here, below it. The
     * split clears BGMODE's BG3-priority bit ($09 -> $01). It lands late on
     * a lag field, so wait for it rather than trusting line 41. */
    if (line >= SGNG_HUD_LINES && !s_bg3_seen.valid && !(ppu->bgmode & 8))
        s_bg3_seen = RegsFromPpu(ppu, 2);
}

static bool InGameplay(void)
{
    return g_snes->nmiEnabled && g_ram[SGNG_HUD_MODE] != 0;
}

static uint16_t Ram16(uint16_t addr)
{
    return (uint16_t)(g_ram[addr] | g_ram[addr + 1] << 8);
}

static int Mod(int v, int m)
{
    v %= m;
    return v < 0 ? v + m : v;
}

/* First scanline a layer's tiles describe this map: BG3 above the split is
 * the HUD, a different map. */
static int FirstLine(int layer)
{
    return layer == 2 ? SGNG_HUD_LINES : 1;
}

/* Map cell index for a world tile. */
static int CellFor(const LayerTags *t, int32_t wx, int32_t wy)
{
    return Mod(wy, t->rows) * 64 + Mod(wx, t->cols);
}

/* The map entry in VRAM for a cell, in the PPU's 32x32-screen layout. */
static uint16_t CellEntry(const LayerTags *t, int c)
{
    const int col = c & 63, row = c >> 6;
    const int screen = (col >= 32 ? 1 : 0) + (row >= 32 ? (t->cols == 64 ? 2 : 1) : 0);
    const uint16_t adr =
        (uint16_t)(t->map_base + screen * 0x400 + (row & 31) * 32 + (col & 31));
    return g_ppu->vram[adr & 0x7fff];
}

static void Tag(LayerTags *t, int c, int32_t wx, int32_t wy)
{
    t->tag_x[c] = wx;
    t->tag_y[c] = wy;
    t->tag_entry[c] = CellEntry(t, c);
    t->tag_valid[c] = 1;
}

static bool CellHolds(const LayerTags *t, int32_t wx, int32_t wy)
{
    const int c = CellFor(t, wx, wy);
    return t->tag_valid[c] && t->tag_x[c] == wx && t->tag_y[c] == wy &&
           t->tag_entry[c] == CellEntry(t, c);
}

/* The congruent value of `cell` (mod `period`) nearest to `centre`. */
static int32_t Nearest(int cell, int period, int32_t centre)
{
    int32_t base = centre - Mod(centre, period) + cell;
    if (base - centre > period / 2) base -= period;
    else if (centre - base > period / 2) base += period;
    return base;
}

static void ForgetLayer(LayerTags *t)
{
    memset(t->tag_valid, 0, sizeof(t->tag_valid));
    memset(t->tag_pending, 0, sizeof(t->tag_pending));
    t->any_pending = false;
    memset(t->col_set, 0, sizeof(t->col_set));
    memset(t->row_set, 0, sizeof(t->row_set));
    t->have_last = false;
}

/* ── Game writes: tag the cell with the world tile it was written for ─────
 *
 * The game keeps a window of the world in the map: each map column holds one
 * world column, each map row one world row. It moves the window with column
 * uploads when it scrolls sideways and with row uploads when it scrolls
 * vertically, each spanning the whole map. A column upload lands at the
 * leading edge, so its world column is the congruent one nearest the view
 * centre, and each of its cells belongs to the world row its map row holds.
 * A row upload is the transpose. It has to be: a row written for any other
 * world column would be wrong as soon as the camera moved onto it. Taking
 * the far coordinate from the window rather than from the view centre keeps
 * a row upload's cells that lie outside the window -- they hold world
 * columns a map width away -- from passing for margin content. */
static int32_t WindowCol(const LayerTags *t, int col, int32_t cx)
{
    return t->col_set[col] ? t->col_world[col] : Nearest(col, t->cols, cx);
}

static int32_t WindowRow(const LayerTags *t, int row, int32_t cy)
{
    return t->row_set[row] ? t->row_world[row] : Nearest(row, t->rows, cy);
}

static void OnVramWrite(uint32_t byte_addr, uint8_t value)
{
    (void)value;
    /* Both halves of a word: the hook runs after each byte lands, so the
     * tag's entry is always the word VRAM now holds. */
    const uint16_t word = (uint16_t)(byte_addr >> 1);
    const bool column_upload = g_ppu->vramIncrement == 32;
    for (int i = 0; i < kLayers; i++) {
        LayerTags *t = &s_layers[i];
        if (!t->cols) continue;
        const uint16_t off = (uint16_t)(word - t->map_base);
        if (off >= (uint16_t)(t->cols * t->rows)) continue;
        /* VRAM layout: 32x32 screens, left to right, then top to bottom. */
        const int screen = off >> 10, within = off & 0x3ff;
        const int col = (within & 31) + ((t->cols == 64 && (screen & 1)) ? 32 : 0);
        const int row = (within >> 5) + ((t->cols == 64 ? screen >> 1 : screen) ? 32 : 0);
        /* The layer's live camera, which the game moves before it streams
         * (and before the scroll registers follow). Before the first view
         * of a stage there is nothing to place it against yet: a load. */
        if (!t->have_last) {
            t->tag_pending[row * 64 + col] = 1;
            t->tag_valid[row * 64 + col] = 0;
            t->any_pending = true;
            continue;
        }
        const int32_t vx = LayerCamX(i), vy = LayerCamY(i);
        const int sh = t->shift;
        const int32_t cx = (vx + 128) >> sh, cy = (vy + 112) >> sh;
        int32_t wx, wy;
        if (column_upload) {
            wx = Nearest(col, t->cols, cx);
            t->col_world[col] = wx;
            t->col_set[col] = 1;
            wy = WindowRow(t, row, cy);
        } else {
            wy = Nearest(row, t->rows, cy);
            t->row_world[row] = wy;
            t->row_set[row] = 1;
            wx = WindowCol(t, col, cx);
        }
        Tag(t, row * 64 + col, wx, wy);
    }
}

/* ── Per-frame geometry ───────────────────────────────────────────────── */

/* Map layout as the PPU is configured now. The write hook needs it outside
 * gameplay too, while a stage loads. A layout change forgets everything. */
static void UpdateGeometry(int i, const LayerRegs *r)
{
    LayerTags *t = &s_layers[i];
    if (r->map_base != t->map_base || r->cols != t->cols || r->rows != t->rows ||
        r->shift != t->shift)
        ForgetLayer(t);
    t->map_base = r->map_base;
    t->cols = r->cols;
    t->rows = r->rows;
    t->shift = r->shift;
}

/* Scroll registers are 10 bits; recover the high bits from the camera. */
static int32_t FromCamera(uint16_t reg, uint16_t camera)
{
    int32_t v = (int32_t)(camera & ~0x3ffu) | (reg & 0x3ff);
    if (v - (int32_t)camera > 0x200) v -= 0x400;
    else if ((int32_t)camera - v > 0x200) v += 0x400;
    return v;
}

static void FindBlankTile(LayerTags *t, const Ppu *ppu, int layer)
{
    const uint16_t base = (uint16_t)PPU_bgTileAdr(ppu, layer);
    for (int n = 0; n < 1024; n++) {
        bool zero = true;
        /* 4bpp: 16 words a tile; a 16x16 tile is n, n+1, n+16, n+17. */
        const int parts = t->shift == 4 ? 4 : 1;
        static const int kSub[4] = {0, 1, 16, 17};
        for (int s = 0; s < parts && zero; s++) {
            const uint16_t *p = &ppu->vram[(base + (n + kSub[s]) * 16) & 0x7fff];
            for (int k = 0; k < 16 && zero; k++) zero = p[k] == 0;
        }
        if (zero) {
            t->blank_entry = (uint16_t)n;
            t->have_blank = true;
            return;
        }
    }
    t->have_blank = false;
}

static void UpdateView(int i, const LayerRegs *r)
{
    LayerTags *t = &s_layers[i];
    const uint16_t h = r->h, v = r->v;

    /* Phase from the scroll register the field renders with, the high bits
     * from the layer's camera (which can lead the register by a frame). */
    t->world_x = FromCamera(h, LayerCamX(i));
    t->world_y = FromCamera(v, LayerCamY(i));
    t->last_h = h;
    t->last_v = v;
    t->have_last = true;

    /* A stage load writes the window around the camera it starts at. */
    if (t->any_pending) {
        const int sh0 = t->shift;
        const int32_t cx = (t->world_x + 128) >> sh0, cy = (t->world_y + 112) >> sh0;
        for (int c = 0; c < kMapCells; c++) {
            if (!t->tag_pending[c]) continue;
            const int col = c & 63, row = c >> 6;
            Tag(t, c, WindowCol(t, col, cx), WindowRow(t, row, cy));
            t->tag_pending[c] = 0;
        }
        t->any_pending = false;
    }

    /* The native view is the truth: tag every cell it shows. */
    const int sh = t->shift;
    const int32_t x0 = t->world_x >> sh, x1 = (t->world_x + 255) >> sh;
    const int32_t y0 = (t->world_y + FirstLine(i)) >> sh, y1 = (t->world_y + 224) >> sh;
    for (int32_t wx = x0; wx <= x1; wx++) {
        t->col_world[Mod(wx, t->cols)] = wx;
        t->col_set[Mod(wx, t->cols)] = 1;
    }
    for (int32_t wy = y0; wy <= y1; wy++) {
        t->row_world[Mod(wy, t->rows)] = wy;
        t->row_set[Mod(wy, t->rows)] = 1;
    }
    for (int32_t wy = y0; wy <= y1; wy++)
        for (int32_t wx = x0; wx <= x1; wx++)
            Tag(t, CellFor(t, wx, wy), wx, wy);

    /* Which native rows show something the stage's map does not. */
    t->dyn_row0 = y0;
    memset(t->dyn_rows, 0, sizeof(t->dyn_rows));
    for (int32_t wy = y0; wy <= y1 && wy - y0 < (int32_t)sizeof(t->dyn_rows); wy++)
        for (int32_t wx = x0; wx <= x1; wx++) {
            uint16_t level;
            if (LevelTile(i, wx << sh, wy << sh, &level) &&
                level != CellEntry(t, CellFor(t, wx, wy))) {
                t->dyn_rows[wy - y0] = 1;
                break;
            }
        }
}

/* A margin tile from VRAM that the stage's map does not have is something
 * the game drew at run time. Extend it only where the same row shows such
 * drawing on screen (Stage 1's heaving ground, the ship's water): elsewhere
 * it is a leftover the game parked off screen, and the map is right. */
static bool RowDrawnAtRunTime(const LayerTags *t, int32_t wy)
{
    const int32_t r = wy - t->dyn_row0;
    return r >= 0 && r < (int32_t)sizeof(t->dyn_rows) && t->dyn_rows[r];
}

/* ── Level data: any tile of a layer, from the game's own map ─────────────
 *
 * The game builds every column and row it streams ($01:C38A) by walking a
 * four-level map, and the same walk gives the tile at any position:
 *
 *   $7E:F700 + layer offset   screen table, one ROM pointer per 256x256 px
 *                             screen, $40 bytes per screen row
 *   screen   (bank at +$46)   8x8 blocks of 32 px, $10 bytes per block row
 *   block    (bank at +$49)   2x2 metatiles of 16 px
 *   metatile (bank at +$4C)   2x2 tilemap entries of 8 px
 *
 * Checked against every native tile of all three layers on Stage 1 and the
 * ship stage: identical, except where the game draws terrain into the map
 * at run time (Stage 1's heaving ground), which the native view and the
 * game's own uploads still supply. */

static uint16_t LayerCamX(int i) { return Ram16((uint16_t)(kLayerDp[i] + LDP_CAMERA_X)); }
static uint16_t LayerCamY(int i) { return Ram16((uint16_t)(kLayerDp[i] + LDP_CAMERA_Y)); }

static uint16_t RomWord(uint8_t bank, uint16_t addr)
{
    const uint8_t *p = RomPtr(((uint32_t)bank << 16) | addr);
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint16_t ScreenPointer(uint16_t table, int sx, int sy)
{
    /* Bank $7E is g_ram[0x0000-0xFFFF]. */
    const uint32_t off = (SGNG_SCREEN_TABLE + table + (uint32_t)sy * 0x40 + (uint32_t)sx * 2) & 0xFFFFu;
    return (uint16_t)(g_ram[off] | g_ram[off + 1] << 8);
}

typedef struct LevelMap {
    uint16_t table;              /* screen-table offset */
    uint8_t bank_screen, bank_block, bank_meta;
    int32_t width_px, height_px; /* extent of the screens the stage uses */
} LevelMap;

static LevelMap s_maps[kLayers];

/* The stage's extent: unused screen slots hold $8000. Screen 0 is always
 * inside, so a stage whose first screen is $8000 (Stage 1's is) still
 * measures right. */
static void UpdateLevelMap(int i)
{
    LevelMap *m = &s_maps[i];
    const uint16_t dp = kLayerDp[i];
    m->table = Ram16((uint16_t)(dp + LDP_SCREENS));
    m->bank_screen = g_ram[dp + LDP_BANK_SCREEN];
    m->bank_block = g_ram[dp + LDP_BANK_BLOCK];
    m->bank_meta = g_ram[dp + LDP_BANK_META];
    int cols = 1, rows = 1;
    for (int sy = 0; sy < SGNG_SCREEN_ROWS; sy++)
        for (int sx = 0; sx < 32; sx++)
            if (ScreenPointer(m->table, sx, sy) != SGNG_SCREEN_EMPTY) {
                if (sx + 1 > cols) cols = sx + 1;
                if (sy + 1 > rows) rows = sy + 1;
            }
    m->width_px = cols * 256;
    m->height_px = rows * 256;
}

/* The tilemap entry at layer pixel (px, py); false outside the stage. */
static bool LevelTile(int i, int32_t px, int32_t py, uint16_t *entry)
{
    const LevelMap *m = &s_maps[i];
    if (px < 0 || py < 0 || px >= m->width_px || py >= m->height_px)
        return false;
    const uint16_t screen = ScreenPointer(m->table, px >> 8, py >> 8) | 0x8000;
    const uint16_t block = RomWord(m->bank_screen,
        (uint16_t)(screen + ((py >> 5) & 7) * 0x10 + ((px >> 5) & 7) * 2)) | 0x8000;
    const uint16_t meta = RomWord(m->bank_block,
        (uint16_t)(block + ((py >> 4) & 1) * 4 + ((px >> 4) & 1) * 2)) | 0x8000;
    *entry = RomWord(m->bank_meta,
        (uint16_t)(meta + ((py >> 3) & 1) * 4 + ((px >> 3) & 1) * 2));
    return true;
}

/* ── BG1/BG2: per-tile check through ws_shadow's world provider ────────── */

static WsShadowSource Provide(void *context, uint32_t worldTileX,
                              uint32_t worldTileY, uint16_t *entry)
{
    const LayerTags *t = (const LayerTags *)context;
    const int32_t wx = (int32_t)worldTileX, wy = (int32_t)worldTileY;
    /* What the game put in VRAM for this spot, when it is known to be that;
     * else the stage's map; else (outside the stage) nothing. */
    uint16_t level;
    const bool have_level =
        t->shift == 3 && LevelTile((int)(t - s_layers), wx << 3, wy << 3, &level);
    if (CellHolds(t, wx, wy)) {
        *entry = CellEntry(t, CellFor(t, wx, wy));
        if (have_level && *entry != level && !RowDrawnAtRunTime(t, wy))
            *entry = level;
        return kWsShadowMarginOverride;
    }
    if (have_level) {
        *entry = level;
        return kWsShadowMarginOverride;
    }
    *entry = t->blank_entry;
    return kWsShadowMarginOverride;
}

static void ApplyShadowLayer(int i)
{
    LayerTags *t = &s_layers[i];
    FindBlankTile(t, g_ppu, i);
    if (!t->have_blank) {
        /* No transparent tile to substitute: keep this layer native. */
        WsShadowUseVram(i);
        PpuSetWidescreenLayerClamp(g_ppu, (uint8_t)(g_ppu->wsLayerClamp | 1u << i));
        return;
    }
    WsShadowSetWorld(i, (uint32_t)t->world_x, (uint32_t)t->world_y);
    WsShadowSetScroll(i, t->last_h, t->last_v);
    WsShadowSetWorldProvider(i, Provide, t, false);
    WsShadowSetBlankTile(i, t->blank_entry);
}

/* ── BG1 and BG3: margins drawn by a line enhancer ──────────────────────
 *
 * ws_shadow serves only BG1/BG2, and once a line enhancer is installed the
 * PPU leaves BG1's margins to it as well. So BG2 keeps the provider above,
 * and the enhancer draws the margins of BG1 (every line) and BG3 (below the
 * status bar; BG3's own margins are clamped off): for each margin tile, the
 * VRAM entry where it is known to be that tile, else the stage's map, else
 * nothing. Pixels go into the PPU's priority buffer with its own values and
 * rule, after it has drawn the line, so they merge exactly as its would. */

/* The layer's hardware window at margin column x, taken at the nearest
 * native column (an edge pinned to 0 or 255 reaches the frame edge). */
static bool WindowHides(const Ppu *ppu, unsigned layer, bool sub, int x)
{
    if (!(ppu->screenWindowed[sub] & (1u << layer)))
        return false;
    const int cx = x < 0 ? 0 : x > 255 ? 255 : x;
    const unsigned sel = (ppu->windowsel >> (layer * 4)) & 0xF;
    const bool en1 = sel & 2, en2 = sel & 8;
    if (!en1 && !en2)
        return false;
    bool w1 = cx >= ppu->window1left && cx <= ppu->window1right;
    bool w2 = cx >= ppu->window2left && cx <= ppu->window2right;
    if (sel & 1) w1 = !w1;
    if (sel & 4) w2 = !w2;
    if (!en2) return w1;
    if (!en1) return w2;
    switch ((ppu->wbgobjlog >> (layer * 2)) & 3) {
    case 0: return w1 || w2;
    case 1: return w1 && w2;
    case 2: return w1 != w2;
    default: return w1 == w2;
    }
}

/* ── BG1's WRAM mirror ─────────────────────────────────────────────────
 *
 * The column builder ($01:C45A) also writes every BG1 column it builds to
 * a WRAM ring at $7E:B000: four blocks of 32 world columns ($800 bytes
 * each, block = world column / 32 mod 4), 32 rows of $40 bytes. It builds
 * about 80 columns ahead of the camera, so the ring holds world columns
 * from about 47 behind the camera to 80 ahead of it. That covers both margins.
 *
 * Stage 1's heaving ground is drawn into this ring, ahead of the view as
 * well as in it, and the whole top half of the map is copied from it to
 * VRAM every few frames as one row upload. Those rows reach map columns
 * the stream has not moved to yet, so VRAM there holds terrain for world
 * columns the tags cannot place, and the stage's map has the flat ground.
 * The ring has the right tiles.
 *
 * A taller stage uses another layout (the builder's $D000 path). So the
 * ring is used only on a stage one screen high, and only while it agrees
 * with what the native view shows. */
#define SGNG_BG1_MIRROR      0xB000u
#define SGNG_MIRROR_BEHIND   40   /* columns behind the view's left edge */
#define SGNG_MIRROR_AHEAD    72   /* columns past it */
static bool s_mirror_ok;

static bool MirrorTile(int32_t wx, int32_t wy, uint16_t *entry)
{
    const int32_t c0 = s_layers[0].world_x >> 3;
    if (!s_mirror_ok || wx < 0 || wy < 0 || wy >= 32 ||
        wx < c0 - SGNG_MIRROR_BEHIND || wx > c0 + SGNG_MIRROR_AHEAD)
        return false;
    const uint32_t off = SGNG_BG1_MIRROR + (uint32_t)((wx >> 5) & 3) * 0x800u +
                         (uint32_t)wy * 0x40u + (uint32_t)(wx & 31) * 2u;
    *entry = (uint16_t)(g_ram[off] | g_ram[off + 1] << 8);
    return true;
}

/* The ring is trusted while nearly every BG1 cell the native view shows
 * matches it. VRAM can trail the ring by a few frames while the ground
 * heaves, so the match need not be exact. */
static void CheckMirror(void)
{
    const LayerTags *t = &s_layers[0];
    s_mirror_ok = false;
    if (!t->have_last || t->shift != 3 || t->cols != 64 || s_maps[0].height_px > 256)
        return;
    const int32_t x0 = t->world_x >> 3, y0 = (t->world_y + 1) >> 3;
    const int32_t y1 = (t->world_y + 224) >> 3;
    if (x0 < 0 || y0 < 0 || y1 >= 32)
        return;
    s_mirror_ok = true;  /* so MirrorTile reads while measuring */
    int cells = 0, same = 0;
    for (int32_t wy = y0; wy <= y1; wy++)
        for (int32_t wx = x0; wx <= x0 + 32; wx++) {
            uint16_t m;
            MirrorTile(wx, wy, &m);
            cells++;
            same += m == CellEntry(t, CellFor(t, wx, wy));
        }
    s_mirror_ok = same * 16 >= cells * 15;
}

/* Margin tile source: BG1's mirror where it holds the tile, else verified
 * VRAM, else the stage's map. */
static bool MarginTile(int i, int32_t px, int32_t py, uint16_t *entry)
{
    const LayerTags *t = &s_layers[i];
    const int32_t wx = px >> 3, wy = py >> 3;
    if (i == 0 && MirrorTile(wx, wy, entry))
        return true;
    uint16_t level;
    const bool have_level = LevelTile(i, px, py, &level);
    if (px >= 0 && py >= 0 && CellHolds(t, wx, wy)) {
        *entry = CellEntry(t, CellFor(t, wx, wy));
        if (have_level && *entry != level && !RowDrawnAtRunTime(t, wy))
            *entry = level;
        return true;
    }
    *entry = level;
    return have_level;
}

static void DrawMarginLayer(Ppu *ppu, unsigned y, bool sub, int i, int bpp,
                            PpuZbufType zhi, PpuZbufType zlo)
{
    const int h = ppu->hScroll[i] & 0x3ff, v = ppu->vScroll[i] & 0x3ff;
    const int32_t px0 = FromCamera((uint16_t)h, LayerCamX(i));
    const int32_t py = FromCamera((uint16_t)v, LayerCamY(i)) + (int32_t)y;
    const uint16_t tile_base = (uint16_t)PPU_bgTileAdr(ppu, i);
    const int words = bpp == 4 ? 16 : 8, pal_shift = bpp == 4 ? 6 : 8;
    PpuZbufType *data = ppu->bgBuffers[sub].data;
    uint16_t tile = 0, lo = 0, hi = 0;
    bool have = false;
    int32_t cur = INT32_MIN;

    for (int x = -(int)ppu->extraLeftCur; x < 256 + (int)ppu->extraRightCur; x++) {
        if (x == 0)
            x = 256;  /* the native columns are the PPU's */
        if (x >= 256 + (int)ppu->extraRightCur)
            break;
        const int32_t px = px0 + x;
        if ((px >> 3) != cur) {
            cur = px >> 3;
            have = MarginTile(i, px, py, &tile);
            if (have) {
                const int row = (tile & 0x8000) ? 7 - (py & 7) : (py & 7);
                const uint16_t at = (uint16_t)(tile_base + (tile & 0x3ff) * words + row);
                lo = ppu->vram[at & 0x7fff];
                hi = bpp == 4 ? ppu->vram[(at + 8) & 0x7fff] : 0;
            }
        }
        if (!have || WindowHides(ppu, (unsigned)i, sub, x))
            continue;
        const int bit = (tile & 0x4000) ? (px & 7) : 7 - (px & 7);
        int pixel = ((lo >> bit) & 1) | (((lo >> (8 + bit)) & 1) << 1);
        if (bpp == 4)
            pixel |= (((hi >> bit) & 1) << 2) | (((hi >> (8 + bit)) & 1) << 3);
        if (!pixel)
            continue;
        PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
        z += (PpuZbufType)((tile & 0x1c00) >> pal_shift);
        PpuZbufType *dst = &data[x + kPpuLineBorder];
        if (z > *dst)
            *dst = (PpuZbufType)(z + pixel);
    }
}

static void DrawMargins(Ppu *ppu, uint y, bool sub, void *context)
{
    (void)context;
    if ((ppu->bgmode & 7) != 1 || (ppu->mosaic & 0xF0))
        return;
    /* Priority values as ppu.c's PpuDrawBackgrounds draws Mode 1. */
    if ((ppu->screenEnabled[sub] & 1) &&
        (uint16_t)PPU_bgTilemapAdr(ppu, 0) == s_layers[0].map_base)
        DrawMarginLayer(ppu, y, sub, 0, 4, 0xc000, 0x8000);
    /* BG3 below the status-bar split, while it shows the playfield map. */
    if ((int)y >= SGNG_HUD_LINES && !(ppu->bgmode & 8) &&
        (ppu->screenEnabled[sub] & 4) &&
        (uint16_t)PPU_bgTilemapAdr(ppu, 2) == s_layers[2].map_base)
        DrawMarginLayer(ppu, y, sub, 2, 2, 0x3200, 0x1200);
}

/* ── Mod registration ─────────────────────────────────────────────────── */

static void EnableWidescreen(void) { s_enabled = true; }
static void ResetWidescreen(void) { s_enabled = false; }

/* Feature "widescreen" of the built-in "sgng.enhancements" package
 * (mods/preloaded/packages). It only changes what is drawn, but it is an
 * ordinary activation plugin, not a presentation-only one: as with every
 * widescreen in the framework, netplay peers are meant to agree on the
 * picture geometry (see presentation_only in snesrecomp's mod_runtime.cpp).
 * The reset runs on every launch, so the mod, not [Graphics] Widescreen in
 * config.ini, decides. */
SNES_MOD_CONSTRUCTOR(register_widescreen)
{
    snes_mod_register_reset_callback(ResetWidescreen);
    snes_mod_register_activation_plugin("sgng.widescreen", EnableWidescreen);
}

/* ── View bounds: the wide picture stays where the original camera can go ─
 *
 * The extra columns go where the game's camera could actually take the 4:3
 * view: at a camera limit (the start of a stage, the castle at its end, the
 * point it will not scroll back past) they all go to the open side, so the
 * picture slides against the limit instead of showing what lies beyond it.
 * Mid-stage both sides get the full margin and nothing moves. The PPU's view
 * anchoring places the frame; the camera itself is untouched. Measured from
 * the rendered scroll (BG1's world X), so the frame origin never runs a
 * field ahead of the picture. */
static void ApplyViewBounds(void)
{
    const int e = g_ws_extra;
    int left = e, right = e;
    const int32_t cam = s_layers[0].world_x;
    const int32_t lo = Ram16(SGNG_CAMERA_MIN_X), hi = Ram16(SGNG_CAMERA_MAX_X);
    if (s_layers[0].have_last && hi >= lo) {
        const int32_t room_left = cam - lo, room_right = hi - cam;
        left = room_left < 0 ? 0 : room_left > e ? e : (int)room_left;
        right = room_right < 0 ? 0 : room_right > 2 * e - left ? 2 * e - left : (int)room_right;
        /* A short right side hands its unused columns back to the left. */
        if (left + right < 2 * e)
            left = room_left > 2 * e - right ? 2 * e - right : (int)(room_left < 0 ? 0 : room_left);
    }
    PpuSetWidescreenViewAnchor(g_ppu, true);
    PpuSetExtraSideSpace(g_ppu, left, right, 0);
}

/* ── Host hooks ───────────────────────────────────────────────────────── */

void SgngWsReset(void)
{
    for (int i = 0; i < kLayers; i++)
        ForgetLayer(&s_layers[i]);
    s_was_wide = false;
    s_gate_misses = 0;
    s_bg3_seen.valid = false;
}

void SgngWsPrepareFrame(int drawable_w, int drawable_h, int *frame_w, int *frame_h)
{
    (void)drawable_w;
    (void)drawable_h;
    /* SNESRECOMP_WS_EXTRA pins the margin for probes and CI (0 forces 4:3). */
    int pinned = PpuWsExtraOverride();
    if (pinned >= 0) {
        *frame_w = 256 + 2 * pinned;
    } else {
        *frame_w = s_enabled ? SnesDisplayAspect_ComputeWideFrameWidth(256) : 256;
    }
    *frame_h = 224;
}

void SgngWsDrawPpuFrame(void)
{
    if (!g_ws_active) {
        snes_beam_frame_driver_draw_ppu_frame();
        return;
    }
    if (!s_hook_installed) {
        ppu_set_vram_write_log_hook(OnVramWrite);
        s_hook_installed = true;
    }

    LayerRegs regs[kLayers] = {RegsFromPpu(g_ppu, 0), RegsFromPpu(g_ppu, 1), s_bg3_seen};
    for (int i = 0; i < kLayers; i++)
        if (regs[i].valid)
            UpdateGeometry(i, &regs[i]);

    const bool gate = InGameplay() && !s_unsupported_mode;
    s_gate_misses = gate ? 0 : s_gate_misses + 1;
    const bool wide = gate || (s_was_wide && s_gate_misses < SGNG_WIDE_HOLD_FIELDS);
    if (wide) {
        for (int i = 0; i < kLayers; i++)
            UpdateLevelMap(i);
        for (int i = 0; i < kLayers; i++)
            if (regs[i].valid)
                UpdateView(i, &regs[i]);
        CheckMirror();
        ApplyViewBounds();
        PpuSetWidescreenHudSplit(g_ppu, SGNG_HUD_LINES, SGNG_HUD_LEFT_END,
                                 SGNG_HUD_RIGHT_START);
        /* The status bar stays at the frame edges whatever the margins do
         * and whatever window the game puts on BG3 in the HUD band. */
        PpuSetWidescreenHudAlwaysVisible(g_ppu, true);
        PpuSetWidescreenBg3Widen(g_ppu, SGNG_HUD_LINES);
        ApplyShadowLayer(1);  /* BG2 */
        WsShadowUseVram(0);   /* BG1: the enhancer draws its margins */
        WsShadowFrame(g_ppu);
        PpuSetWidescreenLayerClampBand(g_ppu, 2, SGNG_HUD_LINES, 225);
        PpuSetWidescreenLineEnhancer(g_ppu, DrawMargins, NULL);
    } else {
        PpuSetWidescreenViewAnchor(g_ppu, false);
        PpuSetExtraSideSpace(g_ppu, 0, 0, 0);  /* centred pillarbox */
        PpuSetWidescreenLineEnhancer(g_ppu, NULL, NULL);
        for (int i = 0; i < kShadowLayers; i++)
            WsShadowUseVram(i);
        WsShadowFrame(g_ppu);
        /* Leaving gameplay (death, stage change): what the map held is no
         * longer known to belong anywhere. Uploads from here on re-tag it. */
        if (s_was_wide)
            for (int i = 0; i < kLayers; i++)
                ForgetLayer(&s_layers[i]);
    }
    s_was_wide = wide;

    s_unsupported_mode = false;
    s_bg3_seen.valid = false;
    snes_beam_frame_driver_draw_ppu_frame_observed(ObserveLine, NULL);
}
