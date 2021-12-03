/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "gl_shaders.h"

glprogs_t glprogs;
static GLuint gl_programs[64];
static GLuint gl_current_program;
static int gl_num_programs;

/*
=============
AppendString
=============
*/
static qboolean AppendString (char **dst, const char *dstend, const char *str, int len)
{
	int avail = dstend - *dst;
	if (len < 0)
		len = Q_strlen (str);
	if (len + 1 > avail)
		return false;
	memcpy (*dst, str, len);
	(*dst)[len] = 0;
	*dst += len;
	return true;
}

/*
=============
GL_CreateShader
=============
*/
static GLuint GL_CreateShader (GLenum type, const char *source, const char *extradefs, const char *name)
{
	const char *strings[16];
	const char *typestr = NULL;
	char header[256];
	int numstrings = 0;
	GLint status;
	GLuint shader;

	switch (type)
	{
		case GL_VERTEX_SHADER:
			typestr = "vertex";
			break;
		case GL_FRAGMENT_SHADER:
			typestr = "fragment";
			break;
		case GL_COMPUTE_SHADER:
			typestr = "compute";
			break;
		default:
			Sys_Error ("GL_CreateShader: unknown type 0x%X for %s", type, name);
			break;
	}

	q_snprintf (header, sizeof (header),
		"#version 430\n"
		"\n"
		"#define BINDLESS %d\n",
		gl_bindless_able
	);
	strings[numstrings++] = header;

	if (extradefs && *extradefs)
		strings[numstrings++] = extradefs;
	strings[numstrings++] = source;

	shader = GL_CreateShaderFunc (type);
	GL_ObjectLabelFunc (GL_SHADER, shader, -1, name);
	GL_ShaderSourceFunc (shader, numstrings, strings, NULL);
	GL_CompileShaderFunc (shader);
	GL_GetShaderivFunc (shader, GL_COMPILE_STATUS, &status);

	if (status != GL_TRUE)
	{
		char infolog[1024];
		memset(infolog, 0, sizeof(infolog));
		GL_GetShaderInfoLogFunc (shader, sizeof(infolog), NULL, infolog);
		Sys_Error ("Error compiling %s %s shader :\n%s", name, typestr, infolog);
	}

	return shader;
}

/*
=============
GL_CreateProgramFromShaders
=============
*/
static GLuint GL_CreateProgramFromShaders (const GLuint *shaders, int numshaders, const char *name)
{
	GLuint program;
	GLint status;

	program = GL_CreateProgramFunc ();
	GL_ObjectLabelFunc (GL_PROGRAM, program, -1, name);

	while (numshaders-- > 0)
	{
		GL_AttachShaderFunc (program, *shaders);
		GL_DeleteShaderFunc (*shaders);
		++shaders;
	}

	GL_LinkProgramFunc (program);
	GL_GetProgramivFunc (program, GL_LINK_STATUS, &status);

	if (status != GL_TRUE)
	{
		char infolog[1024];
		memset(infolog, 0, sizeof(infolog));
		GL_GetProgramInfoLogFunc (program, sizeof(infolog), NULL, infolog);
		Sys_Error ("Error linking %s program: %s", name, infolog);
	}

	if (gl_num_programs == countof(gl_programs))
		Sys_Error ("gl_programs overflow");
	gl_programs[gl_num_programs] = program;
	gl_num_programs++;

	return program;
}

/*
====================
GL_CreateProgram

Compiles and returns GLSL program.
====================
*/
static FUNC_PRINTF(3,4) GLuint GL_CreateProgram (const GLchar *vertSource, const GLchar *fragSource, const char *name, ...)
{
	va_list argptr;
	char macros[1024];
	char eval[256];
	char *pipe = strchr (name, '|');
	GLuint shaders[2]; // vertex, fragment

	va_start (argptr, name);
	q_vsnprintf (eval, sizeof (eval), name, argptr);
	va_end (argptr);

	macros[0] = 0;

	if (pipe) // parse symbol list and generate #defines
	{
		char *dst = macros;
		char *dstend = macros + sizeof (macros);
		char *src = eval + 1 + (pipe - name);

		while (*src == ' ')
			src++;

		while (*src)
		{
			char *srcend = src + 1;
			while (*srcend && *srcend != ';')
				srcend++;

			if (!AppendString (&dst, dstend, "#define ", 8) ||
				!AppendString (&dst, dstend, src, srcend - src) ||
				!AppendString (&dst, dstend, "\n", 1))
				Sys_Error ("GL_CreateProgram: symbol overflow for %s", eval);

			src = srcend;
			while (*src == ';' || *src == ' ')
				src++;
		}

		AppendString (&dst, dstend, "\n", 1);
	}

	name = eval;

	shaders[0] = GL_CreateShader (GL_VERTEX_SHADER, vertSource, macros, name);
	if (fragSource)
		shaders[1] = GL_CreateShader (GL_FRAGMENT_SHADER, fragSource, macros, name);

	return GL_CreateProgramFromShaders (shaders, fragSource ? 2 : 1, name);
}

