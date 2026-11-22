/* Android frontend for DKC1Recomp (SDL2), v4.
 *
 * Modelled on runner/ios/ios_main.m: the recompiled game/runtime is shared and
 * unchanged. This file provides the window, video, audio, touch controls,
 * game-controller input, a small in-game CONFIG panel and the SAVE button.
 *
 * The ROM is never bundled. RomPickerActivity (Java) copies the ROM the user
 * chooses to <app files dir>/game.sfc, and this file loads it from there.
 *
 * Saves go to /storage/emulated/0/DKC1Recomp/saves when the app has all-files
 * access (granted from RomPickerActivity). Otherwise they stay in the app's
 * private folder.
 *
 * Status: not compiled or run on a device yet.
 */
#define _POSIX_C_SOURCE 200809L

#include <SDL.h>
#include <SDL_main.h>   /* renames main() to SDL_main() on Android */
#include <SDL_system.h> /* SDL_AndroidGetInternalStoragePath */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ios_audio_ring.h"
#include "ios_viewport.h"
#include "dkc1_game.h"
#include "dkc1_video.h"
#include "desktop_sram.h"
#include "verified_rom.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/snes.h"

#define TAG "DKC1"
#define CREDIT_TEXT "MR.HIPEE"
#define SAVE_ROOT "/storage/emulated/0/DKC1Recomp"

static const double kGameHz = 60.098811862;
static const int kAudioRate = 32040;

/* SNES joypad bits, identical to the iOS frontend. */
enum {
  PAD_B = 0x001, PAD_Y = 0x002, PAD_SELECT = 0x004, PAD_START = 0x008,
  PAD_UP = 0x010, PAD_DOWN = 0x020, PAD_LEFT = 0x040, PAD_RIGHT = 0x080,
  PAD_A = 0x100, PAD_X = 0x200, PAD_L = 0x400, PAD_R = 0x800
};

/* UI actions use bits above the 12 joypad bits, so one mask carries both. */
#define ACT_SAVE     (1u << 16)
#define ACT_CONFIG   (1u << 17)
#define ACT_SPEED_DN (1u << 18)
#define ACT_SPEED_UP (1u << 19)
#define ACT_FPS      (1u << 20)
#define ACT_SIZE_DN  (1u << 21)
#define ACT_SIZE_UP  (1u << 22)
#define ACT_CLOSE    (1u << 23)
#define ACT_TOUCH    (1u << 24)
#define PAD_MASK     0x0FFFu

/* ------------------------------------------------------------------------ */
/* Global state                                                              */
/* ------------------------------------------------------------------------ */

static uint8_t s_pixels[kDkc1VideoMaxWidth * kDkc1VideoHeight * 4];
static int s_render_width = kDkc1VideoNativeWidth;
static uint8_t *s_rom;
static Dkc1SramStore s_sram;
static Dkc1IOSAudioRing s_audio;

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static SDL_AudioDeviceID s_audio_device;
static SDL_GameController *s_pads[2];

/* Guards the game state between the main loop and the lifecycle watcher. */
static SDL_mutex *s_game_lock;
static volatile int s_paused;
static volatile int s_loaded;
static volatile int s_failed;

/* Player settings, saved to <files>/config.txt. */
typedef struct Settings {
  int speed;    /* percent of normal game speed, 0..200 (0 = paused) */
  int show_fps; /* 1 = draw the frame counter */
  int size;     /* button size percent, 70..140 */
  int touch_buttons; /* 1 = show on-screen buttons, 0 = hide (use a gamepad) */
} Settings;
static Settings s_settings = {100, 0, 100, 1};

static char s_files_dir[1024];
static float s_mm_px = 16.0f; /* screen pixels per millimetre (physical size) */

/* ------------------------------------------------------------------------ */
/* Settings file                                                             */
/* ------------------------------------------------------------------------ */

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void LoadSettings(void) {
  char path[1100];
  snprintf(path, sizeof path, "%s/config.txt", s_files_dir);
  FILE *file = fopen(path, "r");
  if (!file) return;
  char line[128];
  while (fgets(line, sizeof line, file)) {
    int value = 0;
    if (sscanf(line, "speed=%d", &value) == 1)
      s_settings.speed = Clamp(value, 0, 200);
    else if (sscanf(line, "fps=%d", &value) == 1)
      s_settings.show_fps = value ? 1 : 0;
    else if (sscanf(line, "size=%d", &value) == 1)
      s_settings.size = Clamp(value, 70, 140);
    else if (sscanf(line, "buttons=%d", &value) == 1)
      s_settings.touch_buttons = value ? 1 : 0;
  }
  fclose(file);
}

static void SaveSettings(void) {
  char path[1100];
  snprintf(path, sizeof path, "%s/config.txt", s_files_dir);
  FILE *file = fopen(path, "w");
  if (!file) return;
  fprintf(file, "speed=%d\nfps=%d\nsize=%d\nbuttons=%d\n", s_settings.speed,
          s_settings.show_fps, s_settings.size, s_settings.touch_buttons);
  fclose(file);
}

/* ------------------------------------------------------------------------ */
/* Tiny 5x7 bitmap font (no fonts or text libraries needed)                  */
/* ------------------------------------------------------------------------ */

/* Each glyph is 7 rows; the low 5 bits of a row are the pixels, left to right. */
static const uint8_t kBlank[7] = {0, 0, 0, 0, 0, 0, 0};

