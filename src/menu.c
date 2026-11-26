/*
 * Copyright (C) 2024 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "compiler.h"
#include "gbahw.h"
#include "patchengine.h"
#include "fatfs/ff.h"
#include "common.h"
#include "settings.h"
#include "util.h"
#include "utf_util.h"
#include "fonts/font_render.h"
#include "nanoprintf.h"
#include "messages.h"
#include "save.h"
#include "cheats.h"
#include "ingame.h"
#include "emu.h"
#include "recent.h"
#include "flash_mgr.h"
#include "flash.h"
#include "sha256.h"
#include "supercard_driver.h"

#include "res/icons.h"
#include "res/logo.h"

extern t_card_info sd_info;
extern bool fastew;
extern bool slowsd;

enum {
  MENUTAB_RECENT,        // Browses recently loaded ROMs (can be disabled / hidden)
  MENUTAB_ROMBROWSE,     // Browses ROMs and launches games.
  #ifdef SUPPORT_NORGAMES
  MENUTAB_NORBROWSE,     // Browses Flash games and launches them.
  #endif
  MENUTAB_SETTINGS,      // Unused as a tab (settings live in a pop-up), kept for icon numbering
  MENUTAB_UILANG,        // Unused (merged into settings), kept for icon numbering
  MENUTAB_TOOLS,         // Tools (advaned menu)
  MENUTAB_INFO,          // Info / About / Updater?
  MENUTAB_MAX,
};

#define ANIM_INITIAL_WAIT     128    // Intial wait (in anim cycles)

#define KEY_REPEAT_INITIAL    384    // Initial wait for key repeat
#define KEY_REPEAT_MID        160    // Next key presses
#define KEY_REPEAT_FAST        96    // End speed
#define KEY_REPEAT_CNT1         5

enum {
  POPUP_NONE,
  POPUP_GBA_LOAD,              // Load a GBA ROM
  POPUP_SAVFILE,               // Load/Store a SAV file
  POPUP_FWFLASH,               // Flash a new firmware image
  POPUP_FILE_MGR,              // Write ROM to flash, delete, hide/unhide...
#ifdef SUPPORT_NORGAMES
  POPUP_GBA_NORWRITE,          // Write a GBA ROM to NOR
  POPUP_GBA_NORLOAD,           // Launch a NOR game
#endif
  POPUP_SYSTEM,                // START window: settings groups, tools and info (tabs, L/R switch)
};

// File list layout, it depends on the selected font size (see ui_font_init).
// Glyphs and icons live in 16 pixel cells (glyphs are 12 or 10 pixels tall, icons 12 rows tall),
// so the cell starts a few pixels above the row (cell_dy) to center them.
typedef struct {
  uint8_t y0;            // Top of the first row (below the 16 pixel tab bar)
  uint8_t rowh;          // Row height
  uint8_t rows;          // Rows per page
  uint8_t footer_y;      // Footer bar position and height
  uint8_t footer_h;
  uint8_t footer_txt_y;  // Footer text Y position (top of the 16 pixel cell)
  uint8_t cell_dy;       // Row top - cell top
} t_list_layout;

static const t_list_layout list_layout_12 = { 17, 14,  9, 144, 16, 144, 1 };   // 12px font
static const t_list_layout list_layout_10 = { 16, 12, 11, 148, 12, 146, 2 };   // 10px font
static t_list_layout lay = { 17, 14,  9, 144, 16, 144, 1 };

#define LIST_Y0                    (lay.y0)
#define LIST_ROWH                  (lay.rowh)
#define LIST_FOOTER_Y              (lay.footer_y)
#define LIST_FOOTER_H              (lay.footer_h)
#define LIST_FOOTER_TXT_Y          (lay.footer_txt_y)
#define BROWSER_ROWS               (lay.rows)
#define RECENT_ROWS                (lay.rows)
#define NORGAMES_ROWS              (lay.rows)
// Top of a list row, and Y position for text/icons.
#define LIST_ROW_Y(i)         (LIST_Y0 + (i) * LIST_ROWH)
#define LIST_CELL_Y(i)        (LIST_ROW_Y(i) - lay.cell_dy)

// ---- UI animations ---------------------------------------------------------------------------
// Positions are kept in 1/16 pixels and glide towards their target a bit every frame (ease out).
typedef struct { int cur, tgt; } t_anim;

static unsigned anim_frames = 1;                   // Frames since the last render (the animations advance by this)
static int clip_y0 = 0, clip_y1 = SCREEN_HEIGHT;   // Vertical clip of fill_rect and the text helpers

static void anim_snap(t_anim *a, int v) {
  a->cur = a->tgt = v;
}

static void anim_to(t_anim *a, int v) {
  a->tgt = v;
  if (!ui_anim)
    a->cur = v;
}

static void anim_step(t_anim *a) {
  for (unsigned f = 0; f < anim_frames && a->cur != a->tgt; f++) {
    const int d = a->tgt - a->cur;
    if (d > -24 && d < 24)
      a->cur = a->tgt;
    else
      a->cur += (d * 5) / 16;
  }
}

// Masks the glyph rows that fall out of the clip rectangle (a glyph cell is 16 rows tall).
static inline void clip_text(int y) {
  uint16_t m = 0xFFFF;
  if (y < clip_y0 || y + 16 > clip_y1) {
    m = 0;
    for (int j = 0; j < 16; j++)
      if (y + j >= clip_y0 && y + j < clip_y1)
        m |= 1U << j;
  }
  font_row_mask = m;
}

// Selection bar and scroll position of the three file lists (recent, browser, NOR).
static struct {
  t_anim sel, scr;      // In list space (entry * row height * 16)
  bool valid;           // False: jump to the targets (page switch, list reloaded)
} listanim[3];

// First entries reserved for the logo palette.
#define FG_COLOR         16
#define BG_COLOR         17
#define FT_COLOR         18
#define HI_COLOR         19
#define IGM_PAL_FG      240
#define IGM_PAL_BG      241
#define IGM_PAL_HI      242
#define IGM_PAL_SH      243
#define IGM_PAL_BL      244
#define SEL_COLOR       255

#define FLASH_GO_KEYS          (KEY_BUTTUP|KEY_BUTTL|KEY_BUTTR)

enum {
  ToolsSDRAMTest = 0,
  ToolsSRAMTest,
  ToolsBatteryTest,
  ToolsSDBench,
  ToolsFlashBak,
  #ifdef SUPPORT_NORGAMES
  ToolsFlashClr,
  #endif
  ToolsMAX,
};

enum {
  SettTitle1 = 0,
  SettHotkey,
  SettBootType,
  SettFastSD,
  #ifdef SUPPORT_NORGAMES
  SettVerifyNOR,
  #endif
  SettFastEWRAM,
  SettSaveLoc,
  #ifdef SUPPORT_NORGAMES
  SettSaveLocNOR,
  #endif
  SettSaveBkp,
  SettStateLoc,
  SettCheatEn,
  SettTitle2,
  DefsPatchEng,
  DefsGamMenu,
  DefsRTCEnb,
  DefsRTCVal,
  DefsRTCSpeed,
  DefsLoadPol,
  DefsSavePol,
  DefsPrefDS,
  SettUiTheme,
  SettUiLang,
  SettUiASpd,
  #ifdef BUNDLE_FONT10
  SettUiFont,
  #endif
  SettUiHid,
  SettUiAnim,
  SettUiFx,
  SettUiSound,
  SettSave,
  SettMAX
};

// The settings window shows the settings split in groups (L/R switch between them).
enum {
  SetGrpUI,
  SetGrpFx,
  SetGrpSys,
  SetGrpSave,
  SetGrpGame,
  SetGrpCNT
};

enum {
  DefsSave     = 4,
  DefsMAX      = 4,
};

enum {
  GbaLoadPopInfo  = 0,
  GbaLoadPopLoadS = 1,
  GbaLoadPopPatch = 2,
  GbaLoadCNT      = 3,

  GbaNorWrPatch   = 1,
  GbaNorWrCNT     = 2,

  GbaNorLoad      = 1,
  GbaNorLoadCNT   = 2,
};

enum {
  GBAInfoCNT   = 1,
  GBALoadButt  = 0,

  GBALdSetCNT    = 5,
  GBALdSetLoadP  = 0,
  GBALdSetSaveP  = 1,
  GBALdSetRTC    = 2,
  GBALdSetCheats = 3,
  GBALdRemember  = 4,

  GBAPatchCNT  = 5,
  GBALoadPatch = 0,
  GBASavePatch = 1,
  GBAInGameMen = 2,
  GBARTCPatch  = 3,
  GBAPatchGen  = 4,
};

enum {
  SaveWrite  = 0,
  SavLoad    = 1,
  SavClear   = 2,
  SavQuit    = 3,
  SavMAX     = 3,
};

enum {
  FlashingReady    = 0,
  FlashingLoading  = 1,
  FlashingChecking = 2,
  FlashingErasing  = 3,
  FlashingWriting  = 4,
};

enum {
  FiMgrDelete,
  FiMgrHide,
#ifdef SUPPORT_NORGAMES
  FiMgrWriteNOR,
#endif
  FiMgrCNT
};

typedef struct {
  // ROM information
  char romfn[MAX_FN_LEN];             // File to load/write
  uint32_t romfs;                     // File ROM size
  char gcode[5];                      // ASCII sanitized game code.
  t_rom_header romh;                  // ROM header (for info purposes)
  // Patching info
  t_patch patches_datab;              // Loaded patches (from DB)
  t_patch patches_cache;              // Loaded patches (from patch engine's cache)
  bool patches_datab_found;           // Whether we had a patch match in the database
  bool patches_cache_found;           // Same but for the patch cache
  // Patching configuration
  t_patch_policy patch_type;          // Patching type
  bool use_dsaving;                   // Whether we use direct-saving mode
  bool ingame_menu_enabled;           // Enable the in-game menu.
  bool rtc_patch_enabled;             // Patch for RTC workarounds.
} t_load_gba_info;

typedef struct {
  // Save read/write policies and info
  t_sram_load_policy sram_load_type;  // SRAM loading policy
  t_sram_save_policy sram_save_type;  // SRAM auto-saving policy
  char savefn[MAX_FN_LEN];            // Save file path.
  bool savefile_found;                // Whether there's a .sav file.
  // RTC config
  uint32_t rtcval;                    // Initial RTC value.
  // Cheats policy
  bool use_cheats;                    // Whether we want to load cheats to use them.
  bool cheats_found;                  // Whether there's a cheats file (not parsed tho!)
  unsigned cheats_size;               // Size of the cheat buffer
  char cheatsfn[MAX_FN_LEN];          // Cheats file path.
} t_load_gba_lcfg;

typedef void (*t_mrender_fn)(volatile uint8_t *frame);
typedef void (*t_mkeyupd_fn)(unsigned newkeys);

// Info and state for the menu tab
static struct {
  uint8_t menu_tab;
  uint8_t sys_tab;                // Tab of the START window (remembered)

  unsigned anim_state;            // Animation (text rotation) status.

  // Recent ROMs state
  struct {
    int selector;                 // Pointed file offset
    int seloff;                   // Entry at the top of the list
    int maxentries;               // Total file/dir count in current dir
  } recent;

  // ROM browser state
  struct {
    char cpath[MAX_FN_LEN];       // Current path
    int selector;                 // Pointed file offset
    int seloff;                   // Entry at the top of the list
    int filecount;                // Total file/dir count in current dir (unfiltered)
    int dispentries;              // Maximum number of visible entries (filtered)
    uint16_t selhist[16];         // History of directory offsets
  } browser;

  // Flash ROM browser state
  struct {
    int selector;                 // Pointed file offset
    int seloff;                   // Entry at the top of the list
    uint8_t maxentries;           // Total file/dir count in current dir
    uint8_t usedblks, freeblks;   // NOR usage info
  } fbrowser;

  // Main settings
  struct {
    int selector;                 // Pointed option (a Sett*/Defs* number, kept in sync with group/idx)
    uint8_t group;                // Current group (tab)
    uint8_t idx[SetGrpCNT];       // Selected row of each group (the last one is the save button)
  } set;

  // Tools menu
  struct {
    int selector;                 // Render panel
  } tools;

  // Info/About menu
  struct {
    int selector;                 // Render panel
    char tstr[64];                // Temp message render
  } info;
} smenu;

// Same but for popups.
static struct {
  const char *alert_msg;          // Extra pop-up message

  uint8_t pop_num;                // Current pop-up in display
  char submenu;                   // Which submenu tab we are in (if any)
  char selector;                  // Option selector (if any)
  unsigned anim;                  // Animation state

  // Pop up message (for whatever action). Allows returning to previous popup.
  struct {
    const char *message;
    const char *default_button;
    const char *confirm_button;
    void (*callback)(bool confirm);       // Function to call on "confirm".
    uint8_t option;                       // Selected button
    bool clear_popup_ok;                  // Whether any pop up must be cleared.
  } qpop;

  // RTC time set pop up, a bit special.
  struct {
    t_dec_date val;
    int selector;
    void (*callback)();                   // Function to call on "save"
  } rtcpop;

  union {
    // GBA launch ROM pop up menu
    struct {
      t_load_gba_info i;                  // ROM/Patch info and patch policy.
      t_load_gba_lcfg l;                  // ROM loading info and settings;
    } load;

    // Write GBA game to NOR memory
    struct {
      t_load_gba_info i;                  // ROM/Patch info and patch policy.
    } norwr;
    // Launch GBA game from NOR memory
    struct {
      t_load_gba_lcfg l;                  // ROM loading info and settings;
      const t_flash_game_entry *e;        // NOR game entry on RAM
    } norld;

    // Save file menu (.sav files)
    struct {
      char savfn[MAX_FN_LEN];             // SAV file to load/store/mangle
    } savopt;
    // Update menu (for .fw files)
    struct {
      char fn[MAX_FN_LEN];                // FW file to load and flash
      bool issfw;                         // The firmware is a superFW image.
      uint32_t superfw_ver;               // Reported FW version.
      uint32_t fw_size;                   // Size in bytes reported by stat.
      unsigned curr_state;                // Flashing FSM state.
    } update;

    // Not really a pop up, but used as "popup" data for menu questions.
    struct {
      char fn[MAX_FN_LEN];
      unsigned fs;
    } pdb_ld;
  } p;
} spop;

typedef struct {
  uint32_t filesize;
  uint16_t isdir;
  uint16_t attr;
  char fname[MAX_FN_LEN];
  uint16_t sortname[MAX_FN_LEN];       // Pre-decoded and sort-friendly name.
} t_centry;
_Static_assert (sizeof(t_centry) % 4 == 0, "t_centry must be word-friendly");

// Pointer to SDRAM, where we place some data:
//  - Scratch area 2MiB (for FW updates)
//  - File list order (~64KiB)
//  - File list filtered (~64KiB)
//  - Browser file information (~13MB)
//  - Recently played ROMs table (~64KiB)
//  - Font data (placed by the bootloader at the 15..16MB range)
// At the end of the SDRAM, ro-data can be loaded by the loader.
#define scratch_mem_size (2*1024*1024)
typedef struct {
  uint8_t scratch[scratch_mem_size];
  t_centry *fileorder[BROWSER_MAXFN_CNT];
  t_centry *filefilter[BROWSER_MAXFN_CNT];
  t_centry fentries[BROWSER_MAXFN_CNT];
  t_rentry rentries[RECENT_MAXFN_CNT];
  t_reg_entry_max nordata;
} t_sdram_state;

_Static_assert (sizeof(t_sdram_state) <= 14.5*1024*1024, "scratch SDRAM doesn't exceed 14.5MB");

t_sdram_state *sdr_state = (t_sdram_state*)0x08000000;
uint8_t *hiscratch = (uint8_t*)ROM_HISCRATCH_U8;

typedef struct {
  uint16_t x, y;
  unsigned tn;
} t_oamobj;

static bool enable_flashing = false;
static unsigned framen = 0;
static unsigned objnum = 0;
static t_oamobj fobjs[64];

unsigned lang_lookup(uint16_t code) {
  for (unsigned i = 0; i < LANG_COUNT; i++)
    if (lang_codes[i] == code)
      return i;

  return 0;  // Fallback to default (english)
}

inline uint16_t curr_pressed_keys() {
  return REG_KEYINPUT ^ 0x3FF;
}

uint16_t lang_getcode() {
  return lang_codes[lang_id];
}

inline bool isascii(char code) {
  // Abuse signed :D
  return code >= 32;
}

// Uses the FAT attribute and also the unix filename convention.
inline bool ishidden(const t_centry *e) {
  return (e->attr & AM_HID) || e->fname[0] == '.';
}

bool is_superfw(const t_rom_header *h) {
  return !memcmp(&h->data[SUPERFW_COMMENT_DOFFSET], "SUPERFW~DAVIDGF", 16);
}

static int strcmp16(const uint16_t *a, const uint16_t *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a - *b;
}

NOINLINE int filesort(const void *a, const void *b) {
  const t_centry *ca = *(t_centry**)a;
  const t_centry *cb = *(t_centry**)b;

  // Directories some up first.
  if (ca->isdir != cb->isdir)
    return cb->isdir - ca->isdir;

  // Other files are string-ordered
  return strcmp16(ca->sortname, cb->sortname);
}

NOINLINE int romsort(const void *a, const void *b) {
  const t_flash_game_entry *ca = (t_flash_game_entry*)a;
  const t_flash_game_entry *cb = (t_flash_game_entry*)b;

  return strcasecmp(&ca->game_name[ca->bnoffset], &cb->game_name[cb->bnoffset]);
}

static void loadrom_progress(unsigned done, unsigned total) {
  // Draws and flips the buffer, do not care about vsync here
  volatile uint8_t *frame = &MEM_VRAM_U8[0xA000*framen];

  // Render the full background to a solid color
  dma_memset16(&frame[0], dup8(BG_COLOR), SCREEN_WIDTH*SCREEN_HEIGHT/2);

  // Render a simple progress bar
  unsigned prog = done * 200 / total;
  for (unsigned i = 76; i < 84; i++)
    dma_memset16(&frame[SCREEN_WIDTH * i + 20], dup8(FG_COLOR), prog/2);

  dma_memset16(MEM_OAM, 0, 256);  // Clear icons

  REG_DISPCNT = (REG_DISPCNT & ~0x10) | (framen << 4);
  framen ^= 1;
}

static bool loadrom_progress_abort(unsigned done, unsigned total) {
  loadrom_progress(done, total);

  // Capture A/B buttons to abort the progress
  return ((~REG_KEYINPUT) & KEY_BUTTSTA);
}


bool generate_patches_progress(const char *fn, unsigned fs) {
  // Open ROM and load it in the SDRAM. We load it in 4MB chunks. Not ideal but
  // we want to preserve the data loaded in the SDRAM (ie. fonts).
  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res != FR_OK)
    return false;

  t_patch_builder pb;
  patchengine_init(&pb, fs);
  const unsigned max_hiscratch = 8*1024*1024;

  for (unsigned i = 0; i < fs; i += max_hiscratch) {
    for (unsigned j = 0; j < max_hiscratch && i + j < fs; j += 4096) {
      UINT rdbytes;
      uint32_t tmp[4096/4];
      if (FR_OK != f_read(&fd, tmp, sizeof(tmp), &rdbytes))
        return false;

      set_supercard_mode(MAPPED_SDRAM, true, false);
      dma_memcpy32(&hiscratch[j], tmp, sizeof(tmp)/4);
      set_supercard_mode(MAPPED_SDRAM, true, true);
      if (j & ~0x1FFFF)
        loadrom_progress((i*2 + j) >> 8, fs >> 7);
    }
    // Amount to process.
    unsigned blksize = MIN(max_hiscratch, fs - i);

    void upd_pe_prog(unsigned prog) {
      unsigned p = i*2 + blksize + prog*4;
      loadrom_progress(p >> 8, fs >> 7);
    }

    // Process patches. Adds them to the existing patchset.
    set_supercard_mode(MAPPED_SDRAM, true, false);
    patchengine_process_rom((uint32_t*)hiscratch, blksize, &pb, upd_pe_prog);
    set_supercard_mode(MAPPED_SDRAM, true, true);
  }

  f_close(&fd);
  patchengine_finalize(&pb);

  WRITE_LOG("Patch engine done. Found wcnt: %d save: %d (save mode: %d) irqh: %d rtc: %d",
            pb.p.wcnt_ops, pb.p.save_ops, pb.p.save_mode, pb.p.irqh_ops, pb.p.rtc_ops);

  // Proceed to write patches to their cache.
  return write_patches_cache(fn, &pb.p);
}

bool dump_flashmem_backup() {
  f_mkdir(SUPERFW_DIR);

  // Use a different file name to ensure we do not overwrite firmwares by
  // accident. This adds some minimal overhead.
  SHA256_State st;
  sha256_init(&st);

  FIL fd;
  FRESULT res = f_open(&fd, FLASHBACKUPTMP_FILEPATH, FA_WRITE | FA_CREATE_ALWAYS);
  if (res != FR_OK)
    return false;

  const unsigned fsize = flashinfo.size ? flashinfo.size : FW_MAX_SIZE_KB*1024;
  for (unsigned i = 0; i < fsize; i += 4*1024) {
    const uint8_t *faddr = (uint8_t*)(ROM_FLASHFIRMW_ADDR + i);

    uint32_t tmp[4096/4];
    set_supercard_mode(MAPPED_FIRMWARE, true, false);
    dma_memcpy32(tmp, faddr, 1024);
    set_supercard_mode(MAPPED_SDRAM, true, true);

    sha256_transform(&st, tmp, sizeof(tmp));

    UINT wrbytes;
    if (FR_OK != f_write(&fd, tmp, sizeof(tmp), &wrbytes) || wrbytes != sizeof(tmp)) {
      f_close(&fd);
      return false;
    }

    loadrom_progress(i >> 10, fsize >> 10);
  }

  f_close(&fd);

  // Calculate the final hash, use a hash prefix as the filename.
  uint8_t h256[32];
  sha256_finalize(&st, h256);

  char finalfn[64];
  npf_snprintf(finalfn, sizeof(finalfn), FLASHBACKUP_FILEPTRN,
               h256[0], h256[1], h256[2], h256[3]);
  f_rename(FLASHBACKUPTMP_FILEPATH, finalfn);

  return true;
}

void patch_gen_callback(bool confirm);

void sram_battery_test_callback(bool confirm) {
  if (confirm) {
    // Fill SRAM with some pseudorandom data to test later.
    sram_pseudo_fill();
    // Program a check on the next reboot!
    program_sram_check();

    spop.alert_msg = msgs[lang_id][MSG_SRAMTST_RDY];
  }
}


static const t_patch * get_game_patch(const t_load_gba_info *info) {
  return info->patch_type == PatchDatabase && info->patches_datab_found ? &info->patches_datab :
         info->patch_type == PatchEngine   && info->patches_cache_found ? &info->patches_cache : NULL;
}

bool ingame_menu_avail_sdram(const t_load_gba_info *info) {
  const t_patch *p = get_game_patch(info);
  // Necessary size to load the IGM (+fonts +cheats)
  const unsigned igm_reqsz = ROUND_UP2(ingame_menu_payload.menu_rsize + font_block_size() + spop.p.load.l.cheats_size, 1024);

  // If the ROM is too big, must use some hole to load the menu.
  if (info->romfs > MAX_GBA_ROM_SIZE - igm_reqsz) {
    // Discard holes that are too small, or not well formed.
    if (!p || p->hole_size < igm_reqsz || p->hole_addr + p->hole_size > info->romfs)
      return false;   // Too big to fit the menu!
  }

  // Check if the patches exist and have proper IRQ support.
  return p && p->irqh_ops > 0;
}

