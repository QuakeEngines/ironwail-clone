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
//gl_sky.c

#include "quakedef.h"

#define	MAX_CLIP_VERTS 64

float Fog_GetDensity(void);
float *Fog_GetColor(void);

extern GLuint gl_bmodel_vbo;

extern	qmodel_t	*loadmodel;
extern	int	rs_skypolys; //for r_speeds readout
extern	int rs_skypasses; //for r_speeds readout
float	skyflatcolor[3];
float	skymins[2][6], skymaxs[2][6];

char	skybox_name[1024]; //name of current skybox, or "" if no skybox

gltexture_t	*skybox_textures[6];
gltexture_t	*solidskytexture, *alphaskytexture;

extern cvar_t gl_farclip;
cvar_t r_fastsky = {"r_fastsky", "0", CVAR_NONE};
cvar_t r_skyalpha = {"r_skyalpha", "1", CVAR_NONE};
cvar_t r_skyfog = {"r_skyfog","0.5",CVAR_NONE};

static const int skytexorder[6] = {0,2,1,3,4,5}; //for skybox

static const char st_to_vec[6][3] =
{
	{3,-1,2},
	{-3,1,2},
	{1,3,2},
	{-1,-3,2},
 	{-2,-1,3},		// straight up
 	{2,-1,-3}		// straight down
};

static float skyfog; // ericw

static GLuint r_skylayers_program;
static GLuint r_skybox_program;
static GLuint r_skystencil_program;

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
=============
GLSky_CreateShaders
=============
*/
void GLSky_CreateShaders (void)
{
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) in vec3 in_dir;\n"
		"\n"
		"layout(location=0) out vec3 out_dir;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
		"	v.x ^= v.y; // fix winding order\n"
		"	gl_Position = vec4(vec2(v) * 2.0 - 1.0, 1.0, 1.0);\n"
		"	out_dir = in_dir;\n"
		"	out_dir.z *= 3.0; // flatten the sphere\n"
		"}\n";
	
	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D SolidLayer;\n"
		"layout(binding=1) uniform sampler2D AlphaLayer;\n"
		"\n"
		"layout(location=2) uniform float Time;\n"
		"layout(location=3) uniform vec4 Fog;\n"
		"\n"
		"layout(location=0) in vec3 in_dir;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec2 uv = normalize(in_dir).xy * (189.0 / 64.0);\n"
		"	vec4 result = texture(SolidLayer, uv + Time / 16.0);\n"
		"	vec4 layer = texture(AlphaLayer, uv + Time / 8.0);\n"
		"	result.rgb = mix(result.rgb, layer.rgb, layer.a);\n"
		"	result.rgb = mix(result.rgb, Fog.rgb, Fog.w);\n"
		"	out_fragcolor = result;\n"
		"}\n";

	r_skylayers_program = GL_CreateProgram (vertSource, fragSource, "skylayers");

	vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) uniform mat4 MVP;\n"
		"layout(location=1) uniform vec3 EyePos;\n"
		"\n"
		"layout(location=0) in vec3 in_dir;\n"
		"layout(location=1) in vec2 in_uv;\n"
		"\n"
		"layout(location=0) out vec3 out_dir;\n"
		"layout(location=1) out vec2 out_uv;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = MVP * vec4(EyePos + in_dir, 1.0);\n"
		"	gl_Position.z = gl_Position.w; // map to far plane\n"
		"	out_dir = in_dir;\n"
		"	out_uv = in_uv;\n"
		"}\n";
	
	fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"\n"
		"layout(location=2) uniform vec4 Fog;\n"
		"\n"
		"layout(location=0) in vec3 in_dir;\n"
		"layout(location=1) in vec2 in_uv;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	out_fragcolor = texture(Tex, in_uv);\n"
		"	out_fragcolor.rgb = mix(out_fragcolor.rgb, Fog.rgb, Fog.w);\n"
		"}\n";

	r_skybox_program = GL_CreateProgram (vertSource, fragSource, "skybox");

	vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) uniform mat4 MVP;\n"
		"\n"
		"layout(location=0) in vec4 in_pos;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = MVP * in_pos;\n"
		"}\n";
	
	fragSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	out_fragcolor = vec4(1, 0, 1, 1);\n"
		"}\n";

	r_skystencil_program = GL_CreateProgram (vertSource, NULL, "skystencil");
}

