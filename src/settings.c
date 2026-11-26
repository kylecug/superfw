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
#include <stdlib.h>

#include "settings.h"
#include "fatfs/ff.h"
#include "common.h"
#include "nanoprintf.h"
#include "util.h"

#pragma GCC optimize ("Os")

unsigned lang_lookup(uint16_t code);
uint16_t lang_getcode();

const t_theme_colors themes[THEME_COUNT] = {
  { RGB2GBA(0xc5d3ea), RGB2GBA(0xffffff), RGB2GBA(0x0a1428), RGB2GBA(0x6ab0ff), RGB2GBA(0x0d47a1), RGB2GBA(0x8a94a6) }, // Daylight
  { RGB2GBA(0x1e3a8a), RGB2GBA(0x060a18), RGB2GBA(0xf2f6ff), RGB2GBA(0x2a5fdc), RGB2GBA(0x2f6bf0), RGB2GBA(0x7c8db5) }, // Midnight
  { RGB2GBA(0x0b3d1c), RGB2GBA(0x000000), RGB2GBA(0x5cff8f), RGB2GBA(0x0c6b2d), RGB2GBA(0x00a152), RGB2GBA(0x2f8f52) }, // Terminal
  { RGB2GBA(0xf6c1d3), RGB2GBA(0xfff3f7), RGB2GBA(0x2b0a1a), RGB2GBA(0xff86ae), RGB2GBA(0xa3124f), RGB2GBA(0xa58593) }, // Sakura
  { RGB2GBA(0x4a2c00), RGB2GBA(0x100800), RGB2GBA(0xffb938), RGB2GBA(0x704400), RGB2GBA(0xa86a00), RGB2GBA(0x9a6b1e) }, // Amber
  { RGB2GBA(0xb7ddae), RGB2GBA(0xf4fbec), RGB2GBA(0x07240d), RGB2GBA(0x5fc25f), RGB2GBA(0x1b5e20), RGB2GBA(0x7d9c80) }, // Forest
  { RGB2GBA(0x6a2478), RGB2GBA(0x1a0a2e), RGB2GBA(0xffefe0), RGB2GBA(0xb53a08), RGB2GBA(0xc8420a), RGB2GBA(0xb08fa6) }, // Sunset
  { RGB2GBA(0xd9bf97), RGB2GBA(0xf8eee0), RGB2GBA(0x2b1709), RGB2GBA(0xe4953a), RGB2GBA(0x7a3f12), RGB2GBA(0x9b8671) }, // Coffee
  { RGB2GBA(0x0c5c80), RGB2GBA(0x04202f), RGB2GBA(0xe8faff), RGB2GBA(0x0b7a96), RGB2GBA(0x0e9cc0), RGB2GBA(0x6fa3b5) }, // Ocean
  { RGB2GBA(0x3c3c3c), RGB2GBA(0x000000), RGB2GBA(0xffff33), RGB2GBA(0x1010ff), RGB2GBA(0x7070ff), RGB2GBA(0x999933) }, // Contrast
  { RGB2GBA(0xd2c6f0), RGB2GBA(0xf8f4ff), RGB2GBA(0x1e1233), RGB2GBA(0xa98cff), RGB2GBA(0x5b34c7), RGB2GBA(0x9a8eb8) }, // Lavender
  { RGB2GBA(0xafe8d4), RGB2GBA(0xf2fffa), RGB2GBA(0x06291f), RGB2GBA(0x3fcfa2), RGB2GBA(0x0b6b50), RGB2GBA(0x7da99a) }, // Mint
  { RGB2GBA(0x3a3a3c), RGB2GBA(0x151517), RGB2GBA(0xf2f2f7), RGB2GBA(0x5c5c61), RGB2GBA(0xa0a0a8), RGB2GBA(0x8e8e93) }, // Graphite
  { RGB2GBA(0x7a0f1e), RGB2GBA(0x1a0508), RGB2GBA(0xffeaed), RGB2GBA(0xb8182f), RGB2GBA(0xd8344d), RGB2GBA(0xb98a91) }, // Crimson
  { RGB2GBA(0x1b2b6b), RGB2GBA(0x070c26), RGB2GBA(0xffdf70), RGB2GBA(0x34459e), RGB2GBA(0x5468d8), RGB2GBA(0xb8a25a) }, // Royal
  { RGB2GBA(0xffcfb0), RGB2GBA(0xfff5ee), RGB2GBA(0x3a1a0a), RGB2GBA(0xff9e6b), RGB2GBA(0xb5440f), RGB2GBA(0xb08f7c) }, // Peach
  { RGB2GBA(0xc2e4f6), RGB2GBA(0xf5fbff), RGB2GBA(0x06293d), RGB2GBA(0x5cb8ea), RGB2GBA(0x0b5f8f), RGB2GBA(0x7fa3b8) }, // Arctic
  { RGB2GBA(0x3d0b5c), RGB2GBA(0x0a0010), RGB2GBA(0x5cfff5), RGB2GBA(0x8c1486), RGB2GBA(0xb0189f), RGB2GBA(0x4da9a3) }, // Neon
  { RGB2GBA(0x3c4a14), RGB2GBA(0x11130a), RGB2GBA(0xecf3c4), RGB2GBA(0x62771c), RGB2GBA(0x93ac26), RGB2GBA(0x98a36b) }, // Olive
  { RGB2GBA(0xd9d9d1), RGB2GBA(0xf6f6f1), RGB2GBA(0x111111), RGB2GBA(0xa3a39b), RGB2GBA(0x3b3b38), RGB2GBA(0x8c8c86) }, // Ink
};