bool ingame_menu_avail_flash(const t_load_gba_info *info) {
  const t_patch *p = get_game_patch(info);

  // Checks if the ROM is small enough so the last 4MiB block can be remapped.
  if (info->romfs > MAX_GBA_ROM_SIZE - NOR_BLOCK_SIZE) {
    // Otherwise find a gap to flash on NOR our tiny payload
    if (!p || p->hole_size < DIRSAVE_REQ_SPACE || p->hole_addr + p->hole_size > info->romfs)
      return false;   // Too big to fit!
  }

  // Check if the patches exist and have proper IRQ support.
  return p && p->irqh_ops > 0;
}

// Calculates whether DirectSaving can be used given some information.
bool dirsav_avail_sdram(const t_load_gba_info *info) {
  const t_patch *p = get_game_patch(info);

  // Check if there's enough space for it! (Placing it at the end).
  if (info->romfs > MAX_GBA_ROM_SIZE - DIRSAVE_REQ_SPACE) {
    if (!p || p->hole_size < DIRSAVE_REQ_SPACE || p->hole_addr + p->hole_size > info->romfs)
      return false;   // Too big to fit!
  }

  return (p && supports_directsave(p->save_mode));
}

bool dirsav_avail_flash(const t_load_gba_info *info) {
  const t_patch *p = get_game_patch(info);

  // Checks if the ROM is small enough so the last 4MiB block can be remapped.
  if (info->romfs > MAX_GBA_ROM_SIZE - NOR_BLOCK_SIZE) {
    // Otherwise find a gap to flash on NOR our tiny payload
    if (!p || p->hole_size < DIRSAVE_REQ_SPACE || p->hole_addr + p->hole_size > info->romfs)
      return false;   // Too big to fit!
  }

  return (p && supports_directsave(p->save_mode));
}

bool rtcemu_avail(const t_load_gba_info *info) {
  const t_patch *p = get_game_patch(info);
  return (p && p->rtc_ops);
}

static bool prepare_gba_info(
  t_load_gba_info *info, const t_rom_load_settings *st,
  const char *fn, uint32_t fs,
  bool load_sdram
) {
  // Pre-load ROM header
  if (preload_gba_rom(fn, fs, &info->romh))
    return false;

  // Fill/copy ROM info.
  if (fn != info->romfn)
    strcpy(info->romfn, fn);
  info->romfs = fs;

  // Sanitize the game code for display
  for (unsigned i = 0; i < 4; i++)
    info->gcode[i] = isascii(info->romh.gcode[i]) ? info->romh.gcode[i] : 0x1A;
  info->gcode[4] = 0;

  // Look up patches, have them handy.
  uint8_t gamecode[5] = {
    info->romh.gcode[0], info->romh.gcode[1],
    info->romh.gcode[2], info->romh.gcode[3],
    info->romh.version
  };
  set_supercard_mode(MAPPED_SDRAM, true, false);
  info->patches_datab_found = patchmem_lookup(gamecode, (uint8_t*)ROM_PATCHDB_U8, &info->patches_datab);
  set_supercard_mode(MAPPED_SDRAM, true, true);

  if (!info->patches_datab_found)
    WRITE_LOG("No patches in PatchDB found for '%s' with gamecode %c%c%c%c-%d",
              fn, gamecode[0], gamecode[1], gamecode[2], gamecode[3], (int)gamecode[4]);

  // Attempt to load any existing patch and check also the PE cache dir.
  info->patches_cache_found = load_rom_patches(fn, &info->patches_cache);
  if (!info->patches_cache_found) {
    WRITE_LOG("No patch file found for '%s'", fn);
    info->patches_cache_found = load_cached_patches(fn, &info->patches_cache);
    if (!info->patches_cache_found)
      WRITE_LOG("No patch file found in patches cache dir for '%s'", fn);
  }

  // If PatchAuto is selected, resolve it. Downgrade if not found.
  if (st->patch_policy == PatchAuto) {
    if (info->patches_cache_found)
      info->patch_type = PatchEngine;      // Try existing patches
    else if (info->patches_datab_found)
      info->patch_type = PatchDatabase;    // Try the database then
    else
      info->patch_type = PatchNone;
  }
  else {
    // Use the specified patch type.
    info->patch_type = st->patch_policy;

    // Downgrade to no patches if the specified was not found.
    if ((info->patch_type == PatchDatabase && !info->patches_datab_found) ||
        (info->patch_type == PatchEngine   && !info->patches_cache_found))
      info->patch_type = PatchNone;
  }

  // Fill defaults as requested if possible.
  bool allowds = load_sdram ? dirsav_avail_sdram(info) : dirsav_avail_flash(info);
  bool allowigm = load_sdram ? ingame_menu_avail_sdram(info) : ingame_menu_avail_flash(info);

  info->rtc_patch_enabled = st->use_rtc && rtcemu_avail(info);
  info->use_dsaving = st->use_dsaving && allowds;
  info->ingame_menu_enabled = st->use_igm && allowigm;

  return true;
}

static inline char cht_up(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static bool cht_ext_ok(const char *e) {
  return e[0] == '.' && cht_up(e[1]) == 'C' && cht_up(e[2]) == 'H' && cht_up(e[3]) == 'T';
}

// Searches the cheat folder for the best match for a game code and version.
// Accepted names are XXXX-VV.cht or XXXX.cht. Match quality (higher is better):
//  3: same game code and version, 2: same game code (any version),
//  1: same first 3 chars of the code (different language/region letter, ie. fan translations).
static bool find_cheat_file(const char *gcode, uint8_t ver, char *out, unsigned outsz) {
  DIR d;
  if (FR_OK != f_opendir(&d, CHEATS_PATH))
    return false;

  int best = 0;
  while (1) {
    FILINFO info;
    if (f_readdir(&d, &info) != FR_OK || !info.fname[0])
      break;
    if (info.fattrib & AM_DIR)
      continue;

    const char *n = info.fname;
    unsigned len = strlen(n);
    // Only accept XXXX.cht or XXXX-VV.cht
    bool hasver = (len == 4 + 1 + 2 + 4 && n[4] == '-');
    if ((len != 4 + 4 && !hasver) || !cht_ext_ok(&n[len - 4]))
      continue;

    unsigned same = 0;
    while (same < 4 && cht_up(n[same]) == cht_up(gcode[same]))
      same++;
    if (same < 3)
      continue;

    int score = 1;
    if (same == 4) {
      score = 2;
      if (hasver) {
        char vs[3];
        npf_snprintf(vs, sizeof(vs), "%02x", ver);
        if (cht_up(n[5]) == cht_up(vs[0]) && cht_up(n[6]) == cht_up(vs[1]))
          score = 3;
      }
    }

    if (score > best) {
      best = score;
      npf_snprintf(out, outsz, CHEATS_PATH "%s", n);
      if (best == 3)
        break;
    }
  }
  f_closedir(&d);
  return best > 0;
}

static void prepare_gba_cheats(const char *gcode, uint8_t ver, t_load_gba_lcfg *data, const char *fn, bool prefer_cheats) {
  // Attempt to find a cheat file if cheats are enabled.
  data->cheats_size = 0;
  data->cheats_found = false;
  if (enable_cheats) {
    // First try a cheat file next to the ROM, with the same name.
    strcpy(data->cheatsfn, fn);
    replace_extension(data->cheatsfn, ".cht");
    data->cheats_found = check_file_exists(data->cheatsfn);
    if (!data->cheats_found) {
      WRITE_LOG("No cheat file found at '%s'", data->cheatsfn);
      // Search the cheat database by game ID (and version), falling back to looser matches.
      data->cheats_found = find_cheat_file(gcode, ver, data->cheatsfn, sizeof(data->cheatsfn));
      if (!data->cheats_found)
        WRITE_LOG("No cheat file found for %c%c%c%c-%02x", gcode[0], gcode[1], gcode[2], gcode[3], ver);
    }

    // Load the cheats into memory if found.
    if (data->cheats_found) {
      WRITE_LOG("Using cheat file '%s'", data->cheatsfn);
      // Load the cheats to the ROM area, just after the font pack. This is for easier relocation.
      uint8_t *cheat_area = (uint8_t*)(ROM_FONTBASE_U8 + font_block_size());
      unsigned max_area = 1536*1024 - font_block_size();    // 1.5MB is reserved at the end.
      int cheatsz = open_read_cheats(cheat_area, max_area, data->cheatsfn);
      if (cheatsz < 0)
        data->cheats_found = false;
      else
        data->cheats_size = cheatsz;

      WRITE_LOG("Loaded cheats returned %d", cheatsz);
    }
  }
  data->use_cheats = enable_cheats && data->cheats_found && prefer_cheats;
}

static void prepare_gba_settings(t_load_gba_lcfg *data, bool uses_dsaving, uint32_t rtcts, bool game_no_save) {
  // Calculate the .sav file name, and check its existance.
  data->savefile_found = check_file_exists(data->savefn);
  if (data->savefile_found)
    WRITE_LOG("Savefile found at '%s'", data->savefn);
  else
    WRITE_LOG("No savefile found for '%s'", data->savefn);

  // Use default settings (and file existance) to fill in default choice.
  // DirectSaving enabled overrides the other settings.
  if (uses_dsaving) {
    data->sram_load_type = data->savefile_found ? SaveLoadSav : SaveLoadReset;
    data->sram_save_type = SaveDirect;
  }
  else {
    data->sram_load_type = game_no_save         ? SaveLoadDisable :
                           !autoload_default    ? SaveLoadDisable :
                           data->savefile_found ? SaveLoadSav :
                                                  SaveLoadReset;
    data->sram_save_type = autosave_default && !game_no_save ? SaveReboot : SaveDisable;
  }

  data->rtcval = rtcts;
}


static void browser_open_gba(const char *fn, uint32_t fs, bool prompt_patchgen) {
  if (fs > MAX_GBA_ROM_SIZE) {
    // The ROM is too big to be loaded!
    spop.alert_msg = msgs[lang_id][MSG_ERR_TOOBIG];
  } else {
    // Default to global settings (in case the file is not found).
    t_rom_load_settings ld_sett = {
      .patch_policy = patcher_default,
      .use_igm = ingamemenu_default,
      .use_rtc = rtcpatch_default,
      .use_dsaving = autosave_prefer_ds
    };
    t_rom_launch_settings lh_sett = {
      .use_cheats = true,              // Defaults to true (just preferred, might be disabled/N/A)
      .rtcts = rtcvalue_default
    };
    // Check for any game-specific config file, so we don't have to guess the config.
    // The config file can be partial, hence the defaults.
    load_rom_settings(fn, &ld_sett, &lh_sett);

    if (!prepare_gba_info(&spop.p.load.i, &ld_sett, fn, fs, true))
      spop.alert_msg = msgs[lang_id][MSG_ERR_READ];
    else {
      const t_rom_header *rmh = &spop.p.load.i.romh;

      // If patch engine is selected but no patches found, prompt for generation.
      // If auto is selected and no patches nor DB entries found, do prompt too.
      bool no_patches = (ld_sett.patch_policy == PatchAuto &&
                         !spop.p.load.i.patches_datab_found && !spop.p.load.i.patches_cache_found);
      bool no_engine  = (ld_sett.patch_policy == PatchEngine && !spop.p.load.i.patches_cache_found);
      bool issfw = is_superfw(rmh);

      if (prompt_patchgen && !issfw && (no_patches || no_engine)) {
        // No patches found, ask the user if they want to generate patches
        // using the patch engine.
        spop.qpop.message = msgs[lang_id][no_patches ? MSG_Q1_NOPATCH : MSG_Q1_PATCHENG];
        spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
        spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
        spop.qpop.option = 0;
        spop.qpop.callback = patch_gen_callback;
        spop.qpop.clear_popup_ok = true;
        return;
      }

      // What if the game doesn't have a save method? Select sane defaults.
      const t_patch *p = get_game_patch(&spop.p.load.i);
      bool game_no_save = (p && p->save_mode == SaveTypeNone) || issfw;

      // Attempt to find a cheat file if cheats are enabled.
      prepare_gba_cheats((char*)&rmh->gcode[0], rmh->version, &spop.p.load.l, fn, lh_sett.use_cheats);

      // Load and set default and sane settings honoring defaults and preferences.
      sram_filename_calc(fn, spop.p.load.l.savefn, save_path_default);
      prepare_gba_settings(&spop.p.load.l, spop.p.load.i.use_dsaving, lh_sett.rtcts, game_no_save);

      // Show load ROM menu.
      spop.pop_num = POPUP_GBA_LOAD;
      spop.anim = 0;
      spop.submenu = GbaLoadPopInfo;
      spop.selector = GBALoadButt;
    }
  }
}

#ifdef SUPPORT_NORGAMES
static void browser_open_nor(const t_flash_game_entry * e) {
  // Use attributes to determine patched save method.
  const bool game_no_save = GET_GATTR_SAVEM(e->gattrs) <= SaveTypeNone;
  const bool game_uses_dsaving = (e->gattrs & GATTR_SAVEDS);

  t_rom_launch_settings lh_sett = {
    .use_cheats = true,              // Defaults to true (just preferred, might be disabled/N/A)
    .rtcts = rtcvalue_default
  };
  load_rom_settings(e->game_name, NULL, &lh_sett);

  // Attempt to find a cheat file if cheats are enabled.
  prepare_gba_cheats((char*)&e->gamecode, e->gamever, &spop.p.norld.l, e->game_name, lh_sett.use_cheats);

  // Load and set default and sane settings honoring defaults and preferences.
  sram_filename_calc(e->game_name, spop.p.norld.l.savefn, save_path_nor_default);
  WRITE_LOG("loadp: %d savep: %d uses_ds: %d gamens: %d",
            spop.p.norld.l.sram_load_type, spop.p.norld.l.sram_save_type,
            game_uses_dsaving, game_no_save);
  prepare_gba_settings(&spop.p.norld.l, game_uses_dsaving, lh_sett.rtcts, game_no_save);

  // Save entry pointer
  spop.p.norld.e = e;

  // Show load ROM menu.
  spop.pop_num = POPUP_GBA_NORLOAD;
  spop.submenu = GbaLoadPopInfo;
  spop.selector = 0;
}
#endif

void patch_gen_callback(bool confirm) {
  // Generate patches if confirm was selected
  if (confirm) {
    generate_patches_progress(spop.p.load.i.romfn, spop.p.load.i.romfs);
    spop.alert_msg = msgs[lang_id][MSG_PATCHGEN_OK];
  }

  // Either way, show the popup screen afterwards without prompt
  browser_open_gba(spop.p.load.i.romfn, spop.p.load.i.romfs, false);
}

static void load_patchdb_action(bool confirm) {
  if (confirm) {
    FIL fd;
    FRESULT res = f_open(&fd, spop.p.pdb_ld.fn, FA_READ);
    if (res != FR_OK) {
      spop.alert_msg = msgs[lang_id][MSG_ERR_GENERIC];
      return;
    } else {
      for (unsigned off = 0; off < spop.p.pdb_ld.fs; off += 1024) {
        UINT rdbytes;
        uint32_t tmp[1024/4];
        if (FR_OK != f_read(&fd, tmp, sizeof(tmp), &rdbytes)) {
          spop.alert_msg = msgs[lang_id][MSG_ERR_GENERIC];
          return;
        }

        set_supercard_mode(MAPPED_SDRAM, true, false);
        dma_memcpy32(ROM_PATCHDB_U8 + off, tmp, sizeof(tmp)/4);
        set_supercard_mode(MAPPED_SDRAM, true, true);
      }
    }
    spop.alert_msg = msgs[lang_id][MSG_OK_GENERIC];
  }
}

unsigned guess_file_type(const uint8_t *header) {
  const t_rom_header *gbah = (t_rom_header*)header;
  union {
    uint32_t sig;
    uint32_t data[0];
  } *hdr = (void*)header;

  if (gbah->fixed == 0x96 && gbah->unit_code == 0x00 && gbah->devtype == 0x00 &&
      header[3] == 0xEA /* Starts with an unconditional branch */ &&
      validate_gba_header(header))
    return FileTypeGBA;
  else if (validate_gb_header(&header[0x100]))
    return FileTypeGB;
  else if (hdr->sig == 0x1A53454E)
    return FileTypeNES;
  else if (hdr->sig == 0x31424450)
    return FileTypePatchDB;
  else if (hdr->data[3] == 0x4B4E5320 && hdr->data[6] == 0x4E4F4954)
    return FileTypeNGP;

  return FileTypeUnknown;
}

static bool insert_recent_flush(const char *fn, unsigned flags) {
  // Insert element.
  smenu.recent.maxentries = insert_recent_fn(sdr_state->rentries, smenu.recent.maxentries, fn, flags);
  return recent_flush(sdr_state->rentries, smenu.recent.maxentries);
}

static bool delete_recent_flush(unsigned entry_num) {
  smenu.recent.maxentries = delete_recent(sdr_state->rentries, smenu.recent.maxentries, entry_num);

  smenu.recent.selector = MAX(0, MIN(smenu.recent.maxentries - 1, smenu.recent.selector));

  return recent_flush(sdr_state->rentries, smenu.recent.maxentries);
}

static void recent_reload() {
  listanim[MENUTAB_RECENT].valid = false;
  smenu.recent.selector = 0;
  smenu.recent.seloff = 0;
  smenu.anim_state = 0;
  smenu.recent.maxentries = recent_load(RECENT_FILEPATH, sdr_state->rentries);
}

void start_emu_game(const t_emu_loader *ldinfo, const char *fn, uint32_t fs) {
  // Load: Sav/Reset Save: Reboot/Disable
  sram_filename_calc(fn, spop.p.load.l.savefn, save_path_default);
  t_sram_load_policy lp = check_file_exists(spop.p.load.l.savefn) ? SaveLoadSav : SaveLoadReset;
  unsigned errsave = prepare_sram_based_savegame(lp, SaveReboot, spop.p.load.l.savefn);
  if (errsave) {
    unsigned errmsg = (errsave == ERR_SAVE_BADSAVE)   ? MSG_ERR_SAVERD :
                                                        MSG_ERR_SAVEWR;
    spop.alert_msg = msgs[lang_id][errmsg];
  }
  else {
    // Try to load the emu and ROM, keep trying if there's more than one emulatior option.
    unsigned errcode = ERR_LOAD_NOEMU;
    while (ldinfo->emu_name) {
      if (recent_menu)
        insert_recent_flush(fn, FLAG_RECENT_SD);

      errcode = load_extemu_rom(fn, fs, ldinfo, loadrom_progress);
      if (errcode && errcode != ERR_LOAD_NOEMU)
        break;
      ldinfo++;
    }
    unsigned errmsg = (errcode == ERR_LOAD_NOEMU) ? MSG_ERR_NOEMU :
                                                    MSG_ERR_READ;
    spop.alert_msg = msgs[lang_id][errmsg];
  }
}

// Firmware (.fw) files: the user chooses between chain loading them (running them like a game,
// nothing gets modified) and flashing them (needs the flashing unlock and the key combo).
static char fwchoice_fn[MAX_FN_LEN];
static uint32_t fwchoice_size;

static void browser_open_fw_flash(const char *fn, uint32_t fs) {
  // A SuperFW firmware update is selected!
  if (!enable_flashing)
    spop.alert_msg = msgs[lang_id][MSG_FWUP_DISABLED];
  else if (fs > FW_MAX_SIZE_KB*1024 || (flashinfo.size && fs > flashinfo.size))
    spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRSZ];
  else {
    // Read the header and perform some more basic checks!
    FIL fd;
    FRESULT res = f_open(&fd, fn, FA_READ);
    if (res != FR_OK)
      spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRRD];
    else {
      UINT rdbytes;
      uint8_t tmp[512];
      if (FR_OK != f_read(&fd, tmp, sizeof(tmp), &rdbytes) || rdbytes != sizeof(tmp))
        spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRRD];
      else if (!validate_gba_header(tmp))  // Is it a valid GBA ROM header?
        spop.alert_msg = msgs[lang_id][MSG_FWUP_BADHD];
      else {
        spop.p.update.issfw = check_superfw(tmp, &spop.p.update.superfw_ver);
        spop.p.update.fw_size = fs;
        spop.p.update.curr_state = FlashingReady;
        spop.pop_num = POPUP_FWFLASH;
        strbcpy(spop.p.update.fn, fn, sizeof(spop.p.update.fn));
      }
      f_close(&fd);
    }
  }
}

static void fwchoice_callback(bool flash) {
  if (flash)
    browser_open_fw_flash(fwchoice_fn, fwchoice_size);
  else
    browser_open_gba(fwchoice_fn, fwchoice_size, true);    // Chain load
}

NOINLINE static void browser_open(const char *fn, uint32_t fs) {
  const char *ext = find_extension(fn);
  if (ext && !strcasecmp(ext, "gba"))
    // GBA ROMs (most likely)
    browser_open_gba(fn, fs, true);
  else if (ext && !strcasecmp(ext, "sav")) {
    spop.pop_num = POPUP_SAVFILE;
    spop.selector = SavMAX;
    strbcpy(spop.p.savopt.savfn, fn, sizeof(spop.p.savopt.savfn));
  }
  else if (ext && !strcasecmp(ext, "fw")) {
    // Ask what to do with it: chain load (default button) or flash.
    strbcpy(fwchoice_fn, fn, sizeof(fwchoice_fn));
    fwchoice_size = fs;
    spop.qpop.message = msgs[lang_id][MSG_FWCH_Q];
    spop.qpop.default_button = msgs[lang_id][MSG_FWCH_RUN];
    spop.qpop.confirm_button = msgs[lang_id][MSG_FWCH_FLASH];
    spop.qpop.option = 0;
    spop.qpop.callback = fwchoice_callback;
    spop.qpop.clear_popup_ok = false;
  }
  else {
    // Any emulator-based console supported
    if (ext) {
      const t_emu_loader *ldinfo = get_emu_info(ext);
      if (ldinfo) {
        start_emu_game(ldinfo, fn, fs);
        return;
      }
    }

    // Attempt to load the file magic and detect what kind of file this is.
    if (fs >= 512) {
      FIL fi;
      if (FR_OK == f_open(&fi, fn, FA_READ)) {
        uint32_t tmphdr[512 / 4];
        UINT rdbytes;
        if (FR_OK == f_read(&fi, tmphdr, sizeof(tmphdr), &rdbytes) && rdbytes == sizeof(tmphdr)) {
          unsigned guesstype = guess_file_type((uint8_t*)tmphdr);
          switch (guesstype) {
          case FileTypeGBA:
            browser_open_gba(fn, fs, true); break;
          case FileTypeGB:
            start_emu_game(get_emu_info("gbc"), fn, fs);
            break;
          case FileTypeNES:
            start_emu_game(get_emu_info("nes"), fn, fs);
            break;
          case FileTypeNGP:
            start_emu_game(get_emu_info("ngp"), fn, fs);
            break;
          case FileTypePatchDB:
            strbcpy(spop.p.pdb_ld.fn, fn, sizeof(spop.p.pdb_ld.fn));
            spop.p.pdb_ld.fs = fs;
            spop.qpop.message = msgs[lang_id][MSG_Q3_LOADPDB];
            spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
            spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
            spop.qpop.option = 0;
            spop.qpop.callback = load_patchdb_action;
            spop.qpop.clear_popup_ok = false;
            break;
          default:
            spop.alert_msg = msgs[lang_id][MSG_ERR_UNKTYP];
            break;
          };
        }
        f_close(&fi);
      }
    }
  }
}

static void browser_reload_sort() {
  // We take the list of files and sort them using a list of pointers instead of
  // actually copying entries. Faster, less memory and allows us to play with it later.

  // Init pointer table, then sort them.
  for (unsigned i = 0; i < smenu.browser.filecount; i++)
    sdr_state->fileorder[i] = &sdr_state->fentries[i];

  heapsort4(sdr_state->fileorder, smenu.browser.filecount, sizeof(t_centry*) / sizeof(uint32_t), filesort);
}