/*
=============
Sky_LoadTexture

A sky texture is 256*128, with the left side being a masked overlay
==============
*/
void Sky_LoadTexture (texture_t *mt)
{
	char		texturename[64];
	int			i, j, p, r, g, b, count;
	byte		*src;
	static byte	front_data[128*128]; //FIXME: Hunk_Alloc
	static byte	back_data[128*128]; //FIXME: Hunk_Alloc
	unsigned	*rgba;

	src = (byte *)mt + mt->offsets[0];

// extract back layer and upload
	for (i=0 ; i<128 ; i++)
		for (j=0 ; j<128 ; j++)
			back_data[(i*128) + j] = src[i*256 + j + 128];

	q_snprintf(texturename, sizeof(texturename), "%s:%s_back", loadmodel->name, mt->name);
	solidskytexture = TexMgr_LoadImage (loadmodel, texturename, 128, 128, SRC_INDEXED, back_data, "", (src_offset_t)back_data, TEXPREF_NONE);

// extract front layer and upload
	for (i=0 ; i<128 ; i++)
		for (j=0 ; j<128 ; j++)
		{
			front_data[(i*128) + j] = src[i*256 + j];
			if (front_data[(i*128) + j] == 0)
				front_data[(i*128) + j] = 255;
		}

	q_snprintf(texturename, sizeof(texturename), "%s:%s_front", loadmodel->name, mt->name);
	alphaskytexture = TexMgr_LoadImage (loadmodel, texturename, 128, 128, SRC_INDEXED, front_data, "", (src_offset_t)front_data, TEXPREF_ALPHA);

// calculate r_fastsky color based on average of all opaque foreground colors
	r = g = b = count = 0;
	for (i=0 ; i<128 ; i++)
		for (j=0 ; j<128 ; j++)
		{
			p = src[i*256 + j];
			if (p != 0)
			{
				rgba = &d_8to24table[p];
				r += ((byte *)rgba)[0];
				g += ((byte *)rgba)[1];
				b += ((byte *)rgba)[2];
				count++;
			}
		}
	skyflatcolor[0] = (float)r/(count*255);
	skyflatcolor[1] = (float)g/(count*255);
	skyflatcolor[2] = (float)b/(count*255);
}

/*
=============
Sky_LoadTextureQ64

Quake64 sky textures are 32*64
==============
*/
void Sky_LoadTextureQ64 (texture_t *mt)
{
	char		texturename[64];
	int			i, p, r, g, b, count;
	byte		*front, *back, *front_rgba;
	unsigned	*rgba;

	// pointers to both layer textures
	front = (byte *)(mt+1);
	back = (byte *)(mt+1) + (32*32);
	front_rgba = (byte *) Hunk_Alloc(4*(32*32));

	// Normal indexed texture for the back layer
	q_snprintf(texturename, sizeof(texturename), "%s:%s_back", loadmodel->name, mt->name);
	solidskytexture = TexMgr_LoadImage (loadmodel, texturename, 32, 32, SRC_INDEXED, back, "", (src_offset_t)back, TEXPREF_NONE);
	
	// front layer, convert to RGBA and upload
	p = r = g = b = count = 0;

	for (i=0 ; i < (32*32) ; i++)
	{
		rgba = &d_8to24table[*front++];

		// RGB
		front_rgba[p++] = ((byte*)rgba)[0];
		front_rgba[p++] = ((byte*)rgba)[1];
		front_rgba[p++] = ((byte*)rgba)[2];
		// Alpha
		front_rgba[p++] = 128; // this look ok to me!
		
		// Fast sky
		r += ((byte *)rgba)[0];
		g += ((byte *)rgba)[1];
		b += ((byte *)rgba)[2];
		count++;
	}

	q_snprintf(texturename, sizeof(texturename), "%s:%s_front", loadmodel->name, mt->name);
	alphaskytexture = TexMgr_LoadImage (loadmodel, texturename, 32, 32, SRC_RGBA, front_rgba, "", (src_offset_t)front_rgba, TEXPREF_NONE);

	// calculate r_fastsky color based on average of all opaque foreground colors
	skyflatcolor[0] = (float)r/(count*255);
	skyflatcolor[1] = (float)g/(count*255);
	skyflatcolor[2] = (float)b/(count*255);
}

