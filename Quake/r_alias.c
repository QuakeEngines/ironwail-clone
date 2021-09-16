/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

//r_alias.c -- alias model rendering

#include "quakedef.h"

extern cvar_t gl_overbright_models, gl_fullbrights, r_lerpmodels, r_lerpmove; //johnfitz
extern cvar_t scr_fov, cl_gun_fovscale;

//up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz -- changed to an array of pointers

#define NUMVERTEXNORMALS	162

float	r_avertexnormals[NUMVERTEXNORMALS][3] =
{
#include "anorms.h"
};

extern vec3_t	lightcolor; //johnfitz -- replaces "float shadelight" for lit support

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16
float	r_avertexnormal_dots[SHADEDOT_QUANT][256] =
{
#include "anorm_dots.h"
};

extern	vec3_t			lightspot;

float	*shadedots = r_avertexnormal_dots[0];
vec3_t	shadevector;

float	entalpha; //johnfitz

qboolean	overbright; //johnfitz

qboolean shading = true; //johnfitz -- if false, disable vertex shading for various reasons (fullbright, r_lightmap, showtris, etc)

//johnfitz -- struct for passing lerp information to drawing functions
typedef struct {
	short pose1;
	short pose2;
	float blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;
//johnfitz

static GLuint r_alias_program;

// uniforms used in vert shader
static GLuint blendLoc;
static GLuint lightColorLoc;

// uniforms used in frag shader
static GLuint texLoc;
static GLuint fullbrightTexLoc;
static GLuint useAlphaTestLoc;
static GLuint fogLoc;

#define pose1VertexAttrIndex 0
#define pose1NormalAttrIndex 1
#define pose2VertexAttrIndex 2
#define pose2NormalAttrIndex 3
#define texCoordsAttrIndex 4

/*
=============
GLARB_GetXYZOffset

Returns the offset of the first vertex's meshxyz_t.xyz in the vbo for the given
model and pose.
=============
*/
static void *GLARB_GetXYZOffset (aliashdr_t *hdr, int pose)
{
	const int xyzoffs = offsetof (meshxyz_t, xyz);
	return (void *) (currententity->model->vboxyzofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_t)) + xyzoffs);
}

/*
=============
GLARB_GetNormalOffset

Returns the offset of the first vertex's meshxyz_t.normal in the vbo for the
given model and pose.
=============
*/
static void *GLARB_GetNormalOffset (aliashdr_t *hdr, int pose)
{
	const int normaloffs = offsetof (meshxyz_t, normal);
	return (void *)(currententity->model->vboxyzofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_t)) + normaloffs);
}

