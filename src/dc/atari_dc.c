/*
 * SEGA Dreamcast support using KallistiOS (http://cadcdev.sourceforge.net)
 * (c) 2002-2015 Christian Groessler (chris@groessler.org)
 */

#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <kos.h>
#include <sys/types.h>
#include "atari.h"
#include "config.h"
#include "sio.h"
#include "binload.h"
#include "afile.h"
#include "input.h"
#include "akey.h"
#include "colours.h"
#include "colours_ntsc.h"
#include "colours_pal.h"
#include "colours_external.h"
#include "ui.h"
#include "ui_basic.h"
#include "util.h"
#include "time.h"
#include "screen.h"
#include <dc/g2bus.h>
#include <arm/aica_cmd_iface.h>
#include <dc/sound/sound.h>
#include "statesav.h"
#include "sound.h"
#include "pokeysnd.h"
#include "version.h"
#ifdef STORE_QUEUE
#include <dc/sq.h>
#endif

#define REPEAT_DELAY 100  /* in ms */
#define REPEAT_INI_DELAY (5 * REPEAT_DELAY)
/*#define DEBUG*/

#define OVR_DELAY 5  /* # of frames that an overridden console key appears to be pressed */
#define MAX_CONTROLLERS 4  /* # of controllers we support */

void PLATFORM_DisplayScreen(void);
int PLATFORM_Configure(char *, char *);
int stat(const char *, struct stat *);
static void calc_palette(void);
static int check_tray_open(void);
static void autostart(void);
static void controller_update(void);
/*static void dc_atari_sync(void);*/
static void dc_sound_init(void);
static void dc_snd_stream_stop(void);

static unsigned char *atari_screen_backup;
static unsigned char *atari_screen_backup2;
static maple_device_t *mcont_dev[MAX_CONTROLLERS];
static cont_state_t *mcont_state[MAX_CONTROLLERS];
static cont_state_t mcont_state_disconnected;
static maple_device_t *mkeyb_dev;
static int num_cont;   /* # of controllers found */
static int su_first_call = TRUE;
static int b_ui_leave = FALSE;
static int open_tray = FALSE;
static int tray_closed = TRUE;
static int reverse_x_axis = FALSE, reverse_y_axis = FALSE;
static unsigned int cont_dpad_up = CONT_DPAD_UP;
static unsigned int cont_dpad_down = CONT_DPAD_DOWN;
static unsigned int cont_dpad_left = CONT_DPAD_LEFT;
static unsigned int cont_dpad_right = CONT_DPAD_RIGHT;

#ifndef ASSEMBLER_SCREENUPDATE
static
#endif
	int vid_mode_width;

#ifndef ASSEMBLER_SCREENUPDATE
static
#endif
	uint16 *screen_vram;

int x_adj, y_adj;  /* screen position adjustment */
int emulate_paddles = FALSE;
int db_mode = FALSE;
int screen_tv_mode;    /* mode of connected tv set */

int x_ovr = FALSE, y_ovr = FALSE, b_ovr = FALSE;
int x_key = AKEY_NONE, y_key = AKEY_NONE, b_key = AKEY_NONE;
int x_ovr_delay, y_ovr_delay, b_ovr_delay;
int disable_js = FALSE, disable_dpad = FALSE;
int ovr_inject_key = AKEY_NONE;
int DC_in_kbui;

extern char curr_disk_dir[];
extern char curr_cart_dir[];
extern char curr_exe_dir[];
extern char curr_tape_dir[];

static vid_mode_t mymode_pal = {
	DM_320x240,
	320, 240,
	VID_PIXELDOUBLE|VID_LINEDOUBLE|VID_PAL,
	CT_ANY,
	PM_RGB565,
	313, 857,
	0xAE, 0x2D,
	0x15, 312,
	141, 843,
	24, 313*2,
	0, 1,
	{ 0, 0, 0, 0 }
};
static vid_mode_t mymode_ntsc = {
	DM_320x240,
	320, 240,			      /* width, height in pixels */
	VID_PIXELDOUBLE|VID_LINEDOUBLE,	      /* flags */
	CT_ANY,				      /* cable type */
	PM_RGB565,			      /* pixel mode */
	262, 857,			      /* scanlines/frame, clocks/scanline */
	0xAE, 0x10,			      /* bitmap window position x, y */
	0x15, 261,			      /* scanline interrupt positions */
	141, 843,			      /* border x start + stop */
	24, 263*2,			      /* border y start + stop */
	0, 1,
	{ 0, 0, 0, 0 }
};

void update_vidmode(void)
{
	vid_mode_t mymode;

	/* we need to copy it since vid_set_mode_ex() modifies it */
	switch(screen_tv_mode) {
	case Atari800_TV_NTSC:
		memcpy(&mymode, &mymode_ntsc, sizeof(vid_mode_t));
		break;
	case Atari800_TV_PAL:
	default:
		memcpy(&mymode, &mymode_pal, sizeof(vid_mode_t));
		break;
	}
	mymode.cable_type = vid_check_cable();
	mymode.bitmapx += x_adj;
	mymode.bitmapy += y_adj;
	vid_set_mode_ex(&mymode);

	vid_mode_width = vid_mode->width;

	screen_vram = (uint16*)0xA5000000;
	/* point to current line of atari screen in display memory */
	screen_vram += (vid_mode->height - Screen_HEIGHT) / 2 * vid_mode_width;
	/* adjust left column to screen resolution */
	screen_vram += (vid_mode_width - 320) / 2 - (Screen_WIDTH - 320) / 2 ;

#ifdef STORE_QUEUE
	QACR0 = QACR1 = (unsigned int)screen_vram >> 24;
#endif
}

#ifndef ASSEMBLER_SCREENUPDATE
static
#endif
	uint16 mypal[256]
#ifdef USE_OCRAM
			   __attribute__((section (".ocram")))
#endif
	;

static void calc_palette(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		mypal[i] = ((Colours_GetR(i) >> 3) << 11) |
			((Colours_GetG(i) >> 2) << 5) |
			(Colours_GetB(i) >> 3);
	}
}

#ifndef ASSEMBLER_SCREENUPDATE
static
#endif
void vbl_wait(void)
{
	static int last_cntr = 0;

	while (maple_state.vbl_cntr == last_cntr) ;
	last_cntr = maple_state.vbl_cntr;
}

#define START_VAL ((Screen_WIDTH - 320) / 2)  /* shortcut, used in screen update routines */

#ifdef ASSEMBLER_SCREENUPDATE
extern void PLATFORM_DisplayScreen_doubleb(UBYTE *screen);
#else
static void PLATFORM_DisplayScreen_doubleb(UBYTE *screen)
{
	static unsigned int sbase = 0;		/* screen base of current frame */
	unsigned int x, y;
	uint16 *vram;
#ifdef STORE_QUEUE
	uint32 *vram_sq;
#endif

#ifdef SPEED_CHECK
	vid_border_color(0, 0, 0);
#endif
	if (sbase) {
		sbase = 0;
		vram = screen_vram + START_VAL;
	}
	else {
		sbase = 1024 * 768 * 4;
		vram = screen_vram + START_VAL + sbase / 2;	 /* "/ 2" since screen_vram is (uint16 *) */
	}
	screen += START_VAL;

#ifndef STORE_QUEUE
	for (y = 0; y < Screen_HEIGHT; y++) {
		for (x = 0; x < 320; x++)
			*(vram + x) = mypal[*(screen + x)];
		screen += Screen_WIDTH;
		vram += vid_mode_width;
	}
#else
	vram_sq = (uint32 *)(void *)(0xe0000000 | (((unsigned long)vram) & 0x03ffffe0));
	for (y = 0; y < Screen_HEIGHT; y++) {
		for (x = 0; x < 320; x += 16) {
			int i;
			for (i = 0; i < 8; i++) {
				uint32 val = mypal[*(screen + x + 2 * i)] & 0xffff;
				val |= mypal[*(screen + x + 2 * i + 1)] << 16;
				*(vram_sq + x / 2 + i) = val;
			}
			__asm__("pref @%0" : : "r"(vram_sq + x / 2));
		}
		screen += Screen_WIDTH;
		vram_sq += vid_mode_width / 2;
	}
#endif

#ifdef SPEED_CHECK
	vid_border_color(127, 127, 127);
#endif
	vbl_wait();
	vid_set_start(sbase);
#ifdef SPEED_CHECK
	vid_border_color(255, 255, 255);
#endif
}
#endif /* not defined ASSEMBLER_SCREENUPDATE */

