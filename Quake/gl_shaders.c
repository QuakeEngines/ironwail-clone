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
GL_CheckShader
=============
*/
static void GL_CheckShader (GLuint shader, const char *name, const char *type)
{
	GLint status;
	GL_GetShaderivFunc (shader, GL_COMPILE_STATUS, &status);

	if (status != GL_TRUE)
	{
		char infolog[1024];

		memset(infolog, 0, sizeof(infolog));
		GL_GetShaderInfoLogFunc (shader, sizeof(infolog), NULL, infolog);
		
		Sys_Error ("Error compiling %s %s shader :\n%s", name, type, infolog);
	}
}

/*
=============
GL_CheckProgram
=============
*/
static void GL_CheckProgram (GLuint program, const char *name)
{
	GLint status;
	GL_GetProgramivFunc (program, GL_LINK_STATUS, &status);

	if (status != GL_TRUE)
	{
		char infolog[1024];

		memset(infolog, 0, sizeof(infolog));
		GL_GetProgramInfoLogFunc (program, sizeof(infolog), NULL, infolog);

		Sys_Error ("Error linking %s program: %s", name, infolog);
	}
}

/*
====================
GL_CreateProgram

Compiles and returns GLSL program.
====================
*/
GLuint GL_CreateProgram (const GLchar *vertSource, const GLchar *fragSource, const char *name)
{
	GLuint program, vertShader, fragShader;

	vertShader = GL_CreateShaderFunc (GL_VERTEX_SHADER);
	GL_ObjectLabelFunc (GL_SHADER, vertShader, -1, name);
	GL_ShaderSourceFunc (vertShader, 1, &vertSource, NULL);
	GL_CompileShaderFunc (vertShader);
	GL_CheckShader (vertShader, name, "vertex");

	if (fragSource)
	{
		fragShader = GL_CreateShaderFunc (GL_FRAGMENT_SHADER);
		GL_ObjectLabelFunc (GL_SHADER, fragShader, -1, name);
		GL_ShaderSourceFunc (fragShader, 1, &fragSource, NULL);
		GL_CompileShaderFunc (fragShader);
		GL_CheckShader (fragShader, name, "fragment");
	}

	program = GL_CreateProgramFunc ();
	GL_ObjectLabelFunc (GL_PROGRAM, program, -1, name);
	GL_AttachShaderFunc (program, vertShader);
	GL_DeleteShaderFunc (vertShader);

	if (fragSource)
	{
		GL_AttachShaderFunc (program, fragShader);
		GL_DeleteShaderFunc (fragShader);
	}
	
	GL_LinkProgramFunc (program);
	GL_CheckProgram (program, name);

	if (gl_num_programs == countof(gl_programs))
		Sys_Error ("gl_programs overflow");

	gl_programs[gl_num_programs] = program;
	gl_num_programs++;

	return program;
}

/*
====================
GL_CreateComputeProgram

Compiles and returns GLSL program.
====================
*/
GLuint GL_CreateComputeProgram (const GLchar *source, const char *name)
{
	GLuint program, shader;

	shader = GL_CreateShaderFunc (GL_COMPUTE_SHADER);
	GL_ObjectLabelFunc (GL_SHADER, shader, -1, name);
	GL_ShaderSourceFunc (shader, 1, &source, NULL);
	GL_CompileShaderFunc (shader);
	GL_CheckShader (shader, name, "compute");

	program = GL_CreateProgramFunc ();
	GL_ObjectLabelFunc (GL_PROGRAM, program, -1, name);
	GL_AttachShaderFunc (program, shader);
	GL_DeleteShaderFunc (shader);

	GL_LinkProgramFunc (program);
	GL_CheckProgram (program, name);

	if (gl_num_programs == countof(gl_programs))
		Sys_Error ("gl_programs overflow");

	gl_programs[gl_num_programs] = program;
	gl_num_programs++;

	return program;
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
	glprogs.gui = GL_CreateProgram (gui_vertex_shader, gui_fragment_shader, "gui");
	glprogs.viewblend = GL_CreateProgram (viewblend_vertex_shader, viewblend_fragment_shader, "viewblend");
	glprogs.warpscale = GL_CreateProgram (warpscale_vertex_shader, warpscale_fragment_shader, "view warp/scale");
	glprogs.postprocess = GL_CreateProgram (postprocess_vertex_shader, postprocess_fragment_shader, "postprocess");

	glprogs.world[0][0] = GL_CreateProgram (WORLD_VERTEX_SHADER(0), WORLD_FRAGMENT_SHADER(0, 0), "world");
	glprogs.world[0][1] = GL_CreateProgram (WORLD_VERTEX_SHADER(0), WORLD_FRAGMENT_SHADER(0, 1), "world alpha test");
	glprogs.world[1][0] = GL_CreateProgram (WORLD_VERTEX_SHADER(1), WORLD_FRAGMENT_SHADER(1, 0), "world [bindless]");
	glprogs.world[1][1] = GL_CreateProgram (WORLD_VERTEX_SHADER(1), WORLD_FRAGMENT_SHADER(1, 1), "world alpha test [bindless]");
	glprogs.water[0] = GL_CreateProgram (WATER_VERTEX_SHADER(0), WATER_FRAGMENT_SHADER(0), "water");
	glprogs.water[1] = GL_CreateProgram (WATER_VERTEX_SHADER(1), WATER_FRAGMENT_SHADER(1), "water [bindless]");
	glprogs.skystencil[0] = GL_CreateProgram (SKYSTENCIL_VERTEX_SHADER(0), NULL, "sky stencil");
	glprogs.skystencil[1] = GL_CreateProgram (SKYSTENCIL_VERTEX_SHADER(1), NULL, "sky stencil [bindless]");
	glprogs.skylayers = GL_CreateProgram (sky_layers_vertex_shader, sky_layers_fragment_shader, "sky layers");
	glprogs.skybox = GL_CreateProgram (sky_box_vertex_shader, sky_box_fragment_shader, "skybox");

	glprogs.alias = GL_CreateProgram (alias_vertex_shader, alias_fragment_shader, "alias");
	glprogs.sprites = GL_CreateProgram (sprites_vertex_shader, sprites_fragment_shader, "sprites");
	glprogs.particles = GL_CreateProgram (particles_vertex_shader, particles_fragment_shader, "particles");

	glprogs.clear_indirect = GL_CreateComputeProgram (clear_indirect_compute_shader, "clear indirect draw params");
	glprogs.gather_indirect = GL_CreateComputeProgram (gather_indirect_compute_shader, "indirect draw gather");
	glprogs.cull_mark = GL_CreateComputeProgram (cull_mark_compute_shader, "cull/mark");
	glprogs.update_lightmap = GL_CreateComputeProgram (update_lightmap_compute_shader, "lightmap update");
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