static void browser_reload_filter() {
  // Process the sorted pointer table and filter out files that we don't want to display.

  unsigned fcount = 0;
  for (unsigned i = 0; i < smenu.browser.filecount; i++) {
    if (hide_hidden && ishidden(sdr_state->fileorder[i]))
      continue;

    sdr_state->filefilter[fcount++] = sdr_state->fileorder[i];
  }

  if (smenu.browser.selector >= fcount)
    smenu.browser.selector = fcount - 1;
  smenu.browser.seloff = MAX(0, smenu.browser.selector - BROWSER_ROWS / 2);
  smenu.browser.dispentries = fcount;
  listanim[MENUTAB_ROMBROWSE].valid = false;
}

// Loads a new directory list in the ROM browser.
// TODO: Implement filtering (.gba/.rom/.bin... etc) using settings
static void show_loading(void);

static void browser_reload() {
  smenu.anim_state = 0;
  if (smenu.browser.filecount > 100)
    show_loading();      // Reading a big directory takes a moment

  unsigned fcount = 0;
  DIR d;
  if (FR_OK != f_opendir(&d, smenu.browser.cpath))
    return;   // FIXME: Implement error reporting!

  while (1) {
    FILINFO info;
    if (f_readdir(&d, &info) != FR_OK || !info.fname[0])
      break;

    if (fcount >= BROWSER_MAXFN_CNT)
      break;

    t_centry *e = &sdr_state->fentries[fcount++];
    e->filesize = (uint32_t) info.fsize;  // TODO: Support 4GB+ files?
    e->isdir = (info.fattrib & AM_DIR) ? 1 : 0;
    e->attr = info.fattrib;
    dma_memcpy16(e->fname, info.fname, MAX_FN_LEN/2);
    sortable_utf8_u16(info.fname, e->sortname);
  }
  f_closedir(&d);

  smenu.browser.filecount = fcount;

  // Sort and filter the new list of files/dirs
  browser_reload_sort();
  browser_reload_filter();
}

// Loads NOR game entries so they can be browsed.
static void flashbrowser_reload() {
  #ifdef SUPPORT_NORGAMES
  listanim[MENUTAB_NORBROWSE].valid = false;
  smenu.fbrowser.selector = 0;
  smenu.anim_state = 0;

  if (!flashmgr_load(ROM_FLASHMETA_ADDR, FLASH_METADATA_SIZE, (t_reg_entry*)&sdr_state->nordata))
    // No data found, reset the entries
    memset(&sdr_state->nordata, 0, sizeof(sdr_state->nordata));

  // Calculate block usage, free space, etc.
  smenu.fbrowser.usedblks = 0;
  for (unsigned i = 0; i < sdr_state->nordata.gamecnt; i++) {
    const t_flash_game_entry *e = &sdr_state->nordata.games[i];
    for (unsigned j = 0; j < MAX_GAME_BLOCKS; j++)
      if (e->blkmap[j])
        smenu.fbrowser.usedblks++;
  }
  smenu.fbrowser.freeblks = NOR_GAMEBLOCK_COUNT - smenu.fbrowser.usedblks;

  smenu.fbrowser.maxentries = sdr_state->nordata.gamecnt;
  heapsort4(sdr_state->nordata.games, smenu.fbrowser.maxentries, sizeof(t_flash_game_entry) / sizeof(uint32_t), romsort);
  #endif
}

static inline void render_icon(unsigned x, unsigned y, unsigned iconn) {
  fobjs[objnum++] = (t_oamobj){x, y, 8*iconn };
}

static inline void render_icon_trans(unsigned x, unsigned y, unsigned iconn) {
  fobjs[objnum++] = (t_oamobj){x, y | 0x0400, 8*iconn };
}

// Selection bar for the file lists: LIST_ROWH pixels tall, made of two overlapping
// 16x8 sprites per column (tile 63 is a solid color block). Only the top-most
// object pixel gets blended, so the overlap does not get blended twice.
#define OBJ_SHAPE_WIDE   0x4000       // Shape 1 (16x8 with size 0), kept in the Y word
static void render_list_selector(int rowtop) {
  for (unsigned i = 0; i < SCREEN_WIDTH; i += 16) {
    fobjs[objnum++] = (t_oamobj){i, rowtop | 0x0400 | OBJ_SHAPE_WIDE, 8*63 };
    fobjs[objnum++] = (t_oamobj){i, (rowtop + LIST_ROWH - 8) | 0x0400 | OBJ_SHAPE_WIDE, 8*63 };
  }
}

// Guess the file type based on the file name.
static unsigned guessicon(const char *path) {
  const char *ext = find_extension(path);

  if (ext) {
    static const struct {
      const char *ext;
      unsigned icon;
    } exticon[] = {
      {"gba", ICON_GBACART},
      {"gb",  ICON_GBCART},
      {"gbc", ICON_GBCCART},
      {"nes", ICON_NESCART},
      {"sms", ICON_SMSCART},
      {"gg",  ICON_GGCART},
      {"sg",  ICON_SGCART},
      {"sv",  ICON_SVCART},
      {"ngp", ICON_NGPCART},
      {"ngc", ICON_NGCCART},
      {"pce", ICON_PCECART},
      {"fw",  ICON_UPDFILE},
    };
    for (unsigned i = 0; i < sizeof(exticon)/sizeof(exticon[0]); i++)
      if (!strcasecmp(exticon[i].ext, ext))
        return exticon[i].icon;
  }

  return ICON_BINFILE;
}

// Draws text adding some support for overflow.
#define THREEDOTS_WIDTH  9
static void draw_text_ovf(const char *t, volatile uint8_t *frame, unsigned x, unsigned y, unsigned maxw) {
  clip_text((int)y);
  uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x];
  unsigned twidth = font_width(t);
  if (twidth <= maxw)
    draw_text_idx8_bus16(t, basept, SCREEN_WIDTH, FT_COLOR);
  else {
    char tmpbuf[256];
    unsigned numchars = font_width_cap(t, maxw - THREEDOTS_WIDTH);
    memcpy(tmpbuf, t, numchars);
    memcpy(&tmpbuf[numchars], "...", 4);
    draw_text_idx8_bus16(tmpbuf, basept, SCREEN_WIDTH, FT_COLOR);
  }
}

static void draw_text_leftovf(const char *t, volatile uint8_t *frame, unsigned x, unsigned y, unsigned maxw) {
  clip_text((int)y);
  uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x];
  unsigned numchars = font_width_lcap(t, maxw - THREEDOTS_WIDTH);
  if (numchars) {
    draw_text_idx8_bus16("...", basept, SCREEN_WIDTH, FT_COLOR);
    draw_text_idx8_bus16(&t[numchars], basept + THREEDOTS_WIDTH, SCREEN_WIDTH, FT_COLOR);
  } else {
    draw_text_idx8_bus16(t, basept, SCREEN_WIDTH, FT_COLOR);
  }
}

static void draw_text_ovf_rotate(const char *t, volatile uint8_t *frame, unsigned x, unsigned y, unsigned maxw, unsigned *franim) {
  clip_text((int)y);
  uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x];
  unsigned twidth = font_width(t);
  if (twidth <= maxw)
    draw_text_idx8_bus16(t, basept, SCREEN_WIDTH, FT_COLOR);
  else {
    unsigned anim = *franim > ANIM_INITIAL_WAIT ? (*franim - ANIM_INITIAL_WAIT) >> 4 : 0;

    // Wrap around once the text end reaches the mid point aprox.
    char tmpbuf[540];
    strcpy(tmpbuf, t);
    strcat(tmpbuf, "      ");
    unsigned pixw = font_width(tmpbuf);
    if (anim > pixw)
      *franim = ANIM_INITIAL_WAIT + ((anim - pixw) << 4);
    strcat(tmpbuf, t);

    draw_text_idx8_bus16_range(tmpbuf, basept, anim, maxw, SCREEN_WIDTH, FT_COLOR);
  }
}

static void draw_box_outline(volatile uint8_t *frame, unsigned left, unsigned right, unsigned top, unsigned bottom, uint8_t color) {
  dma_memset16(&frame[SCREEN_WIDTH * top + left], dup8(color), (right - left) / 2);
  dma_memset16(&frame[SCREEN_WIDTH * (top + 1) + left], dup8(color), (right - left) / 2);
  dma_memset16(&frame[SCREEN_WIDTH * (bottom - 1) + left], dup8(color), (right - left) / 2);
  dma_memset16(&frame[SCREEN_WIDTH * (bottom - 2) + left], dup8(color), (right - left) / 2);
  while (top < bottom) {
    *((uint16_t*)&frame[SCREEN_WIDTH * top + left]) = dup8(color);
    *((uint16_t*)&frame[SCREEN_WIDTH * top + right - 2]) = dup8(color);
    top++;
  }
}

static void draw_box_full(
  volatile uint8_t *frame, unsigned left, unsigned right, unsigned top, unsigned bottom,
  uint8_t outlinecolor, uint8_t bgcolor
) {
  draw_box_outline(frame, left, right, top, bottom, outlinecolor);
  for (unsigned i = top + 2; i < bottom - 2; i++)
    dma_memset16(&frame[SCREEN_WIDTH * i + left + 2], dup8(bgcolor), (right - left - 4) / 2);
}

static void draw_button_box(
  volatile uint8_t *frame, unsigned left, unsigned right, unsigned top, unsigned bottom, bool selected
) {
  if (selected)
    draw_box_full(frame, left, right, top, bottom, FG_COLOR, HI_COLOR);
  else
    draw_box_outline(frame, left, right, top, bottom, FG_COLOR);
}


static void draw_rightj_text(const char *t, volatile uint8_t *frame, unsigned x, unsigned y) {
  clip_text((int)y);
  unsigned twidth = font_width(t);
  uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x - twidth];
  draw_text_idx8_bus16(t, basept, SCREEN_WIDTH, FT_COLOR);
}

static void draw_central_text(const char *t, volatile uint8_t *frame, unsigned x, unsigned y) {
  clip_text((int)y);
  unsigned twidth = font_width(t);
  uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x - twidth / 2];
  draw_text_idx8_bus16(t, basept, SCREEN_WIDTH, FT_COLOR);
}

static void draw_central_text_ovf(const char *t, volatile uint8_t *frame, unsigned x, unsigned y, unsigned maxw) {
  clip_text((int)y);
  unsigned twidth = font_width(t);
  if (twidth <= maxw) {
    uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x - twidth / 2];
    draw_text_idx8_bus16(t, basept, SCREEN_WIDTH, FT_COLOR);
  } else {
    char tmpbuf[256];
    unsigned numchars = font_width_cap(t, maxw - THREEDOTS_WIDTH);
    memcpy(tmpbuf, t, numchars);
    memcpy(&tmpbuf[numchars], "...", 4);
    uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x - maxw / 2];
    draw_text_idx8_bus16(tmpbuf, basept, SCREEN_WIDTH, FT_COLOR);
  }
}

static void draw_central_text_wrapped(const char *t, volatile uint8_t *frame, unsigned x, unsigned y, unsigned maxw) {
  while (*t) {
    char tmp[128];
    unsigned outw;
    clip_text((int)y);
    unsigned charcnt = font_width_cap_space(t, maxw, &outw);
    const bool nobreak = !charcnt;          // No space to break at (chinese): break anywhere
    if (nobreak) {
      charcnt = font_width_cap(t, maxw);
      if (!charcnt)
        charcnt = utf8_chlen(t);
    }
    if (charcnt > sizeof(tmp) - 1)
      charcnt = sizeof(tmp) - 1;

    memcpy(tmp, t, charcnt);
    tmp[charcnt] = 0;
    if (nobreak)
      outw = font_width(tmp);
    uint8_t *basept = (uint8_t*)&frame[y * SCREEN_WIDTH + x - outw / 2];
    draw_text_idx8_bus16(tmp, basept, SCREEN_WIDTH, FT_COLOR);

    t += charcnt;      // Advance text
    while (*t == ' ')
      t++;
    y += 16;           // Move down in the buffer
  }
}

// ---- Corner windows --------------------------------------------------------
// The pop-ups opened from the START menu are windows anchored to the bottom
// right corner of the 240x160 screen. All x coordinates/widths are even since
// we fill the frame buffer 16 bits at a time.
#define CW_LEFT     48                          // (old corner windows, still used for sizing)
#define CW_RIGHT    238
#define CW_TOP      34
#define CW_BOTTOM   158
#define CW_CX       144
#define CW_ROWH     14                          // Row pitch: 12 pixel high bars plus 2 pixels of gap
#define CW_ROWS     7                           // Visible rows
// System window (START): a row of tabs (settings groups, tools, info), the rows and a help bar
#define SYS_LEFT    4
#define SYS_CX      ((SYS_LEFT + CW_RIGHT) / 2)
#define SW_TOP      24
#define SW_TAB_Y    (SW_TOP + 2)
#define SW_TAB_H    14
#define SW_ROW0Y    (SW_TAB_Y + SW_TAB_H + 2)
#define SW_HELPY    (SW_ROW0Y + CW_ROWS * CW_ROWH + 2)
#define CW_ROW0Y    SW_ROW0Y                    // First row
#define CW_ROWW     (CW_RIGHT - SYS_LEFT - 8)   // Row width

static void fill_rect(volatile uint8_t *frame, unsigned x, unsigned y, unsigned w, unsigned h, uint8_t color) {
  for (unsigned r = 0; r < h; r++) {
    const int yy = (int)(y + r);
    if (yy >= clip_y0 && yy < clip_y1)
      dma_memset16(&frame[(unsigned)yy * SCREEN_WIDTH + x], dup8(color), w / 2);
  }
}

// A box made of rectangles (so it follows the clip rectangle): 2 pixel outline, optional filling.
static void box_clipped(volatile uint8_t *frame, unsigned left, unsigned right, int top, int bottom,
                        uint8_t outline, int fill /* -1: transparent */) {
  fill_rect(frame, left, top, right - left, 2, outline);
  fill_rect(frame, left, bottom - 2, right - left, 2, outline);
  fill_rect(frame, left, top + 2, 2, bottom - top - 4, outline);
  fill_rect(frame, right - 2, top + 2, 2, bottom - top - 4, outline);
  if (fill >= 0)
    fill_rect(frame, left + 2, top + 2, right - left - 4, bottom - top - 4, fill);
}

static void draw_rightj_ovf(const char *t, volatile uint8_t *frame, unsigned xr, unsigned y, unsigned maxw) {
  if (font_width(t) <= maxw)
    draw_rightj_text(t, frame, xr, y);
  else
    draw_text_ovf(t, frame, xr - maxw, y, maxw);
}

// ---- File lists ---------------------------------------------------------------------------------
#define LIST_SCROLL_W   4        // Space reserved on the right for the scroll bar

static unsigned tabbar_end = 100;   // X where the page tabs end (set by render_tabbar)

// Draws one row: top is the Y of the row, inside tells if it is fully visible (sprites can not be clipped)
typedef void (*t_row_fn)(volatile uint8_t *frame, int idx, int top, bool sel, bool inside, unsigned sbw);

// Renders the visible rows of a list, with a selection bar that glides to the selected row and
// rows that glide when the list scrolls.
static void render_list(volatile uint8_t *frame, unsigned tab, int selector, int seloff, int total, t_row_fn rowfn) {
  static unsigned last_tab = 0xFF;
  const int rh16 = LIST_ROWH * 16;
  const int rows = lay.rows;
  const int list_bot = LIST_Y0 + rows * LIST_ROWH;
  const unsigned sbw = total > rows ? LIST_SCROLL_W : 0;

  if (!listanim[tab].valid || tab != last_tab) {
    anim_snap(&listanim[tab].sel, selector * rh16);
    anim_snap(&listanim[tab].scr, seloff * rh16);
    listanim[tab].valid = true;
  } else {
    anim_to(&listanim[tab].sel, selector * rh16);
    anim_to(&listanim[tab].scr, seloff * rh16);
    anim_step(&listanim[tab].sel);
    anim_step(&listanim[tab].scr);
  }
  last_tab = tab;

  const int scr16 = listanim[tab].scr.cur;
  const int base = scr16 / rh16;                 // First (maybe partially) visible entry
  const int frac = (scr16 % rh16) / 16;          // Pixels the list is scrolled up by

  clip_y0 = LIST_Y0;
  clip_y1 = list_bot;
  for (int k = 0; k <= rows; k++) {
    const int idx = base + k;
    if (idx >= total)
      break;
    const int top = LIST_Y0 + k * LIST_ROWH - frac;
    rowfn(frame, idx, top, idx == selector, top >= LIST_Y0 && top + LIST_ROWH <= list_bot, sbw);
  }
  clip_y0 = 0;
  clip_y1 = SCREEN_HEIGHT;

  // Selection bar
  int sy = LIST_Y0 + (listanim[tab].sel.cur - scr16) / 16;
  if (sy < LIST_Y0)
    sy = LIST_Y0;
  if (sy > list_bot - LIST_ROWH)
    sy = list_bot - LIST_ROWH;
  render_list_selector(sy);

  // Scroll bar
  if (sbw) {
    const int track = rows * LIST_ROWH;
    int th = track * rows / total;
    if (th < 8)
      th = 8;
    const int maxscr16 = (total - rows) * rh16;
    int pos = maxscr16 > 0 ? (track - th) * scr16 / maxscr16 : 0;
    if (pos < 0)
      pos = 0;
    if (pos > track - th)
      pos = track - th;
    fill_rect(frame, SCREEN_WIDTH - 4, LIST_Y0, 2, track, FG_COLOR);
    fill_rect(frame, SCREEN_WIDTH - 4, LIST_Y0 + pos, 2, th, HI_COLOR);
  }
}

static void recent_row(volatile uint8_t *frame, int idx, int top, bool sel, bool inside, unsigned sbw) {
  t_rentry *e = &sdr_state->rentries[idx];
  char *fn = &e->fpath[e->fname_offset];
  const int ty = top - lay.cell_dy;
  if (inside)
    render_icon(2, ty, guessicon(fn));
  if (sel)
    draw_text_ovf_rotate(fn, frame, 20, ty, SCREEN_WIDTH - 24 - sbw, &smenu.anim_state);
  else
    draw_text_ovf(fn, frame, 20, ty, SCREEN_WIDTH - 24 - sbw);
}

static void browser_row(volatile uint8_t *frame, int idx, int top, bool sel, bool inside, unsigned sbw) {
  char szstr[16] = {0};
  t_centry *e = sdr_state->filefilter[idx];
  const int ty = top - lay.cell_dy;

  unsigned iconidx = ishidden(e)        ? ((e->attr & AM_DIR) ? ICON_HFOLDER : ICON_HFILE) :
                     (e->attr & AM_DIR) ? ICON_FOLDER :
                     guessicon(e->fname);
  if (inside)
    render_icon(2, ty, iconidx);

  if (!(e->attr & AM_DIR)) {
    human_size(szstr, sizeof(szstr), e->filesize);
    draw_rightj_text(szstr, frame, SCREEN_WIDTH - 2 - sbw, ty);
  }

  // Animate the row entries if they are too long!
  if (sel)
    draw_text_ovf_rotate(e->fname, frame, 20, ty, SCREEN_WIDTH - 26 - sbw - font_width(szstr), &smenu.anim_state);
  else
    draw_text_ovf(e->fname, frame, 20, ty, SCREEN_WIDTH - 26 - sbw - font_width(szstr));
}

#ifdef SUPPORT_NORGAMES
static void nor_row(volatile uint8_t *frame, int idx, int top, bool sel, bool inside, unsigned sbw) {
  const t_flash_game_entry *e = &sdr_state->nordata.games[idx];
  const int ty = top - lay.cell_dy;
  if (inside)
    render_icon(2, ty, ICON_GBACART);

  char szstr[16];
  human_size(szstr, sizeof(szstr), e->numblks * NOR_BLOCK_SIZE);
  draw_rightj_text(szstr, frame, SCREEN_WIDTH - 2 - sbw, ty);

  const char *romname = &e->game_name[e->bnoffset];
  if (sel)
    draw_text_ovf_rotate(romname, frame, 20, ty, SCREEN_WIDTH - 26 - sbw - font_width(szstr), &smenu.anim_state);
  else
    draw_text_ovf(romname, frame, 20, ty, SCREEN_WIDTH - 26 - sbw - font_width(szstr));
}
#endif

// A (drawn) folder and two lines of text, for lists without entries.
static void draw_empty_state(volatile uint8_t *frame, const char *line1, const char *line2) {
  const unsigned cx = SCREEN_WIDTH / 2;
  fill_rect(frame, cx - 16, 44, 14, 6, FG_COLOR);
  fill_rect(frame, cx - 16, 48, 32, 24, FG_COLOR);
  fill_rect(frame, cx - 16, 56, 32, 16, HI_COLOR);
  draw_central_text_ovf(line1, frame, cx, 78, SCREEN_WIDTH - 24);
  draw_central_text_ovf(line2, frame, cx, 94, SCREEN_WIDTH - 24);
}

// Bottom bar: name of the selected entry on the left (scrolls if it is too long), key hints on the right.
static void render_list_footer(volatile uint8_t *frame, const char *name, unsigned hint_msg) {
  dma_memset16(&frame[240*LIST_FOOTER_Y], dup8(FG_COLOR), 240*LIST_FOOTER_H/2);
  const char *hint = msgs[lang_id][hint_msg];
  const unsigned hw = font_width(hint);
  draw_rightj_text(hint, frame, SCREEN_WIDTH - 4, LIST_FOOTER_TXT_Y);
  if (name)
    draw_text_ovf_rotate(name, frame, 6, LIST_FOOTER_TXT_Y, SCREEN_WIDTH - 20 - hw, &smenu.anim_state);
}

// Top bar, right of the tabs: some text (path, NOR usage) and the position in the list.
static void render_topbar_info(volatile uint8_t *frame, const char *left, const char *counter) {
  const unsigned cw = counter ? font_width(counter) : 0;
  if (counter)
    draw_rightj_text(counter, frame, SCREEN_WIDTH - 4, 0);
  if (left) {
    const unsigned x0 = tabbar_end + 6;
    const unsigned used = x0 + cw + (cw ? 10 : 4);
    if (SCREEN_WIDTH > used + 24)
      draw_text_leftovf(left, frame, x0, 0, SCREEN_WIDTH - used);
  }
}

void render_recent(volatile uint8_t *frame) {
  const int total = smenu.recent.maxentries;
  const char *name = NULL;
  char cnt[16] = "";

  if (!total)
    draw_empty_state(frame, msgs[lang_id][MSG_EMPTY_RECENT], msgs[lang_id][MSG_EMPTY_H_RECENT]);
  else {
    render_list(frame, MENUTAB_RECENT, smenu.recent.selector, smenu.recent.seloff, total, recent_row);
    if (smenu.recent.selector >= 0 && smenu.recent.selector < total) {
      const t_rentry *e = &sdr_state->rentries[smenu.recent.selector];
      name = &e->fpath[e->fname_offset];
    }
    npf_snprintf(cnt, sizeof(cnt), "%d/%d", smenu.recent.selector + 1, total);
  }
  render_list_footer(frame, name, MSG_HINT_RECENT);
  render_topbar_info(frame, NULL, cnt[0] ? cnt : NULL);
}

