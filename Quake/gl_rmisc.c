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
// r_misc.c

#include "quakedef.h"

//johnfitz -- new cvars
extern cvar_t r_clearcolor;
extern cvar_t r_flatlightstyles;
extern cvar_t gl_fullbrights;
extern cvar_t gl_farclip;
extern cvar_t gl_overbright;
extern cvar_t gl_overbright_models;
extern cvar_t r_waterwarp;
extern cvar_t r_oldskyleaf;
extern cvar_t r_drawworld;
extern cvar_t r_showtris;
extern cvar_t r_showbboxes;
extern cvar_t r_lerpmodels;
extern cvar_t r_lerpmove;
extern cvar_t r_nolerp_list;
extern cvar_t r_noshadow_list;
//johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix

extern gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz


/*
====================
GL_Overbright_f -- johnfitz
====================
*/
static void GL_Overbright_f (cvar_t *var)
{
	R_RebuildAllLightmaps ();
}

/*
====================
GL_Fullbrights_f -- johnfitz
====================
*/
static void GL_Fullbrights_f (cvar_t *var)
{
	TexMgr_ReloadNobrightImages ();
}

/*
====================
R_SetClearColor_f -- johnfitz
====================
*/
static void R_SetClearColor_f (cvar_t *var)
{
	byte	*rgb;
	int		s;

	s = (int)r_clearcolor.value & 0xFF;
	rgb = (byte*)(d_8to24table + s);
	glClearColor (rgb[0]/255.0,rgb[1]/255.0,rgb[2]/255.0,0);
}

/*
===============
R_Model_ExtraFlags_List_f -- johnfitz -- called when r_nolerp_list or r_noshadow_list cvar changes
===============
*/
static void R_Model_ExtraFlags_List_f (cvar_t *var)
{
	int i;
	for (i=0; i < MAX_MODELS; i++)
		Mod_SetExtraFlags (cl.model_precache[i]);
}

/*
====================
R_SetWateralpha_f -- ericw
====================
*/
static void R_SetWateralpha_f (cvar_t *var)
{
	map_wateralpha = var->value;
}

/*
====================
R_SetLavaalpha_f -- ericw
====================
*/
static void R_SetLavaalpha_f (cvar_t *var)
{
	map_lavaalpha = var->value;
}

/*
====================
R_SetTelealpha_f -- ericw
====================
*/
static void R_SetTelealpha_f (cvar_t *var)
{
	map_telealpha = var->value;
}

/*
====================
R_SetSlimealpha_f -- ericw
====================
*/
static void R_SetSlimealpha_f (cvar_t *var)
{
	map_slimealpha = var->value;
}

/*
====================
GL_WaterAlphaForSurfface -- ericw
====================
*/
float GL_WaterAlphaForSurface (msurface_t *fa)
{
	if (fa->flags & SURF_DRAWLAVA)
		return map_lavaalpha > 0 ? map_lavaalpha : map_wateralpha;
	else if (fa->flags & SURF_DRAWTELE)
		return map_telealpha > 0 ? map_telealpha : map_wateralpha;
	else if (fa->flags & SURF_DRAWSLIME)
		return map_slimealpha > 0 ? map_slimealpha : map_wateralpha;
	else
		return map_wateralpha;
}


