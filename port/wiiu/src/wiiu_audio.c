/*
WIIU_AUDIO.C

The mixer's output (port/linux/src/dsound_sdl.c's, through wiiu_sdl.c's
audio stream) on the console's AX voices, the way SDL's Wii U audio plays an
application's sound (devkitPro's SDL, branch wiiu-sdl2-2.28,
src/audio/wiiu/SDL_wiiuaudio.c, zlib-licensed):

- AX at 48 kHz with the single-stage pipeline (wut's samples, SDL's
  WIIUAUDIO_OpenDevice);
- one voice a channel, top priority, 16-bit big-endian samples, the left
  voice on the TV's and the GamePad's left channel and the right on their
  right, at full volume (SDL's stereo_mix), its rate converted from the
  mixer's by AX's linear sample rate converter;
- two buffers each voice loops over: AX's frame callback, every 3 ms, finds
  the one playing and points the loop at the other once it is filled
  (SDL's _WIIUAUDIO_framecallback); a thread of the port's fills the other
  from the mixer, deinterleaved (SDL's audio thread: WIIUAUDIO_WaitDevice
  and WIIUAUDIO_PlayDevice);
- the voices set up on the core AX runs on (SDL's WIIUAUDIO_OpenDevice: a
  thread on core 1 when the caller is on another).

AX itself is started with ProcUI (the Wii U Menu's music stops with it) and
stopped when the HOME button menu takes the foreground: the voices go with
it and are made again when AX starts again.
*/

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>

#include "wiiu_port.h"

/* (SDL's NUM_BUFFERS) */
#define AUDIO_BUFFERS 2
/* samples a buffer holds, each channel: 21 ms at 48 kHz */
#define AUDIO_SAMPLES 1024
#define AUDIO_CHANNELS 2
/* (SDL's AX_NUM_CHANNELS: decaf's channels of the TV, the GamePad uses
fewer) */
#define AUDIO_DEVICE_CHANNELS 6
/* (SDL's AX_MAIN_AFFINITY) */
#define AUDIO_AX_AFFINITY OS_THREAD_ATTRIB_AFFINITY_CPU1

static struct
{
	int open;                /* a stream is open (wiiu_audio_open) */
	int voices_ready;
	int rate;
	wiiu_audio_fill fill;
	void *context;
	AXVoice *voices[AUDIO_CHANNELS];
	/* the buffers, each AUDIO_SAMPLES of every channel, one channel after
	the other (SDL's mixbufs) */
	int16_t *buffers[AUDIO_BUFFERS];
	void *buffer_memory;
	float *interleaved;
	volatile int playing, rendering;
	/* the filling thread */
	OSThread *thread;
	void *thread_stack;
	volatile int thread_running;
	/* what the log says, now and then */
	volatile unsigned long callbacks, filled, bad_buffers;
	float peak;
} audio;

/* (SDL's stereo_mix: voice 0 to the left channel, 1 to the right, on the
master bus at full volume) */
static AXVoiceDeviceMixData stereo_mix[AUDIO_CHANNELS][AUDIO_DEVICE_CHANNELS];

/* (SDL's calc_ax_offset: an address as a voice's offset, in samples from
its data) */
static uint32_t voice_offset(const AXVoiceOffsets *offsets, const void *address)
{
	return (uint32_t)(((const unsigned char *)address - (const unsigned char *)offsets->data) / sizeof(int16_t));
}

