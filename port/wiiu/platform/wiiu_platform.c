/*
WIIU_PLATFORM.C

The Wii U's side of sdl_platform.h, in place of sdl_platform.c: the screen,
the system's events (ProcUI: the HOME button menu, leaving the game) and
what a desktop's window has that a console does not (a mouse, a keyboard, a
clipboard, links to open).

The screen is the TV's, 1280x720, drawn by the renderer
(port/wiiu/src/wiiu_video.c) once it is up; until it is, the game runs
without a picture, as the dedicated server does (d3d8_gl.c draws nothing).
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wiiu_port.h"

#ifndef HALO_VERSION
#define HALO_VERSION "dev"
#endif

/* ---------- the platform layer's start */

BOOL platform_sdl_initialize(void)
{
	return TRUE;
}

/* (no disc image to offer: the player's maps are on the SD card) */
BOOL platform_offer_game_data(const char *destination)
{
	(void)destination;
	return FALSE;
}

int halo_interpolation_enabled(void)
{
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.interpolation");
	}
	return enabled;
}

/* ---------- the screen */

BOOL platform_screen_mode(long *width, long *height)
{
	*width = WIIU_SCREEN_WIDTH;
	*height = WIIU_SCREEN_HEIGHT;
	return TRUE;
}

/* (Video Setup's lists: the TV's one mode) */
int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	if (maximum < 1)
		return 0;
	widths[0] = WIIU_SCREEN_WIDTH;
	heights[0] = WIIU_SCREEN_HEIGHT;
	return 1;
}

int platform_window_sizes(long *widths, long *heights, int maximum)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	return 0;
}

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
	(void)width;
	(void)height;
	return wiiu_video_initialize();
}

void platform_display_apply(void)
{
}

void platform_video_drawable_size(int *width, int *height)
{
	*width = WIIU_SCREEN_WIDTH;
	*height = WIIU_SCREEN_HEIGHT;
}

void platform_video_window_size(int *width, int *height)
{
	*width = WIIU_SCREEN_WIDTH;
	*height = WIIU_SCREEN_HEIGHT;
}

void platform_video_swap(void)
{
	wiiu_video_swap();
}

void platform_mouse_capture(BOOL capture)
{
	(void)capture;
}

/* ---------- no keyboard, mouse or clipboard */

BOOL platform_next_keystroke(struct platform_keystroke *keystroke)
{
	(void)keystroke;
	return FALSE;
}

int platform_clipboard_get(char *text, int size)
{
	if (size > 0)
		text[0] = 0;
	return 0;
}

void platform_clipboard_set(const char *text)
{
	(void)text;
}

int platform_take_clipboard_invite(char *text, int size)
{
	if (size > 0)
		text[0] = 0;
	return 0;
}

void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
}

void platform_open_url(const char *url)
{
	platform_log("a link to open (no browser here): %s", url);
}

void platform_request_quit(void)
{
	wiiu_proc_request_quit();
}

void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	(void)open;
	if (notches)
		*notches = 0;
	if (pages)
		*pages = 0;
}

void platform_menus_set_active(BOOL active)
{
	(void)active;
}

void platform_binding_capture_begin(void)
{
}

int platform_binding_capture_poll(int *input)
{
	(void)input;
	return 3;
}

void platform_ui_pointer_set_active(BOOL active)
{
	(void)active;
}

BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer)
{
	memset(pointer, 0, sizeof(*pointer));
	return FALSE;
}

void platform_test_pointer_event(float x, float y, int button, int wheel)
{
	(void)x;
	(void)y;
	(void)button;
	(void)wheel;
}

void platform_input_read(struct platform_input_state *state, BOOL consume_motion)
{
	(void)consume_motion;
	memset(state, 0, sizeof(*state));
	state->focused = TRUE;
}

/* ---------- events */

/* every frame (wiiu_input.c's XInputGetState): the system's messages; the
game ends here when the system asks it to (the HOME button menu's Close) */
void platform_pump_events(void)
{
	if (!wiiu_proc_update())
	{
		platform_log("the system asked the game to end");
		wiiu_log_flush();
		exit(EXIT_SUCCESS);
	}
	/* (GX2's screens, cleared while the game draws nothing: wiiu_video.c) */
	wiiu_video_idle();
}

/* ---------- the version (updater.c's: a console is updated with its files) */

void updater_start(void)
{
}

const char *updater_version(void)
{
	return HALO_VERSION;
}
