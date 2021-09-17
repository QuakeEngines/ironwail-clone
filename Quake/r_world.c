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

/*
===============
R_MarkSurfaces -- johnfitz -- mark surfaces based on PVS and rebuild texture chains
===============
*/
void R_MarkSurfaces (void)
{
	byte		*vis;
	mleaf_t		*leaf;
	msurface_t	*surf, **mark;
	int			i, j;
	qboolean	nearwaterportal;

	// clear lightmap chains
	for (i=0 ; i<lightmap_count ; i++)
		lightmaps[i].polys = NULL;

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i=0, mark = r_viewleaf->firstmarksurface; i < r_viewleaf->nummarksurfaces; i++, mark++)
		if ((*mark)->flags & SURF_DRAWTURB)
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
	leaf = &cl.worldmodel->leafs[1];
	for (i=0 ; i<cl.worldmodel->numleafs ; i++, leaf++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			if (R_CullBox(leaf->minmaxs, leaf->minmaxs + 3))
				continue;

			if (r_oldskyleaf.value || leaf->contents != CONTENTS_SKY)
				for (j=0, mark = leaf->firstmarksurface; j<leaf->nummarksurfaces; j++, mark++)
				{
					surf = *mark;
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

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}
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

#define MAX_BATCH_SIZE 4096

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

// uniforms used in vert shader

// uniforms used in frag shader
static GLuint texLoc;
static GLuint LMTexLoc;
static GLuint fullbrightTexLoc;
static GLuint useAlphaTestLoc;
static GLuint alphaLoc;
static GLuint fogLoc;

#define vertAttrIndex 0
#define texCoordsAttrIndex 1
#define LMCoordsAttrIndex 2

/*
=============
GLWorld_CreateShaders
=============
*/
void GLWorld_CreateShaders (void)
{
	// Driver bug workarounds:
	// - "Intel(R) UHD Graphics 600" version "4.6.0 - Build 26.20.100.7263"
	//    crashing on glUseProgram with `vec3 Vert` and
	//    `gl_ModelViewProjectionMatrix * vec4(Vert, 1.0);`. Work around with
	//    making Vert a vec4. (https://sourceforge.net/p/quakespasm/bugs/39/)
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) uniform mat4 MVP;\n"
		"\n"
		"layout(location=0) in vec4 in_pos;\n"
		"layout(location=1) in vec2 TexCoords;\n"
		"layout(location=2) in vec2 LMCoords;\n"
		"\n"
		"layout(location=0) out vec4 out_uv;\n"
		"layout(location=1) out float out_fogdist;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = MVP * in_pos;\n"
		"	out_uv = vec4(TexCoords, LMCoords);\n"
		"	out_fogdist = gl_Position.w;\n"
		"}\n";
	
	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"layout(binding=1) uniform sampler2D FullbrightTex;\n"
		"layout(binding=2) uniform sampler2D LMTex;\n"
		"\n"
		"layout(location=4) uniform bool UseAlphaTest;\n"
		"layout(location=5) uniform float Alpha;\n"
		"layout(location=6) uniform vec4 Fog;\n"
		"\n"
		"layout(location=0) in vec4 in_uv;\n"
		"layout(location=1) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture2D(Tex, in_uv.xy);\n"
		"	if (UseAlphaTest && result.a < 0.666)\n"
		"		discard;\n"
		"	result.rgb *= texture2D(LMTex, in_uv.zw).rgb * 2.0;\n"
		"	result.rgb += texture2D(FullbrightTex, in_uv.xy).rgb;\n"
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
		useAlphaTestLoc = GL_GetUniformLocation (&r_world_program, "UseAlphaTest");
		alphaLoc = GL_GetUniformLocation (&r_world_program, "Alpha");
		fogLoc = GL_GetUniformLocation (&r_world_program, "Fog");
	}

	vertSource = \
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
		"	out_uv = in_uv;\n"
		"	out_fogdist = gl_Position.w;\n"
		"}\n";
	
	fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"\n"
		"layout(location=2) uniform vec2 AlphaTime; // x = Alpha, y = Time\n"
		"layout(location=3) uniform vec4 Fog;\n"
		"\n"
		"layout(location=0) in vec2 in_uv;\n"
		"layout(location=1) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec2 uv = in_uv * 2.0 + 0.125 * sin(in_uv.yx * (3.14159265 * 2.0) + AlphaTime.y);\n"
		"	vec4 result = texture(Tex, uv);\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	result.a *= AlphaTime.x;\n"
		"	out_fragcolor = result;\n"
		"}\n";

	r_water_program = GL_CreateProgram (vertSource, fragSource, "water");
}