/*
==================
Sky_LoadSkyBox
==================
*/
const char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
void Sky_LoadSkyBox (const char *name)
{
	int		i, mark, width, height;
	char	filename[MAX_OSPATH];
	byte	*data;
	qboolean nonefound = true;

	if (strcmp(skybox_name, name) == 0)
		return; //no change

	//purge old textures
	for (i=0; i<6; i++)
	{
		if (skybox_textures[i] && skybox_textures[i] != notexture)
			TexMgr_FreeTexture (skybox_textures[i]);
		skybox_textures[i] = NULL;
	}

	//turn off skybox if sky is set to ""
	if (name[0] == 0)
	{
		skybox_name[0] = 0;
		return;
	}

	//load textures
	for (i=0; i<6; i++)
	{
		mark = Hunk_LowMark ();
		q_snprintf (filename, sizeof(filename), "gfx/env/%s%s", name, suf[i]);
		data = Image_LoadImage (filename, &width, &height);
		if (data)
		{
			skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, filename, width, height, SRC_RGBA, data, filename, 0, TEXPREF_NONE);
			nonefound = false;
		}
		else
		{
			Con_Printf ("Couldn't load %s\n", filename);
			skybox_textures[i] = notexture;
		}
		Hunk_FreeToLowMark (mark);
	}

	if (nonefound) // go back to scrolling sky if skybox is totally missing
	{
		for (i=0; i<6; i++)
		{
			if (skybox_textures[i] && skybox_textures[i] != notexture)
				TexMgr_FreeTexture (skybox_textures[i]);
			skybox_textures[i] = NULL;
		}
		skybox_name[0] = 0;
		return;
	}

	q_strlcpy(skybox_name, name, sizeof(skybox_name));
}

/*
=================
Sky_ClearAll

Called on map unload/game change to avoid keeping pointers to freed data
=================
*/
void Sky_ClearAll (void)
{
	int i;

	skybox_name[0] = 0;
	for (i=0; i<6; i++)
		skybox_textures[i] = NULL;
	solidskytexture = NULL;
	alphaskytexture = NULL;
}

/*
=================
Sky_NewMap
=================
*/
void Sky_NewMap (void)
{
	char	key[128], value[4096];
	const char	*data;

	skyfog = r_skyfog.value;

	//
	// read worldspawn (this is so ugly, and shouldn't it be done on the server?)
	//
	data = cl.worldmodel->entities;
	if (!data)
		return; //FIXME: how could this possibly ever happen? -- if there's no
	// worldspawn then the sever wouldn't send the loadmap message to the client

	data = COM_Parse(data);
	if (!data) //should never happen
		return; // error
	if (com_token[0] != '{') //should never happen
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

		if (!strcmp("sky", key))
			Sky_LoadSkyBox(value);

		if (!strcmp("skyfog", key))
			skyfog = atof(value);

#if 1 //also accept non-standard keys
		else if (!strcmp("skyname", key)) //half-life
			Sky_LoadSkyBox(value);
		else if (!strcmp("qlsky", key)) //quake lives
			Sky_LoadSkyBox(value);
#endif
	}
}

