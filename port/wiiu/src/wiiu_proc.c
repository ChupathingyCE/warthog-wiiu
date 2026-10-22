/*
WIIU_PROC.C

The system's messages to the game (ProcUI): the HOME button menu, which
takes the foreground away and gives it back, and the end of the game, from
that menu's Close or the game's own Quit (which goes back to the Wii U
Menu, as the system asks of an application: it ends when ProcUI says so,
never by itself).
*/

#include <stdlib.h>

#include <coreinit/foreground.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

#include "wiiu_port.h"

static int proc_ready;
static int quit_requested;
static int proc_exiting;
static int proc_in_service;
static OSThread *proc_thread;
static OSTime proc_last_service;

/* The game's end, however it comes (its Quit, a failed assertion's halt):
an application ends when ProcUI says so, never by itself, or the console
waits on it forever. Asked for here if the system has not asked yet. */
static void proc_end(void)
{
	wiiu_log("system: the game ended");
	wiiu_log_flush();
	if (!proc_exiting)
	{
		if (!quit_requested)
			SYSLaunchMenu();
		for (;;)
		{
			ProcUIStatus status = ProcUIProcessMessages(TRUE);

			if (status == PROCUI_STATUS_EXITING)
				break;
			if (status == PROCUI_STATUS_RELEASE_FOREGROUND)
				ProcUIDrawDoneRelease();
		}
	}
	wiiu_audio_shutdown();
	ProcUIShutdown();
}

static void proc_initialize(void)
{
	if (proc_ready)
		return;
	ProcUIInit(&OSSavesDone_ReadyToRelease);
	/* (ProcUI is driven from this thread alone: its messages are taken here) */
	proc_thread = OSGetCurrentThread();
	/* (the HOME button opens its menu; the system's default, said outright) */
	OSEnableHomeButtonMenu(TRUE);
	/* (AX started, which silences the Wii U Menu's music) */
	wiiu_audio_startup();
	proc_ready = 1;
	atexit(proc_end);
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

/* The system's messages, taken until the game is in the foreground: the
HOME button menu releases it (AX with it, which the menu uses) and gives it
back. 0 once the game is to end. */
static int proc_service(void)
{
	for (;;)
	{
		ProcUIStatus status = ProcUIProcessMessages(TRUE);

		switch (status)
		{
		case PROCUI_STATUS_IN_FOREGROUND:
			wiiu_audio_startup();
			return 1;
		case PROCUI_STATUS_RELEASE_FOREGROUND:
			wiiu_log("system: the foreground released (HOME button menu)");
			wiiu_log_flush();
			wiiu_audio_shutdown();
			ProcUIDrawDoneRelease();
			break;
		case PROCUI_STATUS_IN_BACKGROUND:
			OSSleepTicks(OSMillisecondsToTicks(16));
			break;
		case PROCUI_STATUS_EXITING:
		default:
			proc_exiting = 1;
			return 0;
		}
	}
}

int wiiu_proc_update(void)
{
	int status;

	proc_initialize();
	wiiu_screen_update();
	proc_last_service = OSGetSystemTime();
	proc_in_service = 1;
	status = proc_service();
	proc_in_service = 0;
	return status;
}

/* The loading of the maps goes seconds without a frame, and ProcUI
(taken once a frame above) would hear nothing from the HOME button in that
time. The places a long stretch passes through often (the log, the files'
reads, the frame pacing) call this: it takes the messages, no more often than
every 16 ms, and only on the thread that started ProcUI (ProcUIProcessMessages
is that thread's; ProcUISubProcessMessages is for threads of another core,
and a callback would run on a thread that cannot release the foreground for
the game). */
void wiiu_proc_poll(void)
{
	OSTime now;

	if (!proc_ready || proc_in_service || proc_exiting || OSGetCurrentThread() != proc_thread)
		return;
	now = OSGetSystemTime();
	if (now - proc_last_service < OSMillisecondsToTicks(16))
		return;
	proc_last_service = now;
	proc_in_service = 1;
	/* (not blocking while in the foreground; the release and the wait in the
	background are the update's code) */
	switch (ProcUIProcessMessages(FALSE))
	{
	case PROCUI_STATUS_IN_FOREGROUND:
		break;
	case PROCUI_STATUS_RELEASE_FOREGROUND:
		wiiu_log("system: the foreground released (HOME button menu)");
		wiiu_log_flush();
		wiiu_audio_shutdown();
		ProcUIDrawDoneRelease();
		/* (waits here until the game has the foreground again, or is to end) */
		if (!proc_service())
		{
			wiiu_log("system: asked to end while loading");
			wiiu_log_flush();
			exit(EXIT_SUCCESS);
		}
		break;
	case PROCUI_STATUS_IN_BACKGROUND:
		proc_service();
		break;
	case PROCUI_STATUS_EXITING:
		wiiu_log("system: asked to end while loading");
		wiiu_log_flush();
		proc_exiting = 1;
		ProcUIShutdown();
		exit(EXIT_SUCCESS);
	}
	proc_in_service = 0;
}

/* (ProcUI wants to hear from the game from its first frame: started before
the game's own start, which takes seconds) */
__attribute__((constructor(102))) static void proc_attach(void)
{
	proc_initialize();
}
