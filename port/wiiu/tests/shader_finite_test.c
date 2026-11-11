/*
SHADER_FINITE_TEST.C

wiiu_shader_finite.c on the host: a vertex shader's nv2a_finite is given back
as it is when the game writes it as its xgpu.h does (NV2A_FINITE, from the
game's own header: run.sh cuts it out to nv2a_finite_text.h), and any other
spelling of it fails the shader instead of drawing with the test left in.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wiiu_shader_finite.h"
#include "nv2a_finite_text.h"

static int failures;

static void check(int ok, const char *what)
{
	if (!ok)
	{
		printf("shader_finite_test: FAIL: %s\n", what);
		failures++;
	}
}

#define MAIN_TEXT \
	"void main()\n" \
	"{\n" \
	"\tvec4 oD0 = vec4(0.0);\n" \
	"\txD0 = clamp(nv2a_finite(oD0), 0.0, 1.0);\n" \
	"}\n"

/* the rewrite of source: a copy (and how many it rewrote), or NULL */
static char *rewrite(const char *source, int *rewritten, char *error)
{
	return wiiu_shader_finite_passthrough(source, rewritten, error, 200);
}

int main(void)
{
	char error[200];
	int rewritten;
	char *text;

	/* the game's own text: the body goes, main stays whole */
	text = rewrite("#version 450 core\n" NV2A_FINITE MAIN_TEXT, &rewritten, error);
	check(text != NULL, "the game's nv2a_finite (xgpu.h) is rewritten");
	if (text)
	{
		check(rewritten == 1, "one body rewritten");
		check(strstr(text, "vec4 nv2a_finite(vec4 v)\n{\n\treturn v;\n}\n") != NULL, "its body is return v");
		check(strstr(text, "floatBitsToUint") == NULL && strstr(text, "isnan") == NULL, "no test is left");
		check(strstr(text, MAIN_TEXT) != NULL, "main is untouched");
		free(text);
	}

	/* the brace on the signature's line: no match, so the shader fails */
	text = rewrite("vec4 nv2a_finite(vec4 v) {\n\treturn v * 2.0;\n}\n" MAIN_TEXT, &rewritten, error);
	check(text == NULL && error[0], "the brace on the signature's line fails");

	/* a signature that changed: fails, and says why */
	text = rewrite("vec4 nv2a_finite(in vec4 v)\n{\n\treturn v;\n}\n" MAIN_TEXT, &rewritten, error);
	check(text == NULL && strstr(error, "nv2a_finite") != NULL, "a changed signature fails");

	/* the brace not on the next line: no match; main's body is never taken */
	text = rewrite("vec4 nv2a_finite(vec4 v)\n// a comment\n{\n\treturn v;\n}\n" MAIN_TEXT, &rewritten, error);
	check(text == NULL, "the brace further down fails");

	/* a body without its closing brace at the start of a line */
	text = rewrite("vec4 nv2a_finite(vec4 v)\n{\n\treturn v; }\n" MAIN_TEXT "  }\n", &rewritten, error);
	check(text == NULL && error[0], "a body that never ends fails");

	/* a shader without nv2a_finite: the same text back */
	text = rewrite("void main()\n{\n\tgl_Position = vec4(0.0);\n}\n", &rewritten, error);
	check(text != NULL && rewritten == 0 && !strcmp(text, "void main()\n{\n\tgl_Position = vec4(0.0);\n}\n"),
		"a shader without nv2a_finite stays as it is");
	free(text);

	if (failures)
		return 1;
	printf("shader_finite_test: ok\n");
	return 0;
}