#ifdef ASSEMBLER_SCREENUPDATE
extern void PLATFORM_DisplayScreen_singleb(UBYTE *screen);
#else
static void PLATFORM_DisplayScreen_singleb(UBYTE *screen)
{
	unsigned int x, y;
	uint16 *vram = screen_vram + START_VAL;;
#ifdef STORE_QUEUE
	uint32 *vram_sq;
#endif

#ifdef SPEED_CHECK
	vid_border_color(0, 0, 0);
#endif
	screen += START_VAL;

#ifndef STORE_QUEUE
	for (y = 0; y < Screen_HEIGHT; y++) {
		for (x = 0; x < 320; x++)
			*(vram + x) = mypal[*(screen + x)];
		screen += Screen_WIDTH;
		vram += vid_mode_width;
	}
#else
	vram_sq = (uint32 *)(void *)(0xe0000000 | (((unsigned long)vram) & 0x03ffffe0));
	for (y = 0; y < Screen_HEIGHT; y++) {
		for (x = 0; x < 320; x += 16) {
			int i;
			for (i = 0; i < 8; i++) {
				uint32 val = mypal[*(screen + x + 2 * i)] & 0xffff;
				val |= mypal[*(screen + x + 2 * i + 1)] << 16;
				*(vram_sq + x / 2 + i) = val;
			}
			__asm__("pref @%0" : : "r"(vram_sq + x / 2));
		}
		screen += Screen_WIDTH;
		vram_sq += vid_mode_width / 2;
	}
#endif

#ifdef SPEED_CHECK
	vid_border_color(127, 127, 127);
#endif
	vbl_wait();
#ifdef SPEED_CHECK
	vid_border_color(255, 255, 255);
#endif
}
#endif /* not defined ASSEMBLER_SCREENUPDATE */

static void (*screen_updater)(UBYTE *screen);

void PLATFORM_DisplayScreen(void)
{
	screen_updater((UBYTE *)Screen_atari);
}

void update_screen_updater(void)
{
	if (db_mode) {
		screen_updater = PLATFORM_DisplayScreen_doubleb;
	}
	else {
		screen_updater = PLATFORM_DisplayScreen_singleb;
	}
}

int PLATFORM_Initialise(int *argc, char *argv[])
{
	static int fc = TRUE;  /* "first call" */
	(void)argc; /* remove unused parameter warning */
	(void)argv; /*   "  "  "  "  "  "  "  "  "  "  */

	if (! fc) return TRUE; else fc = FALSE;

	screen_tv_mode = Atari800_tv_mode;

#ifndef DEBUG
	/* Bother us with output only if something died */
	dbglog_set_level(DBG_DEAD);
#endif

	/* Set the video mode */
	update_vidmode();
	calc_palette();

	return TRUE;
}

int PLATFORM_Exit(int run_monitor)
{
	(void)run_monitor; /* remove unused parameter warning */
	arch_reboot();
	return(0);  /* not reached */
}

int PLATFORM_Configure(char *option, char *parameters)
{
	int ret, val;

	if (strcmp(option, "DISPLAY_X_ADJUST") == 0) {
		ret = Util_sscansdec(parameters, &val);
		if (ret) {
			x_adj = val;
			update_vidmode();
			update_screen_updater();
		}
		return ret;
	}
	else if (strcmp(option, "DISPLAY_Y_ADJUST") == 0) {
		ret = Util_sscansdec(parameters, &val);
		if (ret) {
			y_adj = val;
			update_vidmode();
			update_screen_updater();
		}
		return ret;
	}
	else if (strcmp(option, "DOUBLE_BUFFERING") == 0) {
		db_mode = Util_sscanbool(parameters);
		update_screen_updater();
		return TRUE;
	}
	return FALSE;
}

/*
 * update num_cont, mkeyb_dev, and mcont_dev[]
 */
static void dc_controller_init(void)
{
	int i, n = maple_enum_count();
	maple_device_t *dev;

	/* start with no controllers and no keyboard */
	mkeyb_dev = NULL;
	num_cont = 0;

	for (i = 0; i < n && num_cont < MAX_CONTROLLERS; i++) {
		if ((dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER)))
			mcont_dev[num_cont++] = dev;
		if (!mkeyb_dev && (dev = maple_enum_type(i, MAPLE_FUNC_KEYBOARD)))
			mkeyb_dev = dev;
	}
	return;
}

/*
 * update the in-core status of the controller buttons/settings
 */
static void controller_update(void)
{
	int i;

	dc_controller_init();  /* update dis-/reconnections */
	for (i = 0; i < num_cont; i++) {
		if (! (mcont_state[i] = (cont_state_t *)maple_dev_status(mcont_dev[i]))) {
#ifdef DEBUG
			printf("controller_update: error getting controller status\n");
#endif
			/* controller has been disconnected in the meanwhile: add a dummy entry */
			mcont_state[i] = &mcont_state_disconnected;
		}
	}
}

static int consol_keys(void)
{
	cont_state_t *state = mcont_state[0];

	if (! num_cont) return(AKEY_NONE);  /* no controller present */

	if (state->buttons & CONT_START) {
		if (UI_is_active)
			arch_reboot();
		else {
			if (state->buttons & CONT_X)
				return(AKEY_COLDSTART);
			else
				return(AKEY_WARMSTART);
		}
	}

	if (Atari800_machine_type != Atari800_MACHINE_5200) {
		if (b_ovr && !UI_is_active && !DC_in_kbui && !b_ui_leave) {
			/* !UI_is_active and !b_ui_leave because B is also the esc key
			   (with rtrig) to leave the ui */
			if (b_key != AKEY_NONE && (state->buttons & CONT_B))
				ovr_inject_key = b_key;
		}
		else {
			if (state->buttons & CONT_B) {
				if (!b_ui_leave && !DC_in_kbui && !UI_is_active) {
					INPUT_key_consol &= ~INPUT_CONSOL_START;
				}
				else {
					INPUT_key_consol |= INPUT_CONSOL_START;
				}
			}
			else {
				b_ui_leave = FALSE;
				INPUT_key_consol |= INPUT_CONSOL_START;
			}
		}

		if (y_ovr) {
			if (y_key != AKEY_NONE && (state->buttons & CONT_Y))
				ovr_inject_key = y_key;
		}
		else {
			if (state->buttons & CONT_Y) {
				INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
			}
			else {
				INPUT_key_consol |= INPUT_CONSOL_SELECT;
			}
		}

		if (x_ovr) {
			if (x_key != AKEY_NONE && (state->buttons & CONT_X))
				ovr_inject_key = x_key;
		}
		else {
			if (state->buttons & CONT_X) {
				INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
			}
			else {
				INPUT_key_consol |= INPUT_CONSOL_OPTION;
			}
		}
	}
	else {
		/* @@@ *_ovr TODO @@@ */

		if (state->buttons & CONT_X) {	/* 2nd action button */
			INPUT_key_shift = 1;
		}
		else {
			INPUT_key_shift = 0;
		}
		if (! UI_is_active) {
			if (state->buttons & CONT_B) {	/* testtest */
				if (! b_ui_leave) {
					return(AKEY_4);	  /* ??? at least on the dc kb "4" starts some games?? */
				}
			}
			else {
				b_ui_leave = FALSE;
			}
			if (state->buttons & CONT_Y) {	/* testtest */
				return(AKEY_5200_START);
			}
		}
	}
	return(AKEY_NONE);
}