const t_combo_key hotkey_list[] = {
  {"L+R+Start",     0x00F7},
  {"L+R+Select",    0x00FB},
  {"L+R+Start+Sel", 0x00F3},
  {"L+R",           0x00FF},
  {"L+R+A",         0x00FE},
  {"L+R+B",         0x00FD},
  {"L+R+⯇+A",       0x00DE},
  {"L+R+⯈+B",       0x00ED},
  {"L+R+⯅+A",       0x00BE},
  {"L+R+⯆+A",       0x007E},
  {"A+B+Start",     0x03F4},
  {"A+B+Select",    0x03F8},
  {"A+B+Start+Sel", 0x03F0},
};

const char *save_paths[] = {
  "/SAVEGAME/",
  "/SAVES/",
};

const char *savestates_paths[] = {
  "/SAVESTATE/",
  "/.superfw/savestate/",
};

const char *savestates_paths_display[] = {
  "/SAVESTATE/",
  ".sfw/savestate",
};

const uint8_t animspd_lut[] = {
  2,    //  8 pix/second
  3,    // 12 pix/second
  6,    // 24 pix/second
  8,    // 32 pix/second
  12,   // 48 pix/second
};

// Menu settings
uint8_t menu_theme = 0;
uint8_t lang_id = 0;
uint8_t recent_menu = 1;   // Always on (the recent tab is no longer optional)
uint8_t hide_hidden = 0;
uint8_t font_size = 0;
uint8_t ui_anim = 1;
uint8_t ui_fx = 1;
uint8_t ui_sound = 0;
uint8_t anim_speed = animspd_cnt / 2;

// Default settings
t_patch_policy patcher_default = PatchAuto;

uint8_t boot_bios_splash = 0;   // Whether the BIOS boots to the splash screen
uint8_t use_slowld = 0;         // Use slow mirrors for ROM loading, check loaded data.
uint8_t use_fastew = 0;         // Overclock EWRAM while playing.
uint8_t use_verify_nor = 0;     // Verify flash writes

uint8_t save_path_default = SaveSavegameDir;
uint8_t save_path_nor_default = SaveSavegameDir;
uint8_t state_path_default = StateSavestateDir;

