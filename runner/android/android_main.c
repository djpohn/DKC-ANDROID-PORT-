/* Android frontend for DKC1Recomp (SDL2), v6.
 *
 * Same role as runner/ios/ios_main.m: the recompiled game is unchanged. This file
 * provides the window, video, audio, touch controls, game controllers and the
 * CONFIG panel (five tabs, 10 save slots).
 *
 * The ROM is never bundled. RomPickerActivity copies the ROM the user picks to
 * <app files dir>/game.sfc. Saves go to /storage/emulated/0/DKC1Recomp/saves when
 * all-files access is granted, otherwise to the app's private folder.
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
#include <time.h>
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
#define VERSION_TEXT "VERSAO V1.0"
#define SAVE_ROOT "/storage/emulated/0/DKC1Recomp"
#define UPDATE_URL "https://github.com/djpohn/DKC-ANDROID-PORT-/releases"
#define SLOT_COUNT 10
#define SLOTS_PER_PAGE 5

static const double kGameHz = 60.098811862;
static const int kAudioRate = 32040;

/* SNES joypad bits, identical to the iOS frontend. */
enum {
  PAD_B = 0x001, PAD_Y = 0x002, PAD_SELECT = 0x004, PAD_START = 0x008,
  PAD_UP = 0x010, PAD_DOWN = 0x020, PAD_LEFT = 0x040, PAD_RIGHT = 0x080,
  PAD_A = 0x100, PAD_X = 0x200, PAD_L = 0x400, PAD_R = 0x800
};
#define PAD_MASK 0x0FFFu

/* UI actions: one bit each in a 64-bit mask, separate from the joypad bits. */
enum {
  A_TAB_GFX, A_TAB_PERF, A_TAB_PLAY, A_TAB_SAVE, A_TAB_SYS, /* 0..4 */
  A_CLOSE, A_CONFIG,                                         /* 5, 6 */
  A_FILTER, A_ASPECT, A_BRIGHT_DN, A_BRIGHT_UP,              /* 7..10 */
  A_FPS_DN, A_FPS_UP, A_VSYNC, A_SHOWFPS,                    /* 11..14 */
  A_SPEED_DN, A_SPEED_UP, A_BUTTONS, A_SIZE_DN, A_SIZE_UP,   /* 15..19 */
  A_PAGE_PREV, A_PAGE_NEXT,                                  /* 20, 21 */
  A_RESET, A_UPDATES,                                        /* 22, 23 */
  A_SLOT0 /* 24..53: slot k uses A_SLOT0 + 3k + op (0 save, 1 load, 2 delete) */
};
#define ACT(a) (1ull << (a))

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
  int filter;        /* 0 = original (sharp pixels), 1 = smooth */
  int aspect;        /* 0 = original proportion, 1 = adapted (fill screen) */
  int brightness;    /* 0..100 */
  int fps_limit;     /* 10..60 frames per second */
  int vsync;         /* 1 = V-Sync (applies on next launch) */
  int show_fps;      /* 1 = draw frame counter */
  int speed;         /* 0..200 percent of normal game speed */
  int size;          /* button size 70..140 percent */
  int touch_buttons; /* 1 = on-screen buttons, 0 = hidden (gamepad) */
} Settings;
static const Settings kDefaults = {0, 0, 100, 60, 1, 0, 100, 100, 1};
static Settings s_settings = {0, 0, 100, 60, 1, 0, 100, 100, 1};

static char s_files_dir[1024];
static char s_save_root[1024];
static char s_slot_dir[1100];
static float s_mm_px = 16.0f; /* screen pixels per millimetre */

/* Menu state. */
static int s_menu_open;
static int s_tab;
static int s_page;
static int s_confirm = -1;      /* action waiting for a second tap */
static char s_notice[64];
static Uint32 s_notice_until;

static void Notify(const char *message) {
  snprintf(s_notice, sizeof s_notice, "%s", message);
  s_notice_until = SDL_GetTicks() + 3000;
}

/* ------------------------------------------------------------------------ */
/* Settings file                                                             */
/* ------------------------------------------------------------------------ */

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int Step(int v, int d, int lo, int hi) { return Clamp(v + d, lo, hi); }

static bool KeyValue(const char *line, const char *key, int *out) {
  const size_t n = strlen(key);
  if (strncmp(line, key, n) != 0 || line[n] != '=') return false;
  *out = atoi(line + n + 1);
  return true;
}

static void LoadSettings(void) {
  char path[1100];
  snprintf(path, sizeof path, "%s/config.txt", s_files_dir);
  FILE *file = fopen(path, "r");
  if (!file) return;
  char line[128];
  while (fgets(line, sizeof line, file)) {
    int v = 0;
    if (KeyValue(line, "filter", &v)) s_settings.filter = Clamp(v, 0, 1);
    else if (KeyValue(line, "aspect", &v)) s_settings.aspect = Clamp(v, 0, 1);
    else if (KeyValue(line, "brightness", &v)) s_settings.brightness = Clamp(v, 0, 100);
    else if (KeyValue(line, "fps_limit", &v)) s_settings.fps_limit = Clamp(v, 10, 60);
    else if (KeyValue(line, "vsync", &v)) s_settings.vsync = Clamp(v, 0, 1);
    else if (KeyValue(line, "show_fps", &v)) s_settings.show_fps = Clamp(v, 0, 1);
    else if (KeyValue(line, "speed", &v)) s_settings.speed = Clamp(v, 0, 200);
    else if (KeyValue(line, "size", &v)) s_settings.size = Clamp(v, 70, 140);
    else if (KeyValue(line, "buttons", &v)) s_settings.touch_buttons = Clamp(v, 0, 1);
  }
  fclose(file);
}

