/*
WIIU_SHADER_CACHE.H

The renderer's compiled shaders kept on the SD card (wiiu_shader_cache.c).
*/

#ifndef __WIIU_SHADER_CACHE_H
#define __WIIU_SHADER_CACHE_H

#include <stdint.h>

/* a GX2VertexShader (vertex nonzero) or GX2PixelShader kept under key, or
NULL; one loaded is let go with wiiu_shader_cache_free */
void *wiiu_shader_cache_load(uint64_t key, int vertex);
int wiiu_shader_cache_store(uint64_t key, int vertex, const void *shader);
void wiiu_shader_cache_free(int vertex, void *shader);

#endif
