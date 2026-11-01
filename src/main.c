/* SDL2 platform layer for TrimUI Brick / Smart Pro (NextUI, tg5040) */
#include "ui.h"
#include "audio.h"
#include <SDL.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>

static SDL_AudioDeviceID g_dev;

void plat_audio_lock(void) { if (g_dev) SDL_LockAudioDevice(g_dev); }
void plat_audio_unlock(void) { if (g_dev) SDL_UnlockAudioDevice(g_dev); }

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103
static int g_saved_brightness = -1;

static int disp_get(void) {
	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0) return -1;
	unsigned long p[4] = {0, 0, 0, 0};
	int v = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, p);
	close(fd);
	return v;
}
static void disp_set(int v) {
	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0) return;
	unsigned long p[4] = {0, (unsigned long)v, 0, 0};
	ioctl(fd, DISP_LCD_SET_BRIGHTNESS, p);
	close(fd);
}

void plat_set_screen(int on) {
	if (!on) {
		int b = disp_get();
		if (b > 0) g_saved_brightness = b;
		disp_set(0);
	} else {
		disp_set(g_saved_brightness > 0 ? g_saved_brightness : 128);
	}
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
	(void)ud;
	player_mix((int16_t *)stream, len / 4);
}

static int map_joy_button(int b) {
	switch (b) {
	case 0: return BTN_B;
	case 1: return BTN_A;
	case 2: return BTN_Y;
	case 3: return BTN_X;
	case 4: return BTN_L1;
	case 5: return BTN_R1;
	case 6: return BTN_SELECT;
	case 7: return BTN_START;
	case 8: return BTN_MENU;
	default: return -1;
	}
}

static int map_key(SDL_Keysym k) {
	if (k.scancode == 102 || k.sym == SDLK_POWER) return BTN_POWER;
	switch (k.sym) {
	case SDLK_UP: return BTN_UP;
	case SDLK_DOWN: return BTN_DOWN;
	case SDLK_LEFT: return BTN_LEFT;
	case SDLK_RIGHT: return BTN_RIGHT;
	case SDLK_RETURN: case SDLK_z: return BTN_A;
	case SDLK_BACKSPACE: case SDLK_x: return BTN_B;
	case SDLK_a: return BTN_X;
	case SDLK_s: return BTN_Y;
	case SDLK_q: return BTN_L1;
	case SDLK_w: return BTN_R1;
	case SDLK_1: return BTN_L2;
	case SDLK_2: return BTN_R2;
	case SDLK_RSHIFT: return BTN_SELECT;
	case SDLK_SPACE: return BTN_START;
	case SDLK_ESCAPE: return BTN_MENU;
	case SDLK_p: return BTN_POWER;
	}
	return -1;
}

typedef struct { int down; uint64_t next; } Rep;
static Rep reps[BTN_COUNT];

static void press(App *app, int btn, int down) {
	if (btn < 0) return;
	if (down && reps[btn].down) return;
	reps[btn].down = down;
	reps[btn].next = now_ms() + 320;
	app_input(app, btn, down, 0);
}