static void SaveSettings(void) {
  char path[1100];
  snprintf(path, sizeof path, "%s/config.txt", s_files_dir);
  FILE *file = fopen(path, "w");
  if (!file) return;
  fprintf(file,
          "filter=%d\naspect=%d\nbrightness=%d\nfps_limit=%d\nvsync=%d\n"
          "show_fps=%d\nspeed=%d\nsize=%d\nbuttons=%d\n",
          s_settings.filter, s_settings.aspect, s_settings.brightness,
          s_settings.fps_limit, s_settings.vsync, s_settings.show_fps,
          s_settings.speed, s_settings.size, s_settings.touch_buttons);
  fclose(file);
}

/* ------------------------------------------------------------------------ */
/* Tiny 5x7 bitmap font (uppercase, digits and a few signs; no accents)      */
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
static const uint8_t kColon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
static const uint8_t kSlash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};

static const uint8_t *GlyphFor(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
  if (c >= '0' && c <= '9') return kDigits[c - '0'];
  switch (c) {
    case '%': return kPercent;
    case '+': return kPlus;
    case '-': return kMinus;
    case '.': return kDot;
    case ':': return kColon;
    case '/': return kSlash;
    default: return kBlank;
  }
}

/* Each glyph is 6 pixels wide including spacing (5 pixels plus 1 gap). */
static int TextWidth(const char *s, int px) {
  const int n = (int)strlen(s);
  return n <= 0 ? 0 : n * 6 * px - px;
}