#ifdef SUPPORT_NORGAMES
void render_flashbrowser(volatile uint8_t *frame) {
  const int total = smenu.fbrowser.maxentries;
  const char *name = NULL;
  char cnt[16] = "";

  if (!total)
    draw_empty_state(frame, msgs[lang_id][MSG_NOR_EMPTY], msgs[lang_id][MSG_EMPTY_H_NOR]);
  else {
    render_list(frame, MENUTAB_NORBROWSE, smenu.fbrowser.selector, smenu.fbrowser.seloff, total, nor_row);
    if (smenu.fbrowser.selector >= 0 && smenu.fbrowser.selector < total) {
      const t_flash_game_entry *e = &sdr_state->nordata.games[smenu.fbrowser.selector];
      name = &e->game_name[e->bnoffset];
    }
    npf_snprintf(cnt, sizeof(cnt), "%d/%d", smenu.fbrowser.selector + 1, total);
  }
  render_list_footer(frame, name, MSG_HINT_NOR);

  char tmp[40], tmp1[16], tmp2[16];
  human_size(tmp1, sizeof(tmp1), smenu.fbrowser.usedblks * NOR_BLOCK_SIZE);
  human_size(tmp2, sizeof(tmp2), NOR_GAMEBLOCK_COUNT * NOR_BLOCK_SIZE);
  npf_snprintf(tmp, sizeof(tmp), "NOR %s/%s", tmp1, tmp2);
  render_topbar_info(frame, tmp, cnt[0] ? cnt : NULL);
}
#endif

void render_browser(volatile uint8_t *frame) {
  const int total = smenu.browser.dispentries;
  const char *name = NULL;
  char cnt[16] = "";

  if (!total)
    draw_empty_state(frame, msgs[lang_id][MSG_BROW_EMPTY], msgs[lang_id][MSG_EMPTY_H_DIR]);
  else {
    render_list(frame, MENUTAB_ROMBROWSE, smenu.browser.selector, smenu.browser.seloff, total, browser_row);
    if (smenu.browser.selector >= 0 && smenu.browser.selector < total)
      name = sdr_state->filefilter[smenu.browser.selector]->fname;
    npf_snprintf(cnt, sizeof(cnt), "%d/%d", smenu.browser.selector + 1, total);
  }
  render_list_footer(frame, name, MSG_HINT_BROWSE);
  render_topbar_info(frame, smenu.browser.cpath, cnt[0] ? cnt : NULL);
}

void render_fw_flash_popup(volatile uint8_t *frame) {
  // Render a box to give a pop-up feeling
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  draw_central_text(msgs[lang_id][MSG_FWUPD_MENU], frame, 120, 30);

  draw_box_outline(frame, 16, 224, 64, 92, FG_COLOR);
  if (spop.p.update.issfw) {
    char tmp[32];
    npf_snprintf(tmp, sizeof(tmp), "SuperFW (ver %lu.%lu)",
                 spop.p.update.superfw_ver >> 16,
                 spop.p.update.superfw_ver & 0xFFFF);
    draw_central_text(tmp, frame, 120, 70);
  } else {
    draw_central_text(msgs[lang_id][MSG_FWUPD_UNK], frame, 120, 70);
  }

  const char *smsg[] = {
    msgs[lang_id][MSG_FWUPD_GO],
    msgs[lang_id][MSG_FWUPD_LOADING],
    msgs[lang_id][MSG_FWUPD_CHECKING],
    msgs[lang_id][MSG_FWUPD_ERASING],
    msgs[lang_id][MSG_FWUPD_PROGRAM],
  };

  draw_central_text(smsg[spop.p.update.curr_state], frame, 120, 120);
}

void render_sav_menu_popup(volatile uint8_t *frame) {
  // Render a box to give a pop-up feeling
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  for (unsigned i = 0; i < 3; i++) {
    draw_button_box(frame, 20, 220, 32 + 28 * i, 32 + 28 * i + 20, spop.selector == i);
    draw_central_text(msgs[lang_id][MSG_SAVOPT_OPT0 + i], frame, 120, 34 + 28 * i);
  }
  draw_button_box(frame, 20, 220, 124, 144, spop.selector == SavQuit);
  draw_central_text(msgs[lang_id][MSG_CANCEL], frame, 120, 126);
}

static void render_gbarom_info(volatile uint8_t *frame, const char *dispname,
                               bool issf, const char *gcode, uint8_t ver, int save_type) {
  char tmp[64];
  draw_central_text(msgs[lang_id][MSG_GBALOAD_MINFO], frame, SCREEN_WIDTH/2, 23);

  const char *romname = file_basename(dispname);
  unsigned twidth = font_width(romname);
  if (twidth > SCREEN_WIDTH - 20)
    draw_text_ovf_rotate(romname, frame, 10, 52,
                         SCREEN_WIDTH - 20, &spop.anim);
  else
    draw_central_text_ovf(romname, frame, SCREEN_WIDTH/2, 52, SCREEN_WIDTH - 20);

  npf_snprintf(tmp, sizeof(tmp), msgs[lang_id][MSG_LOADINFO_GAME], gcode, ver);
  draw_central_text_ovf(tmp, frame, SCREEN_WIDTH/2, 82, SCREEN_WIDTH - 20);

  if (save_type < 0)
    draw_central_text_ovf(msgs[lang_id][MSG_LOADINFO_UNKW], frame, SCREEN_WIDTH/2, 102, SCREEN_WIDTH - 20);
  else if (issf)
    draw_central_text_ovf("SuperFW firmware", frame, SCREEN_WIDTH/2, 102, SCREEN_WIDTH - 20);
  else {
    const char *stype[] = {
      msgs[lang_id][MSG_SAVETYPE_NONE],       // SaveTypeNone
      msgs[lang_id][MSG_SAVETYPE_SRAM],       // SaveTypeSRAM
      msgs[lang_id][MSG_SAVETYPE_EEPROM],     // SaveTypeEEPROM4K
      msgs[lang_id][MSG_SAVETYPE_EEPROM],     // SaveTypeEEPROM64K
      msgs[lang_id][MSG_SAVETYPE_FLASH],      // SaveTypeFlash512K
      msgs[lang_id][MSG_SAVETYPE_FLASH],      // SaveTypeFlash1024K
    };
    const char *ssize[] = {
      "0KB",       // SaveTypeNone
      "32KB",      // SaveTypeSRAM
      "0.5KB",     // SaveTypeEEPROM4K
      "8KB",       // SaveTypeEEPROM64K
      "64KB",      // SaveTypeFlash512K
      "128KB",     // SaveTypeFlash1024K
    };

    npf_snprintf(tmp, sizeof(tmp), msgs[lang_id][MSG_LOADINFO_SAVE], stype[save_type], ssize[save_type]);
    draw_central_text_ovf(tmp, frame, SCREEN_WIDTH/2, 102, SCREEN_WIDTH - 20);
  }

  draw_box_full(frame, 20, 220, 132, 152, FG_COLOR, HI_COLOR);
}

static const char *render_gbarom_patching(volatile uint8_t *frame, const t_load_gba_info *info, int selector) {
  draw_central_text(msgs[lang_id][MSG_GBALOAD_MPATCH], frame, SCREEN_WIDTH/2, 23);
  draw_text_ovf(msgs[lang_id][MSG_DEFS_PATCH], frame, 12, 44, 224);
  draw_central_text(msgs[lang_id][MSG_PATCH_TYPE0 + info->patch_type], frame, 162, 44);
  draw_text_ovf(msgs[lang_id][MSG_LOADER_SAVET], frame, 12, 62, 224);
  draw_central_text(msgs[lang_id][MSG_LOADER_ST0 + (info->use_dsaving ? 0 : 1)], frame, 170, 62);
  draw_text_ovf(msgs[lang_id][MSG_LOADER_MENU], frame, 12, 80, 224);
  draw_central_text(msgs[lang_id][info->ingame_menu_enabled ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED], frame, 170, 80);
  draw_text_ovf(msgs[lang_id][MSG_LOADER_RTCE], frame, 12, 98, 224);
  draw_central_text(msgs[lang_id][info->rtc_patch_enabled ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED], frame, 170, 98);

  draw_text_ovf(msgs[lang_id][MSG_LOADER_PTCH], frame, 12, 116, 224);
  draw_box_outline(frame, 170 - 20, 170 + 20, 115, 133, FG_COLOR);
  draw_central_text("▸", frame, 170, 116);

  return (selector == GBALoadPatch) ? msgs[lang_id][MSG_PATCH_TYPE_I0 + info->patch_type] :
         (selector == GBASavePatch) ? msgs[lang_id][MSG_LOADER_ST_I0 + (info->use_dsaving ? 0 : 1)] :
         (selector == GBAInGameMen) ? msgs[lang_id][MSG_INGAME_I] :
         (selector == GBARTCPatch)  ? msgs[lang_id][MSG_PATCHRTC_I] :
         (selector == GBAPatchGen)  ? msgs[lang_id][MSG_PATCHE_I] : NULL;
}

static const char *render_gbarom_loading(volatile uint8_t *frame, const t_load_gba_lcfg *data, bool rtc_patching, int selector) {
  char tmp[64];
  draw_central_text(msgs[lang_id][MSG_GBALOAD_OPTS], frame, SCREEN_WIDTH/2, 23);
  draw_text_ovf(msgs[lang_id][MSG_LOADER_LOADP], frame, 12, 44, 224);
  draw_central_text(msgs[lang_id][MSG_LOADER_LOADP0 + data->sram_load_type], frame, 170, 44);
  draw_text_ovf(msgs[lang_id][MSG_LOADER_SAVEP], frame, 12, 62, 224);
  draw_central_text(msgs[lang_id][MSG_LOADER_SAVEP0 + data->sram_save_type], frame, 170, 62);
  draw_text_ovf(msgs[lang_id][MSG_DEF_RTCVAL], frame, 12, 80, 224);
  if (rtc_patching) {
    t_dec_date d;
    timestamp2date(data->rtcval, &d);
    npf_snprintf(tmp, sizeof(tmp), "20%02d/%02d/%02d %02d:%02d",
      d.year, d.month, d.day, d.hour, d.min);
    draw_central_text(tmp, frame, 170, 80);
  }
  else
    draw_central_text("-", frame, 170, 80);
  draw_text_ovf(msgs[lang_id][MSG_SETT_LDCHT], frame, 12, 98, 224);
  draw_central_text(msgs[lang_id][data->use_cheats ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED], frame, 170, 98);

  draw_box_outline(frame, 170 - 20, 170 + 20, 115, 133, FG_COLOR);
  draw_text_ovf(msgs[lang_id][MSG_SETT_REMEMB], frame, 12, 116, 224);
  render_icon(170-8, 116, ICON_DISK);

  return (selector == GBALdSetLoadP) ? msgs[lang_id][MSG_LOADER_LOADP_I0 + data->sram_load_type] :
         (selector == GBALdSetSaveP) ? msgs[lang_id][MSG_LOADER_SAVEP_I0 + data->sram_save_type] :
         (selector == GBALdSetCheats && !enable_cheats) ? msgs[lang_id][MSG_CHEATSDIS_I] :
         (selector == GBALdSetCheats && !data->cheats_found) ? msgs[lang_id][MSG_CHEATSNOA_I] :
         (selector == GBALdRemember) ? msgs[lang_id][MSG_REMEMB_I] : NULL;
}

void render_gba_load_popup(volatile uint8_t *frame) {
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);
  draw_text_ovf("⯇", frame, 10, 23, 64);
  draw_rightj_text("⯈", frame, SCREEN_WIDTH - 10, 23);

  const t_load_gba_info *info = &spop.p.load.i;
  const t_patch *p = get_game_patch(info);
  const char *ht = NULL;
  switch (spop.submenu) {
  case GbaLoadPopInfo:
    render_gbarom_info(frame, info->romfn, is_superfw(&info->romh), info->gcode,
                       info->romh.version, p ? p->save_mode : -1);
    draw_central_text(msgs[lang_id][MSG_LOAD_GBA], frame, 120, 134);
    break;
  case GbaLoadPopLoadS:
    ht = render_gbarom_loading(frame, &spop.p.load.l, info->rtc_patch_enabled, spop.selector);
    break;
  case GbaLoadPopPatch:
    ht = render_gbarom_patching(frame, &spop.p.load.i, spop.selector);
    break;
  };

  // Show some help if necessary
  if (ht) {
    unsigned twidth = font_width(ht);
    if (twidth > SCREEN_WIDTH - 20)
      draw_text_ovf_rotate(ht, frame, 10, 137, SCREEN_WIDTH - 20, &spop.anim);
    else
      draw_central_text_ovf(ht, frame, SCREEN_WIDTH/2, 137, SCREEN_WIDTH - 20);
  }

  if (spop.submenu != GbaLoadPopInfo) {
    const unsigned offy = 43;
    for (unsigned i = 8; i < 232; i += 16) {
      render_icon_trans(i, offy + 0 + spop.selector * 18, 63);
      render_icon_trans(i, offy + 2 + spop.selector * 18, 63);
    }
  }
}

void render_filemgr(volatile uint8_t *frame) {
  // Draw the file name and the options available
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  t_centry *e = sdr_state->filefilter[smenu.browser.selector];
  const char *bn = file_basename(e->fname);

  unsigned twidth = font_width(bn);
  if (twidth > SCREEN_WIDTH - 20)
    draw_text_ovf_rotate(bn, frame, 10, 32, SCREEN_WIDTH - 20, &spop.anim);
  else
    draw_central_text_ovf(bn, frame, SCREEN_WIDTH/2, 32, SCREEN_WIDTH - 20);

  for (unsigned i = 0; i < FiMgrCNT; i++)
    draw_button_box(frame, 20, 220, 60 + i*30, 80 + i*30, i == spop.selector);

  draw_central_text(msgs[lang_id][MSG_FMGR_DEL], frame, 120, 62 + 30*FiMgrDelete);
  draw_central_text(msgs[lang_id][(e->attr & AM_HID) ? MSG_FMGR_UNHIDE : MSG_FMGR_HIDE], frame, 120, 62 + 30*FiMgrHide);

  #ifdef SUPPORT_NORGAMES
  draw_central_text(msgs[lang_id][MSG_NOR_WRITE], frame, 120, 62 + 30*FiMgrWriteNOR);
  #endif
}


#ifdef SUPPORT_NORGAMES
void render_gba_norwrite(volatile uint8_t *frame) {
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  draw_text_ovf("⯇", frame, 10, 23, 64);
  draw_rightj_text("⯈", frame, SCREEN_WIDTH - 10, 23);

  if (spop.submenu == GbaLoadPopInfo) {
    const t_load_gba_info *info = &spop.p.norwr.i;
    const t_patch *p = get_game_patch(info);
    render_gbarom_info(frame, info->romfn, is_superfw(&info->romh),
                       info->gcode, info->romh.version, p ? p->save_mode : -1);
    draw_central_text(msgs[lang_id][MSG_NOR_WRITE], frame, 120, 134);
  } else {
    const char *ht = render_gbarom_patching(frame, &spop.p.norwr.i, spop.selector);
    if (ht) {
      unsigned twidth = font_width(ht);
      if (twidth > SCREEN_WIDTH - 20)
        draw_text_ovf_rotate(ht, frame, 10, 137, SCREEN_WIDTH - 20, &spop.anim);
      else
        draw_central_text_ovf(ht, frame, SCREEN_WIDTH/2, 137, SCREEN_WIDTH - 20);
    }
    const unsigned offy = 43;
    for (unsigned i = 8; i < 232; i += 16) {
      render_icon_trans(i, offy + 0 + spop.selector * 18, 63);
      render_icon_trans(i, offy + 2 + spop.selector * 18, 63);
    }
  }
}

void render_gba_norload(volatile uint8_t *frame) {
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  draw_text_ovf("⯇", frame, 10, 23, 64);
  draw_rightj_text("⯈", frame, SCREEN_WIDTH - 10, 23);

  const t_flash_game_entry *e = spop.p.norld.e;
  if (spop.submenu == GbaLoadPopInfo) {
    int save_type = GET_GATTR_SAVEM(e->gattrs);
    char gcode[5] = {
      e->gamecode & 0xFF, (e->gamecode >> 8) & 0xFF, (e->gamecode >> 16) & 0xFF, e->gamecode >> 24, 0
    };
    render_gbarom_info(frame, e->game_name, false, gcode, e->gamever, save_type);
    draw_central_text(msgs[lang_id][MSG_NOR_LAUNCH], frame, 120, 134);
  } else {
    bool rtc_patching = e->gattrs & GATTR_RTC;
    const char *ht = render_gbarom_loading(frame, &spop.p.norld.l, rtc_patching, spop.selector);
    if (ht) {
      unsigned twidth = font_width(ht);
      if (twidth > SCREEN_WIDTH - 20)
        draw_text_ovf_rotate(ht, frame, 10, 137, SCREEN_WIDTH - 20, &spop.anim);
      else
        draw_central_text_ovf(ht, frame, SCREEN_WIDTH/2, 137, SCREEN_WIDTH - 20);
    }
    const unsigned offy = 43;
    for (unsigned i = 8; i < 232; i += 16) {
      render_icon_trans(i, offy + 0 + spop.selector * 18, 63);
      render_icon_trans(i, offy + 2 + spop.selector * 18, 63);
    }
  }
}
#endif

void render_popupq(volatile uint8_t *frame, unsigned fcnt) {
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  // Draw question and two buttons
  draw_central_text_wrapped(spop.qpop.message, frame, SCREEN_WIDTH/2, 32, SCREEN_WIDTH - 20);

  if (spop.qpop.option == 0) {
    draw_box_full(frame, 20, 220, 90, 90 + 20, FG_COLOR, HI_COLOR);
    draw_box_outline(frame, 20, 220, 120, 120 + 20, FG_COLOR);
  } else {
    draw_box_full(frame, 20, 220, 120, 120 + 20, FG_COLOR, HI_COLOR);
    draw_box_outline(frame, 20, 220, 90, 90 + 20, FG_COLOR);
  }

  draw_central_text(spop.qpop.default_button, frame, 120, 92);
  draw_central_text(spop.qpop.confirm_button, frame, 120, 122);
  draw_central_text(msgs[lang_id][MSG_HINT_Q], frame, 120, 142);
}

void render_rtcpop(volatile uint8_t *frame) {
  draw_box_outline(frame, 2, 240-2, 18, 158, FG_COLOR);

  draw_central_text(msgs[lang_id][MSG_DEF_RTCVAL], frame, SCREEN_WIDTH/2, 32);

  const t_dec_date *v = &spop.rtcpop.val;
  char thour[3] = {'0' + v->hour /10, '0' + v->hour  % 10, 0};
  char tmins[3] = {'0' + v->min  /10, '0' + v->min   % 10, 0};
  char tdays[3] = {'0' + v->day  /10, '0' + v->day   % 10, 0};
  char tmont[3] = {'0' + v->month/10, '0' + v->month % 10, 0};
  char tyear[5] = {'2', '0', '0' + v->year/10, '0' + v->year % 10, 0};

  draw_central_text(tyear, frame,  60, 70);
  draw_central_text("-",   frame,  80, 70);
  draw_central_text(tmont, frame,  94, 70);
  draw_central_text("-",   frame, 106, 70);
  draw_central_text(tdays, frame, 120, 70);
  draw_central_text(thour, frame, 154, 70);
  draw_central_text(":",   frame, 166, 70);
  draw_central_text(tmins, frame, 180, 70);

  const uint8_t cox[] = {
    60, 94, 120, 154, 180
  };
  draw_central_text("⯅", frame, cox[spop.rtcpop.selector], 54);
  draw_central_text("⯆", frame, cox[spop.rtcpop.selector], 84);
}

// Settings of each group, in order (the save button is always the last row of every group).
static const uint8_t set_groups[SetGrpCNT][8] = {
  { SettUiTheme, SettUiLang,
    #ifdef BUNDLE_FONT10
    SettUiFont,
    #endif
    SettUiHid, 0xFF },
  { SettUiASpd, SettUiAnim, SettUiFx, SettUiSound, 0xFF },
  { SettHotkey, SettBootType, SettFastSD,
    #ifdef SUPPORT_NORGAMES
    SettVerifyNOR,
    #endif
    SettFastEWRAM, SettCheatEn, 0xFF },
  { SettSaveLoc,
    #ifdef SUPPORT_NORGAMES
    SettSaveLocNOR,
    #endif
    SettSaveBkp, SettStateLoc, DefsSavePol, DefsPrefDS, 0xFF },
  { DefsPatchEng, DefsGamMenu, DefsLoadPol, DefsRTCEnb, DefsRTCVal, DefsRTCSpeed, 0xFF },
};

static const uint16_t set_group_msgs[SetGrpCNT] = {
  MSG_SETG_UI, MSG_SETG_FX, MSG_SETG_SYS, MSG_SETG_SAVE, MSG_SETG_GAME,
};

// Number of settings in a group (without the save button).
static unsigned set_group_len(unsigned g) {
  unsigned n = 0;
  while (set_groups[g][n] != 0xFF)
    n++;
  return n;
}

// Setting number for a row of a group (the row after the last setting is the save button).
static unsigned set_row_id(unsigned g, unsigned row) {
  return set_groups[g][row] == 0xFF ? (unsigned)SettSave : set_groups[g][row];
}

static void set_sync_selector() {
  smenu.set.selector = set_row_id(smenu.set.group, smenu.set.idx[smenu.set.group]);
}

