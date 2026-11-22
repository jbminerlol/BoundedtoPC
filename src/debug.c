/* Test/debug taps called from hooked game functions (see HOOKS in tools/recomp.py).
 * EB_LOG_TEXT=1 logs the text the game prints into its windows, decoded with ebsrc's character
 * table, so scripted test runs can follow menus without screenshots. */
#include "eb_texttable.h"
#include "snes.h"

int g_log_text;
long g_name_prompt_frame; /* frame when "name" was last printed (test auto-play) */
static char s_line[256];
static int s_len;
static char s_recent[8];

void eb_hook_print_letter(void) {
  char ch = eb_text_ascii[cpu.A & 0xFF];
  memmove(s_recent, s_recent + 1, sizeof s_recent - 1);
  s_recent[sizeof s_recent - 1] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
  /* every naming keyboard offers "Don't Care": that marks a name prompt */
  if (!memcmp(s_recent + 4, "care", 4)) g_name_prompt_frame = (long)g_frame_count;
  if (!g_log_text) return;
  if (s_len < (int)sizeof s_line - 1) s_line[s_len++] = ch;
}

void eb_hook_print_newline(void) {
  if (!g_log_text || !s_len) return;
  s_line[s_len] = 0;
  eb_log("text: %s", s_line);
  s_len = 0;
}

void eb_text_flush(void) { eb_hook_print_newline(); }