uint8_t backup_sram_default = 0;  // Number of older SRAM save to keep as backup

uint8_t hotkey_combo = 0;  // Hotkey Combo number
uint8_t enable_cheats = 0; // By default cheats are disabled (it's slightly faster)

uint8_t autoload_default = 1;
uint8_t autosave_default = 1;
uint8_t autosave_prefer_ds = 1;
uint8_t ingamemenu_default = 1;
uint8_t rtcpatch_default = 1;
uint8_t rtcspeed_default = 3;

uint32_t rtcvalue_default = 45568800U;

// Setting loading/saving routines
bool save_ui_settings() {
  // Create the directory (just in case it doesn't exist)
  f_mkdir(SUPERFW_DIR);

  // Proceed to create the file
  FIL fd;
  if (FR_OK != f_open(&fd, UISETTINGS_FILEPATH, FA_WRITE | FA_CREATE_ALWAYS))
    return false;

  // Serialize the settings
  uint16_t lc = lang_getcode();
  char buf[512];
  npf_snprintf(buf, sizeof(buf),
    "theme=%u\n"
    "langcode=%c%c\n"
    "anim_speed=%u\n"
    "hide_hidden=%u\n"
    "font_size=%u\n"
    "ui_anim=%u\n"
    "ui_fx=%u\n"
    "ui_sound=%u\n",
    menu_theme, (lc & 0xFF), (lc >> 8), anim_speed, hide_hidden, font_size,
    ui_anim, ui_fx, ui_sound);

  UINT wrbytes;
  FRESULT res = f_write(&fd, buf, strlen(buf), &wrbytes);
  f_close(&fd);

  return FR_OK == res;
}

bool save_settings() {
  // Create the directory (just in case it doesn't exist)
  f_mkdir(SUPERFW_DIR);

  // Proceed to create the file
  FIL fd;
  if (FR_OK != f_open(&fd, SETTINGS_FILEPATH, FA_WRITE | FA_CREATE_ALWAYS))
    return false;

  // Serialize the settings
  char buf[512];
  npf_snprintf(buf, sizeof(buf),
    "hotkey_opt=%u\n"
    "boot_to_bios=%u\n"
    "save_path_policy=%u\n"
    "save_path_nor_policy=%u\n"
    "state_path_policy=%u\n"
    "sram_backup_count=%u\n"
    "enable_cheats=%u\n"
    "enable_slowld=%u\n"
    "enable_fastewram=%u\n"
    "enable_norwrcheck=%u\n"
    "default_patcher=%u\n"
    "default_igmenu=%u\n"
    "default_rtcpatch=%u\n"
    "default_rtctick=%u\n"
    "default_loadgame=%u\n"
    "default_savegame=%u\n"
    "prefer_directsave=%u\n"
    "default_rtcts=%lu\n",
    hotkey_combo, boot_bios_splash, save_path_default, save_path_nor_default,
    state_path_default, backup_sram_default, enable_cheats, use_slowld, use_fastew,
    use_verify_nor, (unsigned int)patcher_default, ingamemenu_default, rtcpatch_default,
    rtcspeed_default, autoload_default, autosave_default, autosave_prefer_ds,
    rtcvalue_default);

  UINT wrbytes;
  FRESULT res = f_write(&fd, buf, strlen(buf), &wrbytes);
  f_close(&fd);

  return FR_OK == res;
}