// Fills the texts of a row of the settings window.
// Returns 0 for a regular row, 1 for a section title, 2 for the save button.
static int settings_row(unsigned idx, const char **label, const char **value, char *vb, unsigned vbsz) {
  *value = NULL;
  *label = "";
  switch (idx) {
  case SettTitle1:
    *label = msgs[lang_id][MSG_SET_TITL1];
    return 1;
  case SettTitle2:
    *label = msgs[lang_id][MSG_SET_TITL2];
    return 1;
  case SettSave:
    *label = msgs[lang_id][MSG_UIS_SAVE];
    return 2;
  case SettHotkey:
    *label = msgs[lang_id][MSG_SETT_HOTK];
    npf_snprintf(vb, vbsz, "< %s >", hotkey_list[hotkey_combo].cname);
    *value = vb;
    break;
  case SettBootType:
    *label = msgs[lang_id][MSG_SETT_BOOT];
    *value = msgs[lang_id][MSG_BOOT_TYPE0 + boot_bios_splash];
    break;
  case SettFastSD:
    *label = msgs[lang_id][MSG_SETT_FASTSD];
    *value = msgs[lang_id][use_slowld ? MSG_KNOB_DISABLED : MSG_KNOB_ENABLED];
    break;
  #ifdef SUPPORT_NORGAMES
  case SettVerifyNOR:
    *label = msgs[lang_id][MSG_SETT_VERNOR];
    *value = msgs[lang_id][use_verify_nor ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED];
    break;
  #endif
  case SettFastEWRAM:
    *label = msgs[lang_id][MSG_SETT_FASTEW];
    *value = msgs[lang_id][use_fastew ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED];
    break;
  case SettSaveLoc:
    *label = msgs[lang_id][MSG_SETT_SAVET];
    if (save_path_default == SaveRomName)
      *value = msgs[lang_id][MSG_NEXTTO_ROM];
    else {
      npf_snprintf(vb, vbsz, "< %s >", save_paths[save_path_default]);
      *value = vb;
    }
    break;
  #ifdef SUPPORT_NORGAMES
  case SettSaveLocNOR:
    *label = msgs[lang_id][MSG_SETT_SAVETX];
    npf_snprintf(vb, vbsz, "< %s >", save_paths[save_path_nor_default]);
    *value = vb;
    break;
  #endif
  case SettSaveBkp:
    *label = msgs[lang_id][MSG_SETT_SAVEBK];
    npf_snprintf(vb, vbsz, "< %u >", backup_sram_default);
    *value = vb;
    break;
  case SettStateLoc:
    *label = msgs[lang_id][MSG_SETT_STATET];
    npf_snprintf(vb, vbsz, "< %s >", savestates_paths_display[state_path_default]);
    *value = vb;
    break;
  case SettCheatEn:
    *label = msgs[lang_id][MSG_SETT_CHTEN];
    *value = msgs[lang_id][enable_cheats ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED];
    break;
  case DefsPatchEng:
    *label = msgs[lang_id][MSG_DEFS_PATCH];
    *value = msgs[lang_id][MSG_PATCH_TYPE0 + patcher_default];
    break;
  case DefsGamMenu:
    *label = msgs[lang_id][MSG_LOADER_MENU];
    *value = msgs[lang_id][MSG_KNOB_DISABLED + ingamemenu_default];
    break;
  case DefsRTCEnb:
    *label = msgs[lang_id][MSG_LOADER_RTCE];
    *value = msgs[lang_id][MSG_KNOB_DISABLED + rtcpatch_default];
    break;
  case DefsRTCVal: {
    t_dec_date d;
    timestamp2date(rtcvalue_default, &d);
    *label = msgs[lang_id][MSG_DEF_RTCVAL];
    npf_snprintf(vb, vbsz, "%02d/%02d/%02d %02d:%02d", d.year, d.month, d.day, d.hour, d.min);
    *value = vb;
    break;
  }
  case DefsRTCSpeed: {
    unsigned spdmsg = rtcspeed_default ? (MSG_UIS_SPD0 + rtcspeed_default - 1) : MSG_STILLRTC;
    *label = msgs[lang_id][MSG_DEF_SPEED];
    npf_snprintf(vb, vbsz, "< %s >", msgs[lang_id][spdmsg]);
    *value = vb;
    break;
  }
  case DefsLoadPol:
    *label = msgs[lang_id][MSG_LOADER_LOADP];
    *value = msgs[lang_id][MSG_DEF_LOADP0 + (autoload_default ^ 1)];
    break;
  case DefsSavePol:
    *label = msgs[lang_id][MSG_LOADER_SAVEP];
    *value = msgs[lang_id][autosave_default ? MSG_DEF_SAVEP0 : MSG_DEF_SAVEP1];
    break;
  case DefsPrefDS:
    *label = msgs[lang_id][MSG_LOADER_PREFDS];
    *value = msgs[lang_id][autosave_prefer_ds ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED];
    break;
  case SettUiTheme:
    *label = msgs[lang_id][MSG_UIS_THEME];
    npf_snprintf(vb, vbsz, "< %s >", msgs[lang_id][MSG_THEME_0 + menu_theme]);
    *value = vb;
    break;
  case SettUiLang:
    *label = msgs[lang_id][MSG_UIS_LANG];
    npf_snprintf(vb, vbsz, "< %s >", msgs[lang_id][MSG_LANG_NAME]);
    *value = vb;
    break;
  case SettUiASpd:
    *label = msgs[lang_id][MSG_UIS_ANSPD];
    npf_snprintf(vb, vbsz, "< %s >", msgs[lang_id][MSG_UIS_SPD0 + anim_speed]);
    *value = vb;
    break;
  #ifdef BUNDLE_FONT10
  case SettUiFont:
    *label = msgs[lang_id][MSG_UIS_FONT];
    npf_snprintf(vb, vbsz, "< %upx >", font_size ? 10U : 12U);
    *value = vb;
    break;
  #endif
  case SettUiAnim:
    *label = msgs[lang_id][MSG_UIS_ANIM];
    *value = msgs[lang_id][ui_anim ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED];
    break;
  case SettUiFx:
    *label = msgs[lang_id][MSG_UIS_FX];
    *value = msgs[lang_id][ui_fx ? MSG_KNOB_ENABLED : MSG_KNOB_DISABLED];
    break;
  case SettUiSound:
    *label = msgs[lang_id][MSG_UIS_SOUND];
    npf_snprintf(vb, vbsz, "< %s >", msgs[lang_id][MSG_SND_0 + ui_sound]);
    *value = vb;
    break;
  case SettUiHid:
    *label = msgs[lang_id][MSG_UIS_BHID];
    *value = msgs[lang_id][hide_hidden ? MSG_KNOB_DISABLED : MSG_KNOB_ENABLED];
    break;
  }
  return 0;
}

// Help message for the currently selected setting.
static const char *settings_help(char *tmp, unsigned tmpsz) {
  const int sel = smenu.set.selector;
  if (sel == SettSaveLoc) {
    if (save_path_default == SaveRomName)
      return msgs[lang_id][MSG_SAVE_TYPE_NR];
    npf_snprintf(tmp, tmpsz, msgs[lang_id][MSG_SAVE_TYPE_PT], save_paths[save_path_default]);
    return tmp;
  }
  if (sel == SettStateLoc) {
    npf_snprintf(tmp, tmpsz, msgs[lang_id][MSG_STATE_TYPE_PT], savestates_paths[state_path_default]);
    return tmp;
  }
  #ifdef SUPPORT_NORGAMES
  if (sel == SettSaveLocNOR) {
    npf_snprintf(tmp, tmpsz, msgs[lang_id][MSG_SAVE_TYPE_PTX], save_paths[save_path_nor_default]);
    return tmp;
  }
  #endif
  unsigned help_msg = sel == SettBootType ? MSG_BOOT_TYPE_I0 + boot_bios_splash :
                      sel == SettSaveBkp  ? MSG_BACKUP_I :
                      sel == SettFastSD   ? MSG_FASTSD_I :
                      sel == SettFastEWRAM? MSG_FASTEW_I :
                      sel == DefsPatchEng ? MSG_PATCH_TYPE_I0 + patcher_default :
                      sel == DefsLoadPol  ? MSG_DEF_LOADP_I0 + (autoload_default ^ 1) :
                      sel == DefsSavePol  ? MSG_DEF_SAVEP_I0 + (autosave_default ^ 1) :
                      sel == DefsPrefDS   ? MSG_LOADER_PREFDSI :
                      #ifdef BUNDLE_FONT10
                      sel == SettUiFont   ? MSG_UIS_FONTI :
                      #endif
                      #ifdef SUPPORT_NORGAMES
                      sel == SettVerifyNOR ? MSG_VERNOR_I :
                      #endif
                      MSG_EMPTY;
  return msgs[lang_id][help_msg];
}

// Draws text left aligned, wrapping it at spaces (or anywhere if there are none, like in chinese).
static void draw_text_wrapped_left(const char *t, volatile uint8_t *frame, unsigned x, unsigned y,
                                   unsigned maxw, unsigned lineh, unsigned maxlines) {
  while (*t == ' ')
    t++;
  for (unsigned l = 0; *t && l < maxlines; l++) {
    char tmp[128];
    unsigned outw;
    unsigned n = font_width_cap_space(t, maxw, &outw);
    if (!n)
      n = font_width_cap(t, maxw);
    if (!n || n >= sizeof(tmp))
      break;
    memcpy(tmp, t, n);
    tmp[n] = 0;
    clip_text((int)y);
    draw_text_idx8_bus16(tmp, (uint8_t*)&frame[y * SCREEN_WIDTH + x], SCREEN_WIDTH, FT_COLOR);
    t += n;
    while (*t == ' ')
      t++;
    y += lineh;
  }
}

// Tabs of the START window: the five settings groups, then tools and info.
enum {
  SysTabTools = SetGrpCNT,
  SysTabInfo,
  SysTabCNT
};

static const uint16_t sys_tab_msgs[SysTabCNT] = {
  MSG_SETG_UI, MSG_SETG_FX, MSG_SETG_SYS, MSG_SETG_SAVE, MSG_SETG_GAME, MSG_SYS_TOOLS, MSG_SYS_INFO,
};

#define SYS_WIN_H   (CW_BOTTOM - SW_TOP + 2)    // Height of the window, it slides up from the bottom of the screen

static int sys_dy;                                              // Vertical offset while the window slides
static t_anim win_slide = { SYS_WIN_H * 16, SYS_WIN_H * 16 };   // Closed at the start
static t_anim tab_hl_x, tab_hl_w;                               // Highlight of the current tab
static t_anim row_hl_y;                                         // Highlight bar of the selected row
static uint8_t sys_hl_tab = 0xFF;                               // Tab the row highlight belongs to
static uint8_t sys_last_tab = 0xFF;

// The text of a row, drawn on top of the (moving) highlight bar.
static void sys_row_text(volatile uint8_t *frame, int y, const char *label, const char *value, unsigned reserve) {
  const unsigned tx = SYS_LEFT + 6;
  const unsigned xr = SYS_LEFT + 2 + CW_ROWW - 4;
  const unsigned vw = value ? MIN(font_width(value), 104U) : 0;
  const unsigned lw = xr - tx - (vw ? vw + 4 : 0) - reserve;
  draw_text_ovf(label, frame, tx, y + sys_dy - 2, lw);
  if (value)
    draw_rightj_ovf(value, frame, xr, y + sys_dy - 2, vw);
}

// A toggle switch (the knob is on the right when it is on) whose right edge is at xr.
static void sys_switch(volatile uint8_t *frame, int xr, int y, bool on) {
  const int w = 20, h = 10, x = xr - w, yy = y + sys_dy + 1;
  fill_rect(frame, x, yy, w, h, FT_COLOR);
  if (!on)
    fill_rect(frame, x + 2, yy + 2, w - 4, h - 4, BG_COLOR);
  fill_rect(frame, on ? x + w - 8 : x + 2, yy + 2, 6, h - 4, on ? BG_COLOR : FT_COLOR);
}

// A slider with max notches (value 0..max-1) whose right edge is at xr.
static void sys_slider(volatile uint8_t *frame, int xr, int y, unsigned val, unsigned max) {
  const int sp = 12, w = (int)(max - 1) * sp + 6, x = xr - w, yy = y + sys_dy;
  fill_rect(frame, x, yy + 5, w, 2, FT_COLOR);
  for (unsigned i = 0; i < max; i++)
    fill_rect(frame, x + (int)i * sp + 2, yy + 3, 2, 6, FT_COLOR);
  fill_rect(frame, x + (int)val * sp, yy + 1, 6, 10, FT_COLOR);
}

// Moves the highlight bar to row sel (rows are CW_ROWH apart) and draws it.
static void sys_highlight(volatile uint8_t *frame, unsigned tab, unsigned sel) {
  if (sys_hl_tab != tab) {
    anim_snap(&row_hl_y, sel * CW_ROWH * 16);
    sys_hl_tab = tab;
  } else {
    anim_to(&row_hl_y, sel * CW_ROWH * 16);
    anim_step(&row_hl_y);
  }
  fill_rect(frame, SYS_LEFT + 2, SW_ROW0Y + sys_dy + row_hl_y.cur / 16, CW_ROWW, CW_ROWH - 2, HI_COLOR);
}

// Rows of a settings group, each group fits without scrolling.
static void render_sys_settings(volatile uint8_t *frame, unsigned grp) {
  const unsigned total = set_group_len(grp) + 1;
  const char *labels[8], *values[8];
  char vbs[8][48];
  int kinds[8];

  // Backgrounds first (the save button is a bar), then the highlight, then the texts
  for (unsigned i = 0; i < total; i++) {
    kinds[i] = settings_row(set_row_id(grp, i), &labels[i], &values[i], vbs[i], sizeof(vbs[i]));
    fill_rect(frame, SYS_LEFT + 2, SW_ROW0Y + sys_dy + i * CW_ROWH, CW_ROWW, CW_ROWH - 2,
              kinds[i] == 0 ? BG_COLOR : FG_COLOR);
  }
  sys_highlight(frame, grp, smenu.set.idx[grp]);

  for (unsigned i = 0; i < total; i++) {
    const int y = SW_ROW0Y + i * CW_ROWH;
    const unsigned id = set_row_id(grp, i);
    if (kinds[i] != 0) {
      draw_central_text(labels[i], frame, SYS_CX, y + sys_dy - 2);     // Save button
    }
    else if (id == SettUiASpd) {
      sys_row_text(frame, y, labels[i], NULL, 64);
      sys_slider(frame, SYS_LEFT + 2 + CW_ROWW - 6, y, anim_speed, animspd_cnt);
    }
    else if (values[i] == msgs[lang_id][MSG_KNOB_ENABLED] || values[i] == msgs[lang_id][MSG_KNOB_DISABLED]) {
      sys_row_text(frame, y, labels[i], NULL, 30);
      sys_switch(frame, SYS_LEFT + 2 + CW_ROWW - 6, y, values[i] == msgs[lang_id][MSG_KNOB_ENABLED]);
    }
    else
      sys_row_text(frame, y, labels[i], values[i], 0);
  }

  // Help bar at the bottom of the window (key hints if there is no help text)
  char tmp[128];
  const char *help = settings_help(tmp, sizeof(tmp));
  if (!help[0])
    help = msgs[lang_id][MSG_SETG_HINT];
  fill_rect(frame, SYS_LEFT + 2, SW_HELPY + sys_dy, CW_RIGHT - SYS_LEFT - 4, 16, FG_COLOR);
  draw_text_ovf_rotate(help, frame, SYS_LEFT + 6, SW_HELPY + sys_dy,
                       CW_RIGHT - SYS_LEFT - 12, &smenu.anim_state);
}

// Info tab: a few lines about the firmware/flash/patch database/SD card (left/right switch them)
// and, below them, the firmware flashing unlock button with the risks.
static void render_sys_info(volatile uint8_t *frame) {
  uint32_t vmaj = VERSION_WORD >> 16;
  uint32_t vmin = VERSION_WORD & 0xFFFF;
  uint32_t gitver = VERSION_SLUG_WORD;
  char l0[64], l1[64], l2[64], tmp2[32];
  const unsigned x = SYS_LEFT + 8;
  const unsigned maxw = CW_RIGHT - SYS_LEFT - 16;

  switch (smenu.info.selector) {
  case 0:
    npf_snprintf(l0, sizeof(l0), "SuperFW by davidgf");
    npf_snprintf(l1, sizeof(l1), "Version %lu.%lu " CUSTOM_NAME " v%u", vmaj, vmin, (unsigned)CUSTOM_VERSION);
    npf_snprintf(l2, sizeof(l2), FW_FLAVOUR " variant (%08lx)", gitver);
    break;
  case 1:
    npf_snprintf(l0, sizeof(l0), "Flash info");
    npf_snprintf(l1, sizeof(l1), "Dev ID: %08lx", flashinfo.deviceid);
    if (flashinfo.size && flashinfo.blksize && flashinfo.blkcount) {
      human_size_kb(tmp2, sizeof(tmp2), flashinfo.size >> 10);
      npf_snprintf(l2, sizeof(l2), "%s [%lu * %lu]", tmp2, flashinfo.blksize, flashinfo.blkcount);
      if (flashinfo.regioncnt != 1)
        strcat(l2, " !");
    } else {
      npf_snprintf(l2, sizeof(l2), "No CFI (fixed %dKiB)", FW_MAX_SIZE_KB);
    }
    break;
  case 2:
    npf_snprintf(l0, sizeof(l0), "%s", msgs[lang_id][MSG_DBPINFO]);
    npf_snprintf(l1, sizeof(l1), "%s - %s", pdbinfo.version, pdbinfo.date);
    npf_snprintf(l2, sizeof(l2), "Game count: %lu", pdbinfo.patch_count);
    break;
  default:
    npf_snprintf(l0, sizeof(l0), "SD Card ID: %02x | %04x", sd_info.manufacturer, sd_info.oemid);
    human_size_kb(tmp2, sizeof(tmp2), sd_info.block_cnt / 2);
    npf_snprintf(l1, sizeof(l1), "Type: %s  Size: %s", sd_info.sdhc ? "SDHC" : "SDSC", tmp2);
    npf_snprintf(l2, sizeof(l2), "Info: %c%c%c%c%c (%d/%d)",
      sd_info.prodname[0] ?: '#', sd_info.prodname[1] ?: '#',
      sd_info.prodname[2] ?: '#', sd_info.prodname[3] ?: '#',
      sd_info.prodname[4] ?: '#', 2000 + sd_info.year, sd_info.month);
    break;
  }

  // Page indicator on the first line
  const int ty = SW_ROW0Y - 2 + sys_dy;
  char ind[16];
  npf_snprintf(ind, sizeof(ind), "< %u/4 >", smenu.info.selector + 1U);
  draw_text_ovf(l0, frame, x, ty, maxw - font_width(ind) - 8);
  draw_rightj_text(ind, frame, CW_RIGHT - 8, ty);
  draw_text_ovf(l1, frame, x, ty + 14, maxw);
  draw_text_ovf(l2, frame, x, ty + 28, maxw);

  // Firmware flashing: a button that temporarily allows it (until the next boot), and the risks
  const int by = SW_ROW0Y + 46 + sys_dy;
  box_clipped(frame, SYS_LEFT + 8, CW_RIGHT - 8, by, by + 20, FG_COLOR, enable_flashing ? (int)BG_COLOR : (int)HI_COLOR);
  draw_central_text_ovf(msgs[lang_id][enable_flashing ? MSG_FLASH_REVOKE : MSG_FLASH_ALLOW],
                        frame, SYS_CX, by + 2, CW_RIGHT - SYS_LEFT - 24);
  draw_text_wrapped_left(msgs[lang_id][MSG_FLASH_RISK], frame, x, by + 22, maxw, 14, 3);
}

static void render_sys_tools(volatile uint8_t *frame) {
  for (unsigned i = 0; i < ToolsMAX; i++)
    fill_rect(frame, SYS_LEFT + 2, SW_ROW0Y + sys_dy + i * CW_ROWH, CW_ROWW, CW_ROWH - 2, BG_COLOR);
  sys_highlight(frame, SysTabTools, smenu.tools.selector);
  for (unsigned i = 0; i < ToolsMAX; i++)
    sys_row_text(frame, SW_ROW0Y + i * CW_ROWH, msgs[lang_id][MSG_TOOLS0_SDRAM + i], NULL, 0);

  fill_rect(frame, SYS_LEFT + 2, SW_HELPY + sys_dy, CW_RIGHT - SYS_LEFT - 4, 16, FG_COLOR);
  draw_text_ovf(msgs[lang_id][MSG_HINT_TOOLS], frame, SYS_LEFT + 6, SW_HELPY + sys_dy, CW_RIGHT - SYS_LEFT - 12);
}

// The START window: tabs on top (L/R switch them), the content of the tab below. It slides up
// when it opens and down when it closes, and the highlights glide.
void render_system(volatile uint8_t *frame) {
  anim_to(&win_slide, spop.pop_num == POPUP_SYSTEM ? 0 : SYS_WIN_H * 16);
  anim_step(&win_slide);
  sys_dy = win_slide.cur / 16;
  if (sys_dy >= SYS_WIN_H) {
    sys_dy = 0;
    sys_hl_tab = 0xFF;       // The next opening starts with the highlights in place
    sys_last_tab = 0xFF;
    return;
  }

  box_clipped(frame, SYS_LEFT, CW_RIGHT, SW_TOP + sys_dy, CW_BOTTOM + sys_dy, FG_COLOR, BG_COLOR);

  // Tab bar, the tabs get narrower if the texts do not fit
  const unsigned cur = smenu.sys_tab;
  unsigned w[SysTabCNT], px[SysTabCNT], sum = 0;
  for (unsigned i = 0; i < SysTabCNT; i++) {
    w[i] = (font_width(msgs[lang_id][sys_tab_msgs[i]]) + 6 + 1) & ~1U;
    sum += w[i];
  }
  const unsigned avail = CW_RIGHT - SYS_LEFT - 8 - 2 * (SysTabCNT - 1);
  if (sum > avail)
    for (unsigned i = 0; i < SysTabCNT; i++)
      w[i] = MAX(12U, (w[i] * avail / sum) & ~1U);

  unsigned x = SYS_LEFT + 4;
  for (unsigned i = 0; i < SysTabCNT; i++) {
    px[i] = x;
    fill_rect(frame, x, SW_TAB_Y + sys_dy, w[i], SW_TAB_H - 2, FG_COLOR);
    x += w[i] + 2;
  }
  if (sys_last_tab == 0xFF) {
    anim_snap(&tab_hl_x, px[cur] * 16);
    anim_snap(&tab_hl_w, w[cur] * 16);
  } else {
    anim_to(&tab_hl_x, px[cur] * 16);
    anim_to(&tab_hl_w, w[cur] * 16);
    anim_step(&tab_hl_x);
    anim_step(&tab_hl_w);
  }
  sys_last_tab = cur;
  fill_rect(frame, (tab_hl_x.cur / 16) & ~1U, SW_TAB_Y + sys_dy, (tab_hl_w.cur / 16) & ~1U, SW_TAB_H - 2, HI_COLOR);
  for (unsigned i = 0; i < SysTabCNT; i++)
    draw_text_ovf(msgs[lang_id][sys_tab_msgs[i]], frame, px[i] + 3, SW_TAB_Y + sys_dy - 2, w[i] - 3);

  if (cur < SetGrpCNT)
    render_sys_settings(frame, cur);
  else if (cur == SysTabTools)
    render_sys_tools(frame);
  else
    render_sys_info(frame);
  sys_dy = 0;
}

// Tabs that L/R cycle through. Settings, tools and info live in the START menu.
static const uint8_t game_tabs[] = {
  MENUTAB_RECENT,
  MENUTAB_ROMBROWSE,
  #ifdef SUPPORT_NORGAMES
  MENUTAB_NORBROWSE,
  #endif
};
#define game_tab_cnt (sizeof(game_tabs) / sizeof(game_tabs[0]))

static const uint16_t game_tab_names[] = {
  MSG_TAB_RECENT,
  MSG_TAB_BROWSE,
  #ifdef SUPPORT_NORGAMES
  MSG_TAB_NOR,
  #endif
};

// Text tabs on the top bar, the current one is highlighted.
static void render_tabbar(volatile uint8_t *frame) {
  unsigned x = 4;
  for (unsigned i = 0; i < game_tab_cnt; i++) {
    const char *t = msgs[lang_id][game_tab_names[i]];
    const unsigned tw = (font_width(t) + 8 + 1) & ~1U;
    if (game_tabs[i] == smenu.menu_tab)
      fill_rect(frame, x, 0, tw, 16, HI_COLOR);
    draw_text_ovf(t, frame, x + 4, 0, tw - 4);
    x += tw + 2;
  }
  tabbar_end = x;
}

// ---- Effects: dim, fades, toast, button feedback ---------------------------------------------
static bool menu_ready = false;         // The video is set up, the menu can be drawn at any time
static uint8_t dim_level;               // 0..3: how much the page behind the windows is darkened
static uint8_t press_flash;             // Frames left of the selection bar flash after pressing A
static uint16_t fade_buf[512];          // Palette copy used by the fades
static uint8_t fade_level;              // 16: black ... 0: normal (fade in when the menu starts)
static const char *toast_msg;           // Message shown at the bottom, without blocking
static int toast_timer;                 // Frames left
#define TOAST_FRAMES    130

static void show_toast(const char *msg) {
  toast_msg = msg;
  toast_timer = TOAST_FRAMES;
}

static void fade_capture(void) {
  for (unsigned i = 0; i < 512; i++)
    fade_buf[i] = MEM_PALETTE[i];
}

// Palette brightness: level 0 is the captured palette, 16 is black.
static void fade_apply(unsigned level) {
  const unsigned k = 16 - level;
  for (unsigned i = 0; i < 512; i++) {
    const unsigned c = fade_buf[i];
    const unsigned r = ((c & 31) * k) >> 4;
    const unsigned g = (((c >> 5) & 31) * k) >> 4;
    const unsigned b = (((c >> 10) & 31) * k) >> 4;
    MEM_PALETTE[i] = r | (g << 5) | (b << 10);
  }
}

static void render_toast(volatile uint8_t *frame) {
  if (!toast_msg)
    return;
  if (toast_timer <= 0) {
    toast_msg = NULL;
    return;
  }
  toast_timer -= (int)anim_frames;

  int off = 0;      // Pixels below the final position (slides in and out)
  if (ui_anim) {
    if (toast_timer > TOAST_FRAMES - 8)
      off = (toast_timer - (TOAST_FRAMES - 8)) * 3;
    else if (toast_timer < 8)
      off = (8 - toast_timer) * 3;
  }
  const int y = 138 + off;
  box_clipped(frame, 20, 220, y, y + 18, FG_COLOR, HI_COLOR);
  draw_central_text_ovf(toast_msg, frame, SCREEN_WIDTH / 2, y, 192);
}

