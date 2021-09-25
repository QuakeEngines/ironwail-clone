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
// r_world.c: world model rendering

#include "quakedef.h"

extern cvar_t gl_fullbrights, gl_overbright, r_oldskyleaf, r_showtris; //johnfitz

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

//==============================================================================
//
// SETUP CHAINS
//
//==============================================================================

/*
================
R_ClearTextureChains -- ericw 

clears texture chains for all textures used by the given model, and also
clears the lightmap chains
================
*/
void R_ClearTextureChains (qmodel_t *mod, texchain_t chain)
{
	int i;

	// set all chains to null
	for (i=0 ; i<mod->numtextures ; i++)
		if (mod->textures[i])
			mod->textures[i]->texturechains[chain] = NULL;

	// clear lightmap chains
	for (i=0 ; i<lightmap_count ; i++)
		lightmaps[i].polys = NULL;
}

/*
================
R_ChainSurface -- ericw -- adds the given surface to its texture chain
================
*/
void R_ChainSurface (msurface_t *surf, texchain_t chain)
{
	surf->texturechain = surf->texinfo->texture->texturechains[chain];
	surf->texinfo->texture->texturechains[chain] = surf;
}

/*
================
R_BackFaceCull -- johnfitz -- returns true if the surface is facing away from vieworg
================
*/
qboolean R_BackFaceCull (msurface_t *surf)
{
	double dot;

	if (surf->plane->type < 3)
		dot = r_refdef.vieworg[surf->plane->type] - surf->plane->dist;
	else
		dot = DotProduct (r_refdef.vieworg, surf->plane->normal) - surf->plane->dist;

	if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
		return true;

	return false;
}

#ifdef USE_SSE2
/*
===============
R_BackFaceCullSIMD

Performs backface culling for 8 planes
===============
*/
int R_BackFaceCullSIMD (soa_plane_t plane)
{
	__m128 pos = _mm_loadu_ps(r_refdef.vieworg);

	__m128 px = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(0, 0, 0, 0));
	__m128 v0 = _mm_mul_ps(_mm_loadu_ps(plane + 0), px);
	__m128 v1 = _mm_mul_ps(_mm_loadu_ps(plane + 4), px);

	__m128 py = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(1, 1, 1, 1));
	v0 = _mm_add_ps(v0, _mm_mul_ps(_mm_loadu_ps(plane +  8), py));
	v1 = _mm_add_ps(v1, _mm_mul_ps(_mm_loadu_ps(plane + 12), py));

	__m128 pz = _mm_shuffle_ps(pos, pos, _MM_SHUFFLE(2, 2, 2, 2));
	v0 = _mm_add_ps(v0, _mm_mul_ps(_mm_loadu_ps(plane + 16), pz));
	v1 = _mm_add_ps(v1, _mm_mul_ps(_mm_loadu_ps(plane + 20), pz));

	__m128 pd0 = _mm_loadu_ps(plane + 24);
	__m128 pd1 = _mm_loadu_ps(plane + 28);

	return _mm_movemask_ps(_mm_cmplt_ps(pd0, v0)) | (_mm_movemask_ps(_mm_cmplt_ps(pd1, v1)) << 4);
}