/*
=============
GLAlias_CreateShaders
=============
*/
void GLAlias_CreateShaders (void)
{
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) uniform mat4 MVP;\n"
		"layout(location=1) uniform vec4 Blend; // xyz is shadevector\n"
		"layout(location=2) uniform vec4 LightColor;\n"
		"\n"
		"layout(location=0) in vec4 Pose1Vert;\n"
		"layout(location=1) in vec3 Pose1Normal;\n"
		"layout(location=2) in vec4 Pose2Vert;\n"
		"layout(location=3) in vec3 Pose2Normal;\n"
		"layout(location=4) in vec2 TexCoords;\n"
		"\n"
		"layout(location=0) out vec2 out_texcoord;\n"
		"layout(location=1) out vec4 out_color;\n"
		"layout(location=2) out float out_fogdist;\n"
		"\n"
		"float r_avertexnormal_dot(vec3 vertexnormal) // from MH \n"
		"{\n"
		"	float dot = dot(vertexnormal, Blend.xyz);\n"
		"	// wtf - this reproduces anorm_dots within as reasonable a degree of tolerance as the >= 0 case\n"
		"	if (dot < 0.0)\n"
		"		return 1.0 + dot * (13.0 / 44.0);\n"
		"	else\n"
		"		return 1.0 + dot;\n"
		"}\n"
		"void main()\n"
		"{\n"
		"	out_texcoord = TexCoords;\n"
		"	vec4 lerpedVert = mix(vec4(Pose1Vert.xyz, 1.0), vec4(Pose2Vert.xyz, 1.0), Blend.w);\n"
		"	gl_Position = MVP * lerpedVert;\n"
		"	out_fogdist = gl_Position.w;\n"
		"	float dot1 = r_avertexnormal_dot(Pose1Normal);\n"
		"	float dot2 = r_avertexnormal_dot(Pose2Normal);\n"
		"	out_color = LightColor * vec4(vec3(mix(dot1, dot2, Blend.w)), 1.0);\n"
		"}\n";

	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"layout(binding=1) uniform sampler2D FullbrightTex;\n"
		"\n"
		"layout(location=3) uniform bool UseAlphaTest;\n"
		"layout(location=4) uniform vec4 Fog;\n"
		"\n"
		"layout(location=0) in vec2 in_texcoord;\n"
		"layout(location=1) in vec4 in_color;\n"
		"layout(location=2) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture2D(Tex, in_texcoord);\n"
		"	if (UseAlphaTest && (result.a < 0.666))\n"
		"		discard;\n"
		"	result *= in_color * 2.0;\n"
		"	result += texture2D(FullbrightTex, in_texcoord);\n"
		"	result = clamp(result, 0.0, 1.0);\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	result.a = in_color.a;\n" // FIXME: This will make almost transparent things cut holes though heavy fog
		"	out_fragcolor = result;\n"
		"}\n";

	r_alias_program = GL_CreateProgram (vertSource, fragSource, "alias");

	if (r_alias_program != 0)
	{
	// get uniform locations
		blendLoc = GL_GetUniformLocation (&r_alias_program, "Blend");
		lightColorLoc = GL_GetUniformLocation (&r_alias_program, "LightColor");
		texLoc = GL_GetUniformLocation (&r_alias_program, "Tex");
		fullbrightTexLoc = GL_GetUniformLocation (&r_alias_program, "FullbrightTex");
		useAlphaTestLoc = GL_GetUniformLocation (&r_alias_program, "UseAlphaTest");
		fogLoc = GL_GetUniformLocation (&r_alias_program, "Fog");
	}
}

/*
=============
GL_DrawAliasFrame_GLSL -- ericw

Optimized alias model drawing codepath.
Compared to the original GL_DrawAliasFrame, this makes 1 draw call,
no vertex data is uploaded (it's already in the r_meshvbo and r_meshindexesvbo
static VBOs), and lerping and lighting is done in the vertex shader.

Supports optional overbright, optional fullbright pixels.

Based on code by MH from RMQEngine
=============
*/
static int lastfogframe = 0;
void GL_DrawAliasFrame_GLSL (aliashdr_t *paliashdr, lerpdata_t lerpdata, gltexture_t *tx, gltexture_t *fb, const float *mvp)
{
	float		blend;
	unsigned	state;

	if (lerpdata.pose1 != lerpdata.pose2)
	{
		blend = lerpdata.blend;
	}
	else // poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
	{
		blend = 0;
	}

	GL_UseProgram (r_alias_program);

	state = GLS_CULL_BACK | GLS_ATTRIBS(5);
	if (entalpha < 1.f)
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	else
		state |= GLS_BLEND_OPAQUE;
	GL_SetState (state);

	GL_BindBuffer (GL_ARRAY_BUFFER, currententity->model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, currententity->model->meshindexesvbo);

	GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, 0, (void *)(intptr_t)currententity->model->vbostofs);
	GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (meshxyz_t), GLARB_GetXYZOffset (paliashdr, lerpdata.pose1));
	GL_VertexAttribPointerFunc (pose2VertexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (meshxyz_t), GLARB_GetXYZOffset (paliashdr, lerpdata.pose2));
// GL_TRUE to normalize the signed bytes to [-1 .. 1]
	GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_t), GLARB_GetNormalOffset (paliashdr, lerpdata.pose1));
	GL_VertexAttribPointerFunc (pose2NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_t), GLARB_GetNormalOffset (paliashdr, lerpdata.pose2));

