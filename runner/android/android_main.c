/* Android frontend for DKC1Recomp (SDL2).
 *
 * Modelled on runner/ios/ios_main.m: the recompiled game/runtime is shared and
 * unchanged; this file only provides window, video, audio, touch controls and
 * game-controller input. It reuses the portable iOS helpers (audio ring and
 * viewport maths).
 *
 * The ROM is never bundled. RomPickerActivity (Java) copies the ROM the user
 * chooses to <app files dir>/game.sfc, and this file loads it from there.
 *
 * Status: written without the ability to compile or run on a device. Expect
 * to iterate on build errors and on-device behaviour.
 */
#define _POSIX_C_SOURCE 200809L

#include <SDL.h>
#include <SDL_main.h> /* renames main() to SDL_main() on Android */
#include <SDL_system.h>   /* SDL_AndroidGetInternalStoragePath */

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

static const double kGameHz = 60.098811862;
static const int kAudioRate = 32040;

/* SNES joypad bits, identical to the iOS frontend. */
enum {
  PAD_B = 0x001, PAD_Y = 0x002, PAD_SELECT = 0x004, PAD_START = 0x008,
  PAD_UP = 0x010, PAD_DOWN = 0x020, PAD_LEFT = 0x040, PAD_RIGHT = 0x080,
  PAD_A = 0x100, PAD_X = 0x200, PAD_L = 0x400, PAD_R = 0x800
};

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

/* ------------------------------------------------------------------------ */
/* Touch layout                                                              */
/* ------------------------------------------------------------------------ */

typedef struct Circle {
  float cx, cy, r;
  uint32_t mask;
  uint8_t red, green, blue;
} Circle;

typedef struct Box {
  float x, y, w, h;
  uint32_t mask;
} Box;

typedef struct Layout {
  float dpad_cx, dpad_cy, dpad_r;
  Circle face[4]; /* B, A, Y, X in SNES positions */
  Box start, select, left, right;
} Layout;

static void MakeLayout(Layout *layout, float w, float h) {
  const float m = 0.05f * h;
  layout->dpad_r = 0.21f * h;
  layout->dpad_cx = m + layout->dpad_r * 1.1f;
  layout->dpad_cy = h - m - layout->dpad_r * 1.05f;

  const float fr = 0.085f * h;
  const float s = 0.125f * h;
  const float fcx = w - m - s - fr;
  const float fcy = h - m - s - fr - 0.02f * h;
  /* SNES colours: B yellow, A red, Y green, X blue. */
  layout->face[0] = (Circle){fcx, fcy + s, fr, PAD_B, 240, 200, 70};
  layout->face[1] = (Circle){fcx + s, fcy, fr, PAD_A, 230, 80, 80};
  layout->face[2] = (Circle){fcx - s, fcy, fr, PAD_Y, 90, 200, 110};
  layout->face[3] = (Circle){fcx, fcy - s, fr, PAD_X, 90, 140, 230};

  const float bw = 0.15f * h, bh = 0.065f * h, by = h - m - bh;
  layout->select = (Box){w * 0.5f - bw - 0.02f * h, by, bw, bh, PAD_SELECT};
  layout->start = (Box){w * 0.5f + 0.02f * h, by, bw, bh, PAD_START};
  const float sw = 0.22f * h, sh = 0.09f * h;
  layout->left = (Box){m, m, sw, sh, PAD_L};
  layout->right = (Box){w - m - sw, m, sw, sh, PAD_R};
}

static bool InBox(const Box *box, float x, float y) {
  const float pad = 0.02f * box->h * 4.0f; /* generous touch margin */
  return x >= box->x - pad && x <= box->x + box->w + pad &&
         y >= box->y - pad && y <= box->y + box->h + pad;
}

static uint32_t FingerMask(const Layout *layout, float x, float y) {
  uint32_t mask = 0;
  const float dx = x - layout->dpad_cx, dy = y - layout->dpad_cy;
  if (hypotf(dx, dy) < layout->dpad_r * 1.6f) {
    const float nx = dx / layout->dpad_r, ny = dy / layout->dpad_r;
    if (nx < -0.3f) mask |= PAD_LEFT; else if (nx > 0.3f) mask |= PAD_RIGHT;
    if (ny < -0.3f) mask |= PAD_UP; else if (ny > 0.3f) mask |= PAD_DOWN;
  }
  /* A finger between two neighbouring face buttons presses both, which makes
   * run+jump (Y+B) easy and lets a thumb roll from one to the other. */
  for (int i = 0; i < 4; ++i) {
    const Circle *c = &layout->face[i];
    if (hypotf(x - c->cx, y - c->cy) < c->r * 1.35f) mask |= c->mask;
  }
  if (InBox(&layout->start, x, y)) mask |= PAD_START;
  if (InBox(&layout->select, x, y)) mask |= PAD_SELECT;
  if (InBox(&layout->left, x, y)) mask |= PAD_L;
  if (InBox(&layout->right, x, y)) mask |= PAD_R;
  return mask;
}

