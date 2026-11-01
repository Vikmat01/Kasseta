#ifndef UI_H
#define UI_H
#include "gfx.h"

enum {
	BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_A, BTN_B, BTN_X, BTN_Y,
	BTN_L1, BTN_R1, BTN_L2, BTN_R2,
	BTN_SELECT, BTN_START, BTN_MENU, BTN_POWER,
	BTN_COUNT
};

typedef struct App App;

typedef struct {
	int w, h;
	const char *resdir;   /* fonts */
	const char *datadir;  /* cache + state */
	const char *skipfile;
	const char *root;     /* music search root */
} AppConfig;

App *app_create(const AppConfig *cfg);
void app_input(App *a, int btn, int pressed, int repeat);
/* advance time; returns 1 if the canvas was redrawn and should be presented */
int app_update(App *a, float dt);
Canvas *app_canvas(App *a);
int app_wants_quit(App *a);
int app_animating(App *a);
int app_screen_off(App *a);
void app_save(App *a);
void app_destroy(App *a);

/* test hooks */
void app_debug_open(App *a, const char *what, int arg);
void app_force_redraw(App *a);

/* provided by the platform layer */
void plat_audio_lock(void);
void plat_audio_unlock(void);
void plat_set_screen(int on);
#endif