/*
===============
R_CullBoxSIMD

Performs frustum culling for 8 bounding boxes
===============
*/
int R_CullBoxSIMD (soa_aabb_t box, int activelanes)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		mplane_t *p;
		byte signbits;
		int ofs;

		if (activelanes == 0)
			break;

		p = frustum + i;
		signbits = p->signbits;

		__m128 vplane = _mm_loadu_ps(p->normal);

		ofs = signbits & 1 ? 0 : 8; // x min/max
		__m128 px = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(0, 0, 0, 0));
		__m128 v0 = _mm_mul_ps(_mm_loadu_ps(box + ofs), px);
		__m128 v1 = _mm_mul_ps(_mm_loadu_ps(box + ofs + 4), px);

		ofs = signbits & 2 ? 16 : 24; // y min/max
		__m128 py = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(1, 1, 1, 1));
		v0 = _mm_add_ps(v0, _mm_mul_ps(_mm_loadu_ps(box + ofs), py));
		v1 = _mm_add_ps(v1, _mm_mul_ps(_mm_loadu_ps(box + ofs + 4), py));

		ofs = signbits & 4 ? 32 : 40; // z min/max
		__m128 pz = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(2, 2, 2, 2));
		v0 = _mm_add_ps(v0, _mm_mul_ps(_mm_loadu_ps(box + ofs), pz));
		v1 = _mm_add_ps(v1, _mm_mul_ps(_mm_loadu_ps(box + ofs + 4), pz));

		__m128 pd = _mm_shuffle_ps(vplane, vplane, _MM_SHUFFLE(3, 3, 3, 3));
		activelanes &= _mm_movemask_ps(_mm_cmplt_ps(pd, v0)) | (_mm_movemask_ps(_mm_cmplt_ps(pd, v1)) << 4);
	}

	return activelanes;
}
#endif // defined(USE_SSE2)

