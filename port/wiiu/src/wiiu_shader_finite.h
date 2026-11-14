/*
WIIU_SHADER_FINITE.H

A vertex shader's nv2a_finite given back as it is (wiiu_shader_finite.c).
*/

#ifndef __WIIU_SHADER_FINITE_H
#define __WIIU_SHADER_FINITE_H

#include <stddef.h>

/* the function's line in the game's GLSL (xgpu.h's NV2A_FINITE) */
#define WIIU_NV2A_FINITE_SIGNATURE "vec4 nv2a_finite(vec4 v)"

/* A copy of a vertex shader's GLSL whose nv2a_finite body is "return v;"
(malloc'd), with how many bodies it rewrote in *rewritten. NULL, with why in
error, when the source uses nv2a_finite but no definition of it is the one
this rewrites (the signature on a line of its own, "{" alone on the next,
the body's lines indented, "}" at the start of a line to end it), or when
that body never ends: the
shader must then fail to compile, never draw with the test left in. NULL
with an empty error when out of memory. */
char *wiiu_shader_finite_passthrough(const char *source, int *rewritten, char *error, size_t error_size);

#endif