int get_emkey(char *title)
{
	int keycode;

	controller_update();
	DC_in_kbui = TRUE;
	memcpy(atari_screen_backup, Screen_atari, Screen_HEIGHT * Screen_WIDTH);
	keycode = UI_BASIC_OnScreenKeyboard(title, -1);
	memcpy(Screen_atari, atari_screen_backup, Screen_HEIGHT * Screen_WIDTH);
	Screen_EntireDirty();
	PLATFORM_DisplayScreen();
	DC_in_kbui = FALSE;
	return keycode;
}

/*
 * milliseconds since boot
 */
static uint64_t timer_get(void)
{
	uint32_t secs, msecs;
	timer_ms_gettime(&secs, &msecs);
	return (uint64_t)secs * 1000 + msecs;

}

/*
 * do some basic keyboard emulation for the controller,
 * so that the emulator menu can be used without a keyboard
 * (basically for selecting bin files/disk images)
 */
static int controller_kb(void)
{
	static int prev_up = FALSE, prev_down = FALSE, prev_a = FALSE,
		prev_r = FALSE, prev_left = FALSE, prev_right = FALSE,
		prev_b = FALSE, prev_l = FALSE;
	static uint64_t repdelay_timeout;
	cont_state_t *state = mcont_state[0];
	int keycode;

	if (! num_cont) return(AKEY_NONE);  /* no controller present */

	if (!UI_is_active && (state->ltrig > 250 || (state->buttons & CONT_Z))) {
		return(AKEY_UI);
	}
#ifdef USE_UI_BASIC_ONSCREEN_KEYBOARD
	if (!UI_is_active && (state->rtrig > 250 || (state->buttons & CONT_C))) {
		controller_update();
		return(AKEY_KEYB);  /* enter keyboard emulation screen */
	}
	/* provide keyboard emulation to enter file name */
	if (UI_is_active && !DC_in_kbui && (state->rtrig > 250 || (state->buttons & CONT_C))) {
		controller_update();
		DC_in_kbui = TRUE;
		memcpy(atari_screen_backup, Screen_atari, Screen_HEIGHT * Screen_WIDTH);
		keycode = UI_BASIC_OnScreenKeyboard(NULL, -1);
		memcpy(Screen_atari, atari_screen_backup, Screen_HEIGHT * Screen_WIDTH);
		Screen_EntireDirty();
		PLATFORM_DisplayScreen();
		DC_in_kbui = FALSE;
		return keycode;
	}
#endif  /* #ifdef USE_UI_BASIC_ONSCREEN_KEYBOARD */

	if (UI_is_active || DC_in_kbui) {
		if ((state->buttons & cont_dpad_up)) {
			prev_down = FALSE;
			if (! prev_up) {
				repdelay_timeout = timer_get() + REPEAT_INI_DELAY;
				prev_up = 1;
				return(AKEY_UP);
			}
			else {
				if (timer_get() > repdelay_timeout) {
					repdelay_timeout = timer_get() + REPEAT_DELAY;
					return(AKEY_UP);
				}
			}
		}
		else {
			prev_up = FALSE;
		}

		if ((state->buttons & cont_dpad_down)) {
			prev_up = FALSE;
			if (! prev_down) {
				repdelay_timeout = timer_get() + REPEAT_INI_DELAY;
				prev_down = TRUE;
				return(AKEY_DOWN);
			}
			else {
				if (timer_get() > repdelay_timeout) {
					repdelay_timeout = timer_get() + REPEAT_DELAY;
					return(AKEY_DOWN);
				}
			}
		}
		else {
			prev_down = FALSE;
		}

		if ((state->buttons & cont_dpad_left)) {
			prev_right = FALSE;
			if (! prev_left) {
				repdelay_timeout = timer_get() + REPEAT_INI_DELAY;
				prev_left = TRUE;
				return(AKEY_LEFT);
			}
			else {
				if (timer_get() > repdelay_timeout) {
					repdelay_timeout = timer_get() + REPEAT_DELAY;
					return(AKEY_LEFT);
				}
			}
		}
		else {
			prev_left = FALSE;
		}

		if ((state->buttons & cont_dpad_right)) {
			prev_left = FALSE;
			if (! prev_right) {
				repdelay_timeout = timer_get() + REPEAT_INI_DELAY;
				prev_right = TRUE;
				return(AKEY_RIGHT);
			}
			else {
				if (timer_get() > repdelay_timeout) {
					repdelay_timeout = timer_get() + REPEAT_DELAY;
					return(AKEY_RIGHT);
				}
			}
		}
		else {
			prev_right = FALSE;
		}

		if ((state->buttons & CONT_A)) {
			if (! prev_a) {
				prev_a = TRUE;
				return(AKEY_RETURN);
			}
		}
		else {
			prev_a = FALSE;
		}

		if ((state->buttons & CONT_B)) {
			if (! prev_b) {
				prev_b = TRUE;
				b_ui_leave = TRUE;   /* B must be released again */
				return(AKEY_ESCAPE);
			}
		}
		else {
			prev_b = FALSE;
		}

		if (state->ltrig > 250 || (state->buttons & CONT_Z)) {
			if (! prev_l && DC_in_kbui) {
				prev_l = TRUE;
				return(AKEY_ESCAPE);
			}
		}
		else {
			prev_l = FALSE;
		}

		if (state->rtrig > 250 || (state->buttons & CONT_C)) {
			if (! prev_r) {
				prev_r = TRUE;
				return(AKEY_ESCAPE);
			}
		}
		else {
			prev_r = FALSE;
		}
	}
	return(AKEY_NONE);
}