#if defined(USE_SIMD)
/*
===============
R_MarkVisSurfacesSIMD
===============
*/
void R_MarkVisSurfacesSIMD (byte *vis)
{
	msurface_t	*surf;
	int			i, j, k;
	int			numleafs = cl.worldmodel->numleafs;
	int			numsurfaces = cl.worldmodel->numsurfaces;
	soa_aabb_t	*leafbounds = cl.worldmodel->soa_leafbounds;

	memset(cl.worldmodel->surfvis, 0, (cl.worldmodel->numsurfaces + 7) >> 3);

	// iterate through leaves, marking surfaces
	for (i = 0; i < numleafs; i += 8)
	{
		int mask = vis[i>>3];
		if (mask == 0)
			continue;

		mask = R_CullBoxSIMD(leafbounds[i>>3], mask);
		if (mask == 0)
			continue;

		for (j = 0; j < 8 && i + j < numleafs; j++)
		{
			if (!(mask & (1 << j)))
				continue;

			mleaf_t *leaf = &cl.worldmodel->leafs[1 + i + j];
			if (leaf->contents != CONTENTS_SKY || r_oldskyleaf.value)
			{
				byte *surfmask = cl.worldmodel->surfvis;
				int nummarksurfaces = leaf->nummarksurfaces;
				int *marksurfaces = leaf->firstmarksurface;
				for (k = 0; k < nummarksurfaces; k++)
				{
					int index = marksurfaces[k];
					surfmask[index >> 3] |= 1 << (index & 7);
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

	vis = cl.worldmodel->surfvis;
	for (i = 0; i < numsurfaces; i += 8)
	{
		int mask = vis[i >> 3];
		if (mask == 0)
			continue;

		mask &= R_BackFaceCullSIMD(cl.worldmodel->soa_surfplanes[i >> 3]);
		if (mask == 0)
			continue;

		for (j = 0; j < 8; j++)
		{
			if (!(mask & (1 << j)))
				continue;

			surf = &cl.worldmodel->surfaces[i + j];
			rs_brushpolys++; //count wpolys here
			surf->visframe = r_visframecount;
			R_ChainSurface(surf, chain_world);
			R_RenderDynamicLightmaps(surf);
		}
	}
}
#endif // defined(USE_SIMD)

/*
===============
R_MarkVisSurfaces
===============
*/
void R_MarkVisSurfaces (byte* vis)
{
	int			i, j;
	msurface_t	*surf;
	mleaf_t		*leaf;

	leaf = &cl.worldmodel->leafs[1];
	for (i=0 ; i<cl.worldmodel->numleafs ; i++, leaf++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			if (R_CullBox(leaf->minmaxs, leaf->minmaxs + 3))
				continue;

			if (r_oldskyleaf.value || leaf->contents != CONTENTS_SKY)
			{
				for (j=0; j<leaf->nummarksurfaces; j++)
				{
					surf = &cl.worldmodel->surfaces[leaf->firstmarksurface[j]];
					if (surf->visframe != r_visframecount)
					{
						surf->visframe = r_visframecount;
						if (!R_BackFaceCull (surf))
						{
							rs_brushpolys++; //count wpolys here
							R_ChainSurface(surf, chain_world);
							R_RenderDynamicLightmaps(surf);
						}
					}
				}
			}

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}
}

/*
===============
R_MarkSurfaces -- johnfitz -- mark surfaces based on PVS and rebuild texture chains
===============
*/
void R_MarkSurfaces (void)
{
	byte		*vis;
	int			i;
	qboolean	nearwaterportal;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i=0; i < r_viewleaf->nummarksurfaces; i++)
		if (cl.worldmodel->surfaces[r_viewleaf->firstmarksurface[i]].flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (r_novis.value || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		vis = Mod_NoVisPVS (cl.worldmodel);
	else if (nearwaterportal)
		vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	r_visframecount++;

	// set all chains to null
	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
		if (cl.worldmodel->textures[i])
			cl.worldmodel->textures[i]->texturechains[chain_world] = NULL;

	// iterate through leaves, marking surfaces
#if defined(USE_SIMD)
	if (use_simd)
		R_MarkVisSurfacesSIMD(vis);
	else
#endif
		R_MarkVisSurfaces(vis);
}

//==============================================================================
//
// DRAW CHAINS
//
//==============================================================================


//==============================================================================
//
// VBO SUPPORT
//
//==============================================================================

static unsigned int R_NumTriangleIndicesForSurf (msurface_t *s)
{
	return 3 * (s->numedges - 2);
}

/*
================
R_TriangleIndicesForSurf

Writes out the triangle indices needed to draw s as a triangle list.
The number of indices it will write is given by R_NumTriangleIndicesForSurf.
================
*/
static void R_TriangleIndicesForSurf (msurface_t *s, unsigned int *dest)
{
	int i;
	for (i=2; i<s->numedges; i++)
	{
		*dest++ = s->vbo_firstvert;
		*dest++ = s->vbo_firstvert + i - 1;
		*dest++ = s->vbo_firstvert + i;
	}
}

#define MAX_BATCH_SIZE 16384

static unsigned int vbo_indices[MAX_BATCH_SIZE];
static unsigned int num_vbo_indices;

/*
================
R_ClearBatch
================
*/
void R_ClearBatch (void)
{
	num_vbo_indices = 0;
}

/*
================
R_FlushBatch

Draw the current batch if non-empty and clears it, ready for more R_BatchSurface calls.
================
*/
void R_FlushBatch (void)
{
	if (num_vbo_indices > 0)
	{
		GLuint buf;
		GLbyte *ofs;

		GL_Upload (GL_ELEMENT_ARRAY_BUFFER, vbo_indices, sizeof(vbo_indices[0]) * num_vbo_indices, &buf, &ofs);
		GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
		glDrawElements (GL_TRIANGLES, num_vbo_indices, GL_UNSIGNED_INT, ofs);

		num_vbo_indices = 0;
	}
}

/*
================
R_BatchSurface

Add the surface to the current batch, or just draw it immediately if we're not
using VBOs.
================
*/
void R_BatchSurface (msurface_t *s)
{
	int num_surf_indices;

	num_surf_indices = R_NumTriangleIndicesForSurf (s);

	if (num_vbo_indices + num_surf_indices > MAX_BATCH_SIZE)
		R_FlushBatch();

	R_TriangleIndicesForSurf (s, &vbo_indices[num_vbo_indices]);
	num_vbo_indices += num_surf_indices;
}

/*
================
GL_WaterAlphaForEntitySurface -- ericw
 
Returns the water alpha to use for the entity and surface combination.
================
*/
float GL_WaterAlphaForEntitySurface (entity_t *ent, msurface_t *s)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForSurface(s);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}

static GLuint r_world_program;
static GLuint r_water_program;

typedef struct {
	float	mvp[16];
	vec4_t	fog;
	int		use_alpha_test;
	float	alpha;
	float	time;
	int		padding;
} worlduniforms_t;

// uniforms used in vert shader

// uniforms used in frag shader
static GLuint texLoc;
static GLuint LMTexLoc;
static GLuint fullbrightTexLoc;

/*
=============
GLWorld_CreateShaders
=============
*/
void GLWorld_CreateShaders (void)
{
	#define WORLD_PARAM_BUFFER\
		"layout(std430, binding=0) restrict readonly buffer ParamBuffer\n"\
		"{\n"\
		"	mat4	MVP;\n"\
		"	vec4	Fog;\n"\
		"	bool	UseAlphaTest;\n"\
		"	float	Alpha;\n"\
		"	float	Time;\n"\
		"	int		padding;\n"\
		"};\n"\

	#define WORLD_VERTEX_BUFFER\
		"struct PackedVertex\n"\
		"{\n"\
		"	float data[7];\n"\
		"};\n"\
		"\n"\
		"layout(std430, binding=1) restrict readonly buffer VertexBuffer\n"\
		"{\n"\
		"	PackedVertex vertices[];\n"\
		"};\n"\
		"\n"\

	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		WORLD_PARAM_BUFFER
		"\n"
		WORLD_VERTEX_BUFFER
		"\n"
		"layout(location=0) out vec4 out_uv;\n"
		"layout(location=1) out float out_fogdist;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	PackedVertex vert = vertices[gl_VertexID];\n"
		"	gl_Position = MVP * vec4(vert.data[0], vert.data[1], vert.data[2], 1.0);\n"
		"	out_uv = vec4(vert.data[3], vert.data[4], vert.data[5], vert.data[6]);\n"
		"	out_fogdist = gl_Position.w;\n"
		"}\n";
	
	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"layout(binding=1) uniform sampler2D FullbrightTex;\n"
		"layout(binding=2) uniform sampler2D LMTex;\n"
		"\n"
		WORLD_PARAM_BUFFER
		"\n"
		"layout(location=0) in vec4 in_uv;\n"
		"layout(location=1) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture(Tex, in_uv.xy);\n"
		"	if (UseAlphaTest && result.a < 0.666)\n"
		"		discard;\n"
		"	result.rgb *= texture(LMTex, in_uv.zw).rgb * 2.0;\n"
		"	result.rgb += texture(FullbrightTex, in_uv.xy).rgb;\n"
		"	result = clamp(result, 0.0, 1.0);\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	result.a = Alpha;\n" // FIXME: This will make almost transparent things cut holes though heavy fog
		"	out_fragcolor = result;\n"
		"}\n";

	r_world_program = GL_CreateProgram (vertSource, fragSource, "world");
	
	if (r_world_program != 0)
	{
		// get uniform locations
		texLoc = GL_GetUniformLocation (&r_world_program, "Tex");
		LMTexLoc = GL_GetUniformLocation (&r_world_program, "LMTex");
		fullbrightTexLoc = GL_GetUniformLocation (&r_world_program, "FullbrightTex");
	}

	vertSource = \
		"#version 430\n"
		"\n"
		WORLD_PARAM_BUFFER
		"\n"
		WORLD_VERTEX_BUFFER
		"\n"
		"layout(location=0) out vec2 out_uv;\n"
		"layout(location=1) out float out_fogdist;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	PackedVertex vert = vertices[gl_VertexID];\n"
		"	gl_Position = MVP * vec4(vert.data[0], vert.data[1], vert.data[2], 1.0);\n"
		"	out_uv = vec2(vert.data[3], vert.data[4]);\n"
		"	out_fogdist = gl_Position.w;\n"
		"}\n";
	
	fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"\n"
		WORLD_PARAM_BUFFER
		"\n"
		"layout(location=0) in vec2 in_uv;\n"
		"layout(location=1) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec2 uv = in_uv * 2.0 + 0.125 * sin(in_uv.yx * (3.14159265 * 2.0) + Time);\n"
		"	vec4 result = texture(Tex, uv);\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	result.a *= Alpha;\n"
		"	out_fragcolor = result;\n"
		"}\n";

	r_water_program = GL_CreateProgram (vertSource, fragSource, "water");

	#undef WORLD_PARAM_BUFFER
	#undef WORLD_VERTEX_BUFFER
}

extern GLuint gl_bmodel_vbo;
extern size_t gl_bmodel_vbo_size;

/*
================
R_DrawTextureChains_GLSL -- ericw
================
*/
void R_DrawTextureChains_GLSL (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound, setup = false;
	gltexture_t	*fullbright = NULL;
	gltexture_t	*lastlightmap = NULL;
	float		entalpha;
	unsigned	state;
	GLuint		buf;
	GLbyte		*ofs;
	worlduniforms_t uniforms;

	entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

	state = GLS_CULL_BACK | GLS_ATTRIBS(0);
	if (entalpha < 1)
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	else
		state |= GLS_BLEND_OPAQUE;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		if (!gl_fullbrights.value || !(fullbright = R_TextureAnimation(t, ent != NULL ? ent->frame : 0)->fullbright))
			fullbright = blacktexture;

		R_ClearBatch ();

		bound = false;
		lastlightmap = NULL;
		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			int use_alpha_test = (t->texturechains[chain]->flags & SURF_DRAWFENCE) ? 1 : 0;

			if (!setup || uniforms.use_alpha_test != use_alpha_test) // only perform setup when needed
			{
				if (!setup)
				{
					GL_UseProgram (r_world_program);
					GL_SetState (state);
					GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

					memcpy(uniforms.mvp, r_matviewproj, 16 * sizeof(float));
					memcpy(uniforms.fog, fog_data, 4 * sizeof(float));
					uniforms.alpha = entalpha;
					uniforms.time = cl.time;
					uniforms.padding = 0;
				}
				else
				{
					R_FlushBatch ();
				}

				uniforms.use_alpha_test = use_alpha_test;

				GL_Upload (GL_SHADER_STORAGE_BUFFER, &uniforms, sizeof(uniforms), &buf, &ofs);
				GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, buf, (GLintptr)ofs, sizeof(uniforms));

				setup = true;
			}

			if (!bound) //only bind once we are sure we need this texture
			{
				gltexture_t *tx = (R_TextureAnimation(t, ent != NULL ? ent->frame : 0))->gltexture;
				if (r_lightmap_cheatsafe)
				{
					tx = greytexture;
					fullbright = blacktexture;
				}

				GL_Bind (GL_TEXTURE0, tx);
				GL_Bind (GL_TEXTURE1, fullbright);

				bound = true;
			}

			if (lightmaps[s->lightmaptexturenum].texture != lastlightmap)
			{
				R_FlushBatch ();
				lastlightmap = lightmaps[s->lightmaptexturenum].texture;
				GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lastlightmap);
			}

			R_BatchSurface (s);

			rs_brushpasses++;
		}

		R_FlushBatch ();
	}
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
void R_DrawTextureChains_Water (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound, setup = false;
	float		old_alpha;
	worlduniforms_t uniforms;

	for (i=0 ; i<model->numtextures ; i++)
	{
		float alpha;
		t = model->textures[i];
		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;

		s = t->texturechains[chain];
		alpha = GL_WaterAlphaForEntitySurface (ent, s);
		bound = false;

		for (; s; s = s->texturechain)
		{
			if (!setup || alpha != old_alpha) // only perform setup once we are sure we need to
			{
				GLuint		buf;
				GLbyte		*ofs;
				unsigned	state;

				state = GLS_CULL_BACK | GLS_ATTRIBS(0);
				if (alpha < 1.f)
					state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
				else
					state |= GLS_BLEND_OPAQUE;
				old_alpha = alpha;

				if (!setup)
				{
					GL_UseProgram (r_water_program);
					GL_SetState (state);

					memcpy(uniforms.mvp, r_matviewproj, 16 * sizeof(float));
					memcpy(uniforms.fog, fog_data, 4 * sizeof(float));
					uniforms.use_alpha_test = 0;
					uniforms.alpha = alpha;
					uniforms.time = cl.time;
					uniforms.padding = 0;
				}
				else
				{
					GL_SetState (state);
					uniforms.alpha = alpha;
				}

				GL_Upload (GL_SHADER_STORAGE_BUFFER, &uniforms, sizeof(uniforms), &buf, &ofs);
				GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, buf, (GLintptr)ofs, sizeof(uniforms));
				GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

				setup = true;
			}

			if (!bound) //only bind once we are sure we need this texture
			{
				GL_Bind (GL_TEXTURE0, r_lightmap_cheatsafe ? whitetexture : t->gltexture);
				bound = true;
			}

			R_BatchSurface (s);
			rs_brushpasses++;
		}

		R_FlushBatch ();
	}
}