static const uint8_t kLetters[26][7] = {
  {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
  {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
  {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
  {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
  {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
  {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
  {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
  {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
  {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
  {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
  {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
  {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
  {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
  {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
  {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
  {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
  {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
  {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
  {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
  {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
  {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static const uint8_t kDigits[10][7] = {
  {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
  {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
  {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
  {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
  {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
  {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
  {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
  {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
  {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
};

static const uint8_t kPercent[7] = {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13};
static const uint8_t kPlus[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
static const uint8_t kMinus[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
static const uint8_t kDot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};

static const uint8_t *GlyphFor(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
  if (c >= '0' && c <= '9') return kDigits[c - '0'];
  switch (c) {
    case '%': return kPercent;
    case '+': return kPlus;
    case '-': return kMinus;
    case '.': return kDot;
    default: return kBlank;
  }
}

/* Each glyph is 6 pixels wide including spacing (5 pixels plus 1 gap). */
static int TextWidth(const char *s, int px) {
  const int n = (int)strlen(s);
  return n <= 0 ? 0 : n * 6 * px - px;
}

static void DrawText(const char *s, int x, int y, int px, uint8_t r, uint8_t g,
                     uint8_t b, uint8_t a);

static void DrawTextCentered(const char *s, float cx, float cy, int px,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const int w = TextWidth(s, px);
  const int h = 7 * px;
  DrawText(s, (int)(cx - w * 0.5f), (int)(cy - h * 0.5f), px, r, g, b, a);
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct Circle {
  float cx, cy, r;
  uint32_t mask;
  uint8_t red, green, blue; /* colour of the letter */
  char label;
} Circle;

typedef struct Box {
  float x, y, w, h;
  uint32_t mask;
  const char *label;
} Box;

typedef struct Layout {
  float dpad_cx, dpad_cy, dpad_r;
  Circle face[4]; /* B, A, Y, X in SNES positions */
  Box select, start, save, config, left, right;
} Layout;

typedef struct MenuLayout {
  float px0, py0, pw, ph, bh;
  float row_y[4]; /* speed, fps, size, touch buttons */
  Box speed_dn, speed_up, fps, size_dn, size_up, touch, close;
} MenuLayout;

/* Size of a button in pixels, given the size setting and the minimum physical size. */
static void MakeLayout(Layout *l, float w, float h) {
  const float k = s_settings.size / 100.0f;
  const float mm = s_mm_px;
  const float m = 0.04f * h;
  const float min_r = 4.5f * mm; /* 9 mm across: a comfortable finger target */

  float dr = 0.21f * h * k;
  if (dr < 2.2f * min_r) dr = 2.2f * min_r;
  l->dpad_r = dr;
  l->dpad_cx = m + dr * 1.1f;
  l->dpad_cy = h - m - dr * 1.05f;

  float fr = 0.085f * h * k;
  if (fr < min_r) fr = min_r;
  float s = 0.125f * h * k;
  if (s < fr * 1.5f) s = fr * 1.5f;
  const float fcx = w - m - s - fr;
  const float fcy = h - m - s - fr - 0.02f * h;
  l->face[0] = (Circle){fcx, fcy + s, fr, PAD_B, 255, 210, 60, 'B'};
  l->face[1] = (Circle){fcx + s, fcy, fr, PAD_A, 255, 70, 70, 'A'};
  l->face[2] = (Circle){fcx - s, fcy, fr, PAD_Y, 80, 220, 110, 'Y'};
  l->face[3] = (Circle){fcx, fcy - s, fr, PAD_X, 90, 140, 255, 'X'};

  /* Bottom row: SELECT, START, SAVE, CONFIG, centred. */
  float bh = 0.065f * h * k;
  if (bh < 7.0f * mm) bh = 7.0f * mm;
  float pw = 0.16f * h * k;
  if (pw < bh * 2.2f) pw = bh * 2.2f;
  const float gap = 0.02f * h * k;
  const float total = 4 * pw + 3 * gap;
  const float x0 = w * 0.5f - total * 0.5f;
  const float y = h - m - bh;
  l->select = (Box){x0, y, pw, bh, PAD_SELECT, "SELECT"};
  l->start = (Box){x0 + (pw + gap), y, pw, bh, PAD_START, "START"};
  l->save = (Box){x0 + 2 * (pw + gap), y, pw, bh, ACT_SAVE, "SAVE"};
  l->config = (Box){x0 + 3 * (pw + gap), y, pw, bh, ACT_CONFIG, "CONFIG"};

  /* Shoulder buttons sit in the top corners. */
  const float sw = 0.2f * h * k;
  const float sh = bh * 0.75f;
  l->left = (Box){m, m, sw, sh, PAD_L, "L"};
  l->right = (Box){w - m - sw, m, sw, sh, PAD_R, "R"};
}

static void MakeMenu(MenuLayout *ml, float w, float h) {
  ml->pw = w * 0.6f;
  ml->ph = h * 0.82f;
  ml->px0 = (w - ml->pw) * 0.5f;
  ml->py0 = (h - ml->ph) * 0.5f;
  float bh = ml->ph * 0.13f;
  if (bh < 9.0f * s_mm_px) bh = 9.0f * s_mm_px;
  ml->bh = bh;
  const float rowh = ml->ph * 0.14f;
  const float y0 = ml->py0 + ml->ph * 0.26f;
  for (int i = 0; i < 4; ++i) ml->row_y[i] = y0 + i * rowh;

  const float cx = ml->px0 + ml->pw * 0.5f;
  const float plus_x = cx + bh + ml->pw * 0.16f;
  ml->speed_dn = (Box){cx, ml->row_y[0] - bh * 0.5f, bh, bh, ACT_SPEED_DN, "-"};
  ml->speed_up = (Box){plus_x, ml->row_y[0] - bh * 0.5f, bh, bh, ACT_SPEED_UP, "+"};
  ml->fps = (Box){cx, ml->row_y[1] - bh * 0.5f, ml->pw * 0.3f, bh, ACT_FPS, "OFF"};
  ml->size_dn = (Box){cx, ml->row_y[2] - bh * 0.5f, bh, bh, ACT_SIZE_DN, "-"};
  ml->size_up = (Box){plus_x, ml->row_y[2] - bh * 0.5f, bh, bh, ACT_SIZE_UP, "+"};
  ml->touch = (Box){cx, ml->row_y[3] - bh * 0.5f, ml->pw * 0.3f, bh, ACT_TOUCH, "ON"};
  const float close_w = ml->pw * 0.4f;
  ml->close = (Box){ml->px0 + (ml->pw - close_w) * 0.5f, ml->py0 + ml->ph - bh * 1.5f,
                    close_w, bh, ACT_CLOSE, "FECHAR"};
}

static bool InBox(const Box *box, float x, float y) {
  const float pad = 0.08f * box->h; /* generous touch margin */
  return x >= box->x - pad && x <= box->x + box->w + pad &&
         y >= box->y - pad && y <= box->y + box->h + pad;
}

static uint32_t FingerMask(const Layout *l, float x, float y) {
  uint32_t mask = 0;
  if (!s_settings.touch_buttons) {
    /* Buttons hidden (gamepad mode): only SAVE and CONFIG stay tappable. */
    if (InBox(&l->save, x, y)) mask |= l->save.mask;
    if (InBox(&l->config, x, y)) mask |= l->config.mask;
    return mask;
  }
  const float dx = x - l->dpad_cx, dy = y - l->dpad_cy;
  if (hypotf(dx, dy) < l->dpad_r * 1.6f) {
    const float nx = dx / l->dpad_r, ny = dy / l->dpad_r;
    if (nx < -0.3f) mask |= PAD_LEFT; else if (nx > 0.3f) mask |= PAD_RIGHT;
    if (ny < -0.3f) mask |= PAD_UP; else if (ny > 0.3f) mask |= PAD_DOWN;
  }
  /* A finger between two neighbouring face buttons presses both, which makes
   * run+jump (Y+B) easy and lets a thumb roll from one to the other. */
  for (int i = 0; i < 4; ++i) {
    const Circle *c = &l->face[i];
    if (hypotf(x - c->cx, y - c->cy) < c->r * 1.35f) mask |= c->mask;
  }
  if (InBox(&l->select, x, y)) mask |= l->select.mask;
  if (InBox(&l->start, x, y)) mask |= l->start.mask;
  if (InBox(&l->save, x, y)) mask |= l->save.mask;
  if (InBox(&l->config, x, y)) mask |= l->config.mask;
  if (InBox(&l->left, x, y)) mask |= l->left.mask;
  if (InBox(&l->right, x, y)) mask |= l->right.mask;
  return mask;
}

static uint32_t MenuMask(const MenuLayout *ml, float x, float y) {
  uint32_t mask = 0;
  if (InBox(&ml->speed_dn, x, y)) mask |= ml->speed_dn.mask;
  if (InBox(&ml->speed_up, x, y)) mask |= ml->speed_up.mask;
  if (InBox(&ml->fps, x, y)) mask |= ml->fps.mask;
  if (InBox(&ml->size_dn, x, y)) mask |= ml->size_dn.mask;
  if (InBox(&ml->size_up, x, y)) mask |= ml->size_up.mask;
  if (InBox(&ml->touch, x, y)) mask |= ml->touch.mask;
  if (InBox(&ml->close, x, y)) mask |= ml->close.mask;
  return mask;
}

/* Returns pad bits and action bits for every finger currently on screen. */
static uint32_t ReadTouch(const Layout *l, const MenuLayout *ml, bool menu_open,
                          int w, int h) {
  uint32_t mask = 0;
  const int devices = SDL_GetNumTouchDevices();
  for (int d = 0; d < devices; ++d) {
    const SDL_TouchID id = SDL_GetTouchDevice(d);
    const int fingers = SDL_GetNumTouchFingers(id);
    for (int i = 0; i < fingers; ++i) {
      const SDL_Finger *finger = SDL_GetTouchFinger(id, i);
      if (!finger) continue;
      const float x = finger->x * w, y = finger->y * h;
      mask |= menu_open ? MenuMask(ml, x, y) : FingerMask(l, x, y);
    }
  }
  return mask;
}

/* ------------------------------------------------------------------------ */
/* Game controllers                                                          */
/* ------------------------------------------------------------------------ */

static uint32_t ReadPad(SDL_GameController *pad) {
  if (!pad) return 0;
  const float lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
  const float ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
#define BTN(b) (SDL_GameControllerGetButton(pad, (b)) != 0)
  uint32_t mask = 0;
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -0.25f) mask |= PAD_LEFT;
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 0.25f) mask |= PAD_RIGHT;
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -0.25f) mask |= PAD_UP;
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 0.25f) mask |= PAD_DOWN;
  /* Same physical mapping as the iOS frontend: bottom=B, left=Y, right=A, top=X. */
  if (BTN(SDL_CONTROLLER_BUTTON_A)) mask |= PAD_B;
  if (BTN(SDL_CONTROLLER_BUTTON_X)) mask |= PAD_Y;
  if (BTN(SDL_CONTROLLER_BUTTON_B)) mask |= PAD_A;
  if (BTN(SDL_CONTROLLER_BUTTON_Y)) mask |= PAD_X;
  if (BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) mask |= PAD_L;
  if (BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= PAD_R;
  if (BTN(SDL_CONTROLLER_BUTTON_START)) mask |= PAD_START;
  if (BTN(SDL_CONTROLLER_BUTTON_BACK)) mask |= PAD_SELECT;
#undef BTN
  return mask;
}

static void PadAdded(int device_index) {
  for (int i = 0; i < 2; ++i) {
    if (!s_pads[i]) {
      s_pads[i] = SDL_GameControllerOpen(device_index);
      return;
    }
  }
}

static void PadRemoved(SDL_JoystickID instance) {
  for (int i = 0; i < 2; ++i) {
    if (s_pads[i] &&
        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == instance) {
      SDL_GameControllerClose(s_pads[i]);
      s_pads[i] = NULL;
    }
  }
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                     */
/* ------------------------------------------------------------------------ */

static void SDLCALL AudioCallback(void *user, Uint8 *stream, int len) {
  (void)user;
  float *out = (float *)stream;
  int frames = len / (int)(sizeof(float) * 2);
  float left[1024], right[1024];
  while (frames > 0) {
    const int chunk = frames > 1024 ? 1024 : frames;
    Dkc1IOSAudioRead(&s_audio, left, right, (size_t)chunk);
    for (int i = 0; i < chunk; ++i) {
      out[2 * i] = left[i];
      out[2 * i + 1] = right[i];
    }
    out += chunk * 2;
    frames -= chunk;
  }
}

static void StartAudio(void) {
  if (!s_audio_device) return;
  int16_t silence[1070 * 2];
  memset(silence, 0, sizeof silence);
  Dkc1IOSAudioWrite(&s_audio, silence, 1070);
  SDL_PauseAudioDevice(s_audio_device, 0);
}

static void StopAudio(void) {
  if (!s_audio_device) return;
  SDL_PauseAudioDevice(s_audio_device, 1);
  SDL_LockAudioDevice(s_audio_device);
  Dkc1IOSAudioReset(&s_audio);
  SDL_UnlockAudioDevice(s_audio_device);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle and saves                                                       */
/* ------------------------------------------------------------------------ */

static void FlushSave(bool force) {
  if (!s_loaded) return;
  char error[256];
  if (!Dkc1SramFlush(&s_sram, g_sram, (size_t)g_sram_size, force, error, sizeof error))
    SDL_Log(TAG ": save error: %s", error);
}

/* Android may suspend the process right after WILLENTERBACKGROUND, so react
 * from an event watch (synchronously) rather than from the event queue. */
static int SDLCALL LifecycleWatch(void *user, SDL_Event *event) {
  (void)user;
  switch (event->type) {
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_TERMINATING:
      SDL_LockMutex(s_game_lock);
      s_paused = 1;
      FlushSave(true);
      StopAudio();
      SDL_UnlockMutex(s_game_lock);
      break;
    case SDL_APP_DIDENTERFOREGROUND:
      SDL_LockMutex(s_game_lock);
      if (s_loaded && !s_failed) StartAudio();
      s_paused = 0;
      SDL_UnlockMutex(s_game_lock);
      break;
    default:
      break;
  }
  return 1;
}

/* ------------------------------------------------------------------------ */
/* ROM loading                                                               */
/* ------------------------------------------------------------------------ */

static void Fail(const char *message) {
  SDL_Log(TAG ": %s", message);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DKC1Recomp", message, s_window);
}

static bool LoadRom(const char *path, int output_w, int output_h) {
  char error[256] = {0};
  size_t size = 0;
  s_rom = Dkc1ReadVerifiedRom(path, &size, error, sizeof error);
  if (!s_rom) {
    Fail(error[0] ? error : "ROM invalida.");
    return false;
  }
  const Dkc1IOSViewport viewport =
      Dkc1IOSMakeViewport(output_w, output_h, output_w > output_h);
  s_render_width = viewport.source_width;
  Dkc1VideoSetRenderWidth(s_render_width);
  Dkc1VideoSetRom(s_rom, size);
  RtlRegisterGame(Dkc1GameInfo());
  if (!SnesInit(s_rom, (int)size)) {
    free(s_rom);
    s_rom = NULL;
    Fail("O runtime nao conseguiu iniciar a ROM verificada.");
    return false;
  }
  Dkc1BeginDrawing(s_pixels, (size_t)s_render_width * 4);
  RtlSetAudioOutputRate(kAudioRate);
  if (!Dkc1SramLoad(&s_sram, g_sram, (size_t)g_sram_size, error, sizeof error)) {
    Fail(error);
    return false;
  }
  return true;
}

/* ------------------------------------------------------------------------ */
/* Drawing primitives                                                        */
/* ------------------------------------------------------------------------ */

static void DrawText(const char *s, int x, int y, int px, uint8_t r, uint8_t g,
                     uint8_t b, uint8_t a) {
  SDL_SetRenderDrawColor(s_renderer, r, g, b, a);
  for (int i = 0; s[i]; ++i) {
    const uint8_t *glyph = GlyphFor(s[i]);
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 5; ++col) {
        if (glyph[row] & (0x10 >> col)) {
          SDL_Rect pixel = {x + (i * 6 + col) * px, y + row * px, px, px};
          SDL_RenderFillRect(s_renderer, &pixel);
        }
      }
    }
  }
}

/* Filled rounded rectangle, drawn as horizontal scanlines (no textures). */
static void FillRoundRect(float x, float y, float w, float h, float rad,
                          uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
  SDL_SetRenderDrawColor(s_renderer, red, green, blue, alpha);
  const float limit = (w < h ? w : h) * 0.5f;
  if (rad > limit) rad = limit;
  if (rad < 1.0f) rad = 1.0f;
  for (int row = 0; row < (int)h; ++row) {
    const float dy = row + 0.5f;
    float inset = 0.0f;
    if (dy < rad) {
      const float k = rad - dy;
      inset = rad - sqrtf(rad * rad - k * k);
    } else if (dy > h - rad) {
      const float k = dy - (h - rad);
      inset = rad - sqrtf(rad * rad - k * k);
    }
    const int x0 = (int)(x + inset), x1 = (int)(x + w - inset);
    SDL_RenderDrawLine(s_renderer, x0, (int)y + row, x1 - 1, (int)y + row);
  }
}

/* Filled circle, same scanline method. */
static void FillDisc(float cx, float cy, float r,
                     uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
  SDL_SetRenderDrawColor(s_renderer, red, green, blue, alpha);
  const int ir = (int)r;
  for (int dy = -ir; dy <= ir; ++dy) {
    const float half = sqrtf(r * r - (float)(dy * dy));
    SDL_RenderDrawLine(s_renderer, (int)(cx - half), (int)(cy + dy),
                       (int)(cx + half), (int)(cy + dy));
  }
}

static void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  SDL_Vertex v[3];
  const SDL_Color color = {r, g, b, a};
  const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
  for (int i = 0; i < 3; ++i) {
    v[i].position.x = xs[i];
    v[i].position.y = ys[i];
    v[i].color = color;
    v[i].tex_coord.x = 0.0f;
    v[i].tex_coord.y = 0.0f;
  }
  SDL_RenderGeometry(s_renderer, NULL, v, 3, NULL, 0);
}

/* Wooden palette, shared by every button. */
#define WOOD_RIM 70, 38, 18
#define WOOD_BODY 150, 88, 44
#define WOOD_BODY_PRESSED 185, 110, 58
#define WOOD_SHINE 240, 170, 105
#define CREAM 255, 236, 200

/* Round face button: dark rim, wooden body, coloured letter. */
static void DrawFace(const Circle *c, bool pressed) {
  const float r = pressed ? c->r * 1.08f : c->r;
  FillDisc(c->cx + r * 0.05f, c->cy + r * 0.10f, r, 0, 0, 0, 90);
  FillDisc(c->cx, c->cy, r, WOOD_RIM, 235);
  if (pressed)
    FillDisc(c->cx, c->cy, r * 0.86f, WOOD_BODY_PRESSED, 255);
  else
    FillDisc(c->cx, c->cy, r * 0.86f, WOOD_BODY, 255);
  FillDisc(c->cx - r * 0.2f, c->cy - r * 0.3f, r * 0.4f, 255, 255, 255,
           pressed ? 70 : 35);
  char label[2] = {c->label, 0};
  int px = (int)(r * 1.1f / 7.0f);
  if (px < 2) px = 2;
  DrawTextCentered(label, c->cx, c->cy, px, c->red, c->green, c->blue, 255);
}

/* Rounded pill button: SELECT, START, SAVE, CONFIG, L, R, menu buttons. */
static void DrawPill(const Box *b, bool pressed) {
  const float rad = b->h * 0.5f;
  FillRoundRect(b->x + 2, b->y + 3, b->w, b->h, rad, 0, 0, 0, 90);
  FillRoundRect(b->x, b->y, b->w, b->h, rad, WOOD_RIM, 235);
  const uint8_t body_r = pressed ? 185 : 150, body_g = pressed ? 110 : 88,
                body_b = pressed ? 58 : 44;
  FillRoundRect(b->x + 3, b->y + 3, b->w - 6, b->h - 6, rad * 0.85f,
                body_r, body_g, body_b, 255);
  FillRoundRect(b->x + b->w * 0.1f, b->y + 5, b->w * 0.8f, b->h * 0.28f,
                b->h * 0.14f, WOOD_SHINE, pressed ? 150 : 80);
  if (b->label && b->label[0]) {
    const int n = (int)strlen(b->label);
    int px = (int)(b->h * 0.4f / 7.0f);
    const int px_fit = (int)(b->w * 0.8f / (6.0f * n));
    if (px_fit < px) px = px_fit;
    if (px < 1) px = 1;
    DrawTextCentered(b->label, b->x + b->w * 0.5f, b->y + b->h * 0.5f, px,
                     CREAM, 255);
  }
}

/* Solid triangle arrow pointing in direction (ux, uy) from the D-pad centre. */
static void DrawArrow(float cx, float cy, float ux, float uy, float r, float t) {
  const float px = -uy, py = ux;
  const float tipx = cx + ux * r * 0.72f, tipy = cy + uy * r * 0.72f;
  const float bx = cx + ux * r * 0.42f, by = cy + uy * r * 0.42f;
  const float hb = t * 0.45f;
  FillTriangle(tipx, tipy, bx + px * hb, by + py * hb, bx - px * hb, by - py * hb,
               CREAM, 240);
}

static void DrawDpad(const Layout *l, uint32_t pressed) {
  const float r = l->dpad_r, t = r * 0.34f, rad = t * 0.6f;
  const float cx = l->dpad_cx, cy = l->dpad_cy;
  /* Rim, then body. */
  FillRoundRect(cx - r - 2, cy - t - 2, 2 * r + 4, 2 * t + 4, rad, WOOD_RIM, 235);
  FillRoundRect(cx - t - 2, cy - r - 2, 2 * t + 4, 2 * r + 4, rad, WOOD_RIM, 235);
  FillRoundRect(cx - r, cy - t, 2 * r, 2 * t, rad, WOOD_BODY, 255);
  FillRoundRect(cx - t, cy - r, 2 * t, 2 * r, rad, WOOD_BODY, 255);
  /* Pressed arms light up. */
  if (pressed & PAD_LEFT)
    FillRoundRect(cx - r, cy - t, r - t, 2 * t, rad, WOOD_BODY_PRESSED, 255);
  if (pressed & PAD_RIGHT)
    FillRoundRect(cx + t, cy - t, r - t, 2 * t, rad, WOOD_BODY_PRESSED, 255);
  if (pressed & PAD_UP)
    FillRoundRect(cx - t, cy - r, 2 * t, r - t, rad, WOOD_BODY_PRESSED, 255);
  if (pressed & PAD_DOWN)
    FillRoundRect(cx - t, cy + t, 2 * t, r - t, rad, WOOD_BODY_PRESSED, 255);
  /* Arrows. */
  DrawArrow(cx, cy, 0, -1, r, t);
  DrawArrow(cx, cy, 0, 1, r, t);
  DrawArrow(cx, cy, -1, 0, r, t);
  DrawArrow(cx, cy, 1, 0, r, t);
}

static void DrawOverlay(const Layout *l, uint32_t pressed) {
  if (s_settings.touch_buttons) {
    DrawDpad(l, pressed);
    for (int i = 0; i < 4; ++i) DrawFace(&l->face[i], (pressed & l->face[i].mask) != 0);
    DrawPill(&l->select, (pressed & l->select.mask) != 0);
    DrawPill(&l->start, (pressed & l->start.mask) != 0);
    DrawPill(&l->left, (pressed & l->left.mask) != 0);
    DrawPill(&l->right, (pressed & l->right.mask) != 0);
  }
  DrawPill(&l->save, (pressed & l->save.mask) != 0);
  DrawPill(&l->config, (pressed & l->config.mask) != 0);
}

static void DrawMenu(const MenuLayout *ml) {
  FillRoundRect(ml->px0 - 4, ml->py0 - 4, ml->pw + 8, ml->ph + 8, ml->ph * 0.06f,
                WOOD_RIM, 255);
  FillRoundRect(ml->px0, ml->py0, ml->pw, ml->ph, ml->ph * 0.05f, 28, 16, 8, 240);

  int px_title = (int)(ml->ph * 0.06f / 7.0f);
  if (px_title < 2) px_title = 2;
  DrawTextCentered("CONFIG", ml->px0 + ml->pw * 0.5f, ml->py0 + ml->ph * 0.12f,
                   px_title, CREAM, 255);

  int px_row = (int)(ml->bh * 0.35f / 7.0f);
  if (px_row < 2) px_row = 2;
  const float label_x = ml->px0 + ml->pw * 0.06f;
  DrawText("VELOCIDADE", (int)label_x, (int)(ml->row_y[0] - 3.5f * px_row), px_row,
           CREAM, 255);
  DrawText("FPS", (int)label_x, (int)(ml->row_y[1] - 3.5f * px_row), px_row,
           CREAM, 255);
  DrawText("TAMANHO", (int)label_x, (int)(ml->row_y[2] - 3.5f * px_row), px_row,
           CREAM, 255);

  char value[16];
  const float value_x = ml->speed_dn.x + ml->bh + ml->pw * 0.08f;
  snprintf(value, sizeof value, "%d%%", s_settings.speed);
  DrawTextCentered(value, value_x, ml->row_y[0], px_row, CREAM, 255);
  snprintf(value, sizeof value, "%d%%", s_settings.size);
  DrawTextCentered(value, value_x, ml->row_y[2], px_row, CREAM, 255);

  DrawPill(&ml->speed_dn, false);
  DrawPill(&ml->speed_up, false);
  Box fps_box = ml->fps;
  fps_box.label = s_settings.show_fps ? "ON" : "OFF";
  DrawPill(&fps_box, false);
  DrawPill(&ml->size_dn, false);
  DrawPill(&ml->size_up, false);
  DrawText("BOTOES", (int)label_x, (int)(ml->row_y[3] - 3.5f * px_row), px_row,
           CREAM, 255);
  Box touch_box = ml->touch;
  touch_box.label = s_settings.touch_buttons ? "ON" : "OFF";
  DrawPill(&touch_box, false);
  DrawPill(&ml->close, false);
}

static SDL_Rect GameRect(int out_w, int out_h) {
  const double pixel_aspect = 7.0 / 6.0;
  double dh = out_h;
  double dw = dh * s_render_width * pixel_aspect / kDkc1VideoHeight;
  if (dw > out_w) {
    dw = out_w;
    dh = dw * kDkc1VideoHeight / (s_render_width * pixel_aspect);
  }
  SDL_Rect rect = {(int)((out_w - dw) / 2), (int)((out_h - dh) / 2), (int)dw, (int)dh};
  return rect;
}

/* Credit in the black bar beside the picture; top centre if there is no room. */
static void DrawCredit(const SDL_Rect *dest, int w, int h) {
  int px = (int)(s_mm_px * 1.6f / 7.0f);
  if (px < 2) px = 2;
  const int tw = TextWidth(CREDIT_TEXT, px);
  const int left = dest->x;
  const int right = w - (dest->x + dest->w);
  if (left >= right && left >= tw + 12) {
    DrawText(CREDIT_TEXT, (left - tw) / 2, h / 2 - 3 * px, px, CREAM, 220);
  } else if (right >= tw + 12) {
    DrawText(CREDIT_TEXT, dest->x + dest->w + (right - tw) / 2, h / 2 - 3 * px, px,
             CREAM, 220);
  } else {
    DrawText(CREDIT_TEXT, (w - tw) / 2, 4, px, CREAM, 220);
  }
}

/* ------------------------------------------------------------------------ */
/* Main                                                                      */
/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); /* nearest: sharp pixels */
  /* OpenGL ES 2 works on both Mali and Adreno (Snapdragon) GPUs. */
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    SDL_Log(TAG ": SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  const char *files = SDL_AndroidGetInternalStoragePath();
  if (!files) {
    SDL_Log(TAG ": no internal storage path");
    return 1;
  }
  snprintf(s_files_dir, sizeof s_files_dir, "%s", files);
  char rom_path[1024], tier2[1024];
  snprintf(rom_path, sizeof rom_path, "%s/game.sfc", files);
  snprintf(tier2, sizeof tier2, "%s/tier2", files);
  mkdir(tier2, 0755);
  setenv("SNESRECOMP_TIER2_DIR", tier2, 1); /* same role as on iOS */

  /* Saves go to a visible folder when all-files access is granted. */
  const char *save_root = files;
  mkdir(SAVE_ROOT, 0755);
  if (access(SAVE_ROOT, W_OK) == 0) save_root = SAVE_ROOT;
  if (chdir(save_root) != 0) SDL_Log(TAG ": chdir(%s) failed", save_root);
  LoadSettings();

  s_game_lock = SDL_CreateMutex();
  s_window = SDL_CreateWindow("DKC1Recomp", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 0, 0,
                              SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
  if (!s_window) {
    SDL_Log(TAG ": window failed: %s", SDL_GetError());
    return 1;
  }
  s_renderer = SDL_CreateRenderer(s_window, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!s_renderer) s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
  if (!s_renderer) {
    SDL_Log(TAG ": renderer failed: %s", SDL_GetError());
    return 1;
  }
  int out_w = 0, out_h = 0;
  SDL_GetRendererOutputSize(s_renderer, &out_w, &out_h);

  /* Physical size: buttons never shrink below about 9 mm, whatever the screen. */
  float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f;
  if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) != 0 || ddpi <= 0.0f) ddpi = 320.0f;
  s_mm_px = ddpi / 25.4f;

  if (!LoadRom(rom_path, out_w, out_h)) {
    /* Forget a bad ROM so the picker asks again on the next launch. */
    remove(rom_path);
    return 1;
  }
  s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, s_render_width,
                                kDkc1VideoHeight);
  if (!s_texture) {
    Fail("Nao foi possivel criar a textura de video.");
    return 1;
  }

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = kAudioRate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = AudioCallback;
  s_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!s_audio_device) SDL_Log(TAG ": audio unavailable: %s", SDL_GetError());

  for (int i = 0; i < SDL_NumJoysticks(); ++i)
    if (SDL_IsGameController(i)) PadAdded(i);

  s_loaded = 1;
  SDL_AddEventWatch(LifecycleWatch, NULL);
  StartAudio();
  SDL_Log(TAG ": started, source %dx%d, output %dx%d", s_render_width,
          kDkc1VideoHeight, out_w, out_h);

  const Uint64 frequency = SDL_GetPerformanceFrequency();
  Uint64 previous = SDL_GetPerformanceCounter();
  double elapsed = 0, audio_fraction = 0;
  bool running = true;
  bool menu_open = false;
  uint32_t prev_touch = 0;
  Uint32 flash_until = 0;
  Uint32 fps_window_start = SDL_GetTicks();
  int presented_in_window = 0, fps_shown = 0;

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_QUIT: running = false; break;
        case SDL_CONTROLLERDEVICEADDED: PadAdded(event.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: PadRemoved(event.cdevice.which); break;
        case SDL_APP_DIDENTERFOREGROUND:
          previous = SDL_GetPerformanceCounter();
          elapsed = 0;
          break;
        default: break;
      }
    }
    if (!running) break;

    SDL_GetRendererOutputSize(s_renderer, &out_w, &out_h);
    Layout layout;
    MakeLayout(&layout, (float)out_w, (float)out_h);
    MenuLayout menu;
    MakeMenu(&menu, (float)out_w, (float)out_h);

    /* Taps: act on the press (an edge), not on every frame the finger stays down. */
    const uint32_t touch = ReadTouch(&layout, &menu, menu_open, out_w, out_h);
    const uint32_t edges = touch & ~prev_touch;
    prev_touch = touch;

    if (menu_open) {
      if (edges & ACT_SPEED_DN) {
        s_settings.speed = Clamp(s_settings.speed - 25, 0, 200);
        SaveSettings();
      }
      if (edges & ACT_SPEED_UP) {
        s_settings.speed = Clamp(s_settings.speed + 25, 0, 200);
        SaveSettings();
      }
      if (edges & ACT_FPS) {
        s_settings.show_fps = !s_settings.show_fps;
        SaveSettings();
      }
      if (edges & ACT_SIZE_DN) {
        s_settings.size = Clamp(s_settings.size - 10, 70, 140);
        SaveSettings();
      }
      if (edges & ACT_SIZE_UP) {
        s_settings.size = Clamp(s_settings.size + 10, 70, 140);
        SaveSettings();
      }
      if (edges & ACT_TOUCH) {
        s_settings.touch_buttons = !s_settings.touch_buttons;
        SaveSettings();
      }
      if (edges & ACT_CLOSE) menu_open = false;
    } else {
      if (edges & ACT_CONFIG) menu_open = true;
      if (edges & ACT_SAVE) {
        SDL_LockMutex(s_game_lock);
        FlushSave(true);
        SDL_UnlockMutex(s_game_lock);
        flash_until = SDL_GetTicks() + 1500;
      }
    }

    /* Joypad input: touch and gamepads, but nothing while the panel is open. */
    uint32_t input = 0;
    if (!menu_open) {
      input = touch & PAD_MASK;
      for (int i = 0; i < 2; ++i) input |= ReadPad(s_pads[i]) << (i * 12);
    }

    if (s_paused || s_failed) {
      SDL_Delay(30);
      previous = SDL_GetPerformanceCounter();
      continue;
    }

    const Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - previous) / (double)frequency;
    previous = now;
    if (dt > 4.0 / kGameHz) dt = 4.0 / kGameHz;
    /* Speed setting scales how fast game time passes; the panel pauses it. */
    if (!menu_open)
      elapsed += dt * (s_settings.speed / 100.0);
    else
      elapsed = 0;

    bool drew = false;
    SDL_LockMutex(s_game_lock);
    int guard = 0;
    while (elapsed >= 1.0 / kGameHz && !s_paused && guard++ < 8) {
      elapsed -= 1.0 / kGameHz;
      RtlRunFrame(input);
      if (g_fail || !Dkc1LastLleResult()) {
        s_failed = 1;
        char message[160];
        snprintf(message, sizeof message,
                 "O runtime parou no frame %d ($%06x).", snes_frame_counter,
                 Dkc1ResumePc());
        SDL_UnlockMutex(s_game_lock);
        Fail(message);
        SDL_LockMutex(s_game_lock);
        break;
      }
      Dkc1DrawPpuFrame();
      audio_fraction += kAudioRate / kGameHz;
      const int frames = (int)audio_fraction;
      audio_fraction -= frames;
      int16_t samples[534 * 2];
      RtlRenderAudio(samples, frames, 2);
      Dkc1IOSAudioWrite(&s_audio, samples, (size_t)frames);
      FlushSave(false);
      drew = true;
    }
    SDL_UnlockMutex(s_game_lock);

    if (drew) SDL_UpdateTexture(s_texture, NULL, s_pixels, s_render_width * 4);

    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    const SDL_Rect dest = GameRect(out_w, out_h);
    SDL_RenderCopy(s_renderer, s_texture, NULL, &dest);

    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
    DrawCredit(&dest, out_w, out_h);
    DrawOverlay(&layout, touch);
    if (menu_open) DrawMenu(&menu);

    if (SDL_GetTicks() < flash_until) {
      int px = (int)(s_mm_px * 2.0f / 7.0f);
      if (px < 3) px = 3;
      DrawTextCentered("SALVO", out_w * 0.5f, out_h * 0.14f, px, 255, 210, 60, 255);
    }
    if (s_settings.show_fps) {
      char fps_text[16];
      snprintf(fps_text, sizeof fps_text, "FPS %d", fps_shown);
      DrawText(fps_text, 8, 4, 2, 255, 255, 255, 230);
    }
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
    SDL_RenderPresent(s_renderer);

    /* Count presented frames once a second for the FPS readout. */
    ++presented_in_window;
    const Uint32 ticks = SDL_GetTicks();
    if (ticks - fps_window_start >= 1000) {
      fps_shown = presented_in_window;
      presented_in_window = 0;
      fps_window_start = ticks;
    }
  }

  SDL_LockMutex(s_game_lock);
  FlushSave(true);
  SDL_UnlockMutex(s_game_lock);
  SDL_DelEventWatch(LifecycleWatch, NULL);
  StopAudio();
  if (s_audio_device) SDL_CloseAudioDevice(s_audio_device);
  for (int i = 0; i < 2; ++i)
    if (s_pads[i]) SDL_GameControllerClose(s_pads[i]);
  SDL_DestroyTexture(s_texture);
  SDL_DestroyRenderer(s_renderer);
  SDL_DestroyWindow(s_window);
  SDL_Quit();
  return 0;
}