// Updates the hardware effects once per frame: dim behind windows, selection bar flash, fade in.
static void update_effects(void) {
  // Dim the page behind the START window and the alert pop-ups (brightness effect, only applies
  // outside of the windows, see REG_WINOUT).
  const bool window = win_slide.cur < (SYS_WIN_H - 30) * 16 && !spop.qpop.message && !spop.rtcpop.callback &&
                      !(spop.pop_num && spop.pop_num != POPUP_SYSTEM);
  const unsigned target = (ui_fx && (spop.alert_msg || window)) ? 3 : 0;
  if (!ui_anim)
    dim_level = target;
  else {
    for (unsigned f = 0; f < anim_frames && dim_level != target; f++)
      dim_level += dim_level < target ? 1 : -1;
  }
  if (dim_level) {
    // BG2 (first target) blended with the black backdrop (second target) at EVA/16, only where the
    // window effects are enabled (outside of the windows). The semi-transparent selection bar
    // sprites use the same coefficients: they get a bit more opaque, which is fine.
    static const uint16_t dim_alpha[4] = { 0x0808, 0x060D, 0x070B, 0x0709 };    // EVA | EVB << 8
    REG_BLDCNT = 0x3F44;
    REG_BLDALPHA = dim_alpha[dim_level];
    REG_WINOUT = 0x0034;
  } else {
    REG_BLDCNT = 0x1F40;
    REG_BLDALPHA = 0x0808;
    REG_WINOUT = 0x0014;
  }

  // Fade in when the menu starts
  if (fade_level) {
    fade_level = fade_level > anim_frames ? fade_level - anim_frames : 0;
    fade_apply(fade_level);
  }
  else {
    // Selection bar flash when pressing A
    MEM_PALETTE[256 + SEL_COLOR] = press_flash ? themes[menu_theme].hi_color : themes[menu_theme].hi_blend;
  }
  if (press_flash)
    press_flash = press_flash > anim_frames ? press_flash - anim_frames : 0;
}

// Shows a "loading" message right away (for operations that block for a while).
void menu_render(unsigned fcnt);
void menu_flip();
static void show_loading(void) {
  if (!menu_ready)
    return;
  menu_render(0);
  volatile uint8_t *frame = &MEM_VRAM_U8[0xA000*framen];
  box_clipped(frame, 60, 180, 66, 94, FG_COLOR, HI_COLOR);
  draw_central_text(msgs[lang_id][MSG_LOADING], frame, SCREEN_WIDTH / 2, 72);
  menu_flip();
}

// ---- Navigation sounds ------------------------------------------------------------------------
// Square wave (channel 2) tones, plus a noise click, with a few styles. Registers are written
// directly, a sound is a few milliseconds long.
enum { UiSndMove, UiSndOk, UiSndBack, UiSndTab };

typedef struct {
  uint16_t freq;     // Frequency register (2048 - 131072 / Hz), 0: noise click
  uint8_t duty;      // 0: 12.5%, 1: 25%, 2: 50%, 3: 75%
  uint8_t vol;       // Initial volume
  uint8_t step;      // Envelope decay step time (1/64 s per step)
  uint8_t len;       // Length data (sound lasts (64-len)/256 s), 0: until the envelope ends
} t_snd;

static const t_snd snd_tbl[3][4] = {
  // Soft: round 50% square, low notes, slow decay
  { {1849, 2, 6, 2, 0}, {1899, 2, 7, 3, 0}, {1825, 2, 6, 2, 0}, {1923, 2, 6, 2, 0} },
  // Retro: thin pulse waves, high notes, fast decay (chiptune)
  { {1923, 1, 8, 1, 0}, {1964, 1, 9, 1, 0}, {1881, 1, 8, 1, 0}, {1950, 1, 8, 1, 0} },
  // Crisp: noise click for moving, very short high ticks for the rest
  { {0, 0, 0, 0, 0},    {1985, 0, 9, 1, 58}, {1950, 0, 8, 1, 58}, {1964, 0, 8, 1, 58} },
};

static bool snd_on = false;
static uint8_t snd_cool;      // Frames until the next sound is allowed

static void ui_sound_stop(void) {
  REG_SOUNDCNT_X = 0;
  REG_SOUNDCNT_L = 0;
  snd_on = false;
}

static void ui_beep(unsigned ev) {
  if (!ui_sound || snd_cool)
    return;
  if (!snd_on) {
    REG_SOUNDCNT_X = 0x80;      // Master enable
    REG_SOUNDCNT_L = 0xFF77;    // All PSG channels, both sides, full volume
    REG_SOUNDCNT_H = 0x0002;    // PSG at 100%
    snd_on = true;
  }
  const t_snd *s = &snd_tbl[ui_sound - 1][ev];
  if (!s->freq) {
    REG_SOUND4CNT_L = (7 << 12) | (1 << 8) | 62;
    REG_SOUND4CNT_H = 0x8000 | 0x4000 | (6 << 4) | 1;
  } else {
    REG_SOUND2CNT_L = (s->vol << 12) | (s->step << 8) | (s->duty << 6) | s->len;
    REG_SOUND2CNT_H = 0x8000 | (s->len ? 0x4000 : 0) | s->freq;
  }
  snd_cool = 3;
}

// Called right before a game is launched: fade out and silence the interface sounds.
void ui_launch_hook(void) {
  ui_sound_stop();
  if (!ui_fx)
    return;
  fade_capture();
  for (unsigned l = 1; l <= 16; l++) {
    const unsigned f = frame_count;
    fade_apply(l);
    for (unsigned guard = 0; frame_count == f && guard < 2000000; guard++);
  }
}

void reload_theme(unsigned thnum) {
  // Palette 0..15 contains the main menu template colors
  MEM_PALETTE[FG_COLOR] = themes[thnum].fg_color;
  MEM_PALETTE[BG_COLOR] = themes[thnum].bg_color;
  MEM_PALETTE[FT_COLOR] = themes[thnum].ft_color;
  MEM_PALETTE[HI_COLOR] = themes[thnum].hi_color;
  // In-game menu palette
  MEM_PALETTE[IGM_PAL_FG] = themes[thnum].fg_color;
  MEM_PALETTE[IGM_PAL_BG] = themes[thnum].bg_color;
  MEM_PALETTE[IGM_PAL_HI] = themes[thnum].ft_color;
  MEM_PALETTE[IGM_PAL_SH] = themes[thnum].sh_color;
  MEM_PALETTE[IGM_PAL_BL] = themes[thnum].hi_blend;

  // Palette entries for icons and other objects
  MEM_PALETTE[256 + SEL_COLOR] = themes[thnum].hi_blend;
}

static const struct {
  const t_mrender_fn render;
  const int max_submenu;
  const uint8_t cleft;          // Corner window: left edge (0 = full screen pop-up)
  const uint8_t ctop;           // Corner window: top edge
} popup_windows[] = {
  { render_gba_load_popup, GbaLoadCNT },
  { render_sav_menu_popup, 1 },
  { render_fw_flash_popup, 1 },
  { render_filemgr,        1 },
  #ifdef SUPPORT_NORGAMES
  { render_gba_norwrite,   GbaNorWrCNT },
  { render_gba_norload,    GbaNorLoadCNT },
  #endif
  { render_system,         1, SYS_LEFT, SW_TOP },
};

// Renders the menu. Arg0 represents the frame count difference with the
// previous rendered frame (for animations and similar stuff).
void menu_render(unsigned fcnt) {
  anim_frames = fcnt > 6 ? 6 : fcnt;
  objnum = 0;
  volatile uint8_t *frame = &MEM_VRAM_U8[0xA000*framen];
  if (snd_cool)
    snd_cool = snd_cool > anim_frames ? snd_cool - anim_frames : 0;

  // Render the tab menu on top (rows 0..15), highlighting the selected option
  dma_memset16(&frame[0], dup8(FG_COLOR), SCREEN_WIDTH*16/2);

  // Render the text tabs
  render_tabbar(frame);

  // Render the main area
  dma_memset16(&frame[16*SCREEN_WIDTH], dup8(BG_COLOR), SCREEN_WIDTH*(SCREEN_HEIGHT-16) / 2);

  const bool fullscreen = spop.pop_num && spop.pop_num != POPUP_SYSTEM;
  if (spop.qpop.message)
    render_popupq(frame, fcnt);
  else if (spop.rtcpop.callback)
    render_rtcpop(frame);
  else if (fullscreen) {
    anim_snap(&win_slide, SYS_WIN_H * 16);
    popup_windows[spop.pop_num - 1].render(frame);
    spop.anim += fcnt * animspd_lut[anim_speed];
  } else {
    static const t_mrender_fn renderfns[] = {
      render_recent,
      render_browser,
      #ifdef SUPPORT_NORGAMES
      render_flashbrowser,
      #endif
    };
    renderfns[smenu.menu_tab](frame);
    smenu.anim_state += fcnt * animspd_lut[anim_speed];

    // The START window is drawn on top of the current page (it slides in and out)
    render_system(frame);
  }

  // Window 1 hides the page sprites (selection bars, icons) behind the START window.
  const int wtop = SW_TOP + win_slide.cur / 16;
  if (wtop < SCREEN_HEIGHT - 2 && !spop.qpop.message && !spop.rtcpop.callback && !fullscreen) {
    REG_WIN1H = CW_RIGHT | (SYS_LEFT << 8);
    REG_WIN1V = CW_BOTTOM | (wtop << 8);
  } else {
    REG_WIN1H = 0;
    REG_WIN1V = 0;
  }

  // Render popup window. Use windowing to ensure the pop up is not covered by OBJs.
  if (spop.alert_msg) {
    draw_box_full(frame, 15, 227, SCREEN_HEIGHT / 2 - 20, SCREEN_HEIGHT / 2 + 20, FG_COLOR, HI_COLOR);
    draw_central_text(spop.alert_msg, frame, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 8);
    REG_WIN0H = 226 | (14 << 8);
    REG_WIN0V = (SCREEN_HEIGHT / 2 + 20) | ((SCREEN_HEIGHT / 2 - 20) << 8);
  } else {
    REG_WIN0H = 0;
    REG_WIN0V = 0;
  }

  render_toast(frame);
  update_effects();

  // Other code (boot messages, fatal errors) draws text without the clip
  font_row_mask = 0xFFFF;
}

void menu_flip() {
  for (unsigned i = 0; i < objnum; i++) {
    MEM_OAM[i*4+0] = fobjs[i].y | 0x2000;  // Use 256 entries palette
    // Size 16x16 (square) or 16x8 (wide shape, size bits stay 0)
    MEM_OAM[i*4+1] = fobjs[i].x | ((fobjs[i].y & OBJ_SHAPE_WIDE) ? 0 : 0x4000);
    MEM_OAM[i*4+2] = fobjs[i].tn + 512;    // OBJ numbers start at 512 for Mode 4
  }
  dma_memset16(&MEM_OAM[objnum*4], 0, 256 - objnum*2);  // Clear unused objects
  REG_DISPCNT = (REG_DISPCNT & ~0x10) | (framen << 4);
  framen ^= 1;
}

// Applies the font size setting: the 12px font pack is the one copied to SDRAM at boot,
// the 10px one is bundled compressed (as the FNT1 asset) and gets unpacked over it.
// Changing the setting takes effect on the next boot.
static void ui_font_init() {
  lay = list_layout_12;
  #ifdef BUNDLE_FONT10
  if (font_size) {
    set_supercard_mode(MAPPED_SDRAM, true, false);
    const void *src = get_vfile_ptr("FNT1");
    if (src)
      apunpack16((const uint8_t*)src, (uint8_t*)ROM_FONTBASE_U8);
    set_supercard_mode(MAPPED_SDRAM, true, true);
    if (src)
      lay = list_layout_10;
  }
  #endif
}

static uint32_t prev_keys;    // Keys seen on the last poll (defined below)

void menu_init(int sram_testres) {
  // Pick the font (and the list layout that goes with it) before drawing anything.
  ui_font_init();

  // Reset to ROM browser and SD card root.
  memset(&smenu, 0, sizeof(smenu));
  memset(&spop, 0, sizeof(spop));
  set_sync_selector();

  // Reset the file browser as well.
  strcpy(smenu.browser.cpath, "/");
  browser_reload();
  flashbrowser_reload();

  // Load recent ROMs (we could disable this for speed)
  recent_reload();

  reload_theme(menu_theme);

  smenu.menu_tab = MENUTAB_RECENT;

  // Load icons into VRAM
  dma_memcpy16(MEM_VRAM_OBJS, icons_img, sizeof(icons_img) / 2);
  dma_memcpy16(&MEM_PALETTE[256], icons_pal, sizeof(icons_pal) / 2);
  // Generate some icons (selector)
  dma_memset16(&MEM_VRAM_OBJS[63 * 256], dup8(SEL_COLOR), 256 / 2);

  // Further setup initial video regs. BG2 is setup in the bootloader already!
  REG_WININ  = 0x0404;     // Only BG2 is enabled in Win0 and Win1
  REG_WINOUT = 0x0014;     // BG2 and OBJ enabled outside of Win0 (menu_render adds effects for the dim)
  MEM_PALETTE[0] = 0;      // Black backdrop: the dim blends the page with it
  REG_WIN0H = 0;
  REG_WIN0V = 0;
  REG_WIN1H = 0;
  REG_WIN1V = 0;
  REG_DISPCNT |= 0x6000;   // Enable windows 0 and 1

  // Setup alpha blending for the selector knob
  REG_BLDCNT = 0x1F40;
  REG_BLDALPHA = 0x0808;  // 50% alpha

  // If there's a test result to report, create a popup
  if (sram_testres >= 0)
    spop.alert_msg = sram_testres ? msgs[lang_id][MSG_SRAMTST_FAIL] :
                                    msgs[lang_id][MSG_SRAMTST_OK];

  // Keys held while starting (like R for the default settings) must not count as presses.
  prev_keys = curr_pressed_keys();

  // Fade in from black
  menu_ready = true;
  if (ui_fx) {
    fade_capture();
    fade_level = 16;
    fade_apply(16);
  }
}

int movedir_up() {
  char *p = smenu.browser.cpath;
  p = &p[strlen(p)-1];

  if (p != smenu.browser.cpath) {
    do {
      p--;
      if (*p == '/') {
        p[1] = 0;   // Shorten the path here
        return 1;
      }
    } while (p != smenu.browser.cpath);
  }
  return 0;
}

void start_flash_update(const char *fn, unsigned fwsize, bool validate_superfw) {
  // We read the file into SDRAM, apply the update from there.
  FIL fd;
  FRESULT res = f_open(&fd, fn, FA_READ);
  if (res != FR_OK)
    spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRRD];
  else {
    // Loading file...
    spop.p.update.curr_state = FlashingLoading;
    menu_render(1); menu_flip();
    for (unsigned i = 0; i < fwsize; i += 4*1024) {
      UINT rdbytes;
      unsigned tord = fwsize >= i + 4*1024 ? 4*1024 : fwsize - i;
      uint32_t tmp[1024];
      if (FR_OK != f_read(&fd, tmp, tord, &rdbytes) || rdbytes != tord) {
        spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRRD];
        return;
      }
      // Copy (ensure aligned copy!)
      dma_memcpy32(&sdr_state->scratch[i], tmp, 1024);
    }
    spop.p.update.curr_state = FlashingChecking;
    menu_render(1); menu_flip();

    // Now proceed to validate the superfw if necessary.
    if (validate_superfw && !validate_superfw_variant(sdr_state->scratch))
      spop.alert_msg = msgs[lang_id][MSG_FWUP_BADFL];
    else if (validate_superfw && !validate_superfw_checksum(sdr_state->scratch, fwsize))
      spop.alert_msg = msgs[lang_id][MSG_FWUPD_BADCHK];
    else {
      // Can start the flashing!
      spop.p.update.curr_state = FlashingErasing;
      menu_render(1); menu_flip();

      bool erased_ok;
      #ifdef SUPPORT_NORGAMES
      if (flashinfo.blksize)
        erased_ok = flash_erase_sectors(ROM_FLASHFIRMW_ADDR, flashinfo.blksize,
                                        (fwsize + flashinfo.blksize - 1) / flashinfo.blksize);
      else
      #endif
        erased_ok = flash_erase_chip();

      if (!erased_ok)
        spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRCL];
      else {
        spop.p.update.curr_state = FlashingWriting;
        menu_render(1); menu_flip();

        bool programmed_ok;
        #ifdef SUPPORT_NORGAMES
        if (flashinfo.size && flashinfo.blksize && flashinfo.blkcount && flashinfo.blkwrite)
          programmed_ok = flash_program_buffered(ROM_FLASHFIRMW_ADDR, sdr_state->scratch, fwsize, flashinfo.blkwrite);
        else
        #endif
          programmed_ok = flash_program(ROM_FLASHFIRMW_ADDR, sdr_state->scratch, fwsize);

        if (!programmed_ok)
          spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRPG];
        else {
          if (!flash_verify(ROM_FLASHFIRMW_ADDR, sdr_state->scratch, fwsize))
            spop.alert_msg = msgs[lang_id][MSG_FWUP_ERRVR];
          else {
            // Done! Show a pop up, also go up with pop ups too.
            spop.alert_msg = msgs[lang_id][MSG_FWUPD_DONE];
          }
        }
      }
    }
    spop.pop_num = 0;
  }
}

static void keypress_popup_loadgba(unsigned newkeys) {
  const unsigned maxm[] = {
    GBAInfoCNT,
    GBALdSetCNT,
    GBAPatchCNT,
  };
  unsigned maxsel = maxm[(int)spop.submenu];

  const int psel = spop.selector;
  if (newkeys & KEY_BUTTUP)
    spop.selector += maxsel - 1;
  if (newkeys & KEY_BUTTDOWN)
    spop.selector++;

  // Limit selector to its max value
  spop.selector %= maxsel;

  if (newkeys & KEY_BUTTLEFT) {
    if (spop.submenu == GbaLoadPopLoadS) {
      if (spop.selector == GBALdSetCheats)
        spop.p.load.l.use_cheats = !spop.p.load.l.use_cheats;
      if (spop.p.load.i.use_dsaving) {
        if (spop.selector == GBALdSetLoadP)
          spop.p.load.l.sram_load_type = (spop.p.load.l.sram_load_type + SaveLoadDSCNT - 1) % SaveLoadDSCNT;
      } else {
        if (spop.selector == GBALdSetLoadP)
          spop.p.load.l.sram_load_type = (spop.p.load.l.sram_load_type + SaveLoadCNT - 1) % SaveLoadCNT;
        else if (spop.selector == GBALdSetSaveP)
          spop.p.load.l.sram_save_type = (spop.p.load.l.sram_save_type + SaveCNT - 1) % SaveCNT;
      }
    }
    else if (spop.submenu == GbaLoadPopPatch) {
      if (spop.selector == GBALoadPatch)
        spop.p.load.i.patch_type = (spop.p.load.i.patch_type + PatchOptCNT - 1) % PatchOptCNT;
      else if (spop.selector == GBAInGameMen)
        spop.p.load.i.ingame_menu_enabled = !spop.p.load.i.ingame_menu_enabled;
      else if (spop.selector == GBASavePatch)
        spop.p.load.i.use_dsaving = !spop.p.load.i.use_dsaving;
      else if (spop.selector == GBARTCPatch)
        spop.p.load.i.rtc_patch_enabled = !spop.p.load.i.rtc_patch_enabled;
    }

    // Handle the different cases where the user attempts to select an invalid option.
    if (!spop.p.load.i.patches_cache_found && spop.p.load.i.patch_type == PatchEngine)
      spop.p.load.i.patch_type = PatchDatabase;  // Might be invalid, handled below.
    if (!spop.p.load.i.patches_datab_found && spop.p.load.i.patch_type == PatchDatabase)
      spop.p.load.i.patch_type = PatchNone;

    if (!dirsav_avail_sdram(&spop.p.load.i))
      spop.p.load.i.use_dsaving = false;

    // DirSav forces automatic saving
    if (spop.p.load.i.use_dsaving)
      spop.p.load.l.sram_save_type = SaveDirect;
    else if (spop.p.load.l.sram_save_type == SaveDirect)
      spop.p.load.l.sram_save_type = autosave_default ? SaveReboot : SaveDisable;

    // If DS is selected, do not allow manual mode.
    if (spop.p.load.l.sram_load_type == SaveLoadDisable && spop.p.load.i.use_dsaving)
      spop.p.load.l.sram_load_type = SaveLoadSav;
    // If no .sav is available, do not allow that option!
    if (spop.p.load.l.sram_load_type == SaveLoadSav && !spop.p.load.l.savefile_found)
      spop.p.load.l.sram_load_type = spop.p.load.i.use_dsaving ? SaveLoadReset : SaveLoadDisable;
  }
  if (newkeys & KEY_BUTTRIGHT) {
    if (spop.submenu == GbaLoadPopLoadS) {
      if (spop.selector == GBALdSetCheats)
        spop.p.load.l.use_cheats = !spop.p.load.l.use_cheats;
      if (spop.p.load.i.use_dsaving) {
        if (spop.selector == GBALdSetLoadP)
          spop.p.load.l.sram_load_type = (spop.p.load.l.sram_load_type + 1) % SaveLoadDSCNT;
      } else {
        if (spop.selector == GBALdSetLoadP)
          spop.p.load.l.sram_load_type = (spop.p.load.l.sram_load_type + 1) % SaveLoadCNT;
        else if (spop.selector == GBALdSetSaveP)
          spop.p.load.l.sram_save_type = (spop.p.load.l.sram_save_type + 1) % SaveCNT;
      }
    }
    else if (spop.submenu == GbaLoadPopPatch) {
      if (spop.selector == GBALoadPatch)
        spop.p.load.i.patch_type = (spop.p.load.i.patch_type + 1) % PatchOptCNT;
      else if (spop.selector == GBAInGameMen)
        spop.p.load.i.ingame_menu_enabled = !spop.p.load.i.ingame_menu_enabled;
      else if (spop.selector == GBASavePatch)
        spop.p.load.i.use_dsaving = !spop.p.load.i.use_dsaving;
      else if (spop.selector == GBARTCPatch)
        spop.p.load.i.rtc_patch_enabled = !spop.p.load.i.rtc_patch_enabled;
    }

    // If the database has no entry, then do not let the user select that mode.
    if (!spop.p.load.i.patches_datab_found && spop.p.load.i.patch_type == PatchDatabase)
      spop.p.load.i.patch_type = PatchEngine;  // Might be invalid, handled below.
    if (!spop.p.load.i.patches_cache_found && spop.p.load.i.patch_type == PatchEngine)
      spop.p.load.i.patch_type = PatchNone;

    if (!dirsav_avail_sdram(&spop.p.load.i))
      spop.p.load.i.use_dsaving = false;

    // DirSav forces automatic saving
    if (spop.p.load.i.use_dsaving)
      spop.p.load.l.sram_save_type = SaveDirect;
    else if (spop.p.load.l.sram_save_type == SaveDirect)
      spop.p.load.l.sram_save_type = autosave_default ? SaveReboot : SaveDisable;

    // If DS is selected, do not allow manual mode.
    if (spop.p.load.l.sram_load_type == SaveLoadDisable && spop.p.load.i.use_dsaving)
      spop.p.load.l.sram_load_type = SaveLoadSav;
    // If no .sav is available, do not allow that option!
    if (spop.p.load.l.sram_load_type == SaveLoadSav && !spop.p.load.l.savefile_found)
      spop.p.load.l.sram_load_type = SaveLoadReset;
  }

  // Disable ingame-menu if not available.
  if (!ingame_menu_avail_sdram(&spop.p.load.i))
    spop.p.load.i.ingame_menu_enabled = false;

  // If no RTC patches are available, force them to false.
  if (!rtcemu_avail(&spop.p.load.i))
    spop.p.load.i.rtc_patch_enabled = false;

  // Disable cheat loading if no cheats are avail, or IGM is disabled
  if (!spop.p.load.l.cheats_found || !spop.p.load.i.ingame_menu_enabled)
    spop.p.load.l.use_cheats = false;

  if (newkeys & KEY_BUTTA) {
    if (spop.submenu == GbaLoadPopLoadS && spop.selector == GBALdSetRTC && spop.p.load.i.rtc_patch_enabled) {
      void accept_rtc() {
        spop.p.load.l.rtcval = date2timestamp(&spop.rtcpop.val);
      }
      if (spop.p.load.i.rtc_patch_enabled) {
        timestamp2date(spop.p.load.l.rtcval, &spop.rtcpop.val);
        spop.rtcpop.callback = accept_rtc;
      }
    }
    else if (spop.submenu == GbaLoadPopPatch && spop.selector == GBAPatchGen) {
      generate_patches_progress(spop.p.load.i.romfn, spop.p.load.i.romfs);
      spop.alert_msg = msgs[lang_id][MSG_PATCHGEN_OK];
      // Try/Load the just-generated patches.
      spop.p.load.i.patches_cache_found = load_cached_patches(spop.p.load.i.romfn, &spop.p.load.i.patches_cache);
    }
    else if (spop.submenu == GbaLoadPopLoadS && spop.selector == GBALdRemember) {
      // Save settings to disk now!
      t_rom_load_settings ld_sett = {
        .patch_policy = spop.p.load.i.patch_type,
        .use_igm = spop.p.load.i.ingame_menu_enabled,
        .use_rtc = spop.p.load.i.rtc_patch_enabled,
        .use_dsaving = spop.p.load.i.use_dsaving
      };
      t_rom_launch_settings lh_sett = {
        .use_cheats = spop.p.load.l.use_cheats,
        .rtcts = spop.p.load.l.rtcval
      };

      save_rom_settings(spop.p.load.i.romfn, &ld_sett, &lh_sett);
      spop.alert_msg = msgs[lang_id][MSG_REMEMB_CFG_OK];
    }
    else if (GbaLoadPopInfo == spop.submenu) {
      // Insert the ROM into the recent list (or move it around). Flush to disk!
      if (recent_menu)
        insert_recent_flush(spop.p.load.i.romfn, FLAG_RECENT_SD);

      // Honor load.patch_type.
      const t_patch *p = get_game_patch(&spop.p.load.i);
      EnumSavetype st = p ? p->save_mode : SaveTypeNone;

      // Prepare the savegame (load and store stuff, directsave...)
      t_dirsave_info dsinfo;
      unsigned errsave = prepare_savegame(
        spop.p.load.l.sram_load_type, spop.p.load.l.sram_save_type,
        st, &dsinfo, spop.p.load.l.savefn);
      if (errsave) {
        unsigned errmsg = (errsave == ERR_SAVE_BADSAVE)   ? MSG_ERR_SAVERD :
                          (errsave == ERR_SAVE_CANTALLOC) ? MSG_ERR_SAVEPR :
                          (errsave == ERR_SAVE_BADARG)    ? MSG_ERR_SAVEIT :
                                                            MSG_ERR_SAVEWR;
        spop.alert_msg = msgs[lang_id][errmsg];
        return;
      }

      t_rtc_info rtci = {
        .timestamp = spop.p.load.l.rtcval,
        .ts_step = rtcspeed_default
      };

      unsigned err = load_gba_rom(
        spop.p.load.i.romfn, spop.p.load.i.romfs,
        spop.p.load.l.sram_save_type == SaveDisable ? NULL : spop.p.load.l.savefn, p,
        spop.p.load.l.sram_save_type == SaveDirect ? &dsinfo : NULL,
        spop.p.load.i.ingame_menu_enabled,
        spop.p.load.i.rtc_patch_enabled ? &rtci : NULL,
        spop.p.load.l.use_cheats ? spop.p.load.l.cheats_size : 0,
        loadrom_progress);
      if (err) {
        // Show any errors that might have happened!
        spop.alert_msg = msgs[lang_id][MSG_ERR_READ];
        // TODO: We cannot (in many cases) continue since we trash the SDRAM!
      }
    }
  }

  if (psel != spop.selector)
    spop.anim = 0;
}

