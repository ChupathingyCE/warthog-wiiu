/*
WIIU_SHADER_CACHE.C

The renderer's compiled shaders kept on the SD card (WIIU_APP_FOLDER/
shaders), so that CafeGLSL compiles each one once per install, not once a
run (port/wiiu/docs/renderer.md: the desktop keeps its GL programs the same
way, the game's xgpu_shader_cache.c). A file is named by a hash of the text
CafeGLSL was given and of the compiler's version, and holds the compiled
GX2VertexShader or GX2PixelShader: the structure, its program, and its
variables' tables with their names, the pointers made again as it is read.

A file that does not read back whole is ignored and compiled again.
*/

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <gx2/mem.h>
#include <gx2/shaders.h>

#include "wiiu_port.h"
#include "wiiu_shader_cache.h"

#define CACHE_FOLDER WIIU_APP_FOLDER "/shaders"
#define CACHE_MAGIC 0x57534831UL /* 'WSH1' */

/* (the tables both kinds of shader have, after their registers) */
struct shader_tables
{
	uint32_t *block_count;
	GX2UniformBlock **blocks;
	uint32_t *uniform_count;
	GX2UniformVar **uniforms;
	uint32_t *initial_count;
	GX2UniformInitialValue **initials;
	uint32_t *loop_count;
	GX2LoopVar **loops;
	uint32_t *sampler_count;
	GX2SamplerVar **samplers;
	uint32_t *attribute_count;     /* NULL for a pixel shader */
	GX2AttribVar **attributes;
	uint32_t *size;
	void **program;
};

static void tables_of(int vertex, void *shader, struct shader_tables *tables)
{
	if (vertex)
	{
		GX2VertexShader *v = shader;

		tables->block_count = &v->uniformBlockCount; tables->blocks = &v->uniformBlocks;
		tables->uniform_count = &v->uniformVarCount; tables->uniforms = &v->uniformVars;
		tables->initial_count = &v->initialValueCount; tables->initials = &v->initialValues;
		tables->loop_count = &v->loopVarCount; tables->loops = &v->loopVars;
		tables->sampler_count = &v->samplerVarCount; tables->samplers = &v->samplerVars;
		tables->attribute_count = &v->attribVarCount; tables->attributes = &v->attribVars;
		tables->size = &v->size; tables->program = &v->program;
	}
	else
	{
		GX2PixelShader *p = shader;

		tables->block_count = &p->uniformBlockCount; tables->blocks = &p->uniformBlocks;
		tables->uniform_count = &p->uniformVarCount; tables->uniforms = &p->uniformVars;
		tables->initial_count = &p->initialValueCount; tables->initials = &p->initialValues;
		tables->loop_count = &p->loopVarCount; tables->loops = &p->loopVars;
		tables->sampler_count = &p->samplerVarCount; tables->samplers = &p->samplerVars;
		tables->attribute_count = NULL; tables->attributes = NULL;
		tables->size = &p->size; tables->program = &p->program;
	}
}

static void cache_path(char *path, size_t size, uint64_t key, int vertex)
{
	snprintf(path, size, CACHE_FOLDER "/%08lx%08lx.%s", (unsigned long)(key >> 32), (unsigned long)key,
		vertex ? "vsh" : "psh");
}

/* ---------- writing */

static int write_name(FILE *file, const char *name)
{
	uint16_t length = (uint16_t)(name ? strlen(name) : 0);

	return fwrite(&length, sizeof(length), 1, file) == 1 && (!length || fwrite(name, length, 1, file) == 1);
}

void wiiu_shader_cache_store(uint64_t key, int vertex, const void *shader)
{
	static int folder_made;
	struct shader_tables tables;
	char path[128];
	FILE *file;
	uint32_t magic = CACHE_MAGIC, index;
	int ok;

	if (!folder_made)
	{
		mkdir(CACHE_FOLDER, 0777);
		folder_made = 1;
	}
	tables_of(vertex, (void *)shader, &tables);
	cache_path(path, sizeof(path), key, vertex);
	file = fopen(path, "wb");
	if (!file)
		return;
	ok = fwrite(&magic, sizeof(magic), 1, file) == 1 &&
		fwrite(shader, vertex ? sizeof(GX2VertexShader) : sizeof(GX2PixelShader), 1, file) == 1 &&
		fwrite(*tables.program, *tables.size, 1, file) == 1;
	for (index = 0; ok && index < *tables.block_count; index++)
		ok = write_name(file, (*tables.blocks)[index].name) &&
			fwrite(&(*tables.blocks)[index], sizeof(GX2UniformBlock), 1, file) == 1;
	for (index = 0; ok && index < *tables.uniform_count; index++)
		ok = write_name(file, (*tables.uniforms)[index].name) &&
			fwrite(&(*tables.uniforms)[index], sizeof(GX2UniformVar), 1, file) == 1;
	if (ok && *tables.initial_count)
		ok = fwrite(*tables.initials, sizeof(GX2UniformInitialValue), *tables.initial_count, file) == *tables.initial_count;
	if (ok && *tables.loop_count)
		ok = fwrite(*tables.loops, sizeof(GX2LoopVar), *tables.loop_count, file) == *tables.loop_count;
	for (index = 0; ok && index < *tables.sampler_count; index++)
		ok = write_name(file, (*tables.samplers)[index].name) &&
			fwrite(&(*tables.samplers)[index], sizeof(GX2SamplerVar), 1, file) == 1;
	for (index = 0; ok && tables.attribute_count && index < *tables.attribute_count; index++)
		ok = write_name(file, (*tables.attributes)[index].name) &&
			fwrite(&(*tables.attributes)[index], sizeof(GX2AttribVar), 1, file) == 1;
	if (fclose(file) != 0)
		ok = 0;
	if (!ok)
		remove(path);
}

