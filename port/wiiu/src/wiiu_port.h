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

/* the window and the heap, in the log (wiiu_memory.c) */
void wiiu_memory_report(void);

/* the TV's picture (wiiu_video.c) */
#define WIIU_SCREEN_WIDTH 1280
#define WIIU_SCREEN_HEIGHT 720
int wiiu_video_initialize(void);
void wiiu_video_swap(void);

/* the system's messages (wiiu_proc.c): 0 once the game is to end */
int wiiu_proc_update(void);
void wiiu_proc_request_quit(void);

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