// set uniforms
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, mvp);
	GL_Uniform4fFunc (blendLoc, shadevector[0], shadevector[1], shadevector[2], blend);
	GL_Uniform4fFunc (lightColorLoc, lightcolor[0], lightcolor[1], lightcolor[2], entalpha);
	GL_Uniform1iFunc (useAlphaTestLoc, (currententity->model->flags & MF_HOLEY) ? 1 : 0);

	if (lastfogframe != r_framecount) // only set fog once per frame
	{
		lastfogframe = r_framecount;
		GL_Uniform4fvFunc (fogLoc, 1, fog_data);
	}

// set textures
	GL_Bind (GL_TEXTURE0, tx);
	GL_Bind (GL_TEXTURE1, fb);

// draw
	glDrawElements (GL_TRIANGLES, paliashdr->numindexes, GL_UNSIGNED_SHORT, (void *)(intptr_t)currententity->model->vboindexofs);

	rs_aliaspasses += paliashdr->numtris;
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame (aliashdr_t *paliashdr, int frame, lerpdata_t *lerpdata)
{
	entity_t		*e = currententity;
	int				posenum, numposes;

	if ((frame >= paliashdr->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model->name);
		frame = 0;
	}

	posenum = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		e->lerptime = paliashdr->frames[frame].interval;
		posenum += (int)(cl.time / e->lerptime) % numposes;
	}
	else
		e->lerptime = 0.1;

	if (e->lerpflags & LERP_RESETANIM) //kill any lerp in progress
	{
		e->lerpstart = 0;
		e->previouspose = posenum;
		e->currentpose = posenum;
		e->lerpflags -= LERP_RESETANIM;
	}
	else if (e->currentpose != posenum) // pose changed, start new lerp
	{
		if (e->lerpflags & LERP_RESETANIM2) //defer lerping one more time
		{
			e->lerpstart = 0;
			e->previouspose = posenum;
			e->currentpose = posenum;
			e->lerpflags -= LERP_RESETANIM2;
		}
		else
		{
			e->lerpstart = cl.time;
			e->previouspose = e->currentpose;
			e->currentpose = posenum;
		}
	}

	//set up values
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		if (e->lerpflags & LERP_FINISH && numposes == 1)
			lerpdata->blend = CLAMP (0, (cl.time - e->lerpstart) / (e->lerpfinish - e->lerpstart), 1);
		else
			lerpdata->blend = CLAMP (0, (cl.time - e->lerpstart) / e->lerptime, 1);
		lerpdata->pose1 = e->previouspose;
		lerpdata->pose2 = e->currentpose;
	}
	else //don't lerp
	{
		lerpdata->blend = 1;
		lerpdata->pose1 = posenum;
		lerpdata->pose2 = posenum;
	}
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
=================
*/
void R_SetupEntityTransform (entity_t *e, lerpdata_t *lerpdata)
{
	float blend;
	vec3_t d;
	int i;

	// if LERP_RESETMOVE, kill any lerps in progress
	if (e->lerpflags & LERP_RESETMOVE)
	{
		e->movelerpstart = 0;
		VectorCopy (e->origin, e->previousorigin);
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->angles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
		e->lerpflags -= LERP_RESETMOVE;
	}
	else if (!VectorCompare (e->origin, e->currentorigin) || !VectorCompare (e->angles, e->currentangles)) // origin/angles changed, start new lerp
	{
		e->movelerpstart = cl.time;
		VectorCopy (e->currentorigin, e->previousorigin);
		VectorCopy (e->origin,  e->currentorigin);
		VectorCopy (e->currentangles, e->previousangles);
		VectorCopy (e->angles,  e->currentangles);
	}

	//set up values
	if (r_lerpmove.value && e != &cl.viewent && e->lerpflags & LERP_MOVESTEP)
	{
		if (e->lerpflags & LERP_FINISH)
			blend = CLAMP (0, (cl.time - e->movelerpstart) / (e->lerpfinish - e->movelerpstart), 1);
		else
			blend = CLAMP (0, (cl.time - e->movelerpstart) / 0.1, 1);

		//translation
		VectorSubtract (e->currentorigin, e->previousorigin, d);
		lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
		lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
		lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

		//rotation
		VectorSubtract (e->currentangles, e->previousangles, d);
		for (i = 0; i < 3; i++)
		{
			if (d[i] > 180)  d[i] -= 360;
			if (d[i] < -180) d[i] += 360;
		}
		lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
		lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
		lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
	}
	else //don't lerp
	{
		VectorCopy (e->origin, lerpdata->origin);
		VectorCopy (e->angles, lerpdata->angles);
	}
}