static void parse_settings(void *usr, const char *var, const char *value) {
  unsigned valu = parseuint(value);
  if (!strcmp(var, "default_rtcts"))
    rtcvalue_default = valu;
  else {
    static const struct {
      const char *s;
      uint8_t * const var;
    } bolset[] = {
      { "boot_to_bios",      &boot_bios_splash },
      { "enable_cheats",     &enable_cheats },
      { "default_igmenu",    &ingamemenu_default },
      { "enable_slowld",     &use_slowld },
      { "enable_fastewram",  &use_fastew },
      { "enable_norwrcheck", &use_verify_nor },
      { "default_rtcpatch",  &rtcpatch_default },
      { "default_loadgame",  &autoload_default },
      { "default_savegame",  &autosave_default },
      { "prefer_directsave", &autosave_prefer_ds },
    };
    for (unsigned i = 0; i < sizeof(bolset)/sizeof(bolset[0]); i++)
      if (!strcmp(var, bolset[i].s)) {
        *bolset[i].var = valu & 1;
        break;
      }

    static const struct {
      const char *s;
      uint8_t * const var;
      const unsigned modval;
    } uintset[] = {
      { "hotkey_opt",           &hotkey_combo,          sizeof(hotkey_list)/sizeof(hotkey_list[0]) },
      { "save_path_policy",     &save_path_default,     SaveDirCNT },
      { "save_path_nor_policy", &save_path_nor_default, SaveDirNORCNT },
      { "state_path_policy",    &state_path_default,    StateDirCNT },
      { "sram_backup_count",    &backup_sram_default,   MAX_BACKUP_CNT + 1 },
      { "default_patcher",      &patcher_default,       PatchTotalCNT },
      { "default_rtctick",      &rtcspeed_default,      RTC_SPEED_CNT },
    };
    for (unsigned i = 0; i < sizeof(uintset)/sizeof(uintset[0]); i++)
      if (!strcmp(var, uintset[i].s)) {
        *uintset[i].var = valu % uintset[i].modval;
        break;
      }
  }
}

static void parse_ui_settings(void *usr, const char *var, const char *value) {
  if (!strcmp(var, "langcode")) {
    uint16_t code = ((uint8_t)value[0]) | (((uint8_t)value[1]) << 8);
    lang_id = lang_lookup(code);
  } else {
    static const struct {
      const char *s;
      uint8_t * const var;
      unsigned maxval;
    } uintset[] = {
      { "theme",       &menu_theme,  THEME_COUNT },
      { "hide_hidden", &hide_hidden, 2 },
      { "anim_speed",  &anim_speed,  animspd_cnt },
      { "font_size",   &font_size,   2 },
      { "ui_anim",     &ui_anim,     2 },
      { "ui_fx",       &ui_fx,       2 },
      { "ui_sound",    &ui_sound,    4 },
    };
    unsigned valu = parseuint(value);
    for (unsigned i = 0; i < sizeof(uintset)/sizeof(uintset[0]); i++)
      if (!strcmp(var, uintset[i].s)) {
        *uintset[i].var = valu < uintset[i].maxval ? valu : 0;
        break;
      }
  }
}

// Parses a config file and calls the user callback with varname+value
// Pointers are only valid for the duration of the callback!
static void parse_file(char *buf, void(*parse_cb)(void *usr, const char*, const char*), void *usrptr) {
  char *p = buf;
  while (1) {
    char *e = strchr(p, '\n');
    if (e)
      *e = 0;

    char *a = strchr(p, '=');
    if (a) {
      *a = 0;
      parse_cb(usrptr, p, &a[1]);
      *a = '=';
    }

    if (!e)
      break;

    *e = '\n';
    p = &e[1];  // Advance to the next line
  }
}

void load_settings() {
  FIL fd;
  UINT rdbytes;
  char buf[512];
  if (FR_OK == f_open(&fd, SETTINGS_FILEPATH, FA_READ)) {
    if (FR_OK == f_read(&fd, buf, sizeof(buf) - 1, &rdbytes)) {
      buf[rdbytes] = 0;
      parse_file(buf, parse_settings, NULL);
    }
    f_close(&fd);
  }

  if (FR_OK == f_open(&fd, UISETTINGS_FILEPATH, FA_READ)) {
    if (FR_OK == f_read(&fd, buf, sizeof(buf) - 1, &rdbytes)) {
      buf[rdbytes] = 0;
      parse_file(buf, parse_ui_settings, NULL);
    }
    f_close(&fd);
  }
}