int PLATFORM_Keyboard(void)
{
	static int old_open_tray = FALSE;
	int open_tray;
	int keycode;
	int i;

	/* let's do this here... */
	open_tray = check_tray_open();
	if (open_tray != old_open_tray) {
		old_open_tray = open_tray;
		if (open_tray == TRUE) {
#ifdef DEBUG
			printf("XXX TRAY OPEN!\n");
#endif
			tray_closed = FALSE;
			return(AKEY_UI);
		}
		else {
#ifdef DEBUG
			printf("XXX TRAY CLOSE!\n");
#endif
			tray_closed = TRUE;
			cdrom_init();
			iso_reset();
			chdir("/");
			for (i=0; i<UI_n_atari_files_dir; i++)
				strcpy(UI_atari_files_dir[i], "/");
		}
	}

	if (UI_is_active || DC_in_kbui) controller_update();

	if (num_cont && (keycode = consol_keys()) != AKEY_NONE) return(keycode);

#ifdef USE_UI_BASIC_ONSCREEN_KEYBOARD
	if (ovr_inject_key != AKEY_NONE) {
		keycode = ovr_inject_key;
		ovr_inject_key = AKEY_NONE;
		switch(keycode) {
		case AKEY_OPTION:
			INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
			keycode = AKEY_NONE;
			break;
		case AKEY_SELECT:
			INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
			printf("XXX SELECT \n");
			keycode = AKEY_NONE;
			break;
		case AKEY_START:
			INPUT_key_consol &= ~INPUT_CONSOL_START;
			keycode = AKEY_NONE;
			break;
		}
		return(keycode);
	}
#else
#error check me!
#endif  /* #ifdef USE_UI_BASIC_ONSCREEN_KEYBOARD */

	keycode = controller_kb();
	if (keycode != AKEY_NONE) return(keycode);

	if (mkeyb_dev)
		keycode = kbd_queue_pop(mkeyb_dev, 1);
	else
		keycode = -1;
	if (keycode == -1) return(AKEY_NONE);

#ifdef DEBUG
	printf("DC key: %04X (%c)\n", keycode, keycode > 32 && keycode < 127 ? keycode : '.');
#endif

	switch (keycode) {

	/* OPTION / SELECT / START keys */
	/*INPUT_key_consol = INPUT_CONSOL_NONE;  -- already set in consol_keys... */
	case 0x3b00:
		INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
		return(AKEY_NONE);
	case 0x3c00:
		INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
		return(AKEY_NONE);
	case 0x3d00:
		INPUT_key_consol &= ~INPUT_CONSOL_START;
		return(AKEY_NONE);

	case 0x1b:  /* ESC */
		keycode = AKEY_ESCAPE;
		break;
	case 9:	 /* TAB */
		keycode = AKEY_TAB;
		break;
	case '`':
		keycode = AKEY_CAPSTOGGLE;
		break;
	case '!':
		keycode = AKEY_EXCLAMATION;
		break;
	case '"':
		keycode = AKEY_DBLQUOTE;
		break;
	case '#':
		keycode = AKEY_HASH;
		break;
	case '$':
		keycode = AKEY_DOLLAR;
		break;
	case '%':
		keycode = AKEY_PERCENT;
		break;
	case '&':
		keycode = AKEY_AMPERSAND;
		break;
	case '\'':
		keycode = AKEY_QUOTE;
		break;
	case '@':
		keycode = AKEY_AT;
		break;
	case '(':
		keycode = AKEY_PARENLEFT;
		break;
	case ')':
		keycode = AKEY_PARENRIGHT;
		break;
	case '[':
		keycode = AKEY_BRACKETLEFT;
		break;
	case ']':
		keycode = AKEY_BRACKETRIGHT;
		break;
	case '<':
		keycode = AKEY_LESS;
		break;
	case '>':
		keycode = AKEY_GREATER;
		break;
	case '=':
		keycode = AKEY_EQUAL;
		break;
	case '?':
		keycode = AKEY_QUESTION;
		break;
	case '-':
		keycode = AKEY_MINUS;
		break;
	case '+':
		keycode = AKEY_PLUS;
		break;
	case '*':
		keycode = AKEY_ASTERISK;
		break;
	case '/':
		keycode = AKEY_SLASH;
		break;
	case ':':
		keycode = AKEY_COLON;
		break;
	case ';':
		keycode = AKEY_SEMICOLON;
		break;
	case ',':
		keycode = AKEY_COMMA;
		break;
	case '.':
		keycode = AKEY_FULLSTOP;
		break;
	case '_':
		keycode = AKEY_UNDERSCORE;
		break;
	case '^':
		keycode = AKEY_CIRCUMFLEX;
		break;
	case '\\':
		keycode = AKEY_BACKSLASH;
		break;
	case '|':
		keycode = AKEY_BAR;
		break;
	case ' ':
		keycode = AKEY_SPACE;
		break;
	case '0':
		keycode = AKEY_0;
		break;
	case '1':
		keycode = AKEY_1;
		break;
	case '2':
		keycode = AKEY_2;
		break;
	case '3':
		keycode = AKEY_3;
		break;
	case '4':
		keycode = AKEY_4;
		break;
	case '5':
		keycode = AKEY_5;
		break;
	case '6':
		keycode = AKEY_6;
		break;
	case '7':
		keycode = AKEY_7;
		break;
	case '8':
		keycode = AKEY_8;
		break;
	case '9':
		keycode = AKEY_9;
		break;
	case 'a':
		keycode = AKEY_a;
		break;
	case 'b':
		keycode = AKEY_b;
		break;
	case 'c':
		keycode = AKEY_c;
		break;
	case 'd':
		keycode = AKEY_d;
		break;
	case 'e':
		keycode = AKEY_e;
		break;
	case 'f':
		keycode = AKEY_f;
		break;
	case 'g':
		keycode = AKEY_g;
		break;
	case 'h':
		keycode = AKEY_h;
		break;
	case 'i':
		keycode = AKEY_i;
		break;
	case 'j':
		keycode = AKEY_j;
		break;
	case 'k':
		keycode = AKEY_k;
		break;
	case 'l':
		keycode = AKEY_l;
		break;
	case 'm':
		keycode = AKEY_m;
		break;
	case 'n':
		keycode = AKEY_n;
		break;
	case 'o':
		keycode = AKEY_o;
		break;
	case 'p':
		keycode = AKEY_p;
		break;
	case 'q':
		keycode = AKEY_q;
		break;
	case 'r':
		keycode = AKEY_r;
		break;
	case 's':
		keycode = AKEY_s;
		break;
	case 't':
		keycode = AKEY_t;
		break;
	case 'u':
		keycode = AKEY_u;
		break;
	case 'v':
		keycode = AKEY_v;
		break;
	case 'w':
		keycode = AKEY_w;
		break;
	case 'x':
		keycode = AKEY_x;
		break;
	case 'y':
		keycode = AKEY_y;
		break;
	case 'z':
		keycode = AKEY_z;
		break;
	case 'A':
		keycode = AKEY_A;
		break;
	case 'B':
		keycode = AKEY_B;
		break;
	case 'C':
		keycode = AKEY_C;
		break;
	case 'D':
		keycode = AKEY_D;
		break;
	case 'E':
		keycode = AKEY_E;
		break;
	case 'F':
		keycode = AKEY_F;
		break;
	case 'G':
		keycode = AKEY_G;
		break;
	case 'H':
		keycode = AKEY_H;
		break;
	case 'I':
		keycode = AKEY_I;
		break;
	case 'J':
		keycode = AKEY_J;
		break;
	case 'K':
		keycode = AKEY_K;
		break;
	case 'L':
		keycode = AKEY_L;
		break;
	case 'M':
		keycode = AKEY_M;
		break;
	case 'N':
		keycode = AKEY_N;
		break;
	case 'O':
		keycode = AKEY_O;
		break;
	case 'P':
		keycode = AKEY_P;
		break;
	case 'Q':
		keycode = AKEY_Q;
		break;
	case 'R':
		keycode = AKEY_R;
		break;
	case 'S':
		keycode = AKEY_S;
		break;
	case 'T':
		keycode = AKEY_T;
		break;
	case 'U':
		keycode = AKEY_U;
		break;
	case 'V':
		keycode = AKEY_V;
		break;
	case 'W':
		keycode = AKEY_W;
		break;
	case 'X':
		keycode = AKEY_X;
		break;
	case 'Y':
		keycode = AKEY_Y;
		break;
	case 'Z':
		keycode = AKEY_Z;
		break;

	case 0x3a00:  /* F1 */
		keycode = AKEY_UI;
		break;

	case 0x3e00:  /* F5 */
		keycode = AKEY_COLDSTART;
		break;

	case 0x4500:  /* F12 */
		arch_reboot();

	/* cursor keys */
	case 0x5200:
		keycode = AKEY_UP;
		break;
	case 0x5100:
		keycode = AKEY_DOWN;
		break;
	case 0x5000:
		keycode = AKEY_LEFT;
		break;
	case 0x4f00:
		keycode = AKEY_RIGHT;
		break;

	case 0x6500:  /* S3 */
		keycode = AKEY_ATARI;
		break;

	case 0x4a00:  /* Home */
		keycode = AKEY_CLEAR;
		break;
	case 0x4d00:  /* End */
		keycode = AKEY_HELP;
		break;
	case 0x4800:  /* Break/Pause */
		keycode = AKEY_BREAK;
		break;

	case 0x4c00:  /* Del */
		if (INPUT_key_shift)
			keycode = AKEY_DELETE_LINE;
		else
			keycode |= AKEY_DELETE_CHAR;
		break;
	case 0x4900:  /* Ins */
		if (INPUT_key_shift)
			keycode = AKEY_INSERT_LINE;
		else
			keycode |= AKEY_INSERT_CHAR;
		break;

	case 127:
	case 8:
		keycode = AKEY_BACKSPACE;
		break;
	case 13:
	case 10:
		keycode = AKEY_RETURN;
		break;
	default:
		keycode = AKEY_NONE;
		break;
	}
	return keycode;
}

