/*
WIIU_AUDIO.C

The mixer's output (port/linux/src/dsound_sdl.c's, through wiiu_sdl.c's
audio stream) on the console's AX voices. Not yet (port/wiiu/STATUS.md,
milestone 4): AX starts, but no device opens, and the mixer drains its voices in real time
without one, as on a desktop without sound.
*/

#include <sndcore2/core.h>

#include "wiiu_port.h"

/* AX is the application's to start: until it is, the Wii U Menu's own sound
keeps playing under the game. The 48 kHz renderer and the single-stage
pipeline, as wut's samples and SDL's Wii U audio do. It is let go when the
HOME button menu takes the foreground and started again on return. */
void wiiu_audio_startup(void)
{
	AXInitParams parameters = { AX_INIT_RENDERER_48KHZ, 0, AX_INIT_PIPELINE_SINGLE };

	if (AXIsInit())
		return;
	AXInitWithParams(&parameters);
	wiiu_log("audio: AX started (48 kHz)");
}

void wiiu_audio_shutdown(void)
{
	if (!AXIsInit())
		return;
	AXQuit();
	wiiu_log("audio: AX stopped");
}

int wiiu_audio_open(int rate, wiiu_audio_fill fill, void *context)
{
	(void)rate;
	(void)fill;
	(void)context;
	wiiu_log("audio: no sound yet (AX comes with milestone 4)");
	return 0;
}

void wiiu_audio_resume(void)
{
}

void wiiu_audio_close(void)
{
}