void sram_filename_calc(const char *rom, char *savefn, unsigned save_path) {
  if (save_path == SaveRomName) {
    strcpy(savefn, rom);   // Use the full ROM path
  }
  else {
    const char *p = file_basename(rom);
    const char *path = save_paths[save_path];
    strcpy(savefn, path);   // Add the base path
    strcat(savefn, p);      // Append just the basename
  }

  replace_extension(savefn, ".sav");
}

void savestate_filename_calc(const char *rom, char *statefn) {
  const char *p = file_basename(rom);
  strcpy(statefn, savestates_paths[state_path_default]);   // Add the base path
  strcat(statefn, p);               // Append just the basename
  replace_extension(statefn, "");
}

static void parse_rom_load_settings(void *usr, const char *var, const char *value) {
  t_rom_load_settings *rs = (t_rom_load_settings*)usr;
  unsigned valu = parseuint(value);
  if (!strcmp(var, "rtc"))
    rs->use_rtc = valu & 1;
  else if (!strcmp(var, "igm"))
    rs->use_igm = valu & 1;
  else if (!strcmp(var, "directsaving"))
    rs->use_dsaving = valu & 1;
  else if (!strcmp(var, "patchmode"))
    rs->patch_policy = valu % PatchOptCNT;
}

static void parse_rom_launch_settings(void *usr, const char *var, const char *value) {
  t_rom_launch_settings *rs = (t_rom_launch_settings*)usr;
  unsigned valu = parseuint(value);
  if (!strcmp(var, "cheats"))
    rs->use_cheats = valu & 1;
  else if (!strcmp(var, "rtcts"))
    rs->rtcts = valu;
}

bool load_rom_settings(const char *fn, t_rom_load_settings *rld, t_rom_launch_settings *rlh) {
  FIL fd;
  {
    char path[MAX_FN_LEN];
    strcpy(path, ROMCONFIG_PATH);
    strcat(path, file_basename(fn));
    replace_extension(path, ".config");

    // Attempt to open and read the file.
    if (FR_OK != f_open(&fd, path, FA_READ))
      return false;
  }

  {
    char buf[128];
    UINT rdbytes;
    if (FR_OK == f_read(&fd, buf, sizeof(buf) - 1, &rdbytes)) {
      buf[rdbytes] = 0;
      if (rld)
        parse_file(buf, parse_rom_load_settings, rld);
      if (rlh)
        parse_file(buf, parse_rom_launch_settings, rlh);
    }
  }
  f_close(&fd);

  return true;
}

bool save_rom_settings(const char *fn, const t_rom_load_settings *rld, const t_rom_launch_settings *rlh) {
  // Create the directory (just in case it doesn't exist)
  f_mkdir(SUPERFW_DIR);
  f_mkdir(ROMCONFIG_PATH);

  // Proceed to create the file
  FIL fd;
  {
    char path[MAX_FN_LEN];
    strcpy(path, ROMCONFIG_PATH);
    strcat(path, file_basename(fn));
    replace_extension(path, ".config");

    if (FR_OK != f_open(&fd, path, FA_WRITE | FA_CREATE_ALWAYS))
      return false;
  }

  // Serialize the ROM settings
  FRESULT res;
  {
    char buf[128];
    npf_snprintf(buf, sizeof(buf),
      "patchmode=%u\n"
      "igm=%u\n"
      "rtc=%u\n"
      "directsaving=%u\n"
      "cheats=%u\n"
      "rtcts=%u\n",
      rld->patch_policy,
      rld->use_igm ? 1 : 0,
      rld->use_rtc ? 1 : 0,
      rld->use_dsaving ? 1 : 0,
      rlh->use_cheats ? 1 : 0,
      (unsigned int)rlh->rtcts);

    UINT wrbytes;
    res = f_write(&fd, buf, strlen(buf), &wrbytes);
    f_close(&fd);
  }

  return FR_OK == res;
}


