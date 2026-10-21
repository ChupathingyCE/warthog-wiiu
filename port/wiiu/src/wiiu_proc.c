/*
WIIU_PROC.C

The system's messages to the game (ProcUI): the HOME button menu, which
takes the foreground away and gives it back, and the end of the game, from
that menu's Close or the game's own Quit (which goes back to the Wii U
Menu, as the system asks of an application: it ends when ProcUI says so,
never by itself).
*/

#include <coreinit/foreground.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

#include "wiiu_port.h"

static int proc_ready;
static int quit_requested;

static void proc_initialize(void)
{
	if (proc_ready)
		return;
	ProcUIInit(&OSSavesDone_ReadyToRelease);
	proc_ready = 1;
	wiiu_log("system: ProcUI ready");
}

void wiiu_proc_request_quit(void)
{
	if (!quit_requested)
	{
		quit_requested = 1;
		wiiu_log("system: quitting to the Wii U Menu");
		SYSLaunchMenu();
	}
}

int wiiu_proc_update(void)
{
	proc_initialize();
	for (;;)
	{
		ProcUIStatus status = ProcUIProcessMessages(TRUE);

		switch (status)
		{
		case PROCUI_STATUS_IN_FOREGROUND:
			return 1;
		case PROCUI_STATUS_RELEASE_FOREGROUND:
			wiiu_log("system: the foreground released (HOME button menu)");
			wiiu_log_flush();
			ProcUIDrawDoneRelease();
			break;
		case PROCUI_STATUS_IN_BACKGROUND:
			OSSleepTicks(OSMillisecondsToTicks(16));
			break;
		case PROCUI_STATUS_EXITING:
		default:
			ProcUIShutdown();
			return 0;
		}
	}
}

/* (ProcUI wants to hear from the game from its first frame: started before
the game's own start, which takes seconds) */
__attribute__((constructor(102))) static void proc_attach(void)
{
	proc_initialize();
}
