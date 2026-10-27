// On-screen keyboard. Unity's TouchScreenKeyboard calls the Java side
// UnityPlayer.showSoftInput/hideSoftInput/setSoftInputStr; here they open the
// Vita's IME dialog. The dialog runs alongside the game: the render loop polls
// it every frame and, once it closes, hands the text back to Unity through
// nativeSetInputString + nativeSoftInputClosed (what the Android EditText
// dialog does on "done").

#include "bridges.h"
#include "../util/log.h"

#include <stdio.h>
#include <string.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>

#define MAX_TEXT 512

static int running, hide_requested;
static SceWChar16 title16[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static SceWChar16 initial16[MAX_TEXT + 1];
static SceWChar16 input16[MAX_TEXT + 1];
static char initial_utf8[MAX_TEXT * 4 + 1];
static char result_utf8[MAX_TEXT * 4 + 1];

static void utf8_to_utf16(const char *in, SceWChar16 *out, size_t out_len) {
  const uint8_t *s = (const uint8_t *)(in ? in : "");
  size_t o = 0;
  while (*s && o + 2 < out_len) {
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra)
      c &= 0x3F >> extra;
    while (extra-- && (*s & 0xC0) == 0x80)
      c = (c << 6) | (*s++ & 0x3F);
    if (c >= 0x10000) {
      c -= 0x10000;
      out[o++] = 0xD800 | (c >> 10);
      out[o++] = 0xDC00 | (c & 0x3FF);
    } else {
      out[o++] = c;
    }
  }
  out[o] = 0;
}

static void utf16_to_utf8(const SceWChar16 *in, char *out, size_t out_len) {
  size_t o = 0;
  for (; *in && o + 4 < out_len; in++) {
    uint32_t c = *in;
    if (c >= 0xD800 && c < 0xDC00 && in[1] >= 0xDC00 && in[1] < 0xE000)
      c = 0x10000 + ((c - 0xD800) << 10) + (*++in - 0xDC00);
    if (c < 0x80) {
      out[o++] = c;
    } else if (c < 0x800) {
      out[o++] = 0xC0 | (c >> 6);
      out[o++] = 0x80 | (c & 0x3F);
    } else if (c < 0x10000) {
      out[o++] = 0xE0 | (c >> 12);
      out[o++] = 0x80 | ((c >> 6) & 0x3F);
      out[o++] = 0x80 | (c & 0x3F);
    } else {
      out[o++] = 0xF0 | (c >> 18);
      out[o++] = 0x80 | ((c >> 12) & 0x3F);
      out[o++] = 0x80 | ((c >> 6) & 0x3F);
      out[o++] = 0x80 | (c & 0x3F);
    }
  }
  out[o] = 0;
}

// Unity's TouchScreenKeyboardType -> SceImeType.
static SceUInt32 ime_type(int unity_type) {
  switch (unity_type) {
  case 1: return SCE_IME_TYPE_BASIC_LATIN;     // ASCIICapable
  case 2: return SCE_IME_TYPE_EXTENDED_NUMBER; // NumbersAndPunctuation
  case 3: return SCE_IME_TYPE_URL;
  case 4:                                      // NumberPad
  case 5: return SCE_IME_TYPE_NUMBER;          // PhonePad
  case 7: return SCE_IME_TYPE_MAIL;            // EmailAddress
  default: return SCE_IME_TYPE_DEFAULT;
  }
}

void ime_show(const char *text, int type, int multiline, int secure, const char *placeholder) {
  if (running) {
    L_WARN("ime: showSoftInput while the keyboard is already open");
    return;
  }
  snprintf(initial_utf8, sizeof(initial_utf8), "%s", text ? text : "");
  utf8_to_utf16(placeholder && *placeholder ? placeholder : "Sonic Runners", title16,
                SCE_IME_DIALOG_MAX_TITLE_LENGTH);
  utf8_to_utf16(initial_utf8, initial16, MAX_TEXT + 1);
  memset(input16, 0, sizeof(input16));

  SceImeDialogParam param;
  sceImeDialogParamInit(&param);
  param.supportedLanguages = 0;  // every language the system keyboard offers
  param.languagesForced = SCE_FALSE;
  param.type = ime_type(type);
  param.option = multiline ? SCE_IME_OPTION_MULTILINE : 0;
  param.textBoxMode = secure ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  param.title = title16;
  param.maxTextLength = multiline ? MAX_TEXT : 64;
  param.initialText = initial16;
  param.inputTextBuffer = input16;

  int r = sceImeDialogInit(&param);
  L_INFO("ime: open (type %d, multiline %d, secure %d) -> 0x%08X", type, multiline, secure, r);
  if (r >= 0) {
    running = 1;
    hide_requested = 0;
  }
}

void ime_hide(void) {
  if (running && !hide_requested) {
    hide_requested = 1;
    sceImeDialogAbort();
  }
}

void ime_set_text(const char *text) {
  if (!running)
    snprintf(initial_utf8, sizeof(initial_utf8), "%s", text ? text : "");
}

int ime_running(void) { return running; }

const char *ime_poll(void) {
  if (!running || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
    return NULL;
  SceImeDialogResult res;
  memset(&res, 0, sizeof(res));
  sceImeDialogGetResult(&res);
  sceImeDialogTerm();
  running = 0;
  if (res.button == SCE_IME_DIALOG_BUTTON_ENTER)
    utf16_to_utf8(input16, result_utf8, sizeof(result_utf8));
  else
    snprintf(result_utf8, sizeof(result_utf8), "%s", initial_utf8);  // cancelled: keep the old text
  L_INFO("ime: closed (button %d, %u bytes)", res.button, (unsigned)strlen(result_utf8));
  if (hide_requested)
    return NULL;  // Unity closed it itself; nothing to report
  return result_utf8;
}
