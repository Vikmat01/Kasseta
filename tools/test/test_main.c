/* Headless test harness: scans a fake SD card, renders screens to PNG, checks decoders. */
#include "../../src/ui.h"
#include "../../src/audio.h"
#include "../../src/tags.h"
#include "../../src/library.h"
#include <math.h>
#include <unistd.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

void plat_audio_lock(void) {}
void plat_audio_unlock(void) {}
void plat_set_screen(int on) { printf("[screen %s]\n", on ? "on" : "off"); }

static void save_png(App *a, const char *name) {
	Canvas *c = app_canvas(a);
	uint8_t *rgb = malloc(c->w * c->h * 3);
	for (int i = 0; i < c->w * c->h; i++) {
		uint32_t v = c->px[i];
		rgb[i * 3] = v >> 16; rgb[i * 3 + 1] = v >> 8; rgb[i * 3 + 2] = v;
	}
	char p[512];
	snprintf(p, sizeof p, "%s/%s.png", getenv("OUT") ? getenv("OUT") : ".", name);
	stbi_write_png(p, c->w, c->h, 3, rgb, c->w * 3);
	free(rgb);
	printf("wrote %s\n", p);
}

static void settle(App *a, int frames) {
	for (int i = 0; i < frames; i++) {
		int16_t buf[2048];
		player_mix(buf, 735); /* keep audio advancing ~1/60 s */
		app_update(a, 1 / 60.0f);
		usleep(4000);
	}
	app_force_redraw(a);
	app_update(a, 0);
}

static void decode_check(const char *path) {
	Decoder *d = dec_open(path);
	if (!d) { printf("DECODE FAIL %s\n", path); return; }
	int16_t buf[4096];
	long total = 0;
	int n;
	double energy = 0;
	while ((n = dec_read(d, buf, 2048)) > 0) {
		total += n;
		for (int i = 0; i < n * 2; i++) energy += fabs(buf[i]);
	}
	Meta m;
	tags_read(path, &m);
	printf("decode %-60s rate=%u frames=%ld (%.2fs) tagdur=%.2fs avg=%.0f\n", path_basename(path), dec_rate(d), total,
	       (double)total / dec_rate(d), m.duration_ms / 1000.0, energy / (total * 2 + 1));
	meta_free(&m);
	dec_close(d);
}

int main(int argc, char **argv) {
	const char *root = argc > 1 ? argv[1] : "testsd";
	const char *data = argc > 2 ? argv[2] : "/tmp/kdata";
	int w = argc > 3 ? atoi(argv[3]) : 1024, h = argc > 4 ? atoi(argv[4]) : 768;
	const char *prefix = argc > 5 ? argv[5] : "";
	char nm[128];
#define SHOT(x) do { snprintf(nm, sizeof nm, "%s%s", prefix, x); save_png(app, nm); } while (0)
	player_init(44100);
	AppConfig cfg = { w, h, "res", data, "pak/skip.txt", root };
	App *app = app_create(&cfg);
	app_update(app, 0.1f);
	SHOT("00_scanning");
	app_debug_open(app, "scanwait", 0);
	settle(app, 30);
	sleep_ms(400);
	settle(app, 10);
	app_debug_open(app, "tab", 1);
	settle(app, 5); sleep_ms(500); settle(app, 5);
	SHOT("01_albums");
	app_debug_open(app, "tab", 0);
	settle(app, 5); sleep_ms(300); settle(app, 5);
	SHOT("02_artists");
	app_debug_open(app, "tab", 2);
	settle(app, 5); sleep_ms(300); settle(app, 5);
	SHOT("03_tracks");
	app_debug_open(app, "tab", 0);
	app_debug_open(app, "sel", 2);
	app_debug_open(app, "artist", 2);
	settle(app, 5); sleep_ms(300); settle(app, 5);
	SHOT("04_artist");
	app_debug_open(app, "tab", 1);
	app_debug_open(app, "album", 3);
	app_debug_open(app, "sel", 1);
	settle(app, 5); sleep_ms(300); settle(app, 5);
	SHOT("05_album");
	app_debug_open(app, "play_album", 3);
	app_debug_open(app, "player", 0);
	app_debug_open(app, "view", 0);
	settle(app, 5); sleep_ms(300);
	settle(app, 90);
	SHOT("06_cassette");
	if (getenv("FRAMES")) {
		for (int v = 0; v < 2; v++) {
			app_debug_open(app, "seek", 0);
			app_debug_open(app, "view", v);
			settle(app, 3); sleep_ms(400); settle(app, 3);
			for (int i = 0; i < 40; i++) {
				int16_t b[6000]; player_mix(b, 2940);
				app_force_redraw(app); app_update(app, 1 / 15.0f);
				char fn[64]; snprintf(fn, sizeof fn, "anim_%d_%03d", v, i); save_png(app, fn);
			}
		}
		app_debug_open(app, "view", 0);
	}
	if (getenv("BENCH")) {
		uint64_t t0 = now_ms();
		for (int i = 0; i < 100; i++) { app_force_redraw(app); app_update(app, 1 / 30.0f); }
		printf("BENCH cassette: %.2f ms/frame\n", (now_ms() - t0) / 100.0);
		app_debug_open(app, "view", 1); settle(app, 5); sleep_ms(300); settle(app, 5);
		t0 = now_ms();
		for (int i = 0; i < 100; i++) { app_force_redraw(app); app_update(app, 1 / 30.0f); }
		printf("BENCH cd: %.2f ms/frame\n", (now_ms() - t0) / 100.0);
		app_debug_open(app, "view", 0);
		app_debug_open(app, "tab", 1); settle(app, 5); sleep_ms(300); settle(app, 5);
		t0 = now_ms();
		for (int i = 0; i < 100; i++) { app_force_redraw(app); app_update(app, 1 / 30.0f); }
		printf("BENCH albums grid: %.2f ms/frame\n", (now_ms() - t0) / 100.0);
		app_debug_open(app, "player", 0);
	}
	app_debug_open(app, "seek", 9000);
	settle(app, 20);
	SHOT("06b_cassette_late");
	app_debug_open(app, "view", 1);
	settle(app, 5); sleep_ms(400); settle(app, 30);
	SHOT("07_cd");
	app_debug_open(app, "pause", 1);
	settle(app, 60);
	SHOT("07b_cd_paused");
	app_debug_open(app, "menu", 1);
	settle(app, 2);
	SHOT("08_menu");
	app_debug_open(app, "menu", 0);
	app_debug_open(app, "tab", 1);
	app_debug_open(app, "sel", 5);
	settle(app, 5); sleep_ms(300); settle(app, 5);
	SHOT("09_albums_mini");
	/* walk through inputs to exercise navigation */
	int seq[] = { BTN_DOWN, BTN_RIGHT, BTN_RIGHT, BTN_A, BTN_DOWN, BTN_DOWN, BTN_A, BTN_R1, BTN_R1, BTN_L1, BTN_Y, BTN_X, BTN_SELECT, BTN_B, BTN_B, BTN_B, BTN_R1, BTN_DOWN, BTN_A, BTN_B, BTN_R1, BTN_A, BTN_B, BTN_START, BTN_DOWN, BTN_A, BTN_B };
	for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) { app_input(app, seq[i], 1, 0); settle(app, 3); }
	SHOT("10_after_nav");
	app_save(app);
	app_destroy(app);

	if (!*prefix) {
		printf("--- decoders ---\n");
		for (int i = 6; i < argc; i++) decode_check(argv[i]);
	}
	return 0;
}