/*
=================
Sky_SkyCommand_f
=================
*/
void Sky_SkyCommand_f (void)
{
	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf("\"sky\" is \"%s\"\n", skybox_name);
		break;
	case 2:
		Sky_LoadSkyBox(Cmd_Argv(1));
		break;
	default:
		Con_Printf("usage: sky <skyname>\n");
	}
}

/*
====================
R_SetSkyfog_f -- ericw
====================
*/
static void R_SetSkyfog_f (cvar_t *var)
{
// clear any skyfog setting from worldspawn
	skyfog = var->value;
}

/*
=============
Sky_Init
=============
*/
void Sky_Init (void)
{
	int		i;

	Cvar_RegisterVariable (&r_fastsky);
	Cvar_RegisterVariable (&r_skyalpha);
	Cvar_RegisterVariable (&r_skyfog);
	Cvar_SetCallback (&r_skyfog, R_SetSkyfog_f);

	Cmd_AddCommand ("sky",Sky_SkyCommand_f);

	skybox_name[0] = 0;
	for (i=0; i<6; i++)
		skybox_textures[i] = NULL;
}

//==============================================================================
//
//  PROCESS SKY SURFS
//
//==============================================================================

/*
================
Sky_ProcessTextureChains -- handles sky polys in world model
================
*/
void Sky_ProcessTextureChains (void)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	if (!r_drawworld_cheatsafe)
		return;

	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matviewproj);

	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
	{
		t = cl.worldmodel->textures[i];

		if (!t || !t->texturechains[chain_world] || !(t->texturechains[chain_world]->flags & SURF_DRAWSKY))
			continue;

		for (s = t->texturechains[chain_world]; s; s = s->texturechain)
			R_BatchSurface (s);
	}

	R_FlushBatch();
}

/*
================
Sky_ProcessEntities -- handles sky polys on brush models
================
*/
void Sky_ProcessEntities (void)
{
	entity_t	*e;
	msurface_t	*s;
	int			i, j;

	if (!r_drawentities.value)
		return;

	R_ClearBatch();

	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		qboolean setup = false;
		e = cl_visedicts[i];

		if (e->model->type != mod_brush)
			continue;
		if (e->alpha == ENTALPHA_ZERO)
			continue;
		if (R_CullModelForEntity(e))
			continue;

		s = &e->model->surfaces[e->model->firstmodelsurface];
		for (j=0 ; j<e->model->nummodelsurfaces ; j++, s++)
		{
			if (!(s->flags & SURF_DRAWSKY))
				continue;

			if (!setup)
			{
				float mvp[16], model_matrix[16];
				vec3_t angles;

				setup = true;

				angles[0] = -e->angles[0];
				angles[1] =  e->angles[1];
				angles[2] =  e->angles[2];

				memcpy(&mvp, &r_matviewproj, 16 * sizeof(float));
				R_EntityMatrix (model_matrix, e->origin, angles);
				MatrixMultiply (mvp, model_matrix);
				GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, mvp);
			}

			R_BatchSurface (s);
		}

		R_FlushBatch ();
	}
}

//==============================================================================
//
//  RENDER SKYBOX
//
//==============================================================================

/*
==============
Sky_EmitSkyBoxVertex
==============
*/
void Sky_EmitSkyBoxVertex (float s, float t, int axis, float *uv, float *pos)
{
	vec3_t		v, b;
	int			j, k;
	float		w, h;

	b[0] = s * gl_farclip.value / sqrt(3.0);
	b[1] = t * gl_farclip.value / sqrt(3.0);
	b[2] = gl_farclip.value / sqrt(3.0);

	for (j=0 ; j<3 ; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
	}

	// convert from range [-1,1] to [0,1]
	s = (s+1)*0.5;
	t = (t+1)*0.5;

	// avoid bilerp seam
	w = skybox_textures[skytexorder[axis]]->width;
	h = skybox_textures[skytexorder[axis]]->height;
	s = s * (w-1)/w + 0.5/w;
	t = t * (h-1)/h + 0.5/h;

	t = 1.0 - t;
	uv[0] = s;
	uv[1] = t;
	VectorCopy(v, pos);
}

