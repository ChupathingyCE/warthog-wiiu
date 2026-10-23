/*
WIIU_SDL.C

The SDL functions the platform layer calls (port_config.c, menu_files.c,
xbox_files.c, dsound_sdl.c, gl_functions.c), on the Wii U, where there is
no SDL: files and folders with the C library (the SD card through wut's
devoptab), the game's folder (sd:/wiiu/apps/halo, where the package puts
it), and the sound and OpenGL the port gives (wiiu_audio.c, wiiu_gl.c).
Each behaves as SDL 3's does for what those units ask of it.
*/

#include <SDL3/SDL.h>

#include <coreinit/time.h>

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wiiu_port.h"

/* (newlib's, which it declares for BSD and GNU sources only) */
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0x10
#endif

static char sdl_error[160] = "";

static void set_error(const char *text)
{
	snprintf(sdl_error, sizeof(sdl_error), "%s", text);
}

const char *SDL_GetError(void)
{
	return sdl_error;
}

void SDL_free(void *memory)
{
	free(memory);
}

int SDL_strncasecmp(const char *first, const char *second, size_t length)
{
	return strncasecmp(first, second, length);
}

bool SDL_SetHint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return true;
}

/* ---------- the game's folder */

const char *SDL_GetBasePath(void)
{
	return WIIU_APP_FOLDER "/";
}

/* ---------- files */

void *SDL_LoadFile(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	char *data = NULL;
	long length;

	if (size)
		*size = 0;
	if (!file)
	{
		set_error(strerror(errno));
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		set_error("cannot size the file");
		return NULL;
	}
	data = malloc((size_t)length + 1);
	if (!data || fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		free(data);
		fclose(file);
		set_error("cannot read the file");
		return NULL;
	}
	fclose(file);
	data[length] = 0;
	if (size)
		*size = (size_t)length;
	return data;
}

bool SDL_SaveFile(const char *path, const void *data, size_t size)
{
	FILE *file = fopen(path, "wb");
	bool written;

	if (!file)
	{
		set_error(strerror(errno));
		return false;
	}
	written = fwrite(data, 1, size, file) == size;
	return fclose(file) == 0 && written;
}

bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info)
{
	struct stat status;

	if (stat(path, &status) != 0)
	{
		set_error(strerror(errno));
		return false;
	}
	if (info)
	{
		memset(info, 0, sizeof(*info));
		info->type = S_ISREG(status.st_mode) ? SDL_PATHTYPE_FILE :
			S_ISDIR(status.st_mode) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_OTHER;
		info->size = (Uint64)status.st_size;
		info->modify_time = (SDL_Time)status.st_mtime * 1000000000;
		info->create_time = info->modify_time;
		info->access_time = (SDL_Time)status.st_atime * 1000000000;
	}
	return true;
}

bool SDL_CreateDirectory(const char *path)
{
	char partial[1024];
	size_t index;

	snprintf(partial, sizeof(partial), "%s", path);
	/* (from past the device's name, "fs:/") */
	index = strstr(partial, ":/") ? (size_t)(strstr(partial, ":/") - partial) + 2 : 1;
	for (; partial[index]; index++)
	{
		if (partial[index] == '/')
		{
			partial[index] = 0;
			mkdir(partial, 0777);
			partial[index] = '/';
		}
	}
	if (mkdir(partial, 0777) != 0 && errno != EEXIST)
	{
		struct stat status;

		if (stat(partial, &status) == 0 && S_ISDIR(status.st_mode))
			return true;
		set_error(strerror(errno));
		return false;
	}
	return true;
}

/* the names in path that match pattern (no /), as one block SDL_free
frees: the pointers, NULL, the names */
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	DIR *directory = opendir(path);
	struct dirent *entry;
	char **names, *text;
	size_t name_count = 0, bytes = 0, index;
	int pass;

	if (count)
		*count = 0;
	if (!directory)
	{
		set_error(strerror(errno));
		return NULL;
	}
	names = NULL;
	text = NULL;
	/* (twice: the sizes, then the names) */
	for (pass = 0; pass < 2; pass++)
	{
		index = 0;
		rewinddir(directory);
		while ((entry = readdir(directory)))
		{
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
				continue;
			if (pattern && fnmatch(pattern, entry->d_name, (flags & SDL_GLOB_CASEINSENSITIVE) ? FNM_CASEFOLD : 0))
				continue;
			if (!pass)
			{
				name_count++;
				bytes += strlen(entry->d_name) + 1;
			}
			else if (index < name_count)
			{
				names[index++] = text;
				strcpy(text, entry->d_name);
				text += strlen(entry->d_name) + 1;
			}
		}
		if (!pass)
		{
			names = malloc((name_count + 1) * sizeof(char *) + bytes);
			if (!names)
			{
				closedir(directory);
				set_error("out of memory");
				return NULL;
			}
			text = (char *)(names + name_count + 1);
		}
		else
		{
			name_count = index;
		}
	}
	closedir(directory);
	names[name_count] = NULL;
	if (count)
		*count = (int)name_count;
	return names;
}

