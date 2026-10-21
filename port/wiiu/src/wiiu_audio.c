/*
WIIU_AUDIO.C

The mixer's output (port/linux/src/dsound_sdl.c's, through wiiu_sdl.c's
audio stream) on the console's AX voices. Not yet (port/wiiu/STATUS.md,
milestone 4): no device opens, and the mixer drains its voices in real time
without one, as on a desktop without sound.
*/

#include "wiiu_port.h"

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
