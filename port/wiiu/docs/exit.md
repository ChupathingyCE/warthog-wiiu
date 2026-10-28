# The game's end on the Wii U

What happens when the game ends (its Quit, or Close in the HOME button
menu), what the references say about the threads that are still running
then, and what the port does about it.

## On the console

After Close, port.txt shows every step of `proc_end`
(`src/wiiu_proc.c`) finishing: AX stopped, GX2 shut down, the screens let
go, `ProcUIShutdown` returned. The system then starts the Wii U Menu: its
splash shows and stays up. Aroma's UDP log (port 4405) carries only the
plugins' lines, not the game's `OSReport`.

The exit trace added before this (exit.txt, exit_beat.txt) wrote one line
and nothing more. Its writes gave FSA buffers on the stack, at any address
and of any length. wut's own writes never do that: they copy into a
cache-aligned buffer and send at most one cache line at a time
(wut `libraries/wutdevoptab/devoptab_fsa_write.cpp:22-52`). The trace now
writes through the port's log while wut's devoptab is mounted, and only
after that through FSA, from an aligned buffer, one cache line at a time
(`src/wiiu_exit.c`).

## The exit path

wut, at 937f0d6:

- `libraries/wutcrt/crt0_rpx.s:26-31`: `__init_wut`, then `main`, then
  `b exit`.
- newlib's `exit` runs the atexit handlers and the destructors (`proc_end`
  is one of the atexit handlers), then `_exit` calls `__syscall_exit`:
  `libraries/wutnewlib/wut_newlib.c:80-85`, which is `__fini_wut()` followed
  by `__wut_exit(rc)`. `__wut_exit` is coreinit's `exit`
  (`wut_newlib.c:132`).
- `__fini_wut` (`libraries/wutcrt/wut_crt.c:24-30`) does three things in
  order:
  - The sockets (`libraries/wutsocket/wut_socket_common.c:94-101`):
    `ACClose`, `ACFinalize`, the socket devoptab removed,
    `socket_lib_finish`.
  - The SD card's devoptab
    (`libraries/wutdevoptab/devoptab_fsa.cpp:101-114`): `FSAUnmount` and
    `FSADelClient`.
  - The C library's heap: `__fini_wut_newlib` (`wut_newlib.c:136-139`)
    calls `__fini_wut_sbrk_heap`, which gives the whole heap back to MEM2
    (`libraries/wutnewlib/wut_sbrk.c:50-64`, `MEMFreeToExpHeap` at 57-59).
- None of this stops, waits for, or joins any thread.

Where the threads live:

- A pthread's `OSThread` and its stack come from one block of the default
  heap (`libraries/wutnewlib/wut_thread.c:208-237`).