/*
=================
R_SetupAliasLighting -- johnfitz -- broken out from R_DrawAliasModel and rewritten
=================
*/
void R_SetupAliasLighting (entity_t	*e)
{
	vec3_t		dist;
	float		add;
	int			i;
	int		quantizedangle;
	float		radiansangle;
	vec3_t		lpos;

	VectorCopy (e->origin, lpos);
	// start the light trace from slightly above the origin
	// this helps with models whose origin is below ground level, but are otherwise visible
	// (e.g. some of the candles in the DOTM start map, which would otherwise appear black)
	lpos[2] += e->model->maxs[2] * 0.5f;
	R_LightPoint (lpos);

	//add dlights
	for (i=0 ; i<MAX_DLIGHTS ; i++)
	{
		if (cl_dlights[i].die >= cl.time)
		{
			VectorSubtract (currententity->origin, cl_dlights[i].origin, dist);
			add = cl_dlights[i].radius - VectorLength(dist);
			if (add > 0)
				VectorMA (lightcolor, add, cl_dlights[i].color, lightcolor);
		}
	}

	// minimum light value on gun (24)
	if (e == &cl.viewent)
	{
		add = 72.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			lightcolor[0] += add / 3.0f;
			lightcolor[1] += add / 3.0f;
			lightcolor[2] += add / 3.0f;
		}
	}

	// minimum light value on players (8)
	if (currententity > cl_entities && currententity <= cl_entities + cl.maxclients)
	{
		add = 24.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			lightcolor[0] += add / 3.0f;
			lightcolor[1] += add / 3.0f;
			lightcolor[2] += add / 3.0f;
		}
	}

	// clamp lighting so it doesn't overbright as much (96)
	if (overbright)
	{
		add = 288.0f / (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add < 1.0f)
			VectorScale(lightcolor, add, lightcolor);
	}

	//hack up the brightness when fullbrights but no overbrights (256)
	if (gl_fullbrights.value && !gl_overbright_models.value)
		if (e->model->flags & MOD_FBRIGHTHACK)
		{
			lightcolor[0] = 256.0f;
			lightcolor[1] = 256.0f;
			lightcolor[2] = 256.0f;
		}

	quantizedangle = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);

//ericw -- shadevector is passed to the shader to compute shadedots inside the
//shader, see GLAlias_CreateShaders()
	radiansangle = (quantizedangle / 16.0) * 2.0 * 3.14159;
	shadevector[0] = cos(-radiansangle);
	shadevector[1] = sin(-radiansangle);
	shadevector[2] = 1;
	VectorNormalize(shadevector);
//ericw --

	shadedots = r_avertexnormal_dots[quantizedangle];
	VectorScale (lightcolor, 1.0f / 200.0f, lightcolor);
}