int PLATFORM_PORT(int num)
{
	cont_state_t *state;
	int retval = 0xff;
	int cix = 0;

	if (num) {
		if (Atari800_machine_type != Atari800_MACHINE_800) return(retval);
		cix = 2;   /* js #2 and #3 */
	}

	if (num_cont < 1 + cix) return(retval);	 /* no controller present */

	state = mcont_state[cix];
	if (! emulate_paddles) {
		if (! disable_dpad) {
			if (state->buttons & cont_dpad_up) {
				retval &= 0xfe;
			}
			if (state->buttons & cont_dpad_down) {
				retval &= 0xfd;
			}
			if (state->buttons & cont_dpad_left) {
				retval &= 0xfb;
			}
			if (state->buttons & cont_dpad_right) {
				retval &= 0xf7;
			}
		}
		/* if joypad not used, try the joystick instead */
		if (retval == 0xff && !disable_js) {
			if (reverse_x_axis) {
				if (state->joyx < -10) {
					retval &= 0xf7;
				}
				if (state->joyx > 10) {
					retval &= 0xfb;
				}
			}
			else {
				if (state->joyx > 10) {  /* right */
					retval &= 0xf7;
				}
				if (state->joyx < -10) {  /* left */
					retval &= 0xfb;
				}
			}
			if (reverse_y_axis) {
				if (state->joyy < -10) {
					retval &= 0xfd;
				}
				if (state->joyy > 10) {
					retval &= 0xfe;
				}
			}
			else {
				if (state->joyy > 10) {  /* down */
					retval &= 0xfd;
				}
				if (state->joyy < -10) {  /* up */
					retval &= 0xfe;
				}
			}
		}
		if (num_cont > 1 + cix) {
			state = mcont_state[cix+1];
			if (! disable_dpad) {
				if (state->buttons & cont_dpad_up) {
					retval &= 0xef;
				}
				if (state->buttons & cont_dpad_down) {
					retval &= 0xdf;
				}
				if (state->buttons & cont_dpad_left) {
					retval &= 0xbf;
				}
				if (state->buttons & cont_dpad_right) {
					retval &= 0x7f;
				}
			}
			/* if joypad not used, try the joystick instead */
			if ((retval & 0xf0) == 0xf0 && !disable_js) {
				if (reverse_x_axis) {
					if (state->joyx < -10) {
						retval &= 0x7f;
					}
					if (state->joyx > 10) {
						retval &= 0xbf;
					}
				}
				else {
					if (state->joyx > 10) {  /* right */
						retval &= 0x7f;
					}
					if (state->joyx < -10) {  /* left */
						retval &= 0xbf;
					}
				}
				if (reverse_y_axis) {
					if (state->joyy < -10) {
						retval &= 0xdf;
					}
					if (state->joyy > 10) {
						retval &= 0xef;
					}
				}
				else {
					if (state->joyy > 10) {  /* down */
						retval &= 0xdf;
					}
					if (state->joyy < -10) {  /* up */
						retval &= 0xef;
					}
				}
			}
		}
	}
	else {	/* emulate paddles */
		/* 1st paddle trigger */
		if (state->buttons & CONT_A) {
			retval &= (cix > 1) ? 0xbf : 0xfb;
		}
		if (num_cont > 1 + cix) {
			state = mcont_state[cix+1];

			/* 2nd paddle trigger */
			if (state->buttons & CONT_A) {
				retval &= (cix > 1) ? 0x7f : 0xf7;
			}
		}
	}

	return(retval);
}

int Atari_POT(int num)
{
	int val;
	cont_state_t *state;

	if (Atari800_machine_type != Atari800_MACHINE_5200) {
		if (emulate_paddles && !disable_js) {
			if (num + 1 > num_cont) return(228);
			if (Atari800_machine_type != Atari800_MACHINE_800 && num > 3) return(228);

			state = mcont_state[num];
			val = state->joyx;
			if (reverse_x_axis) val = -val;
			val = val * 228 / 255;
			val = 114 + val;
			return(228 - val);
		}
		else {
			return(228);
		}
	}
	else {	/* 5200 version:
		 *
		 * num / 2: which controller
		 * num & 1 == 0: x axis
		 * num & 1 == 1: y axis
		 */

		if (num / 2 + 1 > num_cont || disable_js) return(INPUT_joy_5200_center);

		state = mcont_state[num / 2];
		if (num & 1) {
			if (reverse_y_axis)
				val = -state->joyy;
			else
				val = state->joyy;
		}
		else {
			if (reverse_x_axis)
				val = -state->joyx;
			else
				val = state->joyx;
		}

		/* normalize into 5200 range */
		if (! val) return(INPUT_joy_5200_center);
		if (val < 0) {
			/*val -= joy_5200_min;*/
			val = val * (INPUT_joy_5200_center - INPUT_joy_5200_min) / 127; // @@@ new kos values
			return(val + INPUT_joy_5200_min);
		}
		else {
			val = val * INPUT_joy_5200_max / 255;
			if (val < INPUT_joy_5200_center)
				val = INPUT_joy_5200_center;
			return(val);
		}
	}
}

int PLATFORM_TRIG(int num)
{
	cont_state_t *state;

	if (num + 1 > num_cont) return(1);  /* no controller present */
	if (Atari800_machine_type != Atari800_MACHINE_800 && num > 1) return(1);

	state = mcont_state[num];
	if (state->buttons & CONT_A) {
		return(0);
	}

	return(1);
}

void do_reverse_x_axis(void)
{
	if (reverse_x_axis) {
		cont_dpad_left = CONT_DPAD_RIGHT;
		cont_dpad_right = CONT_DPAD_LEFT;
	}
	else {
		cont_dpad_left = CONT_DPAD_LEFT;
		cont_dpad_right = CONT_DPAD_RIGHT;
	}
}

void do_reverse_y_axis(void)
{
	if (reverse_y_axis) {
		cont_dpad_up = CONT_DPAD_DOWN;
		cont_dpad_down = CONT_DPAD_UP;
	}
	else {
		cont_dpad_up = CONT_DPAD_UP;
		cont_dpad_down = CONT_DPAD_DOWN;
	}
}

#if 0
/*
 * fill up unused (no entry in atari800.cfg) saved_files_dir
 * entries with the available VMUs
 */
static void setup_saved_files_dirs(void)
{
	int p, u;

	/* loop over VMUs */
	for (p=0; p<MAPLE_PORT_COUNT; p++) {
		for (u=0; u<MAPLE_UNIT_COUNT; u++) {
			if (maple_device_func(p, u) & MAPLE_FUNC_MEMCARD) {
				if (n_saved_files_dir < MAX_DIRECTORIES) {
					sprintf(saved_files_dir[n_saved_files_dir], "/vmu/%c%c",
						'a' + p, '0' + u);
					n_saved_files_dir++;
				}
				else
					return;
			}
		}
	}
}
#endif

#ifdef HZ_TEST
static void cls(int xres, int yres)
{
	int x, y;
	for(x = 0; x < xres; x++)
		for(y = 0; y < yres; y++)
			vram_s[xres*y + x] = 0;
}

static void wait_a(void)
{
	cont_state_t *state;

	while (!mcont_dev[0])
		dc_controller_init();

	do {
		if (!(state = (cont_state_t *)maple_dev_status(mcont_dev[0]))) {
			return;
		}

	} while (state && !(state->buttons & CONT_A));
}

void do_hz_test(void)
{
	uint32 s, ms, s2, ms2, z;
	char buffer[80];

	cls(320,240);
	bfont_draw_str(vram_s+64*320+5, 320, 0, "chk dsply hz.. 500 vbls");
	timer_ms_gettime(&s, &ms);
	for (z=0; z<500; z++) {
		vbl_wait();
	}
	timer_ms_gettime(&s2, &ms2);
	sprintf(buffer, "start time: %lu.%03lu\n", s, ms);
	bfont_draw_str(vram_s+89*320+5, 320, 0, buffer);
	sprintf(buffer, "end   time: %lu.%03lu\n", s2, ms2);
	bfont_draw_str(vram_s+114*320+5, 320, 0, buffer);
	sprintf(buffer, "diff(ms) = %ld\n", s2*1000 + ms2 - s * 1000 - ms);
	bfont_draw_str(vram_s+139*320+5, 320, 0, buffer);
	sprintf(buffer, "hz = %f\n", 500.0 / ((s2*1000 + ms2 - s * 1000 - ms) / 1000.0));
	bfont_draw_str(vram_s+164*320+5, 320, 0, buffer);
	wait_a();
	cls(320,240);
}
#endif /* ifdef HZ_TEST */