/*
====================
GL_CreateComputeProgram

Compiles and returns GLSL program.
====================
*/
static GLuint GL_CreateComputeProgram (const GLchar *source, const char *name)
{
	GLuint shader = GL_CreateShader (GL_COMPUTE_SHADER, source, NULL, name);
	return GL_CreateProgramFromShaders (&shader, 1, name);
}

/*
====================
GL_UseProgram
====================
*/
void GL_UseProgram (GLuint program)
{
	if (program == gl_current_program)
		return;
	gl_current_program = program;
	GL_UseProgramFunc (program);
}

/*
====================
GL_ClearCachedProgram

This must be called if you do anything that could make the cached program
invalid (e.g. manually binding, destroying the context).
====================
*/
void GL_ClearCachedProgram (void)
{
	gl_current_program = 0;
	GL_UseProgramFunc (0);
}

/*
=============
GL_CreateShaders
=============
*/
void GL_CreateShaders (void)
{
	int alphatest;

	glprogs.gui = GL_CreateProgram (gui_vertex_shader, gui_fragment_shader, "gui");
	glprogs.viewblend = GL_CreateProgram (viewblend_vertex_shader, viewblend_fragment_shader, "viewblend");
	glprogs.warpscale = GL_CreateProgram (warpscale_vertex_shader, warpscale_fragment_shader, "view warp/scale");
	glprogs.postprocess = GL_CreateProgram (postprocess_vertex_shader, postprocess_fragment_shader, "postprocess");

	for (alphatest = 0; alphatest < 2; alphatest++)
		glprogs.world[alphatest] = GL_CreateProgram (world_vertex_shader, world_fragment_shader, "world|ALPHATEST %d", alphatest);
	glprogs.water = GL_CreateProgram (water_vertex_shader, water_fragment_shader, "water");
	glprogs.skystencil = GL_CreateProgram (skystencil_vertex_shader, NULL, "sky stencil");
	glprogs.skylayers = GL_CreateProgram (sky_layers_vertex_shader, sky_layers_fragment_shader, "sky layers");
	glprogs.skycubemap = GL_CreateProgram (sky_cubemap_vertex_shader, sky_cubemap_fragment_shader, "sky cubemap");

	glprogs.skyboxside = GL_CreateProgram (sky_boxside_vertex_shader, sky_boxside_fragment_shader, "skybox side");

	for (alphatest = 0; alphatest < 2; alphatest++)
		glprogs.alias[alphatest] = GL_CreateProgram (alias_vertex_shader, alias_fragment_shader, "alias|ALPHATEST %d", alphatest);
	glprogs.sprites = GL_CreateProgram (sprites_vertex_shader, sprites_fragment_shader, "sprites");
	glprogs.particles = GL_CreateProgram (particles_vertex_shader, particles_fragment_shader, "particles");

	glprogs.clear_indirect = GL_CreateComputeProgram (clear_indirect_compute_shader, "clear indirect draw params");
	glprogs.gather_indirect = GL_CreateComputeProgram (gather_indirect_compute_shader, "indirect draw gather");
	glprogs.cull_mark = GL_CreateComputeProgram (cull_mark_compute_shader, "cull/mark");
	glprogs.cluster_lights = GL_CreateComputeProgram (cluster_lights_compute_shader, "light cluster");
}

/*
=============
GL_DeleteShaders
=============
*/
void GL_DeleteShaders (void)
{
	int i;
	for (i = 0; i < gl_num_programs; i++)
	{
		GL_DeleteProgramFunc (gl_programs[i]);
		gl_programs[i] = 0;
	}
	gl_num_programs = 0;

	GL_UseProgramFunc (0);
	gl_current_program = 0;

	memset (&glprogs, 0, sizeof(glprogs));
}