/*
===============
R_Init
===============
*/
void R_Init (void)
{
	extern cvar_t gl_finish;

	Cmd_AddCommand ("timerefresh", R_TimeRefresh_f);
	Cmd_AddCommand ("pointfile", R_ReadPointFile_f);

	Cvar_RegisterVariable (&r_norefresh);
	Cvar_RegisterVariable (&r_lightmap);
	Cvar_RegisterVariable (&r_fullbright);
	Cvar_RegisterVariable (&r_drawentities);
	Cvar_RegisterVariable (&r_drawviewmodel);
	Cvar_RegisterVariable (&r_shadows);
	Cvar_RegisterVariable (&r_wateralpha);
	Cvar_SetCallback (&r_wateralpha, R_SetWateralpha_f);
	Cvar_RegisterVariable (&r_dynamic);
	Cvar_RegisterVariable (&r_novis);
	Cvar_RegisterVariable (&r_speeds);
	Cvar_RegisterVariable (&r_pos);

	Cvar_RegisterVariable (&gl_finish);
	Cvar_RegisterVariable (&gl_clear);
	Cvar_RegisterVariable (&gl_polyblend);
	Cvar_RegisterVariable (&gl_playermip);
	Cvar_RegisterVariable (&gl_nocolors);

	//johnfitz -- new cvars
	Cvar_RegisterVariable (&r_clearcolor);
	Cvar_SetCallback (&r_clearcolor, R_SetClearColor_f);
	Cvar_RegisterVariable (&r_waterwarp);
	Cvar_RegisterVariable (&r_flatlightstyles);
	Cvar_RegisterVariable (&r_oldskyleaf);
	Cvar_RegisterVariable (&r_drawworld);
	Cvar_RegisterVariable (&r_showtris);
	Cvar_RegisterVariable (&r_showbboxes);
	Cvar_RegisterVariable (&gl_farclip);
	Cvar_RegisterVariable (&gl_fullbrights);
	Cvar_RegisterVariable (&gl_overbright);
	Cvar_SetCallback (&gl_fullbrights, GL_Fullbrights_f);
	Cvar_SetCallback (&gl_overbright, GL_Overbright_f);
	Cvar_RegisterVariable (&gl_overbright_models);
	Cvar_RegisterVariable (&r_lerpmodels);
	Cvar_RegisterVariable (&r_lerpmove);
	Cvar_RegisterVariable (&r_nolerp_list);
	Cvar_SetCallback (&r_nolerp_list, R_Model_ExtraFlags_List_f);
	Cvar_RegisterVariable (&r_noshadow_list);
	Cvar_SetCallback (&r_noshadow_list, R_Model_ExtraFlags_List_f);
	//johnfitz

	Cvar_RegisterVariable (&gl_zfix); // QuakeSpasm z-fighting fix
	Cvar_RegisterVariable (&r_lavaalpha);
	Cvar_RegisterVariable (&r_telealpha);
	Cvar_RegisterVariable (&r_slimealpha);
	Cvar_RegisterVariable (&r_scale);
	Cvar_SetCallback (&r_lavaalpha, R_SetLavaalpha_f);
	Cvar_SetCallback (&r_telealpha, R_SetTelealpha_f);
	Cvar_SetCallback (&r_slimealpha, R_SetSlimealpha_f);

	R_InitParticles ();
	R_SetClearColor_f (&r_clearcolor); //johnfitz

	Sky_Init (); //johnfitz
	Fog_Init (); //johnfitz
}

/*
===============
R_TranslatePlayerSkin -- johnfitz -- rewritten.  also, only handles new colors, not new skins
===============
*/
void R_TranslatePlayerSkin (int playernum)
{
	int			top, bottom;

	top = (cl.scores[playernum].colors & 0xf0)>>4;
	bottom = cl.scores[playernum].colors &15;

	//FIXME: if gl_nocolors is on, then turned off, the textures may be out of sync with the scoreboard colors.
	if (!gl_nocolors.value)
		if (playertextures[playernum])
			TexMgr_ReloadImage (playertextures[playernum], top, bottom);
}

/*
===============
R_TranslateNewPlayerSkin -- johnfitz -- split off of TranslatePlayerSkin -- this is called when
the skin or model actually changes, instead of just new colors
added bug fix from bengt jardup
===============
*/
void R_TranslateNewPlayerSkin (int playernum)
{
	char		name[64];
	byte		*pixels;
	aliashdr_t	*paliashdr;
	int		skinnum;

//get correct texture pixels
	currententity = &cl_entities[1+playernum];

	if (!currententity->model || currententity->model->type != mod_alias)
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (currententity->model);

	skinnum = currententity->skinnum;

	//TODO: move these tests to the place where skinnum gets received from the server
	if (skinnum < 0 || skinnum >= paliashdr->numskins)
	{
		Con_DPrintf("(%d): Invalid player skin #%d\n", playernum, skinnum);
		skinnum = 0;
	}

	pixels = (byte *)paliashdr + paliashdr->texels[skinnum]; // This is not a persistent place!

//upload new image
	q_snprintf(name, sizeof(name), "player_%i", playernum);
	playertextures[playernum] = TexMgr_LoadImage (currententity->model, name, paliashdr->skinwidth, paliashdr->skinheight,
		SRC_INDEXED, pixels, paliashdr->gltextures[skinnum][0]->source_file, paliashdr->gltextures[skinnum][0]->source_offset, TEXPREF_PAD | TEXPREF_OVERWRITE);

//now recolor it
	R_TranslatePlayerSkin (playernum);
}

