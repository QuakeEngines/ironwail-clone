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

float	entalpha; //johnfitz

//johnfitz -- struct for passing lerp information to drawing functions
typedef struct {
	short pose1;
	short pose2;
	float blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;
//johnfitz

typedef struct aliasinstance_s {
	float		mvp[16];
	vec3_t		lightcolor;
	float		alpha;
	float		shadeangle;
	float		blend;
	int32_t		pose1;
	int32_t		pose2;
} aliasinstance_t;

struct ibuf_s {
	int			count;
	entity_t	*ent;
	gltexture_t	*texture;
	gltexture_t	*fullbright;

	struct {
		vec4_t	fog;
		int		use_alpha_test;
		int		padding[3];
	} global;
	aliasinstance_t inst[MAX_INSTANCES];
} ibuf;

static GLuint r_alias_program;

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
	#define ALIAS_INSTANCE_BUFFER\
		"struct InstanceData\n"\
		"{\n"\
		"	mat4	MVP;\n"\
		"	vec4	LightColor; // xyz=LightColor w=Alpha\n"\
		"	float	ShadeAngle;\n"\
		"	float	Blend;\n"\
		"	int		Pose1;\n"\
		"	int		Pose2;\n"\
		"};\n"\
		"\n"\
		"layout(std430, binding=1) restrict readonly buffer InstanceBuffer\n"\
		"{\n"\
		"	vec4	Fog;\n"\
		"	int		UseAlphaTest;\n"\
		"	int		padding[3];\n"\
		"	InstanceData instances[];\n"\
		"};\n"\

	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		ALIAS_INSTANCE_BUFFER
		"\n"
		"layout(std430, binding=2) restrict readonly buffer PoseBuffer\n"
		"{\n"
		"	uvec2 PackedPosNor[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=3) restrict readonly buffer UVBuffer\n"
		"{\n"
		"	vec2 TexCoords[];\n"
		"};\n"
		"\n"
		"struct Pose\n"
		"{\n"
		"	vec3 pos;\n"
		"	vec3 nor;\n"
		"};\n"
		"\n"
		"Pose GetPose(uint index)\n"
		"{\n"
		"	uvec2 data = PackedPosNor[index + gl_VertexID];\n"
		"	return Pose(vec3((data.xxx >> uvec3(0, 8, 16)) & 255), unpackSnorm4x8(data.y).xyz);\n"
		"}\n"
		"\n"
		"float r_avertexnormal_dot(vec3 vertexnormal, vec3 dir) // from MH \n"
		"{\n"
		"	float dot = dot(vertexnormal, dir);\n"
		"	// wtf - this reproduces anorm_dots within as reasonable a degree of tolerance as the >= 0 case\n"
		"	if (dot < 0.0)\n"
		"		return 1.0 + dot * (13.0 / 44.0);\n"
		"	else\n"
		"		return 1.0 + dot;\n"
		"}\n"
		"\n"
		"layout(location=0) out vec2 out_texcoord;\n"
		"layout(location=1) out vec4 out_color;\n"
		"layout(location=2) out float out_fogdist;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	InstanceData inst = instances[gl_InstanceID];\n"
		"	out_texcoord = TexCoords[gl_VertexID];\n"
		"	Pose pose1 = GetPose(inst.Pose1);\n"
		"	Pose pose2 = GetPose(inst.Pose2);\n"
		"	vec3 lerpedVert = mix(pose1.pos, pose2.pos, inst.Blend);\n"
		"	gl_Position = inst.MVP * vec4(lerpedVert, 1.0);\n"
		"	out_fogdist = gl_Position.w;\n"
		"	vec3 shadevector;\n"
		"	shadevector[0] = cos(inst.ShadeAngle);\n"
		"	shadevector[1] = sin(inst.ShadeAngle);\n"
		"	shadevector[2] = 1.0;\n"
		"	shadevector = normalize(shadevector);\n"
		"	float dot1 = r_avertexnormal_dot(pose1.nor, shadevector);\n"
		"	float dot2 = r_avertexnormal_dot(pose2.nor, shadevector);\n"
		"	out_color = inst.LightColor * vec4(vec3(mix(dot1, dot2, inst.Blend)), 1.0);\n"
		"}\n";

	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		ALIAS_INSTANCE_BUFFER
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"layout(binding=1) uniform sampler2D FullbrightTex;\n"
		"\n"
		"layout(location=0) in vec2 in_texcoord;\n"
		"layout(location=1) in vec4 in_color;\n"
		"layout(location=2) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture(Tex, in_texcoord);\n"
		"	if (UseAlphaTest != 0 && result.a < 0.666)\n"
		"		discard;\n"
		"	result *= in_color * 2.0;\n"
		"	result += texture(FullbrightTex, in_texcoord);\n"
		"	result = clamp(result, 0.0, 1.0);\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	result.a = in_color.a;\n" // FIXME: This will make almost transparent things cut holes though heavy fog
		"	out_fragcolor = result;\n"
		"}\n";

	r_alias_program = GL_CreateProgram (vertSource, fragSource, "alias");
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
			VectorSubtract (e->origin, cl_dlights[i].origin, dist);
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
	if (e > cl_entities && e <= cl_entities + cl.maxclients)
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
	if (gl_overbright_models.value)
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

	VectorScale (lightcolor, 1.0f / 200.0f, lightcolor);
}