- wut points the default heap at newlib's `malloc`
  (`libraries/wutdefaultheap/wut_defaultheap.c:24-29`, installed by this
  port's `__preinit_user`, `src/wiiu_memory.c`).
- So the last step of `__fini_wut` frees the memory of every thread still
  running: its control block and the stack it is running on. Any thread
  still running at that point is also using sockets and files that have
  already been closed under it.

## How the system ends a process

decaf-emu, at e6c528a:

- `exit` (`src/libdecaf/src/cafe/libraries/coreinit/coreinit_ghs.cpp:172-188`)
  runs the atexit cleanup and the stdio cleanup.
- `_Exit` (162-166) runs the `__ghs_at_exit` callbacks.
- `__PPCExit` (151-156) calls the drivers' `OnDone`, which decaf leaves as a
  TODO (`coreinit_driver.cpp:460-463`), then `kernel::exitProcess`
  (`src/libdecaf/src/cafe/kernel/cafe_kernel_process.cpp:410-415`).
- That is `kernel::internal::exit` (`cafe_kernel.cpp:324-348`): a PowerOff
  ioctl to IOS's `/dev/ppc_app`, then the CPU halted. IOS's handler only
  stops its hardware thread (`src/libdecaf/src/ios/mcp/ios_mcp_ppc_thread.cpp:48-49`).
- `OSCancelThread` only marks a thread for cancellation. The thread ends
  itself later, at `OSTestThreadCancel` or when it suspends
  (`coreinit_thread.cpp:108-142`).

Cemu, at a53cf0e:

- `coreinit_exit` (`src/Cafe/OS/libs/coreinit/coreinit_Misc.cpp:869-878`)
  runs the same two cleanups.
- `__PPCExit` (860-867) starts a host thread that shuts down the whole
  title (`OSShutdownThread`, 705-709) and suspends the calling thread.

Neither emulator models how the real kernel ends a process whose threads
are still alive, or whether a request still pending in IOS (a socket call,
a file read) holds up the title switch. Both stop everything at once, and
decaf powers off rather than going back to the menu. So the references do
not show the system waiting for live threads. What they do show is above:
wut's own exit closes the sockets and the SD card and frees the threads'
memory while those threads keep running. Nothing in the exit path stops
them.

## What Wii U applications do

- Butter and Jelly (`src/main.cpp:221-243`, `src/ui/app.cpp:458-470`):
  `App::shutdown` stops playback, aborts HTTP requests (a flag their loops
  check, `src/platform/http_sockets.cpp:645`), and stops the worker pool.
  The pool sets `stopping_`, wakes every worker and joins each one
  (`src/core/worker.cpp:77-89`). Only then do `SDL_Quit` and the return
  from `main` run. Every `bj::Thread` is joined, in its destructor if not
  before (`src/core/thread.cpp:56-67`).
- ftpiiu (wiiu-env/ftpiiu_plugin at c2fc731,
  `source/main.cpp:62-74`): the server is destroyed before
  `platform::exit`. Its destructor sets `m_quit` and joins the thread
  (`source/ftpServer.cpp:212-218`), and `platform::exit` calls
  `WHBProcShutdown` after that (`source/wiiu/platform.cpp:396-400`).
- SDL's Wii U port (at 9c2ad80) has `OSJoinThread` for a wait and
  `OSCancelThread` for a kill (`src/thread/wiiu/SDL_systhread.c:85-98`).
  The cancel is deferred, as in decaf, so a blocked thread is not ended
  by it.

All of these stop and join their own threads before ProcUI and the C
library are taken down.

## What the port does

The threads in the Wii U build, read from its symbols
(`powerpc-eabi-nm build/wiiu/halo.elf`):

- From the platform layer:
  - `p2p_thread` and `upnp_thread` (`port/linux/src/p2p.c`).
  - `silent_clock_thread` (`dsound_sdl.c`).
  - `vertical_blank_thread` (`d3d8_gl.c`), which calls the game's
    vertical blank callback every 16 ms.
- The game's own, through `xbox_kernel.c`'s `CreateThread`:
  - The cache file thread, which waits on its event forever.
  - The file system's start-up thread.
  - The profile and playlist writes.
  - bungie_net's asynchronous connect.

The game list, Delta, the map downloads, the event upload, the crash
report, the map art, the high-resolution HUD and the updater threads are
not in this build.

The fix follows Butter and Jelly:

- `port/linux/src/thread_stop.c` (shared, in chupathingyce) provides one
  "shutting down" signal. Each platform-layer thread above checks it in its
  loop. None of them can block for long: `p2p_thread` waits at most 50 ms
  in `select`, the clocks 10 and 16 ms, and the UPnP thread is a single
  request that the Wii U answers at once. So no socket needs closing to
  wake them. The exception is a DNS lookup on the internet play thread
  (`p2p_resolve`, `getaddrinfo`), which can take seconds and cannot be
  woken. If the end comes during one, the log names that thread as still
  running.
- The vertical blank thread wakes anyone waiting for a vertical blank as
  it leaves, and those waits return once the signal is set.
- `proc_end` calls `thread_stop_all` first, while the files and the
  sockets still work and before AX, GX2 and ProcUI are shut down. It waits
  up to a second for all of them together, and logs each one to port.txt
  by name: `exit: <name> stopped`, or
  `exit: <name> still running after N ms`.
- The game's own threads are named by their entry point
  (`game thread at 0x...`, which the ELF's symbols resolve). They are
  logged if still there, but not waited for: they run the game's code,
  which never sees the signal.
- After that, the threads still alive are listed again with their state.

The desktop builds never set the signal, so their threads behave as they
did before.

If the Wii U Menu still waits after this, the next port.txt shows which
threads were left running, how the exit went past `proc_end` (each
`__fini_wut` step, coreinit's `exit`), and the beat file shows whether the
process was still running after that.

## Close from the HOME button menu (2026-10-28)

On the console, Close from the HOME button menu hung in the end (around
the sound's shutdown and `__fini_wut`), and the next start showed a dark
screen until the console was powered off. Three things still ran or were
still open at the system's exit that none of the references leave:

- The sound's filling thread ("Chupathingy audio") was never stopped:
  `wiiu_audio_shutdown` freed the voices and called `AXQuit` with it still
  running, and it ran on into `__fini_wut`, which frees its stack.
  SDL's `close_audio_device` (src/audio/SDL_audio.c, wiiu-sdl2-2.28) sets
  the device's shutdown and joins its thread (`SDL_WaitThread`) before the
  Wii U driver's `WIIUAUDIO_CloseDevice` frees the voices and quits AX. The
  port now does the same, in that order, with AX's calls on AX's core (as
  SDL's `WIIUAUDIO_OpenDevice` says AX's functions need), and frees the
  buffers after.
- The exit trace's own beat thread kept writing to the SD card through an
  FSA client of its own into coreinit's exit, and neither trace file was
  closed nor its client deleted. It is now asked to stop and joined, and
  both files closed and their clients deleted (as wut's
  `__fini_wut_devoptab` deletes its client), just before coreinit's exit.
- OSScreen was shut down at the end even when the HOME button menu had the
  screens. `WHBLogConsoleFree` (libwhb `console.c`) shuts it down only
  while the application has the foreground; the port now does too.

To be tested on the console: HOME, then Close, then start the game again
(the screen must not be dark); and HOME, then back to the game.
