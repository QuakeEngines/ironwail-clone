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
//r_sprite.c -- sprite model rendering

#include "quakedef.h"

typedef struct spritevert_t {
	vec3_t		pos;
	float		uv[2];
} spritevert_t;

static GLuint r_sprite_program;
static GLushort batchindices[6 * MAX_INSTANCES];
static qboolean batchindices_init = false;

/*
=============
GLSprite_CreateShaders
=============
*/
void GLSprite_CreateShaders (void)
{
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) uniform mat4 MVP;\n"
		"\n"
		"layout(location=0) in vec4 in_pos;\n"
		"layout(location=1) in vec2 in_uv;\n"
		"\n"
		"layout(location=0) out vec2 out_uv;\n"
		"layout(location=1) out float out_fogdist;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = MVP * in_pos;\n"
		"	out_fogdist = gl_Position.w;\n"
		"	out_uv = in_uv;\n"
		"}\n";

	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"layout(location=2) uniform vec4 Fog;\n"
		"\n"
		"layout(location=0) in vec2 in_uv;\n"
		"layout(location=1) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture(Tex, in_uv);\n"
		"	if (result.a < 0.666)\n"
		"		discard;\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	out_fragcolor = result;\n"
		"}\n";

	r_sprite_program = GL_CreateProgram (vertSource, fragSource, "sprite");
}

/*
================
R_InitSpriteIndices
================
*/
static void R_InitSpriteIndices (void)
{
	int i;
	if (batchindices_init)
		return;

	for (i = 0; i < MAX_INSTANCES; i++)
	{
		batchindices[i*6 + 0] = i*4 + 0;
		batchindices[i*6 + 1] = i*4 + 1;
		batchindices[i*6 + 2] = i*4 + 2;
		batchindices[i*6 + 3] = i*4 + 0;
		batchindices[i*6 + 4] = i*4 + 2;
		batchindices[i*6 + 5] = i*4 + 3;
	}

	batchindices_init = true;
}