/* Largest pixel size, up to maxpx, whose text fits in maxw. */
static int FitPx(const char *s, float maxw, int maxpx) {
  int px = maxpx;
  while (px > 1 && TextWidth(s, px) > maxw) --px;
  return px < 1 ? 1 : px;
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
/* Geometry and touch                                                        */
/* ------------------------------------------------------------------------ */

typedef struct Circle {
  float cx, cy, r;
  uint32_t mask; /* joypad bits pressed by this circle */
  uint64_t act;  /* UI action of this circle */
  char label;
} Circle;

typedef struct Box {
  float x, y, w, h;
  uint32_t mask;
  uint64_t act;
  const char *label;
} Box;

typedef struct Layout {
  float dpad_cx, dpad_cy, dpad_r;
  Circle face[4]; /* top=Y, left=X, right=B, bottom=A (as in the mockup) */
  Circle gear;    /* opens CONFIG */
  Box select, start, left, right;
} Layout;

typedef struct Touch {
  uint32_t pad;   /* joypad bits held */
  uint64_t acts;  /* UI actions touched */
} Touch;

/* A button in the CONFIG panel. Rebuilt every frame while the panel is open. */
typedef struct MButton {
  Box box;
  char text[24];
  bool enabled;
  bool active;
} MButton;

typedef struct MText {
  char text[48];
  float x, y; /* x = left edge, or centre when center is true; y = centre */
  int px;
  bool center;
  bool gold;
} MText;

static struct {
  float px0, py0, pw, ph, bh;
  MButton btn[48];
  int nb;
  MText txt[48];
  int nt;
} s_view;

static void MakeLayout(Layout *l, float w, float h) {
  const float k = s_settings.size / 100.0f;
  const float mm = s_mm_px;
  const float m = 0.03f * h;
  const float min_r = 4.5f * mm; /* 9 mm across: a comfortable finger target */
  const float gap = 0.01f * w;

  float dr = 0.20f * h * k;
  if (dr < 2.2f * min_r) dr = 2.2f * min_r;
  l->dpad_r = dr;
  l->dpad_cx = m + dr * 1.1f;
  l->dpad_cy = h - m - dr * 1.05f;

  float fr = 0.078f * h * k;
  if (fr < min_r) fr = min_r;
  float s = 0.125f * h * k;
  if (s < fr * 1.5f) s = fr * 1.5f;
  const float fcx = w - m - s - fr;
  const float fcy = h - m - s - fr;
  /* Letters are what the buttons show. Game actions (from docs/IOS.md):
   * B = jump, Y = roll/run, A = swap Kong. X on the right is unused for now. */
  l->face[0] = (Circle){fcx, fcy - s, fr, PAD_A, 0, 'Y'};  /* top: swap */
  l->face[1] = (Circle){fcx - s, fcy, fr, PAD_Y, 0, 'X'};  /* left: roll */
  l->face[2] = (Circle){fcx + s, fcy, fr, PAD_X, 0, 'B'};  /* right: free */
  l->face[3] = (Circle){fcx, fcy + s, fr, PAD_B, 0, 'A'};  /* bottom: jump */

  float gr = 0.06f * h * k;
  if (gr < min_r) gr = min_r;
  l->gear = (Circle){w - m - gr, m + gr, gr, 0, ACT(A_CONFIG), 0};

  /* Start and Select: small pills at the top centre. Shoulders: top corners. */
  float bh = 6.0f * mm;
  if (bh < 0.07f * h * k) bh = 0.07f * h * k;
  const float pill_w = fmaxf(bh * 2.2f, 0.13f * h);
  l->select = (Box){w * 0.5f - gap - pill_w, m, pill_w, bh, PAD_SELECT, 0, "SELECT"};
  l->start = (Box){w * 0.5f + gap, m, pill_w, bh, PAD_START, 0, "START"};
  const float sw = fmaxf(bh * 1.8f, 0.16f * h);
  l->left = (Box){m, m, sw, bh, PAD_L, 0, "L"};
  l->right = (Box){l->gear.cx - gr - gap - sw, m, sw, bh, PAD_R, 0, "R"};
}

static bool InBox(const Box *box, float x, float y) {
  const float pad = 0.08f * box->h; /* generous touch margin */
  return x >= box->x - pad && x <= box->x + box->w + pad &&
         y >= box->y - pad && y <= box->y + box->h + pad;
}

static void HitBox(const Box *b, float x, float y, Touch *t) {
  if (!InBox(b, x, y)) return;
  t->pad |= b->mask;
  t->acts |= b->act;
}

static void HitCircle(const Circle *c, float scale, float x, float y, Touch *t) {
  if (hypotf(x - c->cx, y - c->cy) > c->r * scale) return;
  t->pad |= c->mask;
  t->acts |= c->act;
}

/* Gameplay screen: the gear always works; the pad buttons only when enabled. */
static void HitGame(const Layout *l, float x, float y, Touch *t) {
  HitCircle(&l->gear, 1.2f, x, y, t);
  if (!s_settings.touch_buttons) return;
  const float dx = x - l->dpad_cx, dy = y - l->dpad_cy;
  if (hypotf(dx, dy) < l->dpad_r * 1.6f) {
    const float nx = dx / l->dpad_r, ny = dy / l->dpad_r;
    if (nx < -0.3f) t->pad |= PAD_LEFT; else if (nx > 0.3f) t->pad |= PAD_RIGHT;
    if (ny < -0.3f) t->pad |= PAD_UP; else if (ny > 0.3f) t->pad |= PAD_DOWN;
  }
  /* A finger between two face buttons presses both, so a thumb can roll. */
  for (int i = 0; i < 4; ++i) HitCircle(&l->face[i], 1.35f, x, y, t);
  HitBox(&l->select, x, y, t);
  HitBox(&l->start, x, y, t);
  HitBox(&l->left, x, y, t);
  HitBox(&l->right, x, y, t);
}

static void HitMenu(float x, float y, Touch *t) {
  for (int i = 0; i < s_view.nb; ++i) {
    const MButton *b = &s_view.btn[i];
    if (b->enabled && InBox(&b->box, x, y)) t->acts |= b->box.act;
  }
}

/* Pad bits and action bits for every finger currently on screen. */
static Touch ReadTouch(const Layout *l, int w, int h) {
  Touch t = {0, 0};
  const int devices = SDL_GetNumTouchDevices();
  for (int d = 0; d < devices; ++d) {
    const SDL_TouchID id = SDL_GetTouchDevice(d);
    const int fingers = SDL_GetNumTouchFingers(id);
    for (int i = 0; i < fingers; ++i) {
      const SDL_Finger *finger = SDL_GetTouchFinger(id, i);
      if (!finger) continue;
      const float x = finger->x * w, y = finger->y * h;
      if (s_menu_open) HitMenu(x, y, &t);
      else HitGame(l, x, y, &t);
    }
  }
  return t;
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
  /* Unchanged from v5: physical bottom=B, left=Y, right=A, top=X (as iOS). */
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
 * from an event watch (synchronously) rather than from the event queue. The game
 * always pauses when the app is left: nothing else would be reliable. */
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

/* Slot k (0..9) is a full save state (RtlSaveSnapshot), kept in saves/slots. */
static void SlotPath(int k, char *out, size_t n) {
  snprintf(out, n, "%s/slot%02d.state", s_slot_dir, k + 1);
}

static bool SlotInfo(int k, time_t *mtime) {
  char path[1200];
  SlotPath(k, path, sizeof path);
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return false;
  *mtime = st.st_mtime;
  return true;
}

static void SlotSave(int k) {
  if (!s_loaded) return;
  char path[1200], tmp[1210];
  SlotPath(k, path, sizeof path);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  SDL_LockMutex(s_game_lock);
  bool ok = RtlSaveSnapshot(tmp) != 0;
  if (ok) ok = rename(tmp, path) == 0;
  if (!ok) remove(tmp);
  SDL_UnlockMutex(s_game_lock);
  char message[64];
  snprintf(message, sizeof message, ok ? "SLOT %d SALVO" : "ERRO AO SALVAR O SLOT", k + 1);
  Notify(message);
}

/* Restores the saved state right away, without restarting the app. */
static void SlotLoad(int k) {
  if (!s_loaded) return;
  char path[1200];
  SlotPath(k, path, sizeof path);
  struct stat st;
  if (stat(path, &st) != 0) {
    Notify("ESSE SLOT ESTA VAZIO");
    return;
  }
  SDL_LockMutex(s_game_lock);
  const bool ok = RtlLoadSnapshot(path) != 0;
  if (ok) {
    Dkc1BeginDrawing(s_pixels, (size_t)s_render_width * 4);
    FlushSave(true);
  }
  SDL_UnlockMutex(s_game_lock);
  if (ok) {
    StopAudio();
    StartAudio();
  }
  Notify(ok ? "SLOT CARREGADO" : "ERRO AO CARREGAR O SLOT");
}

static void SlotDelete(int k) {
  char path[1200];
  SlotPath(k, path, sizeof path);
  remove(path);
  char message[48];
  snprintf(message, sizeof message, "SLOT %d APAGADO", k + 1);
  Notify(message);
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
/* Settings actions                                                          */
/* ------------------------------------------------------------------------ */

static void ApplyFilter(void) {
  if (s_texture)
    SDL_SetTextureScaleMode(s_texture,
                            s_settings.filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

/* Runs one menu action. `pending` is the action that was tapped once before:
 * destructive buttons only act on a second tap of the same button. */
static void DoAction(int a) {
  const int pending = s_confirm;
  s_confirm = -1;
  if (a <= A_TAB_SYS) {
    s_tab = a;
    s_page = 0;
    return;
  }
  if (a >= A_SLOT0 && a < A_SLOT0 + SLOT_COUNT * 3) {
    const int k = (a - A_SLOT0) / 3;
    const int op = (a - A_SLOT0) % 3;
    if (op == 0) SlotSave(k);
    else if (op == 1) SlotLoad(k);
    else if (pending == a) SlotDelete(k);
    else s_confirm = a;
    return;
  }
  switch (a) {
    case A_CLOSE: s_menu_open = 0; return;
    case A_CONFIG: s_menu_open = 1; return;
    case A_FILTER:
      s_settings.filter = !s_settings.filter;
      ApplyFilter();
      break;
    case A_ASPECT: s_settings.aspect = !s_settings.aspect; break;
    case A_BRIGHT_DN: s_settings.brightness = Step(s_settings.brightness, -10, 0, 100); break;
    case A_BRIGHT_UP: s_settings.brightness = Step(s_settings.brightness, 10, 0, 100); break;
    case A_FPS_DN: s_settings.fps_limit = Step(s_settings.fps_limit, -5, 10, 60); break;
    case A_FPS_UP: s_settings.fps_limit = Step(s_settings.fps_limit, 5, 10, 60); break;
    case A_VSYNC: s_settings.vsync = !s_settings.vsync; break;
    case A_SHOWFPS: s_settings.show_fps = !s_settings.show_fps; break;
    case A_SPEED_DN: s_settings.speed = Step(s_settings.speed, -10, 0, 200); break;
    case A_SPEED_UP: s_settings.speed = Step(s_settings.speed, 10, 0, 200); break;
    case A_BUTTONS: s_settings.touch_buttons = !s_settings.touch_buttons; break;
    case A_SIZE_DN: s_settings.size = Step(s_settings.size, -10, 70, 140); break;
    case A_SIZE_UP: s_settings.size = Step(s_settings.size, 10, 70, 140); break;
    case A_PAGE_PREV: s_page = 0; break;
    case A_PAGE_NEXT: s_page = 1; break;
    case A_RESET:
      if (pending != a) {
        s_confirm = a;
        return;
      }
      s_settings = kDefaults;
      ApplyFilter();
      SaveSettings();
      Notify("PADROES RESTAURADOS");
      return;
    case A_UPDATES:
      Notify(SDL_OpenURL(UPDATE_URL) == 0 ? "ABRINDO A PAGINA DE VERSOES"
                                          : "NAO FOI POSSIVEL ABRIR O LINK");
      return;
    default:
      return;
  }
  if (a >= A_FILTER && a <= A_SIZE_UP) SaveSettings();
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

/* Wooden palette, used by the CONFIG panel (DK theme). */
#define WOOD_RIM 70, 38, 18
#define WOOD_BODY 150, 88, 44
#define WOOD_BODY_PRESSED 185, 110, 58
#define WOOD_SHINE 240, 170, 105
#define CREAM 255, 236, 200

/* Rounded wooden pill, used by the CONFIG panel. */
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

/* ------------------------------------------------------------------------ */
/* Translucent gameplay buttons (as in the mockup)                           */
/* ------------------------------------------------------------------------ */

static void DrawGlassFace(const Circle *c, bool pressed) {
  const float r = c->r * (pressed ? 1.06f : 1.0f);
  FillDisc(c->cx, c->cy, r, 255, 255, 255, pressed ? 120 : 70);
  FillDisc(c->cx, c->cy, r * 0.88f, 255, 255, 255, pressed ? 90 : 25);
  char label[2] = {c->label, 0};
  int px = (int)(r * 1.1f / 7.0f);
  if (px < 2) px = 2;
  DrawTextCentered(label, c->cx, c->cy, px, 255, 255, 255, 235);
}

static void DrawGlassPill(const Box *b, bool pressed) {
  const float rad = b->h * 0.5f;
  FillRoundRect(b->x - 1, b->y - 1, b->w + 2, b->h + 2, rad, 255, 255, 255,
                pressed ? 150 : 90);
  FillRoundRect(b->x + 2, b->y + 2, b->w - 4, b->h - 4, rad, 255, 255, 255,
                pressed ? 100 : 35);
  int px = (int)(b->h * 0.38f / 7.0f);
  const int n = (int)strlen(b->label);
  const int px_fit = (int)(b->w * 0.8f / (6.0f * n));
  if (px_fit < px) px = px_fit;
  if (px < 1) px = 1;
  DrawTextCentered(b->label, b->x + b->w * 0.5f, b->y + b->h * 0.5f, px,
                   255, 255, 255, 235);
}

/* Gear icon: translucent disc with eight teeth and a hole. */
static void DrawGear(const Circle *g, bool pressed) {
  const float r = g->r;
  FillDisc(g->cx, g->cy, r, 255, 255, 255, pressed ? 120 : 70);
  FillDisc(g->cx, g->cy, r * 0.8f, 255, 255, 255, pressed ? 90 : 25);
  for (int i = 0; i < 8; ++i) {
    const float a = i * 0.785398f;
    FillDisc(g->cx + cosf(a) * r * 0.82f, g->cy + sinf(a) * r * 0.82f, r * 0.16f,
             255, 255, 255, 230);
  }
  FillDisc(g->cx, g->cy, r * 0.36f, 0, 0, 0, 110);
}

/* Solid arrow pointing in direction (ux, uy) from the D-pad centre. */
static void DrawArrow(float cx, float cy, float ux, float uy, float r, float t) {
  const float px = -uy, py = ux;
  const float tipx = cx + ux * r * 0.72f, tipy = cy + uy * r * 0.72f;
  const float bx = cx + ux * r * 0.42f, by = cy + uy * r * 0.42f;
  const float hb = t * 0.45f;
  FillTriangle(tipx, tipy, bx + px * hb, by + py * hb, bx - px * hb, by - py * hb,
               255, 255, 255, 220);
}

static void DrawGlassDpad(const Layout *l, uint32_t pressed) {
  const float r = l->dpad_r, t = r * 0.34f, rad = t * 0.6f;
  const float cx = l->dpad_cx, cy = l->dpad_cy;
  FillRoundRect(cx - r - 2, cy - t - 2, 2 * r + 4, 2 * t + 4, rad, 255, 255, 255, 80);
  FillRoundRect(cx - t - 2, cy - r - 2, 2 * t + 4, 2 * r + 4, rad, 255, 255, 255, 80);
  FillRoundRect(cx - r, cy - t, 2 * r, 2 * t, rad, 255, 255, 255, 35);
  FillRoundRect(cx - t, cy - r, 2 * t, 2 * r, rad, 255, 255, 255, 35);
  if (pressed & PAD_LEFT)
    FillRoundRect(cx - r, cy - t, r - t, 2 * t, rad, 255, 255, 255, 110);
  if (pressed & PAD_RIGHT)
    FillRoundRect(cx + t, cy - t, r - t, 2 * t, rad, 255, 255, 255, 110);
  if (pressed & PAD_UP)
    FillRoundRect(cx - t, cy - r, 2 * t, r - t, rad, 255, 255, 255, 110);
  if (pressed & PAD_DOWN)
    FillRoundRect(cx - t, cy + t, 2 * t, r - t, rad, 255, 255, 255, 110);
  DrawArrow(cx, cy, 0, -1, r, t);
  DrawArrow(cx, cy, 0, 1, r, t);
  DrawArrow(cx, cy, -1, 0, r, t);
  DrawArrow(cx, cy, 1, 0, r, t);
}

static void DrawOverlay(const Layout *l, const Touch *t) {
  if (s_settings.touch_buttons) {
    DrawGlassDpad(l, t->pad);
    for (int i = 0; i < 4; ++i)
      DrawGlassFace(&l->face[i], (t->pad & l->face[i].mask) != 0);
    DrawGlassPill(&l->select, (t->pad & l->select.mask) != 0);
    DrawGlassPill(&l->start, (t->pad & l->start.mask) != 0);
    DrawGlassPill(&l->left, (t->pad & l->left.mask) != 0);
    DrawGlassPill(&l->right, (t->pad & l->right.mask) != 0);
  }
  DrawGear(&l->gear, (t->acts & l->gear.act) != 0);
}

/* ------------------------------------------------------------------------ */
/* CONFIG panel                                                              */
/* ------------------------------------------------------------------------ */

static void AddBtn(float x, float y, float w, float h, int id, const char *label,
                   bool active, bool enabled) {
  if (s_view.nb >= 48) return;
  MButton *b = &s_view.btn[s_view.nb++];
  snprintf(b->text, sizeof b->text, "%s", label);
  b->box = (Box){x, y, w, h, 0, ACT(id), b->text};
  b->enabled = enabled;
  b->active = active;
}

static void AddText(const char *text, float x, float y, int px, bool center, bool gold) {
  if (s_view.nt >= 48) return;
  MText *t = &s_view.txt[s_view.nt++];
  snprintf(t->text, sizeof t->text, "%s", text);
  t->x = x;
  t->y = y;
  t->px = px;
  t->center = center;
  t->gold = gold;
}

/* Reads the saved slot's date, or false when the slot is empty. */
static bool SlotText(int k, char *out, size_t n) {
  time_t mtime = 0;
  if (!SlotInfo(k, &mtime)) {
    snprintf(out, n, "VAZIO");
    return false;
  }
  struct tm tmv;
  localtime_r(&mtime, &tmv);
  strftime(out, n, "%d/%m %H:%M", &tmv);
  return true;
}

/* Lays out the whole panel for this frame. Row centres are shared by all tabs. */
static void BuildMenu(float w, float h) {
  s_view.nb = 0;
  s_view.nt = 0;
  const float mm = s_mm_px;
  const float pw = w * 0.92f, ph = h * 0.96f;
  const float px0 = (w - pw) * 0.5f, py0 = (h - ph) * 0.5f;
  float bh = 7.0f * mm;
  if (bh < 0.085f * ph) bh = 0.085f * ph;
  if (bh > 0.13f * ph) bh = 0.13f * ph;
  s_view.px0 = px0;
  s_view.py0 = py0;
  s_view.pw = pw;
  s_view.ph = ph;
  s_view.bh = bh;

  const float tab_y = py0 + bh * 1.1f;
  const float rows_top = tab_y + bh + bh * 0.3f;
  const float footer_y = py0 + ph - bh * 1.4f;
  const float rows_bot = footer_y - bh * 0.3f;
  const float rh = (rows_bot - rows_top) / 5.0f;
  const float cb = fminf(bh, rh * 0.9f);
  const float ctl_x = px0 + pw * 0.50f;
  const float lab_x = px0 + pw * 0.04f;
  const int max_px = (int)(cb * 0.45f / 7.0f) > 1 ? (int)(cb * 0.45f / 7.0f) : 1;
  char buf[48];

  /* Tabs. */
  static const char *tabs[5] = {"GRAFICOS", "DESEMPENHO", "JOGABILIDADE", "PROGRESSO", "SISTEMA"};
  const float gap = pw * 0.01f;
  const float tw = (pw * 0.96f - 4 * gap) / 5.0f;
  for (int i = 0; i < 5; ++i)
    AddBtn(px0 + pw * 0.02f + i * (tw + gap), tab_y, tw, bh, A_TAB_GFX + i, tabs[i],
           s_tab == i, true);

  /* Title, notice (in gold) and close button. */
  const float title_cy = py0 + bh * 0.5f;
  AddText("CONFIG", px0 + pw * 0.02f, title_cy, FitPx("CONFIG", pw * 0.2f, max_px), false, false);
  if (SDL_GetTicks() < s_notice_until && s_notice[0])
    AddText(s_notice, px0 + pw * 0.5f, title_cy, FitPx(s_notice, pw * 0.5f, max_px), true, true);
  AddBtn(px0 + pw * 0.02f, footer_y, pw * 0.18f, bh, A_CLOSE, "FECHAR", false, true);

  /* Row helpers. */
  #define ROW_CY(i) (rows_top + rh * ((i) + 0.5f))
  #define ROW_LABEL(text, i) AddText((text), lab_x, ROW_CY(i), \
      FitPx((text), pw * 0.44f, max_px), false, false)

  if (s_tab == A_TAB_GFX) {
    ROW_LABEL("FILTRO", 0);
    AddBtn(ctl_x, ROW_CY(0) - cb * 0.5f, pw * 0.30f, cb, A_FILTER,
           s_settings.filter ? "SUAVIZADO" : "ORIGINAL", false, true);
    ROW_LABEL("PROPORCAO", 1);
    AddBtn(ctl_x, ROW_CY(1) - cb * 0.5f, pw * 0.30f, cb, A_ASPECT,
           s_settings.aspect ? "ADAPTADA" : "ORIGINAL", false, true);
    ROW_LABEL("BRILHO", 2);
    snprintf(buf, sizeof buf, "%d%%", s_settings.brightness);
    AddBtn(ctl_x, ROW_CY(2) - cb * 0.5f, cb, cb, A_BRIGHT_DN, "-", false, true);
    AddText(buf, ctl_x + cb + pw * 0.09f, ROW_CY(2), FitPx(buf, pw * 0.16f, max_px), true, false);
    AddBtn(ctl_x + cb + pw * 0.18f, ROW_CY(2) - cb * 0.5f, cb, cb, A_BRIGHT_UP, "+", false, true);
  } else if (s_tab == A_TAB_PERF) {
    ROW_LABEL("LIMITE FPS", 0);
    snprintf(buf, sizeof buf, "%d", s_settings.fps_limit);
    AddBtn(ctl_x, ROW_CY(0) - cb * 0.5f, cb, cb, A_FPS_DN, "-", false, true);
    AddText(buf, ctl_x + cb + pw * 0.09f, ROW_CY(0), FitPx(buf, pw * 0.16f, max_px), true, false);
    AddBtn(ctl_x + cb + pw * 0.18f, ROW_CY(0) - cb * 0.5f, cb, cb, A_FPS_UP, "+", false, true);
    ROW_LABEL("V-SYNC", 1);
    AddBtn(ctl_x, ROW_CY(1) - cb * 0.5f, pw * 0.30f, cb, A_VSYNC,
           s_settings.vsync ? "ON" : "OFF", false, true);
    AddText("VALE AO REABRIR O APP", ctl_x + pw * 0.32f, ROW_CY(1),
            FitPx("VALE AO REABRIR O APP", pw * 0.16f, max_px), false, false);
    ROW_LABEL("MOSTRAR FPS", 2);
    AddBtn(ctl_x, ROW_CY(2) - cb * 0.5f, pw * 0.30f, cb, A_SHOWFPS,
           s_settings.show_fps ? "ON" : "OFF", false, true);
  } else if (s_tab == A_TAB_PLAY) {
    ROW_LABEL("VELOCIDADE", 0);
    snprintf(buf, sizeof buf, "%d%%", s_settings.speed);
    AddBtn(ctl_x, ROW_CY(0) - cb * 0.5f, cb, cb, A_SPEED_DN, "-", false, true);
    AddText(buf, ctl_x + cb + pw * 0.09f, ROW_CY(0), FitPx(buf, pw * 0.16f, max_px), true, false);
    AddBtn(ctl_x + cb + pw * 0.18f, ROW_CY(0) - cb * 0.5f, cb, cb, A_SPEED_UP, "+", false, true);
    ROW_LABEL("BOTOES NA TELA", 1);
    AddBtn(ctl_x, ROW_CY(1) - cb * 0.5f, pw * 0.30f, cb, A_BUTTONS,
           s_settings.touch_buttons ? "ON" : "OFF", false, true);
    ROW_LABEL("TAMANHO DOS BOTOES", 2);
    snprintf(buf, sizeof buf, "%d%%", s_settings.size);
    AddBtn(ctl_x, ROW_CY(2) - cb * 0.5f, cb, cb, A_SIZE_DN, "-", false, true);
    AddText(buf, ctl_x + cb + pw * 0.09f, ROW_CY(2), FitPx(buf, pw * 0.16f, max_px), true, false);
    AddBtn(ctl_x + cb + pw * 0.18f, ROW_CY(2) - cb * 0.5f, cb, cb, A_SIZE_UP, "+", false, true);
  } else if (s_tab == A_TAB_SAVE) {
    const int first = s_page * SLOTS_PER_PAGE;
    for (int r = 0; r < SLOTS_PER_PAGE; ++r) {
      const int k = first + r;
      char name[16], info[24];
      snprintf(name, sizeof name, "SLOT %02d", k + 1);
      AddText(name, lab_x, ROW_CY(r), FitPx(name, pw * 0.09f, max_px), false, false);
      const bool occupied = SlotText(k, info, sizeof info);
      AddText(info, px0 + pw * 0.14f, ROW_CY(r), FitPx(info, pw * 0.22f, max_px), false, false);
      const float by = ROW_CY(r) - cb * 0.5f, bw = pw * 0.13f;
      const int id_save = A_SLOT0 + k * 3;
      AddBtn(px0 + pw * 0.50f, by, bw, cb, id_save, "SALVAR", false, true);
      AddBtn(px0 + pw * 0.645f, by, bw, cb, id_save + 1, "CARREGAR", false, occupied);
      AddBtn(px0 + pw * 0.79f, by, bw, cb, id_save + 2,
             s_confirm == id_save + 2 ? "CONFIRMAR" : "APAGAR",
             s_confirm == id_save + 2, occupied);
    }
    /* Page navigation in the footer. */
    const float prev_x = px0 + pw * 0.40f, next_x = prev_x + bh * 1.6f + pw * 0.12f;
    AddBtn(prev_x, footer_y, bh * 1.6f, bh, A_PAGE_PREV, "ANT", false, s_page > 0);
    AddBtn(next_x, footer_y, bh * 1.6f, bh, A_PAGE_NEXT, "PROX", false, s_page < 1);
    snprintf(buf, sizeof buf, "PAGINA %d/2", s_page + 1);
    AddText(buf, prev_x + bh * 1.6f + pw * 0.06f, footer_y + bh * 0.5f,
            FitPx(buf, pw * 0.1f, max_px), true, false);
  } else {
    ROW_LABEL("RESTAURAR", 0);
    AddBtn(ctl_x, ROW_CY(0) - cb * 0.5f, pw * 0.30f, cb, A_RESET,
           s_confirm == A_RESET ? "CONFIRMAR" : "PADROES", s_confirm == A_RESET, true);
    ROW_LABEL("ATUALIZACOES", 1);
    AddBtn(ctl_x, ROW_CY(1) - cb * 0.5f, pw * 0.30f, cb, A_UPDATES, "VERIFICAR", false, true);
    AddText(VERSION_TEXT, lab_x, ROW_CY(2), FitPx(VERSION_TEXT, pw * 0.44f, max_px), false, false);
    AddText("CREDITOS " CREDIT_TEXT, lab_x, ROW_CY(3),
            FitPx("CREDITOS " CREDIT_TEXT, pw * 0.44f, max_px), false, false);
  }
  #undef ROW_CY
  #undef ROW_LABEL
}

static void DrawMenu(const Touch *t) {
  FillRoundRect(s_view.px0 - 4, s_view.py0 - 4, s_view.pw + 8, s_view.ph + 8,
                s_view.ph * 0.03f, WOOD_RIM, 255);
  FillRoundRect(s_view.px0, s_view.py0, s_view.pw, s_view.ph, s_view.ph * 0.03f,
                26, 15, 8, 240);
  for (int i = 0; i < s_view.nb; ++i) {
    const MButton *b = &s_view.btn[i];
    const bool pressed = (t->acts & b->box.act) != 0;
    Box box = b->box;
    box.label = b->text;
    DrawPill(&box, pressed || b->active);
    if (!b->enabled)
      FillRoundRect(box.x, box.y, box.w, box.h, box.h * 0.5f, 0, 0, 0, 150);
  }
  /* Banana underline under the current tab. */
  if (s_tab >= 0 && s_tab <= A_TAB_SYS) {
    const Box *tb = &s_view.btn[s_tab].box;
    FillRoundRect(tb->x + tb->w * 0.15f, tb->y + tb->h - s_view.bh * 0.14f,
                  tb->w * 0.7f, s_view.bh * 0.12f, s_view.bh * 0.06f, 255, 210, 60, 255);
  }
  for (int i = 0; i < s_view.nt; ++i) {
    const MText *t2 = &s_view.txt[i];
    const uint8_t r = 255, g = t2->gold ? 210 : 236, b = t2->gold ? 60 : 200;
    if (t2->center) DrawTextCentered(t2->text, t2->x, t2->y, t2->px, r, g, b, 255);
    else DrawText(t2->text, (int)t2->x, (int)(t2->y - 3.5f * t2->px), t2->px, r, g, b, 255);
  }
}

static SDL_Rect GameRect(int out_w, int out_h) {
  if (s_settings.aspect) {
    SDL_Rect full = {0, 0, out_w, out_h};
    return full;
  }
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
    DrawText(CREDIT_TEXT, (left - tw) / 2, h / 2 - 3 * px, px, 255, 236, 200, 220);
  } else if (right >= tw + 12) {
    DrawText(CREDIT_TEXT, dest->x + dest->w + (right - tw) / 2, h / 2 - 3 * px, px,
             255, 236, 200, 220);
  } else {
    DrawText(CREDIT_TEXT, (w - tw) / 2, 4, px, 255, 236, 200, 220);
  }
}

/* ------------------------------------------------------------------------ */
/* Main                                                                      */
/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
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
  setenv("SNESRECOMP_TIER2_DIR", tier2, 1);

  /* Saves go to a visible folder when all-files access is granted. */
  const char *save_root = files;
  mkdir(SAVE_ROOT, 0755);
  if (access(SAVE_ROOT, W_OK) == 0) save_root = SAVE_ROOT;
  snprintf(s_save_root, sizeof s_save_root, "%s", save_root);
  char saves_dir[1100];
  snprintf(saves_dir, sizeof saves_dir, "%s/saves", save_root);
  mkdir(saves_dir, 0755);
  snprintf(s_slot_dir, sizeof s_slot_dir, "%s/slots", saves_dir);
  mkdir(s_slot_dir, 0755);
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
  /* V-Sync is chosen here, so a change in the panel applies on the next launch. */
  const Uint32 renderer_flags =
      SDL_RENDERER_ACCELERATED | (s_settings.vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
  s_renderer = SDL_CreateRenderer(s_window, -1, renderer_flags);
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
    remove(rom_path); /* forget a bad ROM so the picker asks again */
    return 1;
  }
  s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, s_render_width,
                                kDkc1VideoHeight);
  if (!s_texture) {
    Fail("Nao foi possivel criar a textura de video.");
    return 1;
  }
  ApplyFilter();

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
  Uint64 last_loop = previous;
  double elapsed = 0, audio_fraction = 0;
  bool running = true;
  uint64_t prev_acts = 0;
  Uint32 fps_window_start = SDL_GetTicks();
  int presented_in_window = 0, fps_shown = 0;

  while (running) {
    /* FPS limit from the panel: wait so frames are not presented faster. */
    {
      const double budget = 1.0 / s_settings.fps_limit;
      const double since = (double)(SDL_GetPerformanceCounter() - last_loop) / (double)frequency;
      if (since < budget) SDL_Delay((Uint32)((budget - since) * 1000.0));
      last_loop = SDL_GetPerformanceCounter();
    }

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
    if (s_menu_open) BuildMenu((float)out_w, (float)out_h);

    /* Act on the press (an edge), not on every frame the finger stays down. */
    const Touch touch = ReadTouch(&layout, out_w, out_h);
    const uint64_t edges = touch.acts & ~prev_acts;
    prev_acts = touch.acts;
    if (s_menu_open) {
      for (int a = 0; a < 64; ++a)
        if (edges & ACT(a)) DoAction(a);
    } else if (edges & ACT(A_CONFIG)) {
      s_menu_open = 1;
      s_confirm = -1;
    }

    /* Joypad input: touch and gamepads, but nothing while the panel is open. */
    uint32_t input = 0;
    if (!s_menu_open) {
      input = touch.pad;
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
    if (!s_menu_open) elapsed += dt * (s_settings.speed / 100.0);
    else elapsed = 0;

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
    if (s_settings.brightness < 100) {
      SDL_SetRenderDrawColor(s_renderer, 0, 0, 0,
                             (Uint8)((100 - s_settings.brightness) * 255 / 100));
      SDL_RenderFillRect(s_renderer, &dest);
    }
    DrawCredit(&dest, out_w, out_h);
    if (s_menu_open) DrawMenu(&touch);
    else DrawOverlay(&layout, &touch);

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