/*
=================
R_DrawAliasModel_Real
=================
*/
void R_DrawAliasModel_Real (entity_t *e, qboolean showtris)
{
	aliashdr_t	*paliashdr;
	int			i, anim, skinnum;
	gltexture_t	*tx, *fb;
	lerpdata_t	lerpdata;
	int			quantizedangle;
	float		radiansangle;
	float		fovscale = 1.0f;
	float		model_matrix[16];
	float		translation_matrix[16];
	float		scale_matrix[16];
	aliasinstance_t	*instance;

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (lerpdata.pose1 == lerpdata.pose2)
		lerpdata.blend = 0.f;

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
		if (i >= 1 && i<=cl.maxclients /* && !strcmp (e->model->name, "progs/player.mdl") */)
		    tx = playertextures[i - 1];
	}
	if (!gl_fullbrights.value)
		fb = blacktexture;

	//
	// draw it
	//

	if (r_fullbright_cheatsafe)
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 0.5f;

	if (r_lightmap_cheatsafe)
	{
		tx = greytexture;
		fb = blacktexture;
	}

	if (!fb)
		fb = blacktexture;

	if (showtris)
	{
		tx = blacktexture;
		fb = whitetexture;
		lightcolor[0] = lightcolor[2] = lightcolor[3] = 0.5f;
		entalpha = 1.f;
	}

	R_NewModelInstance (mod_alias);

	if (ibuf.count)
	{
		if (ibuf.count == countof(ibuf.inst) ||
			ibuf.ent->model != e->model ||
			ibuf.texture != tx ||
			ibuf.fullbright != fb)
		{
			R_FlushAliasInstances ();
		}
	}

	if (!ibuf.count)
	{
		ibuf.ent        = e;
		ibuf.texture    = tx;
		ibuf.fullbright = fb;
	}

	instance = &ibuf.inst[ibuf.count++];

	memcpy (instance->mvp, r_matviewproj, 16 * sizeof(float));
	MatrixMultiply(instance->mvp, model_matrix);

	quantizedangle = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);
	radiansangle = quantizedangle * (-2.0f * M_PI / SHADEDOT_QUANT);

	instance->lightcolor[0] = lightcolor[0];
	instance->lightcolor[1] = lightcolor[1];
	instance->lightcolor[2] = lightcolor[2];
	instance->alpha = entalpha;
	instance->shadeangle = radiansangle;
	instance->blend = lerpdata.blend;
	instance->pose1 = lerpdata.pose1 * paliashdr->numverts_vbo;
	instance->pose2 = lerpdata.pose2 * paliashdr->numverts_vbo;
}

/*
=================
R_FlushAliasInstances
=================
*/
void R_FlushAliasInstances (void)
{
	qmodel_t	*model;
	aliashdr_t	*paliashdr;
	unsigned	state;
	GLuint		buf;
	GLbyte		*ofs;
	size_t		ibuf_size;

	if (!ibuf.count)
		return;

	model = ibuf.ent->model;
	paliashdr = (aliashdr_t *)Mod_Extradata (model);

	GL_BeginGroup (model->name);

	GL_UseProgram (r_alias_program);

	state = GLS_CULL_BACK | GLS_ATTRIBS(0);
	if (ENTALPHA_DECODE(ibuf.ent->alpha) == 1.f)
		state |= GLS_BLEND_OPAQUE;
	else
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	GL_SetState (state);

	memcpy(ibuf.global.fog, fog_data, 4 * sizeof(float));
	ibuf.global.use_alpha_test = (model->flags & MF_HOLEY) ? 1 : 0;

	ibuf_size = sizeof(ibuf.global) + sizeof(ibuf.inst[0]) * ibuf.count;
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &ibuf.global, ibuf_size, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, ibuf_size);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, model->meshvbo, model->vboxyzofs, sizeof (meshxyz_t) * paliashdr->numverts_vbo * paliashdr->numposes);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, model->meshvbo, model->vbostofs, sizeof (meshst_t) * paliashdr->numverts_vbo);

	GL_Bind (GL_TEXTURE0, ibuf.texture);
	GL_Bind (GL_TEXTURE1, ibuf.fullbright);

	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, model->meshindexesvbo);
	GL_DrawElementsInstancedFunc (GL_TRIANGLES, paliashdr->numindexes, GL_UNSIGNED_SHORT, (void *)(intptr_t)model->vboindexofs, ibuf.count);

	rs_aliaspasses += paliashdr->numtris * ibuf.count;
	ibuf.count = 0;

	GL_EndGroup();
}

/*
=================
R_ClearAliasInstances
=================
*/
void R_ClearAliasInstances (void)
{
	ibuf.count = 0;
}

/*
=================
R_DrawAliasModel
=================
*/
void R_DrawAliasModel (entity_t *e)
{
	R_DrawAliasModel_Real (e, false);
}

/*
=================
R_DrawAliasModel_ShowTris -- johnfitz
=================
*/
void R_DrawAliasModel_ShowTris (entity_t *e)
{
	R_DrawAliasModel_Real (e, true);
}