/*
================
R_DrawTextureChains_ShowTris -- johnfitz
================
*/
void R_DrawTextureChains_ShowTris (qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	setup = false;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain])
			continue;

		if (!setup)
		{
			worlduniforms_t uniforms;
			GLuint		buf;
			GLbyte		*ofs;
			unsigned	state = GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS(0);

			GL_UseProgram (r_world_program);
			GL_SetState (state);

			memcpy(uniforms.mvp, r_matviewproj, 16 * sizeof(float));
			memset(uniforms.fog, 0, 4 * sizeof(float));
			uniforms.use_alpha_test = 0;
			uniforms.alpha = 1.f;
			uniforms.time = cl.time;
			uniforms.padding = 0;

			GL_Upload (GL_SHADER_STORAGE_BUFFER, &uniforms, sizeof(uniforms), &buf, &ofs);
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, buf, (GLintptr)ofs, sizeof(uniforms));
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

			GL_Bind (GL_TEXTURE0, whitetexture);
			GL_Bind (GL_TEXTURE1, whitetexture);
			GL_Bind (GL_TEXTURE2, whitetexture);

			setup = true;
		}

		for (s = t->texturechains[chain]; s; s = s->texturechain)
			R_BatchSurface (s);
	}

	R_FlushBatch ();
}