static void mkdirs(const char *p) {
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", p);
	for (char *s = tmp + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
	mkdir(tmp, 0755);
}

int main(int argc, char **argv) {
	const char *pakdir = getenv("PAK_DIR");
	char resdir[1024], datadir[1024], skip[1100];
	if (!pakdir) pakdir = ".";
	snprintf(resdir, sizeof resdir, "%s/res", pakdir);
	const char *ud = getenv("USERDATA_PATH");
	if (ud) snprintf(datadir, sizeof datadir, "%s/Kasseta", ud);
	else snprintf(datadir, sizeof datadir, "%s/data", pakdir);
	mkdirs(datadir);
	snprintf(skip, sizeof skip, "%s/skip.txt", pakdir);
	const char *root = getenv("SDCARD_PATH");
	if (argc > 1) root = argv[1];
	if (!root) root = "/mnt/SDCARD";

	int w = 1024, h = 768;
	const char *dev = getenv("DEVICE");
	if (dev && !strcmp(dev, "smartpro")) { w = 1280; h = 720; }

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_ShowCursor(0);
#ifdef PLATFORM_TG5040
	SDL_DisplayMode dm;
	if (!dev && SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) { w = dm.w; h = dm.h; }
	Uint32 wflags = SDL_WINDOW_SHOWN;
#else
	Uint32 wflags = SDL_WINDOW_SHOWN;
#endif
	SDL_Window *win = SDL_CreateWindow("Kasseta", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, wflags);
	if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);

	for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 44100;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio_cb;
	g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!g_dev) fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
	player_init(g_dev ? have.freq : 44100);
	if (g_dev) SDL_PauseAudioDevice(g_dev, 0);

	AppConfig cfg = { w, h, resdir, datadir, skip, root };
	App *app = app_create(&cfg);

	int hat = 0, l2 = 0, r2 = 0, ax_x = 0, ax_y = 0;
	uint64_t last = now_ms();
	while (!app_wants_quit(app)) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT: app_input(app, BTN_MENU, 1, 0); break;
			case SDL_KEYDOWN: if (!e.key.repeat) press(app, map_key(e.key.keysym), 1); break;
			case SDL_KEYUP: press(app, map_key(e.key.keysym), 0); break;
			case SDL_JOYBUTTONDOWN: press(app, map_joy_button(e.jbutton.button), 1); break;
			case SDL_JOYBUTTONUP: press(app, map_joy_button(e.jbutton.button), 0); break;
			case SDL_JOYHATMOTION: {
				int v = e.jhat.value;
				int nu = !!(v & SDL_HAT_UP), nd = !!(v & SDL_HAT_DOWN), nl = !!(v & SDL_HAT_LEFT), nr = !!(v & SDL_HAT_RIGHT);
				if (nu != !!(hat & SDL_HAT_UP)) press(app, BTN_UP, nu);
				if (nd != !!(hat & SDL_HAT_DOWN)) press(app, BTN_DOWN, nd);
				if (nl != !!(hat & SDL_HAT_LEFT)) press(app, BTN_LEFT, nl);
				if (nr != !!(hat & SDL_HAT_RIGHT)) press(app, BTN_RIGHT, nr);
				hat = v;
				break;
			}
			case SDL_JOYAXISMOTION: {
				int v = e.jaxis.value;
				if (e.jaxis.axis == 2) { int on = v > 16000; if (on != l2) { l2 = on; press(app, BTN_L2, on); } }
				else if (e.jaxis.axis == 5) { int on = v > 16000; if (on != r2) { r2 = on; press(app, BTN_R2, on); } }
				else if (e.jaxis.axis == 0) {
					int d = v < -20000 ? -1 : v > 20000 ? 1 : 0;
					if (d != ax_x) { if (ax_x) press(app, ax_x < 0 ? BTN_LEFT : BTN_RIGHT, 0); if (d) press(app, d < 0 ? BTN_LEFT : BTN_RIGHT, 1); ax_x = d; }
				} else if (e.jaxis.axis == 1) {
					int d = v < -20000 ? -1 : v > 20000 ? 1 : 0;
					if (d != ax_y) { if (ax_y) press(app, ax_y < 0 ? BTN_UP : BTN_DOWN, 0); if (d) press(app, d < 0 ? BTN_UP : BTN_DOWN, 1); ax_y = d; }
				}
				break;
			}
			case SDL_JOYDEVICEADDED: SDL_JoystickOpen(e.jdevice.which); break;
			}
		}
		/* key repeat for directions */
		uint64_t t = now_ms();
		for (int b = BTN_UP; b <= BTN_RIGHT; b++) {
			if (reps[b].down && t >= reps[b].next) {
				reps[b].next = t + 70;
				app_input(app, b, 1, 1);
			}
		}
		float dt = (t - last) / 1000.0f;
		last = t;
		if (dt > 0.1f) dt = 0.1f;
		if (app_update(app, dt)) {
			Canvas *cv = app_canvas(app);
			SDL_UpdateTexture(tex, NULL, cv->px, cv->stride * 4);
			SDL_RenderClear(ren);
			SDL_RenderCopy(ren, tex, NULL, NULL);
			SDL_RenderPresent(ren);
		}
		uint64_t spent = now_ms() - t;
		int frame = app_screen_off(app) ? 60 : app_animating(app) ? 33 : 16;
		if ((int)spent < frame) SDL_Delay(frame - spent);
	}
	if (app_screen_off(app)) plat_set_screen(1);
	app_destroy(app);
	if (g_dev) SDL_CloseAudioDevice(g_dev);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