KOS_INIT_FLAGS(INIT_IRQ | INIT_THD_PREEMPT
#ifdef USE_OCRAM
 | INIT_OCRAM
#endif
);
#ifdef ROMDISK
extern uint8 romdisk[];
KOS_INIT_ROMDISK(romdisk);
#endif

#ifndef DEBUG
int scif_dbgio_enabled = 1;  /* 1 = disable serial debug output */
#endif

void dc_init_serial(void)
{
	dbgio_disable();
	scif_set_parameters(9600, 1);
	scif_init();
	scif_set_irq_usage(1);
}

void dc_set_baud(int baud)
{
	scif_set_parameters(baud, 1);
	scif_init();
	scif_set_irq_usage(1);
#ifdef DEBUG
	printf("setting baud rate: %d\n", baud);
#endif
}

int dc_write_serial(unsigned char byte)
{
	if (scif_write_buffer(&byte, 1, 0) == 1)
		return 1;
	return 0;
}

int dc_read_serial(unsigned char *byte)
{
	int c = scif_read();
	if (c == -1) return 0;
	*byte = c;
	return 1;
}

int main(int argc, char **argv)
{
	/* initialize screen updater */
	update_screen_updater();

	/* initialize Atari800 core */
	Atari800_Initialise(&argc, argv);

	/* initialize dc controllers for the first time */
	controller_update();

	/* initialize sound */
	dc_sound_init();

	/*
	ser_console_init();
	dbgio_init();
	*/

#ifdef DEBUG
	printf("\nFor Tobias & Dominik\n");
	printf("--------------------\n");
#endif

#ifdef HZ_TEST
	do_hz_test();
#endif

#if 0
	gdb_init();
#endif
#if 0
	asm("mov #7,r12");
	asm("trapa #0x20");
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	//gdb_breakpoint();
#endif

	atari_screen_backup = malloc(Screen_HEIGHT * Screen_WIDTH);
	atari_screen_backup2 = malloc(Screen_HEIGHT * Screen_WIDTH);

	chdir("/");   /* initialize cwd in dc_chdir.c */
	autostart();


#ifdef DEBUG
	/* print configuration settings */
	printf("frame buffer address: %p\n", screen_vram);
	printf("palette address: %p\n", mypal);
#ifdef ASSEMBLER_SCREENUPDATE
	printf("ASSEMBLER: ON\n");
#else
	printf("ASSEMBLER: OFF\n");
#endif
	if (db_mode)
		printf("DOUBLE BUFFERING: ON\n");
	else
		printf("DOUBLE BUFFERING: OFF\n");
#ifdef DIRTYRECT
	printf("DIRTYRECT: ON\n");
#else
	printf("DIRTYRECT: OFF\n");
#endif
#ifdef STORE_QUEUE
	printf("STORE_QUEUE: ON\n");
#else
	printf("STORE_QUEUE: OFF\n");
#endif
#ifdef USE_OCRAM
	printf("OCRAM: ON\n");
#else
	printf("OCRAM: OFF\n");
#endif
#endif


	/* main loop */
	while(TRUE)
	{
		INPUT_key_code = PLATFORM_Keyboard();

		switch (INPUT_key_code) {
		case AKEY_5200_RESET:
			if (Atari800_machine_type == Atari800_MACHINE_5200) {
				Atari800_Coldstart();
			}
			break;
#ifdef USE_UI_BASIC_ONSCREEN_KEYBOARD
		case AKEY_KEYB:
			Sound_Pause();
			DC_in_kbui = TRUE;
			INPUT_key_code = UI_BASIC_OnScreenKeyboard(NULL, Atari800_machine_type);
			DC_in_kbui = FALSE;
			switch (INPUT_key_code) {
				case AKEY_OPTION:
					INPUT_key_consol &= ~INPUT_CONSOL_OPTION;
					x_ovr_delay = OVR_DELAY;
					break;
				case AKEY_SELECT:
					INPUT_key_consol &= ~INPUT_CONSOL_SELECT;
					y_ovr_delay = OVR_DELAY;
					break;
				case AKEY_START:
					b_ui_leave = TRUE;
					INPUT_key_consol &= ~INPUT_CONSOL_START;
					b_ovr_delay = OVR_DELAY;
					break;
			}
			Sound_Continue();
			break;
#endif /* #ifdef USE_UI_BASIC_ONSCREEN_KEYBOARD */
		}

		Atari800_Frame();
		PLATFORM_DisplayScreen();
		controller_update();  /* get new values from the controllers */

		/* if overrides are in place, inject the console keys' releases */
		if (b_ovr && !(INPUT_key_consol & INPUT_CONSOL_START) && !b_ovr_delay--)
			INPUT_key_consol |= INPUT_CONSOL_START;
		if (x_ovr && !(INPUT_key_consol & INPUT_CONSOL_OPTION) && !x_ovr_delay--)
			INPUT_key_consol |= INPUT_CONSOL_OPTION;
		if (y_ovr && !(INPUT_key_consol & INPUT_CONSOL_SELECT) && !y_ovr_delay--)
			INPUT_key_consol |= INPUT_CONSOL_SELECT;
	}
}

/*
 * autostart: check for autorun.xxx files on the
 * CD root directory and start the first one found
 */
static void autostart(void)
{
	struct stat sb;

	if (! stat("/cd/autorun.com", &sb)) {
		if (S_ISREG(sb.st_mode)) {
			if (BINLOAD_Loader("/cd/autorun.com")) {
				Atari800_Coldstart();
				return;
			}
		}
	}
	if (! stat("/cd/autorun.exe", &sb)) {
		if (S_ISREG(sb.st_mode)) {
			if (BINLOAD_Loader("/cd/autorun.exe")) {
				Atari800_Coldstart();
				return;
			}
		}
	}
	if (! stat("/cd/autorun.atr", &sb)) {
		if (S_ISREG(sb.st_mode)) {
			SIO_Mount(1, "/cd/autorun.atr", TRUE);
			return;
		}
	}
}

#if 0  /* not stable, KOS crashes sometimes with files on the ramdisk */
static char mytmpnam[] = "/ram/tmpf";
char *tmpnam(char *space)
{
	static int inc = 0;
	char b[16];
	if (space) {
		strcpy(space, mytmpnam);
		sprintf(b, ".%d\n", inc++);
		strcat(space, b);
		return(space);
	}
	else {
		return(mytmpnam);
	}
}
char *_tmpnam_r (struct reent *r, char *s)
{
	return tmpnam(s);
}
#endif

/* parts taken from KOS' kernel/libc/koslib/opendir.c */
static int odc;
DIR *opendir(const char *name)
{
	file_t handle;
	DIR *newd;
#ifdef DEBUG
	printf("opendir...(%s)\n", name);
#endif
	if (open_tray) return(NULL);
	/*if (tray_closed)*/ tray_closed = FALSE;

	if (! strcmp(name, "/cd")) {
		/* hack */
		cdrom_init();
		iso_reset();
	}
	if (strcmp(name, "/")
	    && strcmp(name, "/cd") && strcmp(name, "/pc") && strcmp(name, "/ram")
	    && strcmp(name, "/vmu") && strcmp(name, "/pty") && strcmp(name, "/rd")
	    && strncmp(name, "/cd/", 4) && strncmp(name, "/pc/", 4) && strncmp(name, "/ram/", 5)
	    && strncmp(name, "/vmu/", 5) && strncmp(name, "/pty/", 5) && strncmp(name, "/rd/", 4)) {
#ifdef DEBUG
		printf("opendir: punt!\n");
#endif
		return(NULL);
	}
	handle = fs_open(name, O_DIR | O_RDONLY);
	if (handle < 0) return(NULL);

	newd = malloc(sizeof(DIR));
	if (!newd) {
		errno = ENOMEM;
		return(NULL);
	}

	newd->fd = handle;
	memset(&newd->d_ent, 0, sizeof(struct dirent));

	if (strcmp(name, "/") && strcmp(name, "/pc")	/* no ".." in the root directory */
	    && strncmp(name, "/pc/", 4))		/* and in /pc and subdirectories */
		odc = 1;				/* (/pc provides ".." by itself) */
	return(newd);
}