/*
=================
R_DrawAliasModel -- johnfitz -- almost completely rewritten
=================
*/
void R_DrawAliasModel (entity_t *e)
{
	aliashdr_t	*paliashdr;
	int			i, anim, skinnum;
	gltexture_t	*tx, *fb;
	lerpdata_t	lerpdata;
	qboolean	alphatest = !!(e->model->flags & MF_HOLEY);
	float		fovscale = 1.0f;
	float		model_matrix[16];
	float		translation_matrix[16];
	float		scale_matrix[16];
	float		mvp[16];

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	//
	// cull it
	//
	if (R_CullModelForEntity(e))
		return;

	//
	// transform it
	//
	if (e == &cl.viewent && scr_fov.value > 90.f && cl_gun_fovscale.value)
		fovscale = tan(scr_fov.value * (0.5f * M_PI / 180.f));

	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles);

	TranslationMatrix (translation_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
	MatrixMultiply (model_matrix, translation_matrix);

	ScaleMatrix (scale_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);
	MatrixMultiply (model_matrix, scale_matrix);
	
	//
	// random stuff
	//
	overbright = gl_overbright_models.value;
	shading = true;

	//
	// set up for alpha blending
	//
	if (r_lightmap_cheatsafe) //no alpha in drawflat or lightmap mode
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE(e->alpha);

	if (entalpha == 0)
		return;

	//
	// set up lighting
	//
	rs_aliaspolys += paliashdr->numtris;
	R_SetupAliasLighting (e);

	//
	// set up textures
	//
	anim = (int)(cl.time*10) & 3;
	skinnum = e->skinnum;
	if ((skinnum >= paliashdr->numskins) || (skinnum < 0))
	{
		Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", skinnum, e->model->name);
		// ericw -- display skin 0 for winquake compatibility
		skinnum = 0;
	}
	tx = paliashdr->gltextures[skinnum][anim];
	fb = paliashdr->fbtextures[skinnum][anim];
	if (e->colormap != vid.colormap && !gl_nocolors.value)
	{
		i = e - cl_entities;
		if (i >= 1 && i<=cl.maxclients /* && !strcmp (currententity->model->name, "progs/player.mdl") */)
		    tx = playertextures[i - 1];
	}
	if (!gl_fullbrights.value)
		fb = NULL;

	//
	// draw it
	//

	memcpy(mvp, r_matviewproj, 16 * sizeof(float));
	MatrixMultiply (mvp, model_matrix);

	if (r_fullbright_cheatsafe)
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;

	if (r_lightmap_cheatsafe)
	{
		tx = greytexture;
		fb = blacktexture;
	}

	if (!fb)
		fb = blacktexture;

	GL_BeginGroup (e->model->name);

	GL_DrawAliasFrame_GLSL (paliashdr, lerpdata, tx, fb, mvp);

	GL_EndGroup();
}

//johnfitz -- values for shadow matrix
#define SHADOW_SKEW_X -0.7 //skew along x axis. -0.7 to mimic glquake shadows
#define SHADOW_SKEW_Y 0 //skew along y axis. 0 to mimic glquake shadows
#define SHADOW_VSCALE 0 //0=completely flat
#define SHADOW_HEIGHT 0.1 //how far above the floor to render the shadow
//johnfitz

/*
=================
R_DrawAliasModel_ShowTris -- johnfitz
=================
*/
void R_DrawAliasModel_ShowTris (entity_t *e)
{
	aliashdr_t	*paliashdr;
	lerpdata_t	lerpdata;
	float		fovscale = 1.0f;
	float		model_matrix[16];
	float		translation_matrix[16];
	float		scale_matrix[16];
	float		mvp[16];

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	//
	// cull it
	//
	if (R_CullModelForEntity(e))
		return;

	//
	// transform it
	//
	if (e == &cl.viewent && scr_fov.value > 90.f && cl_gun_fovscale.value)
		fovscale = tan(scr_fov.value * (0.5f * M_PI / 180.f));

	R_EntityMatrix (model_matrix, lerpdata.origin, lerpdata.angles);

	TranslationMatrix (translation_matrix, paliashdr->scale_origin[0], paliashdr->scale_origin[1] * fovscale, paliashdr->scale_origin[2] * fovscale);
	MatrixMultiply (model_matrix, translation_matrix);

	ScaleMatrix (scale_matrix, paliashdr->scale[0], paliashdr->scale[1] * fovscale, paliashdr->scale[2] * fovscale);
	MatrixMultiply (model_matrix, scale_matrix);

	if (r_lightmap_cheatsafe) //no alpha in drawflat or lightmap mode
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE(e->alpha);

	if (entalpha == 0)
		return;

	entalpha = 1.0f;

	//
	// draw it
	//

	memcpy(mvp, r_matviewproj, 16 * sizeof(float));
	MatrixMultiply (mvp, model_matrix);

	GL_DrawAliasFrame_GLSL (paliashdr, lerpdata, whitetexture, whitetexture, mvp);
}