/*
================
R_GetSpriteFrame
================
*/
mspriteframe_t *R_GetSpriteFrame (entity_t *currentent)
{
	msprite_t		*psprite;
	mspritegroup_t	*pspritegroup;
	mspriteframe_t	*pspriteframe;
	int				i, numframes, frame;
	float			*pintervals, fullinterval, targettime, time;

	psprite = (msprite_t *) currentent->model->cache.data;
	frame = currentent->frame;

	if ((frame >= psprite->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_DrawSprite: no such frame %d for '%s'\n", frame, currentent->model->name);
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes-1];

		time = cl.time + currentent->syncbase;

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i=0 ; i<(numframes-1) ; i++)
		{
			if (pintervals[i] > targettime)
				break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}

/*
=================
R_DrawSpriteModel
=================
*/
static void R_DrawSpriteModel_Real (entity_t *e, qboolean showtris)
{
	msprite_t		*psprite;
	mspriteframe_t	*frame;
	instance_t		*instance;

	//TODO: frustum cull it?

	frame = R_GetSpriteFrame (e);
	psprite = (msprite_t *) e->model->cache.data;

	if (num_model_instances)
	{
		instance_t *first = &model_instances[0];
		if (num_model_instances == countof(model_instances) ||
			first->ent->model != e->model ||
			first->data.sprite.frame->gltexture != frame->gltexture)
		{
			R_FlushModelInstances ();
		}
	}

	instance = &model_instances[num_model_instances++];

	instance->ent                   = e;
	instance->data.sprite.frame     = frame;
	instance->data.sprite.showtris  = showtris;
}

/*
=================
R_DrawSpriteInstances
=================
*/
static int lastfogframe = 0;
void R_DrawSpriteInstances (instance_t *inst, int count)
{
	qmodel_t		*model = inst->ent->model;
	qboolean		showtris = inst->data.sprite.showtris;
	vec3_t			point, v_forward, v_right, v_up;
	msprite_t		*psprite;
	float			*s_up, *s_right;
	float			angle, sr, cr;
	spritevert_t	verts[4 * MAX_INSTANCES];
	int				i, j;
	GLuint			buf;
	GLbyte*			ofs;

	R_InitSpriteIndices ();

	psprite = (msprite_t *) model->cache.data;

	for (i = 0; i < count; i++)
	{
		entity_t		*e = inst[i].ent;
		mspriteframe_t	*frame = inst->data.sprite.frame;

		switch(psprite->type)
		{
		case SPR_VP_PARALLEL_UPRIGHT: //faces view plane, up is towards the heavens
			v_up[0] = 0;
			v_up[1] = 0;
			v_up[2] = 1;
			s_up = v_up;
			s_right = vright;
			break;
		case SPR_FACING_UPRIGHT: //faces camera origin, up is towards the heavens
			VectorSubtract(e->origin, r_origin, v_forward);
			v_forward[2] = 0;
			VectorNormalizeFast(v_forward);
			v_right[0] = v_forward[1];
			v_right[1] = -v_forward[0];
			v_right[2] = 0;
			v_up[0] = 0;
			v_up[1] = 0;
			v_up[2] = 1;
			s_up = v_up;
			s_right = v_right;
			break;
		case SPR_VP_PARALLEL: //faces view plane, up is towards the top of the screen
			s_up = vup;
			s_right = vright;
			break;
		case SPR_ORIENTED: //pitch yaw roll are independent of camera
			AngleVectors (e->angles, v_forward, v_right, v_up);
			s_up = v_up;
			s_right = v_right;
			break;
		case SPR_VP_PARALLEL_ORIENTED: //faces view plane, but obeys roll value
			angle = e->angles[ROLL] * M_PI_DIV_180;
			sr = sin(angle);
			cr = cos(angle);
			v_right[0] = vright[0] * cr + vup[0] * sr;
			v_right[1] = vright[1] * cr + vup[1] * sr;
			v_right[2] = vright[2] * cr + vup[2] * sr;
			v_up[0] = vright[0] * -sr + vup[0] * cr;
			v_up[1] = vright[1] * -sr + vup[1] * cr;
			v_up[2] = vright[2] * -sr + vup[2] * cr;
			s_up = v_up;
			s_right = v_right;
			break;
		default:
			return;
		}

		j = 0;
		#define ADD_VERTEX(uvx, uvy, p)			\
			VectorCopy(p, verts[i*4+j].pos);	\
			verts[i*4+j].uv[0] = uvx;			\
			verts[i*4+j].uv[1] = uvy;			\
			++j

		VectorMA (e->origin, frame->down, s_up, point);
		VectorMA (point, frame->left, s_right, point);
		ADD_VERTEX (0, frame->tmax, point);

		VectorMA (e->origin, frame->up, s_up, point);
		VectorMA (point, frame->left, s_right, point);
		ADD_VERTEX (0, 0, point);

		VectorMA (e->origin, frame->up, s_up, point);
		VectorMA (point, frame->right, s_right, point);
		ADD_VERTEX (frame->smax, 0, point);

		VectorMA (e->origin, frame->down, s_up, point);
		VectorMA (point, frame->right, s_right, point);
		ADD_VERTEX (frame->smax, frame->tmax, point);

		#undef ADD_VERTEX
	}

	GL_BeginGroup (model->name);

	//johnfitz: offset decals
	if (psprite->type == SPR_ORIENTED)
		GL_PolygonOffset (OFFSET_DECAL);

	GL_UseProgram (r_sprite_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matviewproj);

	if (lastfogframe != r_framecount)
	{
		lastfogframe = r_framecount;
		GL_Uniform4fvFunc (2, 1, fog_data);
	}

	if (showtris)
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	else
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS(2));

	GL_Bind (GL_TEXTURE0, showtris ? whitetexture : inst->data.sprite.frame->gltexture);

	GL_Upload (GL_ARRAY_BUFFER, verts, sizeof(verts[0]) * 4 * count, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(verts[0]), ofs + offsetof(spritevert_t, pos));
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(verts[0]), ofs + offsetof(spritevert_t, uv));

	GL_Upload (GL_ELEMENT_ARRAY_BUFFER, batchindices, sizeof(batchindices[0]) * 6 * count, &buf, &ofs);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
	glDrawElements (GL_TRIANGLES, 6 * count, GL_UNSIGNED_SHORT, ofs);

	//johnfitz: offset decals
	if (psprite->type == SPR_ORIENTED)
		GL_PolygonOffset (OFFSET_NONE);

	GL_EndGroup ();
}

/*
=================
R_DrawSpriteModel
=================
*/
void R_DrawSpriteModel (entity_t *e)
{
	R_DrawSpriteModel_Real (e, false);
}

/*
=================
R_DrawSpriteModel_ShowTris
=================
*/
void R_DrawSpriteModel_ShowTris (entity_t *e)
{
	R_DrawSpriteModel_Real (e, true);
}