/*
===============
R_NewGame -- johnfitz -- handle a game switch
===============
*/
void R_NewGame (void)
{
	int i;

	//clear playertexture pointers (the textures themselves were freed by texmgr_newgame)
	for (i=0; i<MAX_SCOREBOARD; i++)
		playertextures[i] = NULL;
}

/*
=============
R_ParseWorldspawn

called at map load
=============
*/
static void R_ParseWorldspawn (void)
{
	char key[128], value[4096];
	const char *data;

	map_wateralpha = r_wateralpha.value;
	map_lavaalpha = r_lavaalpha.value;
	map_telealpha = r_telealpha.value;
	map_slimealpha = r_slimealpha.value;

	data = COM_Parse(cl.worldmodel->entities);
	if (!data)
		return; // error
	if (com_token[0] != '{')
		return; // error
	while (1)
	{
		data = COM_Parse(data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy(key, com_token + 1, sizeof(key));
		else
			q_strlcpy(key, com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_Parse(data);
		if (!data)
			return; // error
		q_strlcpy(value, com_token, sizeof(value));

		if (!strcmp("wateralpha", key))
			map_wateralpha = atof(value);

		if (!strcmp("lavaalpha", key))
			map_lavaalpha = atof(value);

		if (!strcmp("telealpha", key))
			map_telealpha = atof(value);

		if (!strcmp("slimealpha", key))
			map_slimealpha = atof(value);
	}
}


/*
===============
R_NewMap
===============
*/
void R_NewMap (void)
{
	int		i;

	for (i=0 ; i<256 ; i++)
		d_lightstylevalue[i] = 264;		// normal light value

// clear out efrags in case the level hasn't been reloaded
// FIXME: is this one short?
	for (i=0 ; i<cl.worldmodel->numleafs ; i++)
		cl.worldmodel->leafs[i].efrags = NULL;

	r_viewleaf = NULL;
	R_ClearParticles ();

	GL_BuildLightmaps ();
	GL_BuildBModelVertexBuffer ();
	//ericw -- no longer load alias models into a VBO here, it's done in Mod_LoadAliasModel

	r_framecount = 0; //johnfitz -- paranoid?
	r_visframecount = 0; //johnfitz -- paranoid?

	Sky_NewMap (); //johnfitz -- skybox in worldspawn
	Fog_NewMap (); //johnfitz -- global fog in worldspawn
	R_ParseWorldspawn (); //ericw -- wateralpha, lavaalpha, telealpha, slimealpha in worldspawn
}

/*
====================
R_TimeRefresh_f

For program optimization
====================
*/
void R_TimeRefresh_f (void)
{
	int		i;
	float		start, stop, time;

	if (cls.state != ca_connected)
	{
		Con_Printf("Not connected to a server\n");
		return;
	}

	start = Sys_DoubleTime ();
	for (i = 0; i < 128; i++)
	{
		GL_BeginRendering(&glx, &gly, &glwidth, &glheight);
		r_refdef.viewangles[1] = i/128.0*360.0;
		R_RenderView ();
		GL_EndRendering ();
	}

	glFinish ();
	stop = Sys_DoubleTime ();
	time = stop-start;
	Con_Printf ("%f seconds (%f fps)\n", time, 128/time);
}

void D_FlushCaches (void)
{
}

static GLuint gl_programs[64];
static GLuint gl_current_program = 0;
static int gl_num_programs;

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
=============
GL_GetUniformLocation
=============
*/
GLint GL_GetUniformLocation (GLuint *programPtr, const char *name)
{
	GLint location;

	if (!programPtr)
		return -1;

	location = GL_GetUniformLocationFunc(*programPtr, name);
	if (location == -1)
	{
		Con_Warning("GL_GetUniformLocationFunc %s failed\n", name);
		*programPtr = 0;
	}
	return location;
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
R_DeleteShaders

Deletes any GLSL programs that have been created.
====================
*/
void R_DeleteShaders (void)
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

GLuint current_array_buffer, current_element_array_buffer;

/*
====================
GL_BindBuffer

glBindBuffer wrapper
====================
*/
void GL_BindBuffer (GLenum target, GLuint buffer)
{
	GLuint *cache;

	switch (target)
	{
		case GL_ARRAY_BUFFER:
			cache = &current_array_buffer;
			break;
		case GL_ELEMENT_ARRAY_BUFFER:
			cache = &current_element_array_buffer;
			break;
		default:
			Host_Error("GL_BindBuffer: unsupported target %d", (int)target);
			return;
	}

	if (*cache != buffer)
	{
		*cache = buffer;
		GL_BindBufferFunc (target, *cache);
	}
}

/*
====================
GL_DeleteBuffer
====================
*/
void GL_DeleteBuffer (GLuint buffer)
{
	if (buffer == current_array_buffer)
		current_array_buffer = 0;
	if (buffer == current_element_array_buffer)
		current_element_array_buffer = 0;

	GL_DeleteBuffersFunc (1, &buffer);
}

/*
====================
GL_ClearBufferBindings

This must be called if you do anything that could make the cached bindings
invalid (e.g. manually binding, destroying the context).
====================
*/
void GL_ClearBufferBindings ()
{
	current_array_buffer = 0;
	current_element_array_buffer = 0;
	GL_BindBufferFunc (GL_ARRAY_BUFFER, 0);
	GL_BindBufferFunc (GL_ELEMENT_ARRAY_BUFFER, 0);
}

/*
============================================================================
							DYNAMIC BUFFERS
============================================================================
*/

static GLuint dynabuf[2];
static int dynabuf_idx = 0;
static GLsizeiptr dynabuf_offset = 0;
static GLsizeiptr dynabuf_size = 16 * 1024 * 1024;
static GLuint *dynabuf_garbage[2];

/*
====================
GL_AllocDynamicBuffers
====================
*/
static void GL_AllocDynamicBuffers (void)
{
	int i;
	for (i = 0; i < countof(dynabuf); i++)
	{
		if (dynabuf[i])
			VEC_PUSH (dynabuf_garbage[dynabuf_idx], dynabuf[i]);

		GL_GenBuffersFunc (1, &dynabuf[i]);
		GL_BindBuffer (GL_ARRAY_BUFFER, dynabuf[i]);
		GL_BufferDataFunc (GL_ARRAY_BUFFER, dynabuf_size, NULL, GL_STREAM_DRAW);
	}

	dynabuf_offset = 0;
}

/*
====================
GL_InitDynamicBuffers
====================
*/
void GL_InitDynamicBuffers (void)
{
	GL_AllocDynamicBuffers ();
}

/*
====================
GL_SwapDynamicBuffers
====================
*/
void GL_SwapDynamicBuffers (void)
{
	size_t i, count;

	dynabuf_offset = 0;
	if (++dynabuf_idx == countof(dynabuf))
		dynabuf_idx = 0;

	count = VEC_SIZE (dynabuf_garbage[dynabuf_idx]);
	for (i = 0; i < count; i++)
		GL_DeleteBuffer (dynabuf_garbage[dynabuf_idx][i]);

	VEC_CLEAR (dynabuf_garbage[dynabuf_idx]);
}

/*
====================
GL_Upload
====================
*/
void GL_Upload (GLenum target, const void *data, size_t numbytes, GLuint *buf, GLbyte **ofs)
{
	if (dynabuf_offset + numbytes > dynabuf_size)
	{
		dynabuf_size = dynabuf_offset + numbytes;
		dynabuf_size += dynabuf_size >> 1;
		GL_AllocDynamicBuffers ();
	}

	GL_BindBuffer (target, dynabuf[dynabuf_idx]);
	GL_BufferSubDataFunc (target, dynabuf_offset, numbytes, data);

	*buf = dynabuf[dynabuf_idx];
	*ofs = (GLbyte*) dynabuf_offset;

	dynabuf_offset += (numbytes + 63) & ~63;
}