/* ---------- time */

Uint64 SDL_GetTicks(void)
{
	return (Uint64)OSTicksToMilliseconds(OSGetSystemTime());
}

/* ---------- OpenGL: the port's (wiiu_gl.c) */

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *name)
{
	SDL_FunctionPointer function = (SDL_FunctionPointer)wiiu_gl_proc_address(name);

	if (!function)
		set_error("no such OpenGL function");
	return function;
}

/* ---------- sound: the port's (wiiu_audio.c) */

SDL_AudioDeviceID *SDL_GetAudioPlaybackDevices(int *count)
{
	SDL_AudioDeviceID *devices = malloc(2 * sizeof(*devices));

	if (!devices)
	{
		if (count)
			*count = 0;
		return NULL;
	}
	devices[0] = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
	devices[1] = 0;
	if (count)
		*count = 1;
	return devices;
}

const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID device)
{
	(void)device;
	return "Wii U";
}

/* the one output: the mixer's float stereo (SDL_OpenAudioDeviceStream) */
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID device, SDL_AudioSpec *specification, int *sample_frames)
{
	(void)device;
	if (specification)
	{
		specification->format = SDL_AUDIO_F32;
		specification->channels = 2;
		specification->freq = 48000;
	}
	if (sample_frames)
		*sample_frames = 0;
	return true;
}

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream)
{
	return stream ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : 0;
}

struct SDL_AudioStream
{
	SDL_AudioStreamCallback callback;
	void *userdata;
	SDL_AudioSpec specification;
};

/* the mixer's callback asks for this many bytes; the stream hands them on
to the console's voices (wiiu_audio.c) */
static void stream_pull(void *context, void *buffer, int length)
{
	struct SDL_AudioStream *stream = context;

	stream->callback(stream->userdata, stream, length, length);
	(void)buffer;
}

static struct SDL_AudioStream *audio_stream;
static unsigned char *pulled;
static int pulled_length, pulled_capacity;

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buffer, int length)
{
	if (stream != audio_stream || length <= 0)
		return false;
	if (pulled_length + length > pulled_capacity)
		return false;
	memcpy(pulled + pulled_length, buffer, (size_t)length);
	pulled_length += length;
	return true;
}

/* (wiiu_audio.c: fill buffer with length bytes from the mixer) */
static void stream_fill(void *context, void *buffer, int length)
{
	struct SDL_AudioStream *stream = context;

	if (length > pulled_capacity)
	{
		unsigned char *larger = realloc(pulled, (size_t)length);

		if (!larger)
		{
			memset(buffer, 0, (size_t)length);
			return;
		}
		pulled = larger;
		pulled_capacity = length;
	}
	pulled_length = 0;
	stream_pull(stream, buffer, length);
	memcpy(buffer, pulled, (size_t)pulled_length);
	if (pulled_length < length)
		memset((unsigned char *)buffer + pulled_length, 0, (size_t)(length - pulled_length));
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *specification,
	SDL_AudioStreamCallback callback, void *userdata)
{
	struct SDL_AudioStream *stream;

	(void)device;
	if (!specification || !callback || specification->format != SDL_AUDIO_F32 || specification->channels != 2)
	{
		set_error("the Wii U plays the mixer's float stereo through a callback");
		return NULL;
	}
	stream = calloc(1, sizeof(*stream));
	if (!stream)
		return NULL;
	stream->callback = callback;
	stream->userdata = userdata;
	stream->specification = *specification;
	audio_stream = stream;
	if (!wiiu_audio_open(specification->freq, stream_fill, stream))
	{
		audio_stream = NULL;
		free(stream);
		set_error("the console's sound did not start");
		return NULL;
	}
	return stream;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream)
{
	if (stream != audio_stream)
		return false;
	wiiu_audio_resume();
	return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream *stream)
{
	if (!stream)
		return;
	if (stream == audio_stream)
	{
		wiiu_audio_close();
		audio_stream = NULL;
	}
	free(stream);
}
