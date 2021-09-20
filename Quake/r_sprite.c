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
R_DrawSpriteModel -- johnfitz -- rewritten: now supports all orientations
=================
*/
static int lastfogframe = 0;
static void R_DrawSpriteModel_Real (entity_t *e, qboolean showtris)
{
	vec3_t			point, v_forward, v_right, v_up;
	msprite_t		*psprite;
	mspriteframe_t	*frame;
	float			*s_up, *s_right;
	float			angle, sr, cr;
	spritevert_t	verts[4];
	int				i = 0;
	GLuint			buf;
	GLbyte*			ofs;

	//TODO: frustum cull it?

	frame = R_GetSpriteFrame (e);
	psprite = (msprite_t *) currententity->model->cache.data;

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
		VectorSubtract(currententity->origin, r_origin, v_forward);
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
		AngleVectors (currententity->angles, v_forward, v_right, v_up);
		s_up = v_up;
		s_right = v_right;
		break;
	case SPR_VP_PARALLEL_ORIENTED: //faces view plane, but obeys roll value
		angle = currententity->angles[ROLL] * M_PI_DIV_180;
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

	GL_BeginGroup (e->model->name);

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

	GL_Bind (GL_TEXTURE0, showtris ? whitetexture : frame->gltexture);

	#define ADD_VERTEX(uvx, uvy, p)		\
		VectorCopy(p, verts[i].pos);	\
		verts[i].uv[0] = uvx;			\
		verts[i].uv[1] = uvy;			\
		++i

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

	GL_Upload (GL_ARRAY_BUFFER, verts, sizeof(verts), &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(verts[0]), ofs + offsetof(spritevert_t, pos));
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(verts[0]), ofs + offsetof(spritevert_t, uv));
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);

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