static struct dirent myd;

/* parts taken from KOS' kernel/libc/koslib/readdir.c */
struct dirent *readdir(DIR *dir)
{
	dirent_t *d;

	if (open_tray) return(NULL);
	if (tray_closed) {
		tray_closed = FALSE;
		return(NULL);
	}
	if (odc) {
		odc = 0;
		memset(&myd, 0, sizeof(struct dirent));
		strcpy(myd.d_name, "..");
		myd.d_type = 4;	// DT_DIR
		return(&myd);
	}

	if (!dir) {
		errno = EBADF;
		return(NULL);
	}
	d = fs_readdir(dir->fd);
	if (!d) return(NULL);

	dir->d_ent.d_ino = 0;
	dir->d_ent.d_off = 0;
	dir->d_ent.d_reclen = 0;
	if (d->size < 0)
		dir->d_ent.d_type = 4;	// DT_DIR
	else
		dir->d_ent.d_type = 8;	// DT_REG
	strncpy(dir->d_ent.d_name, d->name, 255);

	return(&dir->d_ent);
}

/* our super-duper stat */
/* only check, whether the S_IFDIR bit has to be set */
int stat(const char *path, struct stat *sb)
{
	file_t handle;

	memset(sb, 0, sizeof(struct stat));  /* preinitialize result */

	if (open_tray) return(-1);
	if (tray_closed) tray_closed = FALSE;

	/* special ".." case */
	if (strlen(path) > 3 &&
	    ! strcmp(path + strlen(path) - 3, "/..")) {
		sb->st_mode = _IFDIR;
		return(0);  /* success */
	}

	/* check if dir */
	if ((handle = fs_open(path, O_DIR | O_RDONLY)) != -1) {	/* is dir */
		sb->st_mode = _IFDIR;
	}
	else if ((handle = fs_open(path, O_RDONLY)) != -1) {	/* is file */
		sb->st_mode = _IFREG;
	}
	else {	/* is not here */
#ifdef DEBUG
		printf("stat: error with '%s'\n", path);
#endif
		return(-1);
	}
	fs_close(handle);
	return(0);  /* success */
}

#if 0
static void dc_atari_sync(void)
{
	static unsigned long long nextclock = 0;
	unsigned long long curclock;
	uint32 s, ms;

	do {
		timer_ms_gettime(&s, &ms);
		curclock = ((unsigned long long)s << 32) + ms;
	} while (curclock < nextclock);

	nextclock = curclock + CLK_TCK / ((Atari800_tv_mode == Atari800_TV_PAL ? 50 : 60));
}
#endif

static int check_tray_open(void)
{
	int status, disk_type, retval;

	retval = cdrom_get_status(&status, &disk_type);
	if (retval == ERR_OK) {
		if ((status & 15) == 6) return(TRUE);
		return(FALSE);
	}
	else {
		return(FALSE); /* error case: assume tray not open */
	}
}

void DCStateSave(void)
{
	unsigned int i;
	int f = 0;

	StateSav_SaveINT(&screen_tv_mode, 1);
	StateSav_SaveINT(&Atari800_tv_mode, 1);
	StateSav_SaveINT(&x_ovr, 1);
	StateSav_SaveINT(&x_key, 1);
	StateSav_SaveINT(&y_ovr, 1);
	StateSav_SaveINT(&y_key, 1);
	StateSav_SaveINT(&b_ovr, 1);
	StateSav_SaveINT(&b_key, 1);
	StateSav_SaveINT(&emulate_paddles, 1);
	StateSav_SaveINT(&db_mode, 1);
	StateSav_SaveINT(&INPUT_joy_autofire[0], 1);
	StateSav_SaveINT(&disable_js, 1);
	StateSav_SaveINT(&disable_dpad, 1);
	StateSav_SaveINT(&reverse_x_axis, 1);
	StateSav_SaveINT(&reverse_y_axis, 1);
	StateSav_SaveINT(&x_adj, 1);
	StateSav_SaveINT(&y_adj, 1);
	for (i=0; i<16; i++) StateSav_SaveINT(&f, 1);  /* future stuff */

#ifdef DEBUG
	printf("DCStateSave: tv_mode = %d, screen_tv_mode = %d, db_mode = %d\n",
	       Atari800_tv_mode, screen_tv_mode, db_mode);
#endif
}

void DCStateRead(void)
{
	StateSav_ReadINT(&screen_tv_mode, 1);
	StateSav_ReadINT(&Atari800_tv_mode, 1);
	StateSav_ReadINT(&x_ovr, 1);
	StateSav_ReadINT(&x_key, 1);
	StateSav_ReadINT(&y_ovr, 1);
	StateSav_ReadINT(&y_key, 1);
	StateSav_ReadINT(&b_ovr, 1);
	StateSav_ReadINT(&b_key, 1);
	StateSav_ReadINT(&emulate_paddles, 1);
	StateSav_ReadINT(&db_mode, 1);
	StateSav_ReadINT(&INPUT_joy_autofire[0], 1);
	StateSav_ReadINT(&disable_js, 1);
	StateSav_ReadINT(&disable_dpad, 1);
	StateSav_ReadINT(&reverse_x_axis, 1);
	StateSav_ReadINT(&reverse_y_axis, 1);
	StateSav_ReadINT(&x_adj, 1);
	StateSav_ReadINT(&y_adj, 1);

#ifdef DEBUG
	printf("DCStateRead: tv_mode = %d, screen_tv_mode = %d, db_mode = %d\n",
	       Atari800_tv_mode, screen_tv_mode, db_mode);
#endif

	/* sanity check */
	if (Atari800_tv_mode != screen_tv_mode) db_mode = FALSE;

	update_vidmode();
	update_screen_updater();
}

void AboutAtariDC(void)
{
	UI_driver->fInfoScreen("About AtariDC",
			       "AtariDC v" A800DCVERASC " ("__DATE__")\0"
			       "(c) 2002-2015 Christian Groessler\0"
			       "http://www.groessler.org/a800dc\0"
			       "\0"
			       "Please report all problems\0"
			       "to chris@groessler.org\0"
			       "\0"
			       "This port is based on\0"
			       Atari800_TITLE "\0"
			       "http://atari800.atari.org\0"
			       "\0"
			       "It uses the KallistiOS library\0"
			       "http://cadcdev.sourceforge.net\0"
			       "\0"
#if A800DCBETA
			       "THIS IS A *BETA* VERSION!\0"
			       "PLEASE  DO NOT DISTRIBUTE\0\0"
#endif
			       "Dedicated to Tobias & Dominik\0\n");
}


static void ovr2str(int keycode, char *keystr)
{
	sprintf(keystr, "\"%c\" ($%02X)", UI_BASIC_key_to_ascii[keycode], keycode);
}

/* "Button configuration" submenu of "Controller Configuration" */
void ButtonConfiguration(void)
{
	char keystr[3][10];
	int option = 0;
	static UI_tMenuItem menu_array[] = {
		UI_MENU_ACTION(0, "Override X key: "),
		UI_MENU_ACTION(1, "Override Y key: "),
		UI_MENU_ACTION(2, "Override B key: "),
		UI_MENU_END
	};


	do {
		if (x_ovr) {
			ovr2str(x_key, keystr[0]);
			menu_array[0].suffix = keystr[0];
		}
		else {
			menu_array[0].suffix = "OFF      ";
		}

		if (y_ovr) {
			ovr2str(y_key, keystr[1]);
			menu_array[1].suffix = keystr[1];
		}
		else {
			menu_array[1].suffix = "OFF      ";
		}

		if (b_ovr) {
			ovr2str(b_key, keystr[2]);
			menu_array[2].suffix = keystr[2];
		}
		else {
			menu_array[2].suffix = "OFF      ";
		}

		option = UI_driver->fSelect(NULL, TRUE, option, menu_array, NULL);

		switch(option) {
		case 0:
			if (x_ovr) x_ovr = FALSE;
			else {
				x_key = get_emkey("Select X button definition");
				if (x_key != AKEY_NONE) {
					x_ovr = TRUE;
				}
				else {
					x_ovr = FALSE;
				}
			}
			break;
		case 1:
			if (y_ovr) y_ovr = FALSE;
			else {
				y_key = get_emkey("Select Y button definition");
				if (y_key != AKEY_NONE) {
					y_ovr = TRUE;
				}
				else {
					y_ovr = FALSE;
				}
			}
			break;
		case 2:
			if (b_ovr) b_ovr = FALSE;
			else {
				b_key = get_emkey("Select B button definition");
				if (b_key != AKEY_NONE) {
					b_ovr = TRUE;
				}
				else {
					b_ovr = FALSE;
				}
			}
			break;
		}
	} while (option >= 0);
}

