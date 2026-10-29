/*
WIIU_PORT.H

The Wii U port's own units (port/wiiu/src): what they share.
*/

#ifndef __WIIU_PORT_H
#define __WIIU_PORT_H

#include <stdarg.h>

/* the port's log: sd:/wiiu/apps/halo/port.txt, the system's (OSReport:
Cemu's log, udplogserver on a console) */
void wiiu_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void wiiu_log_v(const char *format, va_list arguments);
void wiiu_log_flush(void);
/* the time spent writing the log's lines to the SD card since the start,
in OSTime ticks (the renderer's frame lines), and the share of it the
threads that logged waited for (none while the writer thread takes them) */
long long wiiu_log_write_ticks(void);
long long wiiu_log_wait_ticks(void);
/* the writer thread: started once ProcUI is up, stopped at the end's start
(wiiu_proc.c) */
void wiiu_log_start_writer(void);
void wiiu_log_stop_writer(void);

/* the log on the TV and the GamePad until the renderer starts
(wiiu_screen.c) */
void wiiu_screen_line(const char *text);
void wiiu_screen_update(void);
void wiiu_log_lock(void);
void wiiu_log_unlock(void);
void wiiu_screen_stop(void);
void wiiu_screen_release(void);
void wiiu_screen_acquire(void);
void wiiu_log_hold(int held);

/* the window and the heap, in the log (wiiu_memory.c) */
void wiiu_memory_report(void);

/* the game's folder on the SD card (wut's devoptab), where the package puts
halo.rpx and maps/ */
#define WIIU_APP_FOLDER "fs:/vol/external01/wiiu/apps/halo"

/* OpenGL's entry points, the renderer's (wiiu_gl.c): NULL for one there is
not */
void *wiiu_gl_proc_address(const char *name);

/* the mixer's output (wiiu_audio.c): fill is called with a buffer of
length bytes of float stereo at rate to fill, from the console's sound
thread */
typedef void (*wiiu_audio_fill)(void *context, void *buffer, int length);
int wiiu_audio_open(int rate, wiiu_audio_fill fill, void *context);
void wiiu_audio_resume(void);
void wiiu_audio_close(void);
/* AX itself, started with ProcUI (the Wii U Menu's music stops with it) and
stopped when the foreground is released and at the end */
void wiiu_audio_startup(void);
void wiiu_audio_shutdown(void);

/* the TV's and the GamePad's picture, through GX2 (wiiu_video.c): started
with the game's device, cleared every frame while the game draws nothing
(wiiu_video_idle), shut down at the end */
#define WIIU_SCREEN_WIDTH 1280
#define WIIU_SCREEN_HEIGHT 720
int wiiu_video_initialize(void);
void wiiu_video_swap(void);
void wiiu_video_idle(void);
void wiiu_video_shutdown(void);

/* the end, noted step by step: the port's log, then FSA directly once wut's
devoptab is down (wiiu_exit.c) */
void wiiu_exit_begin(void);
void wiiu_exit_phase(const char *text);
void wiiu_exit_threads(const char *when);

/* the system's messages (wiiu_proc.c): 0 once the game is to end */
int wiiu_proc_update(void);
void wiiu_proc_request_quit(void);
void wiiu_proc_poll(void);

/* the network (wiiu_net.c): up as Butter and Jelly brings it up, once;
nonzero if it is */
int wiiu_network_start(void);

/* the start-up's network self-test and the console game list, on a thread
of their own (wiiu_online.c, wiiu_game_list.h) */
void wiiu_online_start(void);

/* HTTPS (wiiu_https.c): the game's shared client set up (once the network
is up; any thread, once); the signed legacy table's cache read and the
game's network rules told of it (the game's start, once:
platform_sdl_initialize, platform/wiiu_platform.c); and the table fetched
again when it is due (at the first call, then every 4 hours, or 30 minutes
after a failure), blocking, on a thread with CONSOLE_HTTPS_STACK_SIZE and
DELTA_REFRESH_STACK_SIZE to spare */
void wiiu_https_start(void);
/* one plain HTTP GET of host's path on port 80, the whole response into
response (wiiu_online.c, as the plain list asks): 1 if one came. The HTTPS
client's plain_get, for the signed pin document */
int wiiu_online_plain_get(const char *host, const char *path, char *response, unsigned long size,
	unsigned long *received, unsigned long timeout_ms);
void wiiu_https_table_start(void);
void wiiu_https_table_refresh(void);

/* the controllers (wiiu_pad.c): the GamePad, then the Wii Remotes' Pro and
Classic Controllers, in the game's four ports, as the Xbox's gamepad state
(buttons as XINPUT_GAMEPAD's digital bits, analog buttons and sticks) */
#define WIIU_PAD_PORTS 4
struct wiiu_pad_state
{
	int connected;
	unsigned short digital;
	unsigned char analog[8];
	short thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
void wiiu_pad_update(void);
int wiiu_pad_read(int port, struct wiiu_pad_state *state);
void wiiu_pad_rumble(int port, int on);

#endif