static void keypress_popup_savefile(unsigned newkeys) {
  if (newkeys & KEY_BUTTUP)
    spop.selector = MAX(0, spop.selector - 1);
  if (newkeys & KEY_BUTTDOWN)
    spop.selector = MIN(SavMAX, spop.selector + 1);

  if (newkeys & KEY_BUTTA) {
    switch (spop.selector) {
    case SaveWrite:
      if (write_save_sram(spop.p.savopt.savfn))
        spop.alert_msg = msgs[lang_id][MSG_SAVOPT_MSG0];
      else
        spop.alert_msg = msgs[lang_id][MSG_SAVOPT_MSG_WERR];
      break;
    case SavLoad:
      if (load_save_sram(spop.p.savopt.savfn))
        show_toast(msgs[lang_id][MSG_SAVOPT_MSG1]);
      else
        spop.alert_msg = msgs[lang_id][MSG_SAVOPT_MSG_RERR];
      break;
    case SavClear:
      if (wipe_sav_file(spop.p.savopt.savfn))
        show_toast(msgs[lang_id][MSG_SAVOPT_MSG2]);
      else
        spop.alert_msg = msgs[lang_id][MSG_SAVOPT_MSG_WERR];
      break;
    case SavQuit:
      spop.pop_num = 0;
      break;
    };
  }
}

static void keypress_popup_flash(unsigned newkeys) {
  if ((curr_pressed_keys() & FLASH_GO_KEYS) == FLASH_GO_KEYS)
    start_flash_update(spop.p.update.fn, spop.p.update.fw_size, spop.p.update.issfw);
}

#ifdef SUPPORT_NORGAMES
static void keypress_popup_norwrite(unsigned newkeys) {
  if (newkeys & KEY_BUTTUP)
    spop.selector = MAX(0, spop.selector - 1);
  if (newkeys & KEY_BUTTDOWN)
    spop.selector = MIN(GBAPatchCNT - 1, spop.selector + 1);

  if (spop.submenu == GbaNorWrPatch) {
    if (newkeys & (KEY_BUTTLEFT|KEY_BUTTRIGHT)) {
      if (spop.selector == GBALoadPatch)
        spop.p.norwr.i.patch_type = (spop.p.norwr.i.patch_type +
                                     ((newkeys & KEY_BUTTRIGHT) ? 1 : PatchOptCNT - 1)) % PatchOptCNT;
      else if (spop.selector == GBAInGameMen)
        spop.p.norwr.i.ingame_menu_enabled = !spop.p.norwr.i.ingame_menu_enabled;
      else if (spop.selector == GBASavePatch)
        spop.p.norwr.i.use_dsaving = !spop.p.norwr.i.use_dsaving;
      else if (spop.selector == GBARTCPatch)
        spop.p.norwr.i.rtc_patch_enabled = !spop.p.norwr.i.rtc_patch_enabled;
    }

    if (newkeys & KEY_BUTTLEFT) {
      // Handle the different cases where the user attempts to select an invalid option.
      if (!spop.p.norwr.i.patches_cache_found && spop.p.norwr.i.patch_type == PatchEngine)
        spop.p.norwr.i.patch_type = PatchDatabase;  // Might be invalid, handled below.
      if (!spop.p.norwr.i.patches_datab_found && spop.p.norwr.i.patch_type == PatchDatabase)
        spop.p.norwr.i.patch_type = PatchNone;
    }
    if (newkeys & KEY_BUTTRIGHT) {
      // If the database has no entry, then do not let the user select that mode.
      if (!spop.p.norwr.i.patches_datab_found && spop.p.norwr.i.patch_type == PatchDatabase)
        spop.p.norwr.i.patch_type = PatchEngine;  // Might be invalid, handled below.
      if (!spop.p.norwr.i.patches_cache_found && spop.p.norwr.i.patch_type == PatchEngine)
        spop.p.norwr.i.patch_type = PatchNone;
    }

    // Disable certain features (depends on patch types)
    if (!dirsav_avail_flash(&spop.p.norwr.i))
      spop.p.norwr.i.use_dsaving = false;
    if (!ingame_menu_avail_flash(&spop.p.norwr.i))
      spop.p.norwr.i.ingame_menu_enabled = false;
    if (!rtcemu_avail(&spop.p.norwr.i))
      spop.p.norwr.i.rtc_patch_enabled = false;

    if ((newkeys & KEY_BUTTA) && spop.selector == GBAPatchGen) {
      generate_patches_progress(spop.p.norwr.i.romfn, spop.p.norwr.i.romfs);
      spop.alert_msg = msgs[lang_id][MSG_PATCHGEN_OK];
      // Try/Load the just-generated patches.
      spop.p.norwr.i.patches_cache_found = load_cached_patches(spop.p.norwr.i.romfn, &spop.p.norwr.i.patches_cache);
    }
  } else {
    if (newkeys & KEY_BUTTA) {
      // Check whether we have enough space.
      unsigned blkcnt = (spop.p.norwr.i.romfs + NOR_BLOCK_SIZE - 1) / NOR_BLOCK_SIZE;
      if (smenu.fbrowser.freeblks < blkcnt || smenu.fbrowser.maxentries + 1 >= FLASHG_MAXFN_CNT)
        spop.alert_msg = msgs[lang_id][MSG_ERR_NORSPC];
      else {
        const t_load_gba_info *info = &spop.p.norwr.i;
        const t_patch *p = get_game_patch(info);

        // Allocate the last entry for the new game.
        t_flash_game_entry ne = {
          .gamecode = *(uint32_t*)info->romh.gcode,
          .gamever = info->romh.version,
          .numblks = blkcnt,
          .gattrs = (info->use_dsaving         ? GATTR_SAVEDS : 0) |
                    (info->ingame_menu_enabled ? GATTR_IGM    : 0) |
                    (info->rtc_patch_enabled   ? GATTR_RTC    : 0) |
                    GATTR_SAVEM(p),
          .bnoffset = (uint8_t)(file_basename(info->romfn) - info->romfn),
          .entry_addr = ROM_ENTRYPOINT(info->romh)
        };
        memset(&ne.blkmap, 0, sizeof(ne.blkmap));
        strbcpy(ne.game_name, info->romfn, sizeof(ne.game_name));

        flashmgr_allocate_blocks(ne.blkmap, blkcnt, (t_reg_entry*)&sdr_state->nordata);

        // Go ahead and start the flasher-loader with patching support.
        unsigned errc = flash_gba_nor(info->romfn, info->romfs, &info->romh, p,
                                      info->use_dsaving, info->ingame_menu_enabled,
                                      info->rtc_patch_enabled,
                                      ne.blkmap, loadrom_progress,
                                      sdr_state->scratch, scratch_mem_size);
        if (errc)
          spop.alert_msg = msgs[lang_id][errc == ERR_LOAD_BADROM ? MSG_ERR_READ : MSG_ERR_NORUPD];
        else {
          // Now we can just write the metadata entry!
          memcpy32(&sdr_state->nordata.games[smenu.fbrowser.maxentries], &ne, sizeof(ne));
          sdr_state->nordata.gamecnt++;
          if (!flashmgr_store(ROM_FLASHMETA_ADDR, FLASH_METADATA_SIZE, (t_reg_entry*)&sdr_state->nordata))
            spop.alert_msg = msgs[lang_id][MSG_ERR_NORUPD];
          else {
            spop.alert_msg = msgs[lang_id][MSG_NOR_WROK];
            spop.pop_num = POPUP_NONE;
          }
        }

        flashbrowser_reload();
      }
    }
  }
}

static void keypress_popup_norload(unsigned newkeys) {
  if (newkeys & KEY_BUTTUP)
    spop.selector = MAX(0, spop.selector - 1);
  if (newkeys & KEY_BUTTDOWN)
    spop.selector = MIN(GBALdSetCNT - 1, spop.selector + 1);

  const t_flash_game_entry *e = spop.p.norld.e;
  bool uses_dsave = e->gattrs & GATTR_SAVEDS;
  bool uses_igm   = e->gattrs & GATTR_IGM;
  bool uses_rtc   = e->gattrs & GATTR_RTC;

  if (newkeys & KEY_BUTTLEFT) {
    if (spop.submenu == GbaNorLoad) {
      if (spop.selector == GBALdSetCheats)
        spop.p.norld.l.use_cheats = !spop.p.norld.l.use_cheats;
      if (uses_dsave) {
        if (spop.selector == GBALdSetLoadP)
          spop.p.norld.l.sram_load_type = (spop.p.norld.l.sram_load_type + SaveLoadDSCNT - 1) % SaveLoadDSCNT;
      } else {
        if (spop.selector == GBALdSetLoadP)
          spop.p.norld.l.sram_load_type = (spop.p.norld.l.sram_load_type + SaveLoadCNT - 1) % SaveLoadCNT;
        else if (spop.selector == GBALdSetSaveP)
          spop.p.norld.l.sram_save_type = (spop.p.norld.l.sram_save_type + SaveCNT - 1) % SaveCNT;
      }
    }

    // DirSav forces automatic saving
    if (uses_dsave)
      spop.p.norld.l.sram_save_type = SaveDirect;
    else if (spop.p.norld.l.sram_save_type == SaveDirect)
      spop.p.norld.l.sram_save_type = autosave_default ? SaveReboot : SaveDisable;

    // If DS is selected, do not allow manual mode.
    if (spop.p.norld.l.sram_load_type == SaveLoadDisable && uses_dsave)
      spop.p.norld.l.sram_load_type = SaveLoadSav;
    // If no .sav is available, do not allow that option!
    if (spop.p.norld.l.sram_load_type == SaveLoadSav && !spop.p.norld.l.savefile_found)
      spop.p.norld.l.sram_load_type = uses_dsave ? SaveLoadReset : SaveLoadDisable;
  }
  if (newkeys & KEY_BUTTRIGHT) {
    if (spop.submenu == GbaNorLoad) {
      if (spop.selector == GBALdSetCheats)
        spop.p.norld.l.use_cheats = !spop.p.norld.l.use_cheats;
      if (uses_dsave) {
        if (spop.selector == GBALdSetLoadP)
          spop.p.norld.l.sram_load_type = (spop.p.norld.l.sram_load_type + 1) % SaveLoadDSCNT;
      } else {
        if (spop.selector == GBALdSetLoadP)
          spop.p.norld.l.sram_load_type = (spop.p.norld.l.sram_load_type + 1) % SaveLoadCNT;
        else if (spop.selector == GBALdSetSaveP)
          spop.p.norld.l.sram_save_type = (spop.p.norld.l.sram_save_type + 1) % SaveCNT;
      }
    }

    // DirSav forces automatic saving
    if (uses_dsave)
      spop.p.norld.l.sram_save_type = SaveDirect;
    else if (spop.p.norld.l.sram_save_type == SaveDirect)
      spop.p.norld.l.sram_save_type = autosave_default ? SaveReboot : SaveDisable;

    // If DS is selected, do not allow manual mode.
    if (spop.p.norld.l.sram_load_type == SaveLoadDisable && uses_dsave)
      spop.p.norld.l.sram_load_type = SaveLoadSav;
    // If no .sav is available, do not allow that option!
    if (spop.p.norld.l.sram_load_type == SaveLoadSav && !spop.p.norld.l.savefile_found)
      spop.p.norld.l.sram_load_type = SaveLoadReset;
  }

  // Disable cheat loading if no cheats are avail, or IGM is disabled
  if (!spop.p.norld.l.cheats_found || !uses_igm)
    spop.p.norld.l.use_cheats = false;

  if (newkeys & KEY_BUTTA) {
    if (spop.submenu == GbaLoadPopInfo) {
      const int stype = GET_GATTR_SAVEM(e->gattrs);
      const EnumSavetype st = stype < 0 ? SaveTypeNone : stype;
      bool uses_dsave = e->gattrs & GATTR_SAVEDS;
      bool uses_igm   = e->gattrs & GATTR_IGM;
      bool uses_rtc   = e->gattrs & GATTR_RTC;

      t_dirsave_info dsinfo;
      unsigned errsave = prepare_savegame(
        spop.p.norld.l.sram_load_type, spop.p.norld.l.sram_save_type,
        st, &dsinfo, spop.p.norld.l.savefn);
      if (errsave) {
        unsigned errmsg = (errsave == ERR_SAVE_BADSAVE)   ? MSG_ERR_SAVERD :
                          (errsave == ERR_SAVE_CANTALLOC) ? MSG_ERR_SAVEPR :
                          (errsave == ERR_SAVE_BADARG)    ? MSG_ERR_SAVEIT :
                                                            MSG_ERR_SAVEWR;
        spop.alert_msg = msgs[lang_id][errmsg];
        return;
      }
      t_rtc_info rtci = {
        .timestamp = spop.p.norld.l.rtcval,
        .ts_step = rtcspeed_default
      };

      if (recent_menu)
        insert_recent_flush(e->game_name, FLAG_RECENT_NOR);

      // TODO Handle errors, finish missing stuff.
      unsigned err = launch_gba_nor(
        e->game_name,
        spop.p.norld.l.sram_save_type == SaveDisable ? NULL : spop.p.norld.l.savefn,
        e->blkmap, e->numblks,
        uses_dsave ? &dsinfo : NULL,
        uses_rtc ? &rtci : NULL,
        uses_igm,
        spop.p.norld.l.use_cheats ? spop.p.norld.l.cheats_size : 0);
    }
    else if (spop.selector == GBALdRemember) {
      // Save settings to disk now!
      t_rom_load_settings ld_sett = {  // Use defaults in case it doesn't really exist
        .patch_policy = patcher_default,
        .use_igm = ingamemenu_default,
        .use_rtc = rtcpatch_default,
        .use_dsaving = autosave_prefer_ds
      };
      t_rom_launch_settings lh_sett = {
        .use_cheats = spop.p.norld.l.use_cheats,
        .rtcts = spop.p.norld.l.rtcval
      };

      // We load the loading settings to ensure we do not overwrite them.
      load_rom_settings(e->game_name, &ld_sett, NULL);
      save_rom_settings(e->game_name, &ld_sett, &lh_sett);
      spop.alert_msg = msgs[lang_id][MSG_REMEMB_CFG_OK];
    }
    else if (spop.selector == GBALdSetRTC) {
      void accept_rtc() {
        spop.p.norld.l.rtcval = date2timestamp(&spop.rtcpop.val);
      }
      if (uses_rtc) {
        timestamp2date(spop.p.norld.l.rtcval, &spop.rtcpop.val);
        spop.rtcpop.callback = accept_rtc;
      }
    }
  }
}
#endif

static void keypress_popup_filemgr(unsigned newkeys) {
  if (newkeys & KEY_BUTTUP)
    spop.selector = MAX(0, spop.selector - 1);
  if (newkeys & KEY_BUTTDOWN)
    spop.selector = MIN(FiMgrCNT - 1, spop.selector + 1);

  if (newkeys & KEY_BUTTA) {
    t_centry *e = sdr_state->filefilter[smenu.browser.selector];
    switch (spop.selector) {
    case FiMgrDelete:
      {
        void remove_file_action(bool confirm) {
          char tmpfn[MAX_FN_LEN];
          strcpy(tmpfn, smenu.browser.cpath);
          strcat(tmpfn, sdr_state->filefilter[smenu.browser.selector]->fname);

          if (confirm) {
            if (FR_OK != f_unlink(tmpfn))
              spop.alert_msg = msgs[lang_id][MSG_ERR_DELFILE];
            else
              spop.alert_msg = msgs[lang_id][MSG_OK_DELFILE];

            browser_reload();   // Force reload so the file disappears!
          }
        }
        spop.qpop.message = msgs[lang_id][MSG_Q0_DELFILE];
        spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
        spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
        spop.qpop.option = 0;
        spop.qpop.callback = remove_file_action;
        spop.qpop.clear_popup_ok = true;
      }
      break;
    case FiMgrHide:
      {
        char tmpfn[MAX_FN_LEN];
        strcpy(tmpfn, smenu.browser.cpath);
        strcat(tmpfn, sdr_state->filefilter[smenu.browser.selector]->fname);

        if (FR_OK == f_chmod(tmpfn, e->attr ^ AM_HID, AM_HID))
          e->attr ^= AM_HID;
        else
          spop.alert_msg = msgs[lang_id][MSG_ERR_GENERIC];
      }
      spop.pop_num = POPUP_NONE;
      break;

    #ifdef SUPPORT_NORGAMES
    case FiMgrWriteNOR:
      if (e->filesize > MAX_GBA_ROM_SIZE)
        spop.alert_msg = msgs[lang_id][MSG_ERR_TOOBIG];
      else {
        char path[MAX_FN_LEN];
        strcpy(path, smenu.browser.cpath);
        strcat(path, e->fname);

        // Load default loading settings if any.
        t_rom_load_settings ld_sett = {
          .patch_policy = patcher_default,
          .use_igm = ingamemenu_default,
          .use_rtc = rtcpatch_default,
          .use_dsaving = autosave_prefer_ds
        };
        load_rom_settings(path, &ld_sett, NULL);

        if (!prepare_gba_info(&spop.p.norwr.i, &ld_sett, path, e->filesize, false))
          spop.alert_msg = msgs[lang_id][MSG_ERR_READ];
        else {
          spop.pop_num = POPUP_GBA_NORWRITE;
          spop.submenu = GbaLoadPopInfo;
          spop.selector = 0;
        }
      }
      break;
    #endif
    };
  }
}

static void keypress_menu_recent(unsigned newkeys) {
  if (smenu.recent.maxentries) {
    if (newkeys & KEY_BUTTUP)
      smenu.recent.selector = MAX(0, smenu.recent.selector - 1);
    else if (newkeys & KEY_BUTTDOWN)
      smenu.recent.selector = MIN(smenu.recent.maxentries - 1, smenu.recent.selector + 1);
    if (newkeys & KEY_BUTTLEFT) {
      smenu.recent.selector = MAX(0, smenu.recent.selector - RECENT_ROWS);
      smenu.recent.seloff   = MAX(0, smenu.recent.seloff - RECENT_ROWS);
    }
    else if (newkeys & KEY_BUTTRIGHT) {
      smenu.recent.selector = MIN(smenu.recent.maxentries - 1, smenu.recent.selector + RECENT_ROWS);
      smenu.recent.seloff   = MIN(smenu.recent.maxentries - 1, smenu.recent.seloff   + RECENT_ROWS);
    }
    if (newkeys & KEY_BUTTA) {
      t_rentry *e = &sdr_state->rentries[smenu.recent.selector];
      #ifdef SUPPORT_NORGAMES
      if (e->flags & FLAG_RECENT_NOR) {
        // Try to find the ROM in the current flash metadata.
        for (unsigned i = 0; i < sdr_state->nordata.gamecnt; i++) {
          const t_flash_game_entry *fe = &sdr_state->nordata.games[i];
          if (!strcmp(e->fpath, fe->game_name)) {
            browser_open_nor(fe);
            return;
          }
        }
        spop.alert_msg = msgs[lang_id][MSG_ERR_GENERIC];
      }
      else
      #endif
      {
        // stat() the file since we need the size, and validate that it exists!
        FILINFO info;
        FRESULT res = f_stat(e->fpath, &info);
        if (res == FR_OK)
          browser_open(e->fpath, info.fsize);
        else
          spop.alert_msg = msgs[lang_id][MSG_ERR_READ];
      }
    }
    else if (newkeys & KEY_BUTTSEL) {
      void recent_del_cb(bool confirm) {
        if (confirm)
          delete_recent_flush(smenu.recent.selector);
      }
      spop.qpop.message = msgs[lang_id][MSG_Q4_DELREC];
      spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
      spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
      spop.qpop.option = 0;
      spop.qpop.callback = recent_del_cb;
      spop.qpop.clear_popup_ok = false;
    }
  }

  if (smenu.recent.selector < smenu.recent.seloff)
    smenu.recent.seloff = smenu.recent.selector;
  else if (smenu.recent.selector >= smenu.recent.seloff + RECENT_ROWS)
    smenu.recent.seloff = smenu.recent.selector - RECENT_ROWS + 1;
}

