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
static ProcUIStatus proc_status;

/* The game's end, however it comes (its Quit, a failed assertion's halt):
an application ends when ProcUI says so, never by itself, or the console
waits on it forever. The order is SDL's Wii U port: the audio stopped
first (SDL_AudioQuit), then the video's quit (WIIU_VideoQuit): the Wii U
Menu asked for if the system has not asked yet and the messages taken until
ProcUI says to exit, the screens let go, ProcUIShutdown last. Every step is
logged and flushed before the next, for the one that does not return. */
static void proc_end(void)
{
	wiiu_exit_begin();
	wiiu_exit_phase("proc_end: begun");
	wiiu_log("system: the game ended");
	wiiu_log_flush();
	wiiu_audio_shutdown();
	wiiu_log_flush();
	if (!proc_exiting)
	{
		wiiu_log("system: asking for the Wii U Menu");
		wiiu_log_flush();
		SYSLaunchMenu();
		for (;;)
		{
			ProcUIStatus status = ProcUIProcessMessages(TRUE);

			if (status == PROCUI_STATUS_EXITING)
				break;
			if (status == PROCUI_STATUS_RELEASE_FOREGROUND)
				ProcUIDrawDoneRelease();
		}
		proc_exiting = 1;
		wiiu_log("system: ProcUI says to exit");
		wiiu_log_flush();
	}
	wiiu_log_hold(0);
	wiiu_screen_stop();
	wiiu_video_shutdown();
	wiiu_log("system: the screens let go");
	wiiu_log_flush();
	wiiu_log("system: ProcUIShutdown next");
	wiiu_log_flush();
	ProcUIShutdown();
	wiiu_log("system: ProcUI shut down");
	wiiu_log_flush();
	wiiu_log("system: the game's end, to the system's exit");
	wiiu_log_flush();
	wiiu_exit_threads("threads at the end of proc_end:");
	wiiu_exit_phase("proc_end: done");
}

/* The foreground's resources, let go and taken again where ProcUI says so
(its callbacks run inside ProcUIProcessMessages, on this thread). The
audio is left alone, as the system's samples leave it: ProcUI suspends the
game's other threads once the release is done. */
static uint32_t proc_release(void *context)
{
	(void)context;
	wiiu_log("system: release 1/3, the foreground released (HOME button menu)");
	wiiu_log("system: release 2/3, the log held in memory");
	wiiu_log_hold(1);
	wiiu_screen_release();
	wiiu_log("system: release 3/3, drawing paused");
	return 0;
}

static uint32_t proc_acquire(void *context)
{
	(void)context;
	wiiu_log_hold(0);
	wiiu_log("system: acquire 1/2, the foreground is back");
	wiiu_screen_acquire();
	wiiu_log("system: acquire 2/2, the screens drawn at the next frame");
	return 0;
}

static uint32_t proc_exit_callback(void *context)
{
	(void)context;
	wiiu_log_hold(0);
	wiiu_log("system: exit asked for (Close in the HOME button menu)");
	wiiu_log_flush();
	return 0;
}

static void proc_initialize(void)
{
	if (proc_ready)
		return;
	ProcUIInit(&OSSavesDone_ReadyToRelease);
	/* (ProcUI is driven from this thread alone: its messages are taken here) */
	proc_thread = OSGetCurrentThread();
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, proc_release, NULL, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, proc_acquire, NULL, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_EXIT, proc_exit_callback, NULL, 100);
	/* (the HOME button opens its menu; the system's default, said outright) */
	OSEnableHomeButtonMenu(TRUE);
	/* (AX started, which silences the Wii U Menu's music) */
	wiiu_audio_startup();
	/* (the first poll a frame's time from now: never inside the
	constructors, where ProcUI's messages hung the start on the console) */
	proc_last_service = OSGetSystemTime();
	proc_ready = 1;
	atexit(proc_end);
	wiiu_log("system: ProcUI ready");
}

void wiiu_proc_request_quit(void)
{
	/* (the end is taken at the next frame's update, on the thread that
	owns ProcUI, which asks the system for the Wii U Menu as SDL's quit
	does: proc_end) */
	if (!quit_requested)
	{
		quit_requested = 1;
		wiiu_log("system: quitting to the Wii U Menu");
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
			return 1;
		case PROCUI_STATUS_RELEASE_FOREGROUND:
			ProcUIDrawDoneRelease();
			break;
		case PROCUI_STATUS_IN_BACKGROUND:
			OSSleepTicks(OSMillisecondsToTicks(16));
			break;
		case PROCUI_STATUS_EXITING:
		default:
			wiiu_log("system: ProcUI says to exit");
			proc_exiting = 1;
			return 0;
		}
	}
}

int wiiu_proc_update(void)
{
	int status;

	proc_initialize();
	if (quit_requested)
		return 0;
	wiiu_screen_update();
	proc_last_service = OSGetSystemTime();
	proc_in_service = 1;
	status = proc_service();
	proc_in_service = 0;
	return status;
}

/* The loading of the maps goes seconds without a frame, and ProcUI
(taken once a frame above) would hear nothing from the HOME button in that
time. The places a long stretch passes through often (the files' reads,
the frame pacing) call this, but not the log, which the constructors use
before the game starts: it takes the messages, no more often than
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
	/* (TRUE, as the system's own samples ask: it returns at once while the
	game is in the foreground, and the first call with FALSE never returned
	on the console; the release and the wait in the background are the
	update's code) */
	{
		static int traced;
		ProcUIStatus status;

		if (traced < 3)
			wiiu_log("system: messages taken (%d)", traced);
		status = ProcUIProcessMessages(TRUE);
		if (traced < 3)
		{
			traced++;
			wiiu_log("system: messages answered %d", (int)status);
		}
		proc_status = status;
	}
	switch (proc_status)
	{
	case PROCUI_STATUS_IN_FOREGROUND:
		break;
	case PROCUI_STATUS_RELEASE_FOREGROUND:
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