/* ---------- reading */

/* names go in a block of their own after the tables, made as they are read */
struct reader
{
	FILE *file;
	char *names;
	size_t names_used, names_size;
	int ok;
};

static void read_bytes(struct reader *reader, void *into, size_t size)
{
	if (reader->ok && size && fread(into, size, 1, reader->file) != 1)
		reader->ok = 0;
}

static const char *read_name(struct reader *reader)
{
	uint16_t length = 0;
	char *name;

	read_bytes(reader, &length, sizeof(length));
	if (!reader->ok || reader->names_used + length + 1 > reader->names_size)
	{
		reader->ok = 0;
		return "";
	}
	name = reader->names + reader->names_used;
	read_bytes(reader, name, length);
	name[length] = '\0';
	reader->names_used += (size_t)length + 1;
	return name;
}

void *wiiu_shader_cache_load(uint64_t key, int vertex)
{
	struct shader_tables tables;
	struct reader reader;
	size_t structure = vertex ? sizeof(GX2VertexShader) : sizeof(GX2PixelShader);
	size_t tables_size, file_size;
	unsigned char *block = NULL;
	char path[128];
	uint32_t magic = 0, index;
	void *shader;
	unsigned char *at;
	struct stat information;

	cache_path(path, sizeof(path), key, vertex);
	if (stat(path, &information) != 0)
		return NULL;
	file_size = (size_t)information.st_size;
	memset(&reader, 0, sizeof(reader));
	reader.file = fopen(path, "rb");
	if (!reader.file)
		return NULL;
	reader.ok = 1;
	read_bytes(&reader, &magic, sizeof(magic));
	/* (one block: the structure, its tables, the names; the tables are no
	bigger than the file) */
	tables_size = file_size;
	block = calloc(1, structure + tables_size + file_size + 16);
	if (!block || magic != CACHE_MAGIC)
		goto failed;
	shader = block;
	read_bytes(&reader, shader, structure);
	if (!reader.ok)
		goto failed;
	tables_of(vertex, shader, &tables);
	at = block + ((structure + 15) & ~(size_t)15);
	reader.names = (char *)(at + tables_size);
	reader.names_size = file_size;
	*tables.program = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, *tables.size ? *tables.size : 4);
	if (!*tables.program)
		goto failed;
	read_bytes(&reader, *tables.program, *tables.size);
#define TABLE(count, pointer, type) \
	*(pointer) = (type *)at; \
	at += sizeof(type) * *(count); \
	if (at > block + structure + 16 + tables_size) goto failed_program;
	TABLE(tables.block_count, tables.blocks, GX2UniformBlock)
	for (index = 0; reader.ok && index < *tables.block_count; index++)
	{
		const char *name = read_name(&reader);

		read_bytes(&reader, &(*tables.blocks)[index], sizeof(GX2UniformBlock));
		(*tables.blocks)[index].name = name;
	}
	TABLE(tables.uniform_count, tables.uniforms, GX2UniformVar)
	for (index = 0; reader.ok && index < *tables.uniform_count; index++)
	{
		const char *name = read_name(&reader);

		read_bytes(&reader, &(*tables.uniforms)[index], sizeof(GX2UniformVar));
		(*tables.uniforms)[index].name = name;
	}
	TABLE(tables.initial_count, tables.initials, GX2UniformInitialValue)
	read_bytes(&reader, *tables.initials, sizeof(GX2UniformInitialValue) * *tables.initial_count);
	TABLE(tables.loop_count, tables.loops, GX2LoopVar)
	read_bytes(&reader, *tables.loops, sizeof(GX2LoopVar) * *tables.loop_count);
	TABLE(tables.sampler_count, tables.samplers, GX2SamplerVar)
	for (index = 0; reader.ok && index < *tables.sampler_count; index++)
	{
		const char *name = read_name(&reader);

		read_bytes(&reader, &(*tables.samplers)[index], sizeof(GX2SamplerVar));
		(*tables.samplers)[index].name = name;
	}
	if (tables.attribute_count)
	{
		TABLE(tables.attribute_count, tables.attributes, GX2AttribVar)
		for (index = 0; reader.ok && index < *tables.attribute_count; index++)
		{
			const char *name = read_name(&reader);

			read_bytes(&reader, &(*tables.attributes)[index], sizeof(GX2AttribVar));
			(*tables.attributes)[index].name = name;
		}
	}
#undef TABLE
	if (!reader.ok)
		goto failed_program;
	fclose(reader.file);
	/* (no GX2R buffer: the program is the memory above) */
	if (vertex)
		memset(&((GX2VertexShader *)shader)->gx2rBuffer, 0, sizeof(GX2RBuffer));
	else
		memset(&((GX2PixelShader *)shader)->gx2rBuffer, 0, sizeof(GX2RBuffer));
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, *tables.program, *tables.size);
	return shader;

failed_program:
	free(*tables.program);
failed:
	fclose(reader.file);
	free(block);
	wiiu_log("gl: a cached shader did not read back (%s); compiled again", path);
	remove(path);
	return NULL;
}

void wiiu_shader_cache_free(int vertex, void *shader)
{
	struct shader_tables tables;

	if (!shader)
		return;
	tables_of(vertex, shader, &tables);
	free(*tables.program);
	free(shader);
}