/*
=============
R_DrawWorld -- johnfitz -- rewritten
=============
*/
void R_DrawTextureChains (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	float entalpha;
	
	if (ent != NULL)
		entalpha = ENTALPHA_DECODE(ent->alpha);
	else
		entalpha = 1;

	R_UploadLightmaps ();
	R_DrawTextureChains_GLSL (model, ent, chain);
}

/*
=============
R_DrawWorld -- ericw -- moved from R_DrawTextureChains, which is no longer specific to the world.
=============
*/
void R_DrawWorld (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	GL_BeginGroup ("World");

	R_DrawTextureChains (cl.worldmodel, NULL, chain_world);

	GL_EndGroup ();
}

/*
=============
R_DrawWorld_Water -- ericw -- moved from R_DrawTextureChains_Water, which is no longer specific to the world.
=============
*/
void R_DrawWorld_Water (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	GL_BeginGroup ("World water");

	R_DrawTextureChains_Water (cl.worldmodel, NULL, chain_world);

	GL_EndGroup ();
}

/*
=============
R_DrawWorld_ShowTris -- ericw -- moved from R_DrawTextureChains_ShowTris, which is no longer specific to the world.
=============
*/
void R_DrawWorld_ShowTris (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	GL_BeginGroup ("World tris");

	R_DrawTextureChains_ShowTris (cl.worldmodel, chain_world);

	GL_EndGroup ();
}
