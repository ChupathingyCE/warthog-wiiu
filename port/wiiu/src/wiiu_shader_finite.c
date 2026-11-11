/*
WIIU_SHADER_FINITE.C

The game's nv2a_finite (its xgpu.h): a component that is not a number made 0.
In a vertex shader, any test there (the game's by the float's bits, isnan, or
the bits compared one component at a time) drew the main menu without its
logo and items, and Blood Gulch's sky hazed over without its stars, in Cemu
on Linux (game 53d267269, #985), and the world flat and untextured in Cemu on
macOS. The pixel shaders' test draws right, so it stays. A vertex shader's
nv2a_finite gives its value back as it is: the game already makes a vertex
constant that is not a number 0 before it gets here (d3d8_gl.c
constants_store), and the lights that made them are refused
(rasterizer_lights.c), so the GPU has little left to catch there.

The rewrite only takes the definition as the game writes it. Anything else
(the brace on the signature's line, a renamed or changed signature) is no
match, and a source that still uses nv2a_finite then fails: a quiet miss
would put the broken pictures back. tests/shader_finite_test.c checks it on
the host against the game's own xgpu.h.

Pure C, no Wii U headers, so the host tests build it as it is.
*/

#include <stdlib.h>
#include <string.h>

#include "wiiu_shader_finite.h"

/* nothing but spaces, tabs and the line's end from text on */
static int rest_is_blank(const char *text, const char *end)
{
	for (; text < end; text++)
	{
		if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n')
			return 0;
	}
	return 1;
}

/* why it failed, cut to error's size (C89 has no snprintf) */
static void set_error(char *error, size_t error_size, const char *text)
{
	size_t length = strlen(text);

	if (!error_size)
		return;
	if (length >= error_size)
		length = error_size - 1;
	memcpy(error, text, length);
	error[length] = '\0';
}

char *wiiu_shader_finite_passthrough(const char *source, int *rewritten, char *error, size_t error_size)
{
	size_t size = strlen(source);
	/* (a body is replaced by one shorter line: the source's size and a
	little more is enough) */
	char *text = malloc(size + 64);
	const char *line = source;
	char *out = text;
	/* 1: the signature's line was just read; 2: in its body */
	int state = 0;

	*rewritten = 0;
	if (error_size)
		error[0] = '\0';
	if (!text)
		return NULL;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line + 1) : strlen(line);
		const char *word = line;

		while (*word == ' ' || *word == '\t')
			word++;
		if (state == 2)
		{
			/* (the body's lines go; the brace at the start of a line ends it) */
			if (*line != '}' && *line != ' ' && *line != '\t' && !rest_is_blank(line, line + length))
			{
				/* (a line at the start that is not its brace: the body ran
				on into what follows, which is never taken) */
				set_error(error, error_size, "nv2a_finite's body has no closing brace at the start of a line");
				free(text);
				return NULL;
			}
			if (*line == '}')
			{
				memcpy(out, "\treturn v;\n", 11);
				out += 11;
				(*rewritten)++;
				state = 0;
			}
			else
			{
				line += length;
				continue;
			}
		}
		else if (state == 1)
		{
			state = 0;
			if (*word == '{' && rest_is_blank(word + 1, line + length))
				state = 2;
		}
		else if (!strncmp(word, WIIU_NV2A_FINITE_SIGNATURE, sizeof(WIIU_NV2A_FINITE_SIGNATURE) - 1) &&
			rest_is_blank(word + sizeof(WIIU_NV2A_FINITE_SIGNATURE) - 1, line + length))
		{
			state = 1;
		}
		memcpy(out, line, length);
		out += length;
		line += length;
	}
	*out = '\0';
	if (state == 2)
	{
		set_error(error, error_size, "nv2a_finite's body has no closing brace at the start of a line");
		free(text);
		return NULL;
	}
	if (!*rewritten && strstr(source, "nv2a_finite("))
	{
		set_error(error, error_size,
			"the vertex shader uses nv2a_finite, but not as \"" WIIU_NV2A_FINITE_SIGNATURE "\" with \"{\" on the next line");
		free(text);
		return NULL;
	}
	return text;
}