/* "Joystick/D-Pad configuration" submenu of "Controller Configuration" */
void JoystickConfiguration(void)
{
	int option = 0;
	static UI_tMenuItem menu_array[] = {
		UI_MENU_ACTION(0, "Disable Joystick: "),
		UI_MENU_ACTION(1, "Disable D-Pad:"),
		UI_MENU_ACTION(2, "Reverse X axis:"),
		UI_MENU_ACTION(3, "Reverse Y axis:"),
		UI_MENU_END
	};


	do {
		if (disable_js)
			menu_array[0].suffix = "ON ";
		else
			menu_array[0].suffix = "OFF";

		if (disable_dpad)
			menu_array[1].suffix = "ON ";
		else
			menu_array[1].suffix = "OFF";

		if (reverse_x_axis)
			menu_array[2].suffix = "ON ";
		else
			menu_array[2].suffix = "OFF";

		if (reverse_y_axis)
			menu_array[3].suffix = "ON ";
		else
			menu_array[3].suffix = "OFF";

		option = UI_driver->fSelect(NULL, TRUE, option, menu_array, NULL);

		switch(option) {
		case 0:
			disable_js = !disable_js;
			break;
		case 1:
			disable_dpad = !disable_dpad;
			break;
		case 2:
			reverse_x_axis = !reverse_x_axis;
			do_reverse_x_axis();
			break;
		case 3:
			reverse_y_axis = !reverse_y_axis;
			do_reverse_y_axis();
			break;
		}
#if 0
		if (disable_js && disable_dpad)	 /* don't allow both to be disabled */
			disable_js = disable_dpad = FALSE;
#endif
	} while (option >= 0);
}


static int sound_enabled = TRUE;	/* sound: on or off */

#define DSPRATE 22050

void dc_sound_init(void)
{
	snd_init();
#ifdef STEREO
	POKEYSND_Init(POKEYSND_FREQ_17_EXACT, DSPRATE, 2, 0);
#else
	POKEYSND_Init(POKEYSND_FREQ_17_EXACT, DSPRATE, 1, 0);
#endif
}

void Sound_Pause(void)
{
	if (sound_enabled) {
		sound_enabled = FALSE;
		dc_snd_stream_stop();
	}
}

void Sound_Continue(void)
{
	if (! sound_enabled && Sound_enabled) {
		sound_enabled = TRUE;
		su_first_call = TRUE;
	}
}

void Sound_Exit(void)
{
#ifdef DEBUG
	printf("Sound_Exit called\n");
#endif
}

/* taken from KOS' snd_stream.c */
#define SPU_RAM_BASE		0xa0800000

#define FRAG_SIZE    0x80      /* size of one fragment */
#define FRAG_NUM     16	       /* max. # of fragments in the buffer */
#define FRAG_BUFSZ   (FRAG_NUM * FRAG_SIZE)

static unsigned char fragbuf[FRAG_BUFSZ]; /* scratch buffer to generate sound data for a fragment */
static uint32 spu_ram_sch1 = 0;

/* ripped from KOS */
static void dc_snd_stream_stop(void)
{
	AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

	/* Stop stream */
	/* Channel 0 */
	cmd->cmd = AICA_CMD_CHAN;
	cmd->timestamp = 0;
	cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
	cmd->cmd_id = 0;
	chan->cmd = AICA_CH_CMD_STOP;
	snd_sh4_to_aica(tmp, cmd->size);
}

/* ripped from KOS */
static inline unsigned int aica_getpos(int chan)
{
	return(g2_read_32(SPU_RAM_BASE + AICA_CHANNEL(chan) + offsetof(aica_channel_t, pos)));
}

/* some parts taken from KOS' snd_stream.c */
void Sound_Update(void)
{
	static int last_frag;
	int cur_pos, cur_frag, fill_frag, first_frag_to_fill, last_frag_to_fill;
	int n, k;

	if (! sound_enabled) return;

	if (su_first_call) {
		AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

		su_first_call = FALSE;

		if (! spu_ram_sch1)
			spu_ram_sch1 = snd_mem_malloc(FRAG_BUFSZ);

		/* prefill buffers */
		POKEYSND_Process(fragbuf, FRAG_BUFSZ);
		spu_memload(spu_ram_sch1, fragbuf, FRAG_SIZE);

		last_frag = 0;

#ifdef STEREO
#error STEREO not implemented!
#endif
		/* start streaming */
		/* use channel 0 */
		cmd->cmd = AICA_CMD_CHAN;
		cmd->timestamp = 0;
		cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
		cmd->cmd_id = 0;
		chan->cmd = AICA_CH_CMD_START;
		chan->base = spu_ram_sch1;
		chan->type = AICA_SM_8BIT;
		chan->length = FRAG_BUFSZ;
		chan->loop = 1;
		chan->loopstart = 0;
		chan->loopend = FRAG_BUFSZ - 1;
		chan->freq = DSPRATE;
		chan->vol = 240;
		chan->pan = 127;
		snd_sh4_to_aica(tmp, cmd->size);
	}

	/* get current playing position */
	cur_pos = aica_getpos(0);
	cur_frag = cur_pos / FRAG_SIZE;

#if 0 /* new */
	if (cur_frag > last_frag) {
		n = cur_frag - last_frag - 1;
		Pokey_process(fragbuf, n * FRAG_SIZE);
		spu_memload(spu_ram_sch1 + FRAG_SIZE * (last_frag + 1),
			    fragbuf, FRAG_SIZE * n);
	}
	else {
		n = cur_frag - last_frag - 1 + FRAG_NUM;
		Pokey_process(fragbuf, n * FRAG_SIZE);
		k = FRAG_NUM - last_frag - 1;
		if (k) {
			spu_memload(spu_ram_sch1 + FRAG_SIZE * (last_frag + 1),
				    fragbuf, FRAG_SIZE * k);
		}
		n = n - k;
		if (n) {
			spu_memload(spu_ram_sch1,
				    fragbuf + FRAG_SIZE * k, FRAG_SIZE * n);
		}
	}
	last_frag = cur_frag - 1;
	if (last_frag < 0) last_frag = 0;
#else
	/* calc # of new fragments needed to fill sound buffer */
	first_frag_to_fill = last_frag + 1;
	first_frag_to_fill %= FRAG_NUM;
	last_frag_to_fill = cur_frag;

	fill_frag = first_frag_to_fill;

	n = last_frag_to_fill - first_frag_to_fill;
	if (n < 0) n += FRAG_NUM;
	POKEYSND_Process(fragbuf, n * FRAG_SIZE);
	k = 0;
	while (fill_frag != last_frag_to_fill) {
		spu_memload(spu_ram_sch1 + FRAG_SIZE * fill_frag, fragbuf + k * FRAG_SIZE, FRAG_SIZE);
		last_frag = fill_frag;
		fill_frag++;
		fill_frag %= FRAG_NUM;
		k++;
	}
#endif

#if 0  /* some arm debug stuff */
	printf("cur_pos was %x (%x) (%x)\n", cur_pos,
	       g2_read_32(SPU_RAM_BASE + 0x1f800),
	       g2_read_32(SPU_RAM_BASE + 0x1f804));
#endif
}
/* eval: (c-set-offset 'case-label '+) */