/* Every 3 ms before AX renders a frame (SDL's _WIIUAUDIO_framecallback):
which buffer plays, and the loop pointed at the next once it is filled. */
static void audio_frame_callback(void)
{
	AXVoiceOffsets offsets[AUDIO_CHANNELS];
	int playing = -1, index, channel;

	if (!audio.voices_ready)
		return;
	audio.callbacks++;
	for (channel = 0; channel < AUDIO_CHANNELS; channel++)
		AXGetVoiceOffsets(audio.voices[channel], &offsets[channel]);
	for (index = 0; index < AUDIO_BUFFERS; index++)
	{
		uint32_t start = voice_offset(&offsets[0], audio.buffers[index]);
		uint32_t end = start + AUDIO_SAMPLES;

		if (offsets[0].currentOffset >= start && offsets[0].currentOffset <= end)
		{
			playing = index;
			break;
		}
	}
	if (playing < 0)
	{
		audio.bad_buffers++;
		playing = 0;
	}
	audio.playing = playing;
	for (channel = 0; channel < AUDIO_CHANNELS; channel++)
	{
		/* (the end: the channel's last sample in the playing buffer, less
		one, SDL's "trial and error to limit popping") */
		const int16_t *end = audio.buffers[playing] + AUDIO_SAMPLES * (channel + 1) - 1;
		int next = (playing + 1) % AUDIO_BUFFERS;
		const int16_t *loop = audio.rendering != next ? audio.buffers[next] + AUDIO_SAMPLES * channel :
			audio.buffers[playing] + AUDIO_SAMPLES * channel;

		AXSetVoiceEndOffset(audio.voices[channel], voice_offset(&offsets[channel], end));
		AXSetVoiceLoopOffset(audio.voices[channel], voice_offset(&offsets[channel], loop));
	}
}

/* the voices made, playing the buffers (silent until filled): on AX's core
(SDL's _WIIUAUDIO_OpenDeviceFunction) */
static int audio_voices_make(int argument, const char **unused)
{
	AXVoiceVeData volume;
	int channel;

	(void)argument;
	(void)unused;
	memset(&volume, 0, sizeof(volume));
	volume.volume = 0x8000;
	for (channel = 0; channel < AUDIO_CHANNELS; channel++)
	{
		AXVoiceOffsets offsets;
		AXVoice *voice = AXAcquireVoice(31, NULL, NULL);

		if (!voice)
		{
			wiiu_log("audio: no AX voice for channel %d", channel);
			return -1;
		}
		audio.voices[channel] = voice;
		AXVoiceBegin(voice);
		AXSetVoiceType(voice, 0);
		AXSetVoiceVe(voice, &volume);
		AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_DRC, 0, stereo_mix[channel]);
		AXSetVoiceDeviceMix(voice, AX_DEVICE_TYPE_TV, 0, stereo_mix[channel]);
		AXSetVoiceSrcRatio(voice, (float)audio.rate / (float)AXGetInputSamplesPerSec());
		AXSetVoiceSrcType(voice, AX_VOICE_SRC_TYPE_LINEAR);
		memset(&offsets, 0, sizeof(offsets));
		offsets.dataType = AX_VOICE_FORMAT_LPCM16;
		offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
		offsets.loopOffset = 0;
		offsets.endOffset = AUDIO_SAMPLES;
		offsets.currentOffset = 0;
		offsets.data = audio.buffers[0] + AUDIO_SAMPLES * channel;
		AXSetVoiceOffsets(voice, &offsets);
		AXSetVoiceState(voice, AX_VOICE_STATE_PLAYING);
		AXVoiceEnd(voice);
	}
	audio.playing = 0;
	audio.rendering = 1;
	audio.voices_ready = 1;
	AXRegisterAppFrameCallback(audio_frame_callback);
	return 0;
}

static void audio_voices_free(void)
{
	int channel;

	if (!audio.voices_ready)
		return;
	audio.voices_ready = 0;
	AXDeregisterAppFrameCallback(audio_frame_callback);
	for (channel = 0; channel < AUDIO_CHANNELS; channel++)
	{
		if (audio.voices[channel])
			AXFreeVoice(audio.voices[channel]);
		audio.voices[channel] = NULL;
	}
}

static void thread_deallocate(OSThread *thread, void *stack)
{
	free(thread);
	free(stack);
}