/*
==============
Sky_DrawSkyBox
==============
*/
void Sky_DrawSkyBox (void)
{
	int i, j;

	vec4_t fog;
	fog[0] = fog_data[0];
	fog[1] = fog_data[1];
	fog[2] = fog_data[2];
	fog[3] = fog_data[3] > 0.f ? skyfog : 0.f;

	GL_UseProgram (r_skybox_program);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));

	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matviewproj);
	GL_Uniform3fvFunc (1, 1, r_refdef.vieworg);
	GL_Uniform4fvFunc (2, 1, fog);

	for (i = 0; i < 6; i++)
	{
		struct skyboxvert_s {
			vec3_t pos;
			float uv[2];
		} verts[4];

		GLuint buf;
		GLbyte *ofs;

		float st[2] = {1.f, 1.f};
		for (j = 0; j < 4; j++)
		{
			Sky_EmitSkyBoxVertex(st[0], st[1], i, verts[j].uv, verts[j].pos);
			st[j & 1] *= -1.f;
		}

		GL_Upload (GL_ARRAY_BUFFER, verts, sizeof(verts), &buf, &ofs);
		GL_BindBuffer (GL_ARRAY_BUFFER, buf);
		GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(verts[0]), ofs + offsetof(struct skyboxvert_s, pos));
		GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(verts[0]), ofs + offsetof(struct skyboxvert_s, uv));

		GL_Bind (GL_TEXTURE0, skybox_textures[skytexorder[i]]);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	}
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================

/*
==============
Sky_DrawSkyLayers

draws the old-style scrolling cloud layers
==============
*/
void Sky_DrawSkyLayers (void)
{
	GLuint buf;
	GLbyte *ofs;
	vec3_t dirs[4];
	vec4_t fog;

	fog[0] = fog_data[0];
	fog[1] = fog_data[1];
	fog[2] = fog_data[2];
	fog[3] = fog_data[3] > 0.f ? skyfog : 0.f;

	CrossProduct(frustum[2].normal, frustum[1].normal, dirs[0]); // bottom left
	CrossProduct(frustum[0].normal, frustum[2].normal, dirs[1]); // bottom right
	CrossProduct(frustum[3].normal, frustum[0].normal, dirs[2]); // top right
	CrossProduct(frustum[1].normal, frustum[3].normal, dirs[3]); // top left

	GL_UseProgram (r_skylayers_program);

	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(1));
	GL_Upload (GL_ARRAY_BUFFER, dirs, sizeof(dirs), &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, 0, ofs);

	GL_Uniform1fFunc (2, cl.time);
	GL_Uniform4fvFunc (3, 1, fog);
	GL_Bind (GL_TEXTURE0, solidskytexture);
	GL_Bind (GL_TEXTURE1, alphaskytexture);

	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
}

/*
==============
Sky_DrawSky
==============
*/
void Sky_DrawSky (void)
{
	GL_BeginGroup ("Sky");

	glEnable (GL_STENCIL_TEST);
	glStencilFunc (GL_ALWAYS, 1, 1);
	glStencilOp (GL_KEEP, GL_KEEP, GL_REPLACE);
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	GL_UseProgram (r_skystencil_program);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS(1));

	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0));

	Sky_ProcessTextureChains ();
	Sky_ProcessEntities ();

	glStencilFunc (GL_EQUAL, 1, 1);
	glStencilOp (GL_KEEP, GL_KEEP, GL_KEEP);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	if (skybox_name[0])
		Sky_DrawSkyBox ();
	else
		Sky_DrawSkyLayers ();

	glDisable (GL_STENCIL_TEST);

	GL_EndGroup ();
}