extern GLuint gl_bmodel_vbo;

/*
================
R_DrawTextureChains_GLSL -- ericw
================
*/
static int lastfogframe = 0;
void R_DrawTextureChains_GLSL (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;
	int		lastlightmap;
	gltexture_t	*fullbright = NULL;
	float		entalpha;
	unsigned	state = GLS_CULL_BACK | GLS_ATTRIBS(3);

	entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

// enable blending / disable depth writes
	if (entalpha < 1)
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	else
		state |= GLS_BLEND_OPAQUE;

	GL_UseProgram (r_world_program);
	GL_SetState (state);

// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);

	GL_VertexAttribPointerFunc (vertAttrIndex,      3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0));
	GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0) + 3);
	GL_VertexAttribPointerFunc (LMCoordsAttrIndex,  2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0) + 5);

// set uniforms
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matviewproj);

	GL_Uniform1iFunc (useAlphaTestLoc, 0);
	GL_Uniform1fFunc (alphaLoc, entalpha);
	if (lastfogframe != r_framecount) // only set fog once per frame
	{
		lastfogframe = r_framecount;
		GL_Uniform4fvFunc (fogLoc, 1, fog_data);
	}

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

		if (!gl_fullbrights.value || !(fullbright = R_TextureAnimation(t, ent != NULL ? ent->frame : 0)->fullbright))
			fullbright = blacktexture;

		R_ClearBatch ();

		bound = false;
		lastlightmap = 0; // avoid compiler warning
		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
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
				if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
					GL_Uniform1iFunc (useAlphaTestLoc, 1); // Flip alpha test back on

				bound = true;
				lastlightmap = s->lightmaptexturenum;
			}

			if (s->lightmaptexturenum != lastlightmap)
				R_FlushBatch ();

			GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmaps[s->lightmaptexturenum].texture);
			lastlightmap = s->lightmaptexturenum;
			R_BatchSurface (s);

			rs_brushpasses++;
		}

		R_FlushBatch ();

		if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
			GL_Uniform1iFunc (useAlphaTestLoc, 0); // Flip alpha test back off
	}
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
static int lastfogframe_water = 0;
void R_DrawTextureChains_Water (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;
	float		entalpha;

	GL_UseProgram (r_water_program);

// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);

	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0));
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0) + 3);

// set uniforms
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matviewproj);
	if (lastfogframe_water != r_framecount) // only set fog once per frame
	{
		lastfogframe_water = r_framecount;
		GL_Uniform4fvFunc (3, 1, fog_data);
	}

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;

		bound = false;
		entalpha = 1.0f;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
		{
			if (!bound) //only bind once we are sure we need this texture
			{
				unsigned state = GLS_CULL_BACK | GLS_ATTRIBS(2);
				entalpha = GL_WaterAlphaForEntitySurface (ent, s);

				if (entalpha < 1)
					state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
				else
					state |= GLS_BLEND_OPAQUE;

				GL_SetState (state);
				GL_Uniform2fFunc (2, entalpha, cl.time);
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
	unsigned	state = GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_BACK | GLS_ATTRIBS(3);

	GL_UseProgram (r_world_program);
	GL_SetState (state);

// Bind the buffers
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);

	GL_VertexAttribPointerFunc (vertAttrIndex,      3, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0));
	GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0) + 3);
	GL_VertexAttribPointerFunc (LMCoordsAttrIndex,  2, GL_FLOAT, GL_FALSE, VERTEXSIZE * sizeof(float), ((float *)0) + 5);

// set uniforms
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, r_matviewproj);

	GL_Uniform1iFunc (useAlphaTestLoc, 0);
	GL_Uniform1fFunc (alphaLoc, 1.f);
	GL_Uniform4fvFunc (fogLoc, 1, fog_data);

	GL_Bind (GL_TEXTURE0, whitetexture);
	GL_Bind (GL_TEXTURE1, whitetexture);
	GL_Bind (GL_TEXTURE2, whitetexture);

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain])
			continue;

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