static uint32_t ReadTouch(const Layout *layout, int w, int h) {
  uint32_t mask = 0;
  const int devices = SDL_GetNumTouchDevices();
  for (int d = 0; d < devices; ++d) {
    const SDL_TouchID id = SDL_GetTouchDevice(d);
    const int fingers = SDL_GetNumTouchFingers(id);
    for (int i = 0; i < fingers; ++i) {
      const SDL_Finger *finger = SDL_GetTouchFinger(id, i);
      if (finger) mask |= FingerMask(layout, finger->x * w, finger->y * h);
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
/* Lifecycle                                                                 */
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
/* Drawing                                                                   */
/* ------------------------------------------------------------------------ */

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

static void FillCircle(const Circle *c, bool pressed) {
  SDL_SetRenderDrawColor(s_renderer, c->red, c->green, c->blue, pressed ? 200 : 80);
  const int r = (int)c->r;
  for (int dy = -r; dy <= r; ++dy) {
    const float half = sqrtf((float)(r * r - dy * dy));
    SDL_RenderDrawLine(s_renderer, (int)(c->cx - half), (int)c->cy + dy,
                       (int)(c->cx + half), (int)c->cy + dy);
  }
}

static void FillBox(const Box *b, bool pressed) {
  SDL_SetRenderDrawColor(s_renderer, 220, 220, 220, pressed ? 190 : 70);
  SDL_Rect rect = {(int)b->x, (int)b->y, (int)b->w, (int)b->h};
  SDL_RenderFillRect(s_renderer, &rect);
}

static void DrawOverlay(const Layout *layout, uint32_t pressed) {
  SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
  /* D-pad as a plus sign, with the active directions highlighted. */
  const float r = layout->dpad_r, t = r * 0.34f;
  SDL_SetRenderDrawColor(s_renderer, 220, 220, 220, 60);
  SDL_Rect horizontal = {(int)(layout->dpad_cx - r), (int)(layout->dpad_cy - t),
                         (int)(2 * r), (int)(2 * t)};
  SDL_Rect vertical = {(int)(layout->dpad_cx - t), (int)(layout->dpad_cy - r),
                       (int)(2 * t), (int)(2 * r)};
  SDL_RenderFillRect(s_renderer, &horizontal);
  SDL_RenderFillRect(s_renderer, &vertical);
  SDL_SetRenderDrawColor(s_renderer, 220, 220, 220, 170);
  if (pressed & PAD_LEFT) {
    SDL_Rect q = {(int)(layout->dpad_cx - r), (int)(layout->dpad_cy - t), (int)(r - t), (int)(2 * t)};
    SDL_RenderFillRect(s_renderer, &q);
  }
  if (pressed & PAD_RIGHT) {
    SDL_Rect q = {(int)(layout->dpad_cx + t), (int)(layout->dpad_cy - t), (int)(r - t), (int)(2 * t)};
    SDL_RenderFillRect(s_renderer, &q);
  }
  if (pressed & PAD_UP) {
    SDL_Rect q = {(int)(layout->dpad_cx - t), (int)(layout->dpad_cy - r), (int)(2 * t), (int)(r - t)};
    SDL_RenderFillRect(s_renderer, &q);
  }
  if (pressed & PAD_DOWN) {
    SDL_Rect q = {(int)(layout->dpad_cx - t), (int)(layout->dpad_cy + t), (int)(2 * t), (int)(r - t)};
    SDL_RenderFillRect(s_renderer, &q);
  }
  for (int i = 0; i < 4; ++i) FillCircle(&layout->face[i], (pressed & layout->face[i].mask) != 0);
  FillBox(&layout->start, (pressed & PAD_START) != 0);
  FillBox(&layout->select, (pressed & PAD_SELECT) != 0);
  FillBox(&layout->left, (pressed & PAD_L) != 0);
  FillBox(&layout->right, (pressed & PAD_R) != 0);
  SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_NONE);
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
  char rom_path[1024], tier2[1024];
  snprintf(rom_path, sizeof rom_path, "%s/game.sfc", files);
  snprintf(tier2, sizeof tier2, "%s/tier2", files);
  mkdir(tier2, 0755);
  if (chdir(files) != 0) SDL_Log(TAG ": chdir(%s) failed", files);
  setenv("SNESRECOMP_TIER2_DIR", tier2, 1); /* same role as on iOS */

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

    if (s_paused || s_failed) {
      SDL_Delay(30);
      previous = SDL_GetPerformanceCounter();
      continue;
    }

    const Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - previous) / (double)frequency;
    previous = now;
    if (dt > 4.0 / kGameHz) dt = 4.0 / kGameHz;
    elapsed += dt;

    const uint32_t touch = ReadTouch(&layout, out_w, out_h);
    uint32_t input = touch;
    for (int i = 0; i < 2; ++i) input |= ReadPad(s_pads[i]) << (i * 12);
    /* Controller 2 shares the same layout as iOS: shifted by 12 bits. */

    bool drew = false;
    SDL_LockMutex(s_game_lock);
    while (elapsed >= 1.0 / kGameHz && !s_paused) {
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
    uint32_t visual = touch;
    for (int i = 0; i < 2; ++i) visual |= ReadPad(s_pads[i]);
    DrawOverlay(&layout, visual);
    SDL_RenderPresent(s_renderer);
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