/* (SDL's WIIUAUDIO_OpenDevice: AX's calls on AX's core, on a thread there
when this one is elsewhere) */
static int audio_voices_start(void)
{
	OSThread *thread;
	uint32_t stack_size = 32 * 1024;
	unsigned char *stack;
	int result = -1;

	if (!AXIsInit() || audio.voices_ready)
		return audio.voices_ready;
	if (OSGetThreadAffinity(OSGetCurrentThread()) == AUDIO_AX_AFFINITY)
		return audio_voices_make(0, NULL) == 0;
	thread = memalign(16, sizeof(*thread));
	stack = memalign(16, stack_size);
	if (!thread || !stack)
	{
		free(thread);
		free(stack);
		return 0;
	}
	if (!OSCreateThread(thread, audio_voices_make, 0, NULL, stack + stack_size, stack_size,
		OSGetThreadPriority(OSGetCurrentThread()), AUDIO_AX_AFFINITY))
	{
		free(thread);
		free(stack);
		return 0;
	}
	OSSetThreadDeallocator(thread, thread_deallocate);
	OSResumeThread(thread);
	OSJoinThread(thread, &result);
	return result == 0;
}

/* a buffer filled from the mixer: its float stereo made 16-bit big-endian
samples, each channel's together (SDL's WIIUAUDIO_PlayDevice) */
static void audio_buffer_fill(int index)
{
	int16_t *out = audio.buffers[index];
	int sample, channel;
	float peak = 0.0f;

	audio.fill(audio.context, audio.interleaved, AUDIO_SAMPLES * AUDIO_CHANNELS * (int)sizeof(float));
	for (sample = 0; sample < AUDIO_SAMPLES; sample++)
	{
		for (channel = 0; channel < AUDIO_CHANNELS; channel++)
		{
			float value = audio.interleaved[sample * AUDIO_CHANNELS + channel];
			float magnitude = value < 0.0f ? -value : value;

			if (magnitude > peak)
				peak = magnitude;
			if (value > 1.0f)
				value = 1.0f;
			else if (value < -1.0f)
				value = -1.0f;
			out[AUDIO_SAMPLES * channel + sample] = (int16_t)(value * 32767.0f);
		}
	}
	DCStoreRange(out, AUDIO_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
	if (peak > audio.peak)
		audio.peak = peak;
	audio.filled++;
}

/* The filling thread (SDL's audio thread): the buffer after the one
playing filled whenever AX has moved on to it. */
static int audio_thread(int argument, const char **unused)
{
	OSTime last_report = OSGetSystemTime();

	(void)argument;
	(void)unused;
	while (audio.thread_running)
	{
		/* (SDL's WIIUAUDIO_WaitDevice: a look every 3 ms) */
		if (!audio.voices_ready || audio.rendering == audio.playing)
		{
			OSSleepTicks(OSMillisecondsToTicks(3));
		}
		else
		{
			audio_buffer_fill(audio.rendering);
			audio.rendering = (audio.rendering + 1) % AUDIO_BUFFERS;
		}
		if (OSTicksToMilliseconds(OSGetSystemTime() - last_report) >= 30000)
		{
			last_report = OSGetSystemTime();
			wiiu_log("audio: %lu AX frames, %lu buffers filled, %lu lost track, loudest %.3f", audio.callbacks,
				audio.filled, audio.bad_buffers, (double)audio.peak);
			audio.peak = 0.0f;
		}
	}
	return 0;
}

static void audio_thread_stop(void)
{
	int result;

	if (!audio.thread)
		return;
	audio.thread_running = 0;
	OSJoinThread(audio.thread, &result);
	audio.thread = NULL;
}

/* AX is the application's to start: until it is, the Wii U Menu's own sound
keeps playing under the game. The 48 kHz renderer and the single-stage
pipeline, as wut's samples and SDL's Wii U audio do. It is let go when the
HOME button menu takes the foreground and started again on return, with
the voices of a stream that is open. */
void wiiu_audio_startup(void)
{
	AXInitParams parameters = { .renderer = AX_INIT_RENDERER_48KHZ, .pipeline = AX_INIT_PIPELINE_SINGLE };

	if (AXIsInit())
		return;
	AXInitWithParams(&parameters);
	wiiu_log("audio: AX started (48 kHz)");
	if (audio.open && !audio_voices_start())
		wiiu_log("audio: the voices could not be made again");
}

void wiiu_audio_shutdown(void)
{
	if (!AXIsInit())
		return;
	audio_voices_free();
	AXQuit();
	wiiu_log("audio: AX stopped");
}

int wiiu_audio_open(int rate, wiiu_audio_fill fill, void *context)
{
	size_t bytes = AUDIO_BUFFERS * AUDIO_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t);
	uint32_t stack_size = 64 * 1024;
	int index;

	if (audio.open)
		return 0;
	if (!AXIsInit())
	{
		wiiu_log("audio: AX is not started");
		return 0;
	}
	stereo_mix[0][0].bus[0].volume = 0x8000;
	stereo_mix[1][1].bus[0].volume = 0x8000;
	/* (the DSP reads the buffers in the first 512 MB of physical memory:
	SDL's mixbuf allocation retries until one is there) */
	{
		void *tries[32];
		int count = 0;

		audio.buffer_memory = NULL;
		while (count < 32)
		{
			void *memory = memalign(0x40, bytes);
			uint32_t start, end;

			if (!memory)
				break;
			start = OSEffectiveToPhysical((uint32_t)(uintptr_t)memory) & 0x1fffffff;
			end = start + (uint32_t)bytes;
			if ((end & 0xe0000000) == 0)
			{
				audio.buffer_memory = memory;
				break;
			}
			tries[count++] = memory;
		}
		while (count--)
			free(tries[count]);
	}
	audio.interleaved = malloc(AUDIO_SAMPLES * AUDIO_CHANNELS * sizeof(float));
	if (!audio.buffer_memory || !audio.interleaved)
	{
		wiiu_log("audio: no memory for the voices' buffers");
		free(audio.buffer_memory);
		free(audio.interleaved);
		audio.buffer_memory = NULL;
		audio.interleaved = NULL;
		return 0;
	}
	memset(audio.buffer_memory, 0, bytes);
	DCStoreRange(audio.buffer_memory, bytes);
	for (index = 0; index < AUDIO_BUFFERS; index++)
		audio.buffers[index] = (int16_t *)audio.buffer_memory + index * AUDIO_SAMPLES * AUDIO_CHANNELS;
	audio.rate = rate;
	audio.fill = fill;
	audio.context = context;
	if (!audio_voices_start())
	{
		wiiu_log("audio: the AX voices could not be made");
		free(audio.buffer_memory);
		free(audio.interleaved);
		audio.buffer_memory = NULL;
		audio.interleaved = NULL;
		return 0;
	}
	audio.open = 1;
	/* (the filling thread, a step above the caller's priority: SDL's
	WIIUAUDIO_ThreadInit) */
	audio.thread = memalign(16, sizeof(OSThread));
	audio.thread_stack = memalign(16, stack_size);
	audio.thread_running = 1;
	if (!audio.thread || !audio.thread_stack ||
		!OSCreateThread(audio.thread, audio_thread, 0, NULL, (unsigned char *)audio.thread_stack + stack_size, stack_size,
		OSGetThreadPriority(OSGetCurrentThread()) - 1, OS_THREAD_ATTRIB_AFFINITY_ANY))
	{
		wiiu_log("audio: the filling thread could not be made");
		free(audio.thread);
		free(audio.thread_stack);
		audio.thread = NULL;
		audio.thread_stack = NULL;
		audio.thread_running = 0;
		audio_voices_free();
		audio.open = 0;
		return 0;
	}
	OSSetThreadName(audio.thread, "Chupathingy audio");
	OSSetThreadDeallocator(audio.thread, thread_deallocate);
	OSResumeThread(audio.thread);
	wiiu_log("audio: %d Hz float stereo on two AX voices, buffers of %d samples", rate, AUDIO_SAMPLES);
	return 1;
}

void wiiu_audio_resume(void)
{
}

void wiiu_audio_close(void)
{
	if (!audio.open)
		return;
	audio_thread_stop();
	audio_voices_free();
	free(audio.buffer_memory);
	free(audio.interleaved);
	audio.buffer_memory = NULL;
	audio.interleaved = NULL;
	audio.open = 0;
	wiiu_log("audio: closed");
}