static void keypress_menu_browse(unsigned newkeys) {
  if (smenu.browser.dispentries) {
    // Move menu up and down
    if (newkeys & KEY_BUTTUP)
      smenu.browser.selector = MAX(0, smenu.browser.selector - 1);
    if (newkeys & KEY_BUTTDOWN)
      smenu.browser.selector = MIN(smenu.browser.dispentries - 1, smenu.browser.selector + 1);
    if (newkeys & KEY_BUTTLEFT) {
      smenu.browser.selector = MAX(0, smenu.browser.selector - BROWSER_ROWS);
      smenu.browser.seloff   = MAX(0, smenu.browser.seloff - BROWSER_ROWS);
    }
    if (newkeys & KEY_BUTTRIGHT) {
      smenu.browser.selector = MIN(smenu.browser.dispentries - 1, smenu.browser.selector + BROWSER_ROWS);
      smenu.browser.seloff   = MIN(smenu.browser.dispentries - 1, smenu.browser.seloff   + BROWSER_ROWS);
    }
    // Move into a new dir and/or open a file
    if (newkeys & KEY_BUTTA) {
      t_centry *e = sdr_state->filefilter[smenu.browser.selector];
      if (e->isdir) {
        strcat(smenu.browser.cpath, e->fname);
        strcat(smenu.browser.cpath, "/");
        // Push selector history and reset it in the new dir
        memmove(&smenu.browser.selhist[1], &smenu.browser.selhist[0],
                sizeof(smenu.browser.selhist) - sizeof(smenu.browser.selhist[0]));
        smenu.browser.selhist[0] = smenu.browser.selector;
        smenu.browser.selector = 0;
        browser_reload();
      } else {
        char path[MAX_FN_LEN];
        strcpy(path, smenu.browser.cpath);
        strcat(path, e->fname);
        browser_open(path, e->filesize);
      }
    }
    else if (newkeys & KEY_BUTTSEL) {
      // Shows a file management menu.
      spop.pop_num = POPUP_FILE_MGR;
      spop.anim = 0;
      spop.selector = 0;
    }
  }
  if (newkeys & KEY_BUTTB) {
    // Try to go up in the dir structure
    if (movedir_up()) {
      smenu.browser.selector = smenu.browser.selhist[0];
      memmove(&smenu.browser.selhist[0], &smenu.browser.selhist[1],
              sizeof(smenu.browser.selhist) - sizeof(smenu.browser.selhist[0]));
      browser_reload();
    }
  }

  // Selector was updated, figure out how we update the menu params so it
  // can be rendered properly.
  if (smenu.browser.selector < smenu.browser.seloff)
    smenu.browser.seloff = smenu.browser.selector;
  else if (smenu.browser.selector >= smenu.browser.seloff + BROWSER_ROWS)
    smenu.browser.seloff = smenu.browser.selector - BROWSER_ROWS + 1;
}

#ifdef SUPPORT_NORGAMES
static void keypress_menu_norbrowse(unsigned newkeys) {
  if (smenu.fbrowser.maxentries) {
    if (newkeys & KEY_BUTTUP)
      smenu.fbrowser.selector = MAX(0, smenu.fbrowser.selector - 1);
    if (newkeys & KEY_BUTTDOWN)
      smenu.fbrowser.selector = MIN(smenu.fbrowser.maxentries - 1, smenu.fbrowser.selector + 1);
    if (newkeys & KEY_BUTTLEFT) {
      smenu.fbrowser.selector = MAX(0, smenu.fbrowser.selector - NORGAMES_ROWS);
      smenu.fbrowser.seloff   = MAX(0, smenu.fbrowser.seloff - NORGAMES_ROWS);
    }
    if (newkeys & KEY_BUTTRIGHT) {
      smenu.fbrowser.selector = MIN(smenu.fbrowser.maxentries - 1, smenu.fbrowser.selector + NORGAMES_ROWS);
      smenu.fbrowser.seloff   = MIN(smenu.fbrowser.maxentries - 1, smenu.fbrowser.seloff   + NORGAMES_ROWS);
    }

    if (newkeys & KEY_BUTTA)
      browser_open_nor(&sdr_state->nordata.games[smenu.fbrowser.selector]);
    else if (newkeys & KEY_BUTTSEL) {
      // Prompt NOR entry deletion.
      void remove_nor_action(bool confirm) {
        if (!confirm)
          return;

        // Remove game entry, just memmove the other games on top.
        sdr_state->nordata.gamecnt--;
        memmove32(&sdr_state->nordata.games[smenu.fbrowser.selector],
                  &sdr_state->nordata.games[smenu.fbrowser.selector + 1],
                  (sdr_state->nordata.gamecnt - smenu.fbrowser.selector) * sizeof(t_flash_game_entry));

        // Go ahead and write a new metadata entry;
        if (!flashmgr_store(ROM_FLASHMETA_ADDR, FLASH_METADATA_SIZE, (t_reg_entry*)&sdr_state->nordata))
          spop.alert_msg = msgs[lang_id][MSG_ERR_NORUPD];
        flashbrowser_reload();   // Force list reload, free block calculation, etc.
      }
      spop.qpop.message = msgs[lang_id][MSG_Q5_DELNORG];
      spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
      spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
      spop.qpop.option = 0;
      spop.qpop.callback = remove_nor_action;
      spop.qpop.clear_popup_ok = true;
    }

    if (smenu.fbrowser.selector < smenu.fbrowser.seloff)
      smenu.fbrowser.seloff = smenu.fbrowser.selector;
    else if (smenu.fbrowser.selector >= smenu.fbrowser.seloff + NORGAMES_ROWS)
      smenu.fbrowser.seloff = smenu.fbrowser.selector - NORGAMES_ROWS + 1;
  }
}
#endif

static void keypress_menu_settings(unsigned newkeys) {
  // Up/Down move through the rows of the group (wrapping around), the window switches the group.
  // Every group remembers its own selected row.
  const unsigned rows = set_group_len(smenu.set.group) + 1;
  uint8_t *rowp = &smenu.set.idx[smenu.set.group];
  if (newkeys & KEY_BUTTUP)
    *rowp = *rowp ? *rowp - 1 : rows - 1;
  if (newkeys & KEY_BUTTDOWN)
    *rowp = (*rowp + 1 < rows) ? *rowp + 1 : 0;
  if (newkeys & (KEY_BUTTUP | KEY_BUTTDOWN)) {
    smenu.anim_state = 0;
    set_sync_selector();
  }
  if (newkeys & KEY_BUTTLEFT) {
    if (smenu.set.selector == SettHotkey)
      hotkey_combo = (hotkey_combo + hotkey_listcnt - 1) % hotkey_listcnt;
    else if (smenu.set.selector == SettSaveLoc)
      save_path_default = (save_path_default + SaveDirCNT - 1) % SaveDirCNT;
    #ifdef SUPPORT_NORGAMES
    else if (smenu.set.selector == SettSaveLocNOR)
      save_path_nor_default = (save_path_nor_default + SaveDirNORCNT - 1) % SaveDirNORCNT;
    #endif
    else if (smenu.set.selector == SettStateLoc)
      state_path_default = (state_path_default + StateDirCNT - 1) % StateDirCNT;
    else if (smenu.set.selector == SettSaveBkp)
      backup_sram_default = backup_sram_default ? backup_sram_default - 1 : 0;
    else if (smenu.set.selector == DefsPatchEng)
      patcher_default = (patcher_default + PatchTotalCNT - 1) % PatchTotalCNT;
    else if (smenu.set.selector == DefsRTCSpeed)
      rtcspeed_default = (rtcspeed_default + RTC_SPEED_CNT - 1) % RTC_SPEED_CNT;
  }
  if (newkeys & KEY_BUTTRIGHT) {
    if (smenu.set.selector == SettHotkey)
      hotkey_combo = (hotkey_combo + 1) % hotkey_listcnt;
    else if (smenu.set.selector == SettSaveLoc)
      save_path_default = (save_path_default + 1) % SaveDirCNT;
    #ifdef SUPPORT_NORGAMES
    else if (smenu.set.selector == SettSaveLocNOR)
      save_path_nor_default = (save_path_nor_default + 1) % SaveDirNORCNT;
    #endif
    else if (smenu.set.selector == SettStateLoc)
      state_path_default = (state_path_default + 1) % StateDirCNT;
    else if (smenu.set.selector == SettSaveBkp)
      backup_sram_default = MIN(MAX_BACKUP_CNT, backup_sram_default + 1);
    else if (smenu.set.selector == DefsPatchEng)
      patcher_default = (patcher_default + 1) % PatchTotalCNT;
    else if (smenu.set.selector == DefsRTCSpeed)
      rtcspeed_default = (rtcspeed_default + 1) % RTC_SPEED_CNT;
  }
  if (newkeys & KEY_BUTTLEFT) {
    if (smenu.set.selector == SettUiTheme)
      menu_theme = menu_theme ? menu_theme - 1 : 0;
    else if (smenu.set.selector == SettUiASpd)
      anim_speed = anim_speed ? anim_speed - 1 : 0;
    else if (smenu.set.selector == SettUiLang)
      lang_id = (lang_id + LANG_COUNT - 1) % LANG_COUNT;
    else if (smenu.set.selector == SettUiSound)
      ui_sound = (ui_sound + 3) % 4;
  }
  if (newkeys & KEY_BUTTRIGHT) {
    if (smenu.set.selector == SettUiTheme)
      menu_theme = MIN(THEME_COUNT - 1, menu_theme + 1);
    else if (smenu.set.selector == SettUiASpd)
      anim_speed = MIN(animspd_cnt - 1, anim_speed + 1);
    else if (smenu.set.selector == SettUiLang)
      lang_id = (lang_id + 1) % LANG_COUNT;
    else if (smenu.set.selector == SettUiSound)
      ui_sound = (ui_sound + 1) % 4;
  }
  if (newkeys & (KEY_BUTTLEFT | KEY_BUTTRIGHT)) {
    if (smenu.set.selector == SettUiHid) {
      hide_hidden ^= 1;
      browser_reload_filter();
    }
    #ifdef BUNDLE_FONT10
    else if (smenu.set.selector == SettUiFont)
      font_size ^= 1;
    #endif
    else if (smenu.set.selector == SettUiAnim)
      ui_anim ^= 1;
    else if (smenu.set.selector == SettUiFx)
      ui_fx ^= 1;
    else if (smenu.set.selector == SettBootType)
      boot_bios_splash ^= 1;
    else if (smenu.set.selector == SettCheatEn)
      enable_cheats ^= 1;
    else if (smenu.set.selector == DefsGamMenu)
      ingamemenu_default ^= 1;
    else if (smenu.set.selector == DefsRTCEnb)
      rtcpatch_default ^= 1;
    else if (smenu.set.selector == DefsLoadPol)
      autoload_default ^= 1;
    else if (smenu.set.selector == DefsSavePol)
      autosave_default ^= 1;
    else if (smenu.set.selector == DefsPrefDS)
      autosave_prefer_ds ^= 1;
    else if (smenu.set.selector == SettFastSD)
      use_slowld ^= 1;
    else if (smenu.set.selector == SettFastEWRAM)
      use_fastew = fastew ? (use_fastew ^ 1) : 0;
    #ifdef SUPPORT_NORGAMES
    else if (smenu.set.selector == SettVerifyNOR)
      use_verify_nor ^= 1;
    #endif
  }

  if (newkeys & KEY_BUTTA && smenu.set.selector == DefsRTCVal) {
    void accept_rtc() {
      rtcvalue_default = date2timestamp(&spop.rtcpop.val);
    }
    timestamp2date(rtcvalue_default, &spop.rtcpop.val);
    spop.rtcpop.callback = accept_rtc;
  }
  if (newkeys & KEY_BUTTA && smenu.set.selector == SettSave) {
    bool ok = save_settings();
    ok = save_ui_settings() && ok;
    if (ok)
      show_toast(msgs[lang_id][MSG_OK_SETSAVE]);
    else
      spop.alert_msg = msgs[lang_id][MSG_ERR_SETSAVE];
  }

  // Apply theme changes immediately
  reload_theme(menu_theme);
}

static void keypress_menu_tools(unsigned newkeys) {
  if (newkeys & KEY_BUTTUP)
    smenu.tools.selector = MAX(0, smenu.tools.selector - 1);
  if (newkeys & KEY_BUTTDOWN)
    smenu.tools.selector = MIN(ToolsMAX - 1, smenu.tools.selector + 1);

  if (newkeys & KEY_BUTTA) {
    if (smenu.tools.selector == ToolsSDRAMTest) {
      // Performs a test on the SRAM/SDRAM, ensure they are fine.
      set_supercard_mode(MAPPED_SDRAM, true, false);

      if (sdram_test(loadrom_progress_abort))
        spop.alert_msg = msgs[lang_id][MSG_BAD_SDRAM];
      else
        spop.alert_msg = msgs[lang_id][MSG_GOOD_RAM];

      set_supercard_mode(MAPPED_SDRAM, true, true);
    }
    if (smenu.tools.selector == ToolsSRAMTest) {
      if (sram_test())
        spop.alert_msg = msgs[lang_id][MSG_BAD_SRAM];
      else
        spop.alert_msg = msgs[lang_id][MSG_GOOD_RAM];
    }
    else if (smenu.tools.selector == ToolsBatteryTest) {
      // Go ahead and fill in SRAM with a pattern.
      spop.qpop.message = msgs[lang_id][MSG_Q2_SRAMTST];
      spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
      spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
      spop.qpop.option = 0;
      spop.qpop.callback = sram_battery_test_callback;
      spop.qpop.clear_popup_ok = true;
    }
    else if (smenu.tools.selector == ToolsSDBench) {
      slowsd = use_slowld;
      int ret = sdbench_read(loadrom_progress_abort);
      slowsd = true;
      if (ret < 0)
        spop.alert_msg = msgs[lang_id][MSG_ERR_GENERIC];
      else {
        unsigned speed = 8*1024*1024 / (unsigned)ret;
        npf_snprintf(smenu.info.tstr, sizeof(smenu.info.tstr), msgs[lang_id][MSG_BENCHSPD], speed);
        spop.alert_msg = smenu.info.tstr;
      }
    }
    else if (smenu.tools.selector == ToolsFlashBak) {
      // Backup the flash contents to a file.
      if (dump_flashmem_backup())
        show_toast(msgs[lang_id][MSG_FLASH_READOK]);
      else
        spop.alert_msg = msgs[lang_id][MSG_ERR_GENERIC];

      browser_reload();
    }
    #ifdef SUPPORT_NORGAMES
    else if (smenu.tools.selector == ToolsFlashClr) {
      void flash_clear_callback(bool confirm) {
        if (confirm) {
          // Delete all metadata (data is not really wiped, takes too long)
          if (flashmgr_wipe(ROM_FLASHMETA_ADDR, FLASH_METADATA_SIZE))
            spop.alert_msg = msgs[lang_id][MSG_NOR_CLOK];
          else
            spop.alert_msg = msgs[lang_id][MSG_ERR_NORUPD];

          flashbrowser_reload();     // Ensure we clear the NOR entries from RAM.
        }
      }
      // Prompt the user for clearing the memory.
      spop.qpop.message = msgs[lang_id][MSG_Q6_CLRNOR];
      spop.qpop.default_button = msgs[lang_id][MSG_Q_NO];
      spop.qpop.confirm_button = msgs[lang_id][MSG_Q_YES];
      spop.qpop.option = 0;
      spop.qpop.callback = flash_clear_callback;
      spop.qpop.clear_popup_ok = true;
    }
    #endif
  }
}

// Info tab: left/right switch the info panel, A toggles the temporary flashing permission.
static void keypress_menu_info(unsigned newkeys) {
  if (newkeys & KEY_BUTTRIGHT)
    smenu.info.selector = (smenu.info.selector + 1) % 4;
  if (newkeys & KEY_BUTTLEFT)
    smenu.info.selector = (smenu.info.selector + 3) % 4;
  if (newkeys & KEY_BUTTA)
    enable_flashing = !enable_flashing;
}

// The START window: L/R switch the tab, the keys of the tab go to the tab itself.
static void keypress_popup_system(unsigned newkeys) {
  if (newkeys & KEY_BUTTSTA) {
    spop.pop_num = POPUP_NONE;
    return;
  }
  if (newkeys & KEY_BUTTL)
    smenu.sys_tab = (smenu.sys_tab + SysTabCNT - 1) % SysTabCNT;
  if (newkeys & KEY_BUTTR)
    smenu.sys_tab = (smenu.sys_tab + 1) % SysTabCNT;
  if (newkeys & (KEY_BUTTL | KEY_BUTTR))
    smenu.anim_state = 0;
  newkeys &= ~(unsigned)(KEY_BUTTL | KEY_BUTTR);

  if (smenu.sys_tab < SetGrpCNT) {
    smenu.set.group = smenu.sys_tab;
    set_sync_selector();
    keypress_menu_settings(newkeys);
  }
  else if (smenu.sys_tab == SysTabTools)
    keypress_menu_tools(newkeys);
  else
    keypress_menu_info(newkeys);
}

static void menu_keypress_inner(unsigned newkeys) {
  if (spop.alert_msg) {
    // Modal message pop up!
    if (newkeys & (KEY_BUTTA | KEY_BUTTB))
      spop.alert_msg = NULL;
  }
  else if (spop.qpop.message) {
    // Modal confirm/question dialog
    if (newkeys & (KEY_BUTTUP | KEY_BUTTDOWN))
      spop.qpop.option ^= 1;
    else if (newkeys & KEY_BUTTB)
      spop.qpop.message = NULL;   // Exit the modal dialog.
    else if (newkeys & KEY_BUTTA) {
      if (spop.qpop.callback) {
        if (spop.qpop.option && spop.qpop.clear_popup_ok)
          spop.pop_num = POPUP_NONE;
        spop.qpop.callback(spop.qpop.option);
      }
      spop.qpop.message = NULL;   // Exit the modal dialog.
    }
  }
  else if (spop.rtcpop.callback) {
    if (newkeys & KEY_BUTTLEFT)
      spop.rtcpop.selector = MAX(0, spop.rtcpop.selector - 1);
    if (newkeys & KEY_BUTTRIGHT)
      spop.rtcpop.selector = MIN(4, spop.rtcpop.selector + 1);

    if (newkeys & KEY_BUTTUP)
      ((uint8_t*)&spop.rtcpop.val)[spop.rtcpop.selector]++;
    if (newkeys & KEY_BUTTDOWN)
      ((uint8_t*)&spop.rtcpop.val)[spop.rtcpop.selector]--;

    if (newkeys & (KEY_BUTTUP|KEY_BUTTDOWN))
      fixdate(&spop.rtcpop.val);

    if (newkeys & KEY_BUTTB) {
      spop.rtcpop.selector = 0;
      spop.rtcpop.callback = NULL;
    }
    else if (newkeys & KEY_BUTTA) {
      spop.rtcpop.selector = 0;
      spop.rtcpop.callback();
      spop.rtcpop.callback = NULL;
    }
  }
  else if (spop.pop_num) {
    const int subcnt = popup_windows[spop.pop_num - 1].max_submenu;
    // L/R switch sub-pages, but only in pop-ups that actually have sub-pages
    // (the START menu and the settings window do not use them).
    if (subcnt > 1 && (newkeys & KEY_BUTTL))
      spop.submenu = (spop.submenu + subcnt - 1) % subcnt;
    if (subcnt > 1 && (newkeys & KEY_BUTTR))
      spop.submenu = (spop.submenu + 1) % subcnt;

    // Close pop-up on B button.
    if (newkeys & KEY_BUTTB) {
      spop.pop_num = POPUP_NONE;
    }
    else {
      const t_mkeyupd_fn keyfns[] = {
        NULL,
        keypress_popup_loadgba,
        keypress_popup_savefile,
        keypress_popup_flash,
        keypress_popup_filemgr,
        #ifdef SUPPORT_NORGAMES
        keypress_popup_norwrite,
        keypress_popup_norload,
        #endif
        keypress_popup_system,
      };
      keyfns[spop.pop_num](newkeys);
    }
  } else {
    // L/R cycles through the game tabs, START opens the system menu.
    if (newkeys & (KEY_BUTTL | KEY_BUTTR)) {
      unsigned cur = 0;
      for (unsigned i = 0; i < game_tab_cnt; i++)
        if (game_tabs[i] == smenu.menu_tab)
          cur = i;
      cur = (newkeys & KEY_BUTTL) ? (cur + game_tab_cnt - 1) % game_tab_cnt
                                  : (cur + 1) % game_tab_cnt;
      smenu.menu_tab = game_tabs[cur];
    }
    if (newkeys & KEY_BUTTSTA) {
      spop.pop_num = POPUP_SYSTEM;       // Opens in the last tab used
      spop.anim = 0;
      smenu.anim_state = 0;
    }

    if (newkeys & (KEY_BUTTL | KEY_BUTTR | KEY_BUTTUP | KEY_BUTTDOWN))
      smenu.anim_state = 0;
    if ((newkeys & KEY_BUTTA) && !spop.pop_num)
      press_flash = 7;

    const t_mkeyupd_fn keyfns[] = {
      keypress_menu_recent,
      keypress_menu_browse,
      #ifdef SUPPORT_NORGAMES
      keypress_menu_norbrowse,
      #endif
    };
    // Do not handle the key that just opened a pop-up in the page below.
    if (!spop.pop_num)
      keyfns[smenu.menu_tab](newkeys);
  }
}

void menu_keypress(unsigned newkeys) {
  menu_keypress_inner(newkeys);

  // Navigation sounds (the keys are handled first so a changed sound setting applies right away)
  if (!ui_sound) {
    if (snd_on)
      ui_sound_stop();
    return;
  }
  if (newkeys & KEY_BUTTA)
    ui_beep(UiSndOk);
  else if (newkeys & KEY_BUTTB)
    ui_beep(UiSndBack);
  else if (newkeys & (KEY_BUTTL | KEY_BUTTR | KEY_BUTTSTA))
    ui_beep(UiSndTab);
  else if (newkeys & (KEY_BUTTUP | KEY_BUTTDOWN | KEY_BUTTLEFT | KEY_BUTTRIGHT))
    ui_beep(UiSndMove);
}

// Only repeat keys A/B and Dir
const uint16_t keyrep = 0x0F3;
static uint32_t keyreptmr[10] = {0};
static uint8_t  keyrepcnt[10] = {0};
static uint32_t prev_keys = 0;

// Handle button input. Supports key re-press whenever a button is held for a while.
// This key repeat pattern can be tuned for speed and what not.
uint16_t get_keypress() {
  uint32_t ckeys = curr_pressed_keys();
  uint32_t mkeys = 0;
  for (unsigned i = 0; i < 10; i++) {
    if (ckeys & (1 << i)) {
      if (!(prev_keys & (1 << i))) {
        keyreptmr[i] = systime() + KEY_REPEAT_INITIAL;
        mkeys |= (1 << i);
        keyrepcnt[i] = 0;
      }
      else if (((1 << i) & keyrep) && systime() > keyreptmr[i]) {
        if (keyrepcnt[i] > KEY_REPEAT_CNT1)
          keyreptmr[i] = systime() + KEY_REPEAT_FAST;
        else {
          keyreptmr[i] = systime() + KEY_REPEAT_MID;
          keyrepcnt[i]++;
        }
        mkeys |= (1 << i);
      }
    }
    else
      keyreptmr[i] = 0;
  }

  prev_keys = ckeys;
  return mkeys;
}

