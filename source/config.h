/* Jetpack Joyride 1.104.1 / Halfbrick Mortar wrapper configuration. */

#ifndef JETPACKJOYRIDE_CONFIG_H
#define JETPACKJOYRIDE_CONFIG_H

#include <stddef.h>

/* The Bionic mmap bridge uses a separately aligned arena. */
#define MMAP_ARENA_ALIGN_36 ((size_t) 64 * 1024 * 1024)
#define MMAP_ARENA_ALIGN_39 ((size_t)128 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)384 * 1024 * 1024)
#define OC_WINDOW_BYTES     ((size_t)384 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t)256 * 1024 * 1024)

#define GAME_PACKAGE      "com.halfbrick.jetpackjoyride"
#define GAME_VERSION_CODE 853070000
#define GAME_VERSION_NAME "1.104.1"

#define CONFIG_NAME "config.txt"
#define GAME_HOME   "sdmc:/switch/jetpackjoyride_nx"
extern int screen_width;
extern int screen_height;

#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

typedef struct {
  int screen_width;
  int screen_height;
  int language;
  int portrait;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
