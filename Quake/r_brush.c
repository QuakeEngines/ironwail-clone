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
// r_brush.c: brush model rendering. renamed from r_surf.c

#include "quakedef.h"

extern cvar_t gl_fullbrights, gl_overbright; //johnfitz
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix
extern cvar_t r_compute_mark;

int		gl_lightmap_format;
int		lightmap_bytes;

#define MAX_SANITY_LIGHTMAPS (1u<<20)
struct lightmap_s	*lightmaps;
int					lightmap_count;
int					last_lightmap_allocated;
int					allocated[LMBLOCK_WIDTH];
unsigned			*lightmap_data;
gltexture_t			*lightmap_texture;
gltexture_t			*lightmap_samples_texture;
int					lightmap_width;
int					lightmap_height;
int					cached_lightstyles[MAX_LIGHTSTYLES];
unsigned			*lightmap_offsets;


/*
===============
R_TextureAnimation -- johnfitz -- added "frame" param to eliminate use of "currententity" global

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base, int frame)
{
	int		relative;
	int		count;

	if (frame)
		if (base->alternate_anims)
			base = base->alternate_anims;

	if (!base->anim_total)
		return base;

	relative = (int)(cl.time*10) % base->anim_total;

	count = 0;
	while (base->anim_min > relative || base->anim_max <= relative)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}

/*
=============================================================

	BRUSH MODELS

=============================================================
*/

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (entity_t *e)
{
	int			i;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;
	float		oldmvp[16], model_matrix[16];

	if (R_CullModelForEntity(e))
		return;
	R_NewModelInstance (mod_brush);

	GL_BeginGroup (e->model->name);

	currententity = e;
	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	e->angles[0] = -e->angles[0];	// stupid quake bug
	if (gl_zfix.value)
	{
		e->origin[0] -= DIST_EPSILON;
		e->origin[1] -= DIST_EPSILON;
		e->origin[2] -= DIST_EPSILON;
	}

	memcpy(oldmvp, r_matviewproj, 16 * sizeof(float));

	R_EntityMatrix (model_matrix, e->origin, e->angles);
	MatrixMultiply (r_matviewproj, model_matrix);

	if (gl_zfix.value)
	{
		e->origin[0] += DIST_EPSILON;
		e->origin[1] += DIST_EPSILON;
		e->origin[2] += DIST_EPSILON;
	}
	e->angles[0] = -e->angles[0];	// stupid quake bug

	if (!r_compute_mark.value)
	{
		R_ClearTextureChains (clmodel, chain_model);
		for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
		{
			pplane = psurf->plane;
			dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
			if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
				(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
			{
				R_ChainSurface (clmodel, psurf, chain_model);
				rs_brushpolys++;
			}
		}
	}

	R_DrawTextureChains (clmodel, e, chain_model);
	R_DrawTextureChains_Water (clmodel, e, chain_model);

	memcpy(r_matviewproj, oldmvp, 16 * sizeof(float));

	GL_EndGroup ();
}

/*
=================
R_DrawBrushModel_ShowTris -- johnfitz
=================
*/
void R_DrawBrushModel_ShowTris (entity_t *e)
{
	int			i;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;
	qmodel_t	*clmodel;

	if (R_CullModelForEntity(e))
		return;
	R_NewModelInstance (mod_brush);

	currententity = e;
	clmodel = e->model;

	VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}

	psurf = &clmodel->surfaces[clmodel->firstmodelsurface];

	e->angles[0] = -e->angles[0];	// stupid quake bug
	if (gl_zfix.value)
	{
		e->origin[0] -= DIST_EPSILON;
		e->origin[1] -= DIST_EPSILON;
		e->origin[2] -= DIST_EPSILON;
	}

	float oldmvp[16], model_matrix[16];
	memcpy(oldmvp, r_matviewproj, 16 * sizeof(float));

	R_EntityMatrix (model_matrix, e->origin, e->angles);
	MatrixMultiply (r_matviewproj, model_matrix);

	if (gl_zfix.value)
	{
		e->origin[0] += DIST_EPSILON;
		e->origin[1] += DIST_EPSILON;
		e->origin[2] += DIST_EPSILON;
	}
	e->angles[0] = -e->angles[0];	// stupid quake bug

	R_ClearTextureChains (clmodel, chain_model);
	for (i=0 ; i<clmodel->nummodelsurfaces ; i++, psurf++)
	{
		pplane = psurf->plane;
		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			R_ChainSurface (clmodel, psurf, chain_model);
		}
	}

	R_DrawTextureChains_ShowTris (clmodel, chain_model);

	memcpy(r_matviewproj, oldmvp, 16 * sizeof(float));
}

/*
=============================================================

	LIGHTMAPS

=============================================================
*/

static GLuint r_lightmap_update_program;

/*
================
GLLightmap_CreateShaders
================
*/
void GLLightmap_CreateShaders (void)
{
	const char* computeSource = \
		"#version 430\n"
		"\n"
		"layout(local_size_x=256) in;\n"
		"\n"
		"layout(rgba8ui, binding=0) readonly uniform uimage2DArray LightmapSamples;\n"
		"layout(rgba8ui, binding=1) writeonly uniform uimage2D Lightmap;\n"
		"\n"
		"layout(std430, binding=0) restrict readonly buffer LightStyles\n"
		"{\n"
		"	uint lightstyles[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=1) restrict readonly buffer Blocks\n"
		"{\n"
		"	uint blockofs[]; // 16:16\n"
		"};\n"
		"\n"
		"uint deinterleave_odd(uint x)\n"
		"{\n"
		"	x &= 0x55555555u;\n"
		"	x = (x ^ (x >> 1u)) & 0x33333333u;\n"
		"	x = (x ^ (x >> 2u)) & 0x0F0F0F0Fu;\n"
		"	x = (x ^ (x >> 4u)) & 0x00FF00FFu;\n"
		"	x = (x ^ (x >> 8u)) & 0x0000FFFFu;\n"
		"	return x;\n"
		"}\n"
		"\n"
		"void main()\n"
		"{\n"
		"	uvec3 thread_id = gl_GlobalInvocationID;\n"
		"	uint xy = thread_id.x | (thread_id.y << 8u);\n"
		"	xy |= (xy >> 1u) << 16u;\n"
		"	xy = deinterleave_odd(xy); // morton order\n"
		"	uvec2 coord = uvec2(xy & 0xffu, xy >> 8u);\n"
		"	xy = blockofs[thread_id.z];\n"
		"	coord += uvec2(xy & 0xffffu, xy >> 16u);\n"
		"	uvec3 accum = uvec3(0u);\n"
		"	int i;\n"
		"	for (i = 0; i < " QS_STRINGIFY(MAXLIGHTMAPS) "; i++)\n"
		"	{\n"
		"		uvec4 s = imageLoad(LightmapSamples, ivec3(coord, i));\n"
		"		if (s.w == 255u)\n"
		"			break;\n"
		"		accum += s.xyz * lightstyles[s.w];\n"
		"	}\n"
		"	accum = min(accum >> 8u, uvec3(255u));\n"
		"	imageStore(Lightmap, ivec2(coord), uvec4(accum, 255u));\n"
		"}\n";

	r_lightmap_update_program = GL_CreateComputeProgram (computeSource, "lightmap update");
}

/*
========================
AllocBlock -- returns a texture number and the position inside it
========================
*/
int AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		texnum;

	// ericw -- rather than searching starting at lightmap 0 every time,
	// start at the last lightmap we allocated a surface in.
	// This makes AllocBlock much faster on large levels (can shave off 3+ seconds
	// of load time on a level with 180 lightmaps), at a cost of not quite packing
	// lightmaps as tightly vs. not doing this (uses ~5% more lightmaps)
	for (texnum=last_lightmap_allocated ; texnum<MAX_SANITY_LIGHTMAPS ; texnum++)
	{
		if (texnum == lightmap_count)
		{
			lightmap_count++;
			lightmaps = (struct lightmap_s *) realloc(lightmaps, sizeof(*lightmaps)*lightmap_count);
			memset(&lightmaps[texnum], 0, sizeof(lightmaps[texnum]));
			//as we're only tracking one texture, we don't need multiple copies of allocated any more.
			memset(allocated, 0, sizeof(allocated));
		}
		best = LMBLOCK_HEIGHT;

		for (i=0 ; i<LMBLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (allocated[i+j] >= best)
					break;
				if (allocated[i+j] > best2)
					best2 = allocated[i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > LMBLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			allocated[*x + i] = best + h;

		last_lightmap_allocated = texnum;
		return texnum;
	}

	Sys_Error ("AllocBlock: full");
	return 0; //johnfitz -- shut up compiler
}


mvertex_t	*r_pcurrentvertbase;
qmodel_t	*currentmodel;

int	nColinElim;

/*
========================
GL_AllocSurfaceLightmap
========================
*/
void GL_AllocSurfaceLightmap (msurface_t *surf)
{
	int smax = (surf->extents[0]>>4)+1;
	int tmax = (surf->extents[1]>>4)+1;
	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
}

/*
========================
GL_FillSurfaceLightmap
========================
*/
void GL_FillSurfaceLightmap (msurface_t *surf)
{
	struct lightmap_s *lm;
	int			smax, tmax;
	int			xofs, yofs;
	int			map;
	byte		*src;
	unsigned	*dst;

	if (!cl.worldmodel->lightdata || !surf->samples)
		return;

	lm = &lightmaps[surf->lightmaptexturenum];
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	xofs = lm->xofs + surf->light_s;
	yofs = lm->yofs + surf->light_t;

	src = surf->samples;
	dst = lightmap_data + yofs * lightmap_width + xofs;

	for (map = 0; map < MAXLIGHTMAPS && surf->styles[map] != 255; map++, dst += lightmap_width * lightmap_height)
	{
		unsigned style = surf->styles[map];
		int s, t;
		for (t = 0; t < tmax; t++)
			for (s = 0; s < smax; s++, src += 3)
				dst[t*lightmap_width + s] = src[0] | (src[1]<<8) | (src[2]<<16) | (style<<24);
		lm->stylemask[style >> 5] |= 1 << (style & 31);
	}
}

/*
================
BuildSurfaceDisplayList -- called at level load time
================
*/
void BuildSurfaceDisplayList (msurface_t *fa)
{
	int			i, lindex, lnumverts;
	medge_t		*pedges, *r_pedge;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;
	struct lightmap_s *lm = &lightmaps[fa->lightmaptexturenum];

// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;

	//
	// draw texture
	//
	poly = (glpoly_t *) Hunk_Alloc (sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = fa->polys;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for (i=0 ; i<lnumverts ; i++)
	{
		texture_t *texture = currentmodel->textures[fa->texinfo->texnum];
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = r_pcurrentvertbase[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = r_pcurrentvertbase[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= texture->height;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		// Q64 RERELEASE texture shift
		if (texture->shift > 0)
		{
			poly->verts[i][3] /= (2 * texture->shift);
			poly->verts[i][4] /= (2 * texture->shift);
		}

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += (fa->light_s + lm->xofs) * 16;
		s += 8;
		s /= lightmap_width*16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += (fa->light_t + lm->yofs) * 16;
		t += 8;
		t /= lightmap_height*16; //fa->texinfo->texture->height;

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

	//johnfitz -- removed gl_keeptjunctions code

	poly->numverts = lnumverts;
}

/*
==================
GL_FreeLightmap
==================
*/
static void GL_FreeLightmapData (void)
{
	if (lightmap_data)
	{
		free (lightmap_data);
		lightmap_data = NULL;
	}
	if (lightmap_offsets)
	{
		free (lightmap_offsets);
		lightmap_offsets = NULL;
	}
	if (lightmaps)
	{
		free (lightmaps);
		lightmaps = NULL;
	}
	lightmap_texture = NULL;
	lightmap_samples_texture = NULL;
	last_lightmap_allocated = 0;
	lightmap_count = 0;
	lightmap_width = 0;
	lightmap_height = 0;

	R_InvalidateLightmaps ();
}

/*
==================
R_InvalidateLightmaps
==================
*/
void R_InvalidateLightmaps (void)
{
	memset (cached_lightstyles, 0xff, sizeof(cached_lightstyles));
}

/*
==================
R_UpdateLightmaps
==================
*/
void R_UpdateLightmaps (void)
{
	unsigned	changemask[MAX_LIGHTSTYLES >> 5];
	int			i, j, count;
	GLuint		buf;
	GLbyte		*ofs;

	memset (changemask, 0, sizeof(changemask));
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
		if (d_lightstylevalue[i] != cached_lightstyles[i])
			changemask[i >> 5] |= 1 << (i & 31);
	memcpy (cached_lightstyles, d_lightstylevalue, sizeof(cached_lightstyles));

	for (i = 0, count = 0; i < lightmap_count; i++)
	{
		struct lightmap_s *lm = &lightmaps[i];
		for (j = 0; j < countof (changemask); j++)
		{
			if (changemask[j] & lm->stylemask[j])
			{
				lightmap_offsets[count++] = lm->xofs | (lm->yofs << 16);
				break;
			}
		}
	}

	if (!count)
		return;

	GL_BeginGroup ("Lightmap update");

	GL_UseProgram (r_lightmap_update_program);
	GL_BindImageTextureFunc (0, lightmap_samples_texture->texnum, 0, GL_TRUE, 0,  GL_READ_ONLY, GL_RGBA8UI);
	GL_BindImageTextureFunc (1, lightmap_texture->texnum, 0, GL_FALSE, 0,  GL_WRITE_ONLY, GL_RGBA8UI);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, d_lightstylevalue, sizeof(d_lightstylevalue), &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, buf, (GLintptr)ofs, sizeof(d_lightstylevalue));

	GL_Upload (GL_SHADER_STORAGE_BUFFER, lightmap_offsets, sizeof(GLuint) * count, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(GLuint) * count);

	GL_DispatchComputeFunc (LMBLOCK_WIDTH / 256, LMBLOCK_HEIGHT / 1, count);
	GL_MemoryBarrierFunc (GL_TEXTURE_FETCH_BARRIER_BIT);

	GL_EndGroup ();

	rs_dynamiclightmaps += count;
}

/*
==================
GL_BuildLightmaps -- called at level load time

Builds the lightmap texture
with all the surfaces from all brush models
==================
*/
void GL_BuildLightmaps (void)
{
	int		i, j, xblocks, yblocks;
	struct lightmap_s *lm;
	qmodel_t	*m;

	r_framecount = 1; // no dlightcache

	//Spike -- wipe out all the lightmap data (johnfitz -- the gltexture objects were already freed by Mod_ClearAll)
	GL_FreeLightmapData ();

	gl_lightmap_format = GL_RGBA;//FIXME: hardcoded for now!

	switch (gl_lightmap_format)
	{
	case GL_RGBA:
		lightmap_bytes = 4;
		break;
	case GL_BGRA:
		lightmap_bytes = 4;
		break;
	default:
		Sys_Error ("GL_BuildLightmaps: bad lightmap format");
	}

	// allocate lightmap blocks
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			//johnfitz -- rewritten to use SURF_DRAWTILED instead of the sky/water flags
			if (m->surfaces[i].flags & SURF_DRAWTILED)
				continue;
			GL_AllocSurfaceLightmap (m->surfaces + i);
			//johnfitz
		}
	}

	// determine combined texture size and allocate memory for it
	xblocks = (int) ceil (sqrt (lightmap_count));
	yblocks = (lightmap_count + xblocks - 1) / xblocks;
	lightmap_width = xblocks * LMBLOCK_WIDTH;
	lightmap_height = yblocks * LMBLOCK_HEIGHT;
	if (q_max(lightmap_width, lightmap_height) > gl_max_texture_size)
	{
		// dimensions get zero-ed out in GL_FreeLightmapData, save them for the error message
		int w = lightmap_width;
		int h = lightmap_height;
		GL_FreeLightmapData ();
		Host_Error ("Lightmap texture overflow: needed %dx%d, max is %dx%d\n", w, h, gl_max_texture_size, gl_max_texture_size);
	}
	Con_DPrintf ("Lightmap size: %d x %d (%d/%d blocks)\n", lightmap_width, lightmap_height, lightmap_count, xblocks * yblocks);

	lightmap_offsets = (unsigned *) calloc (sizeof(*lightmap_offsets), lightmap_count);
	lightmap_data = (unsigned *) malloc (sizeof(*lightmap_data) * MAXLIGHTMAPS * lightmap_width * lightmap_height);
	for (i = 0; i < lightmap_width * lightmap_height * MAXLIGHTMAPS; i++)
		lightmap_data[i] = 0xff000000u; // rgb=0, style=255

	// compute offsets for each lightmap block
	for (i=0; i<lightmap_count; i++)
	{
		lm = &lightmaps[i];
		lm->xofs = (i % xblocks) * LMBLOCK_WIDTH;
		lm->yofs = (i / xblocks) * LMBLOCK_HEIGHT;
	}

	// fill lightmap data and assign lightmap UVs to surface vertices
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		r_pcurrentvertbase = m->vertexes;
		currentmodel = m;
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			//johnfitz -- rewritten to use SURF_DRAWTILED instead of the sky/water flags
			if (m->surfaces[i].flags & SURF_DRAWTILED)
				continue;
			GL_FillSurfaceLightmap (m->surfaces + i);
			BuildSurfaceDisplayList (m->surfaces + i);
			//johnfitz
		}
	}

	lightmap_samples_texture =
		TexMgr_LoadImageEx (cl.worldmodel, "lightmap_samples", lightmap_width, lightmap_height, MAXLIGHTMAPS,
			SRC_RGBA, (byte *)lightmap_data, "", (src_offset_t)lightmap_data, TEXPREF_ARRAY | TEXPREF_ALPHA | TEXPREF_NEAREST | TEXPREF_NOPICMIP
		);

	lightmap_texture =
		TexMgr_LoadImage (cl.worldmodel, "lightmap", lightmap_width, lightmap_height, SRC_LIGHTMAP, NULL, "", 0, TEXPREF_LINEAR | TEXPREF_NOPICMIP);

	for (i=0; i<lightmap_count; i++)
		lightmaps[i].texture = lightmap_texture;

	R_UpdateLightmaps ();

	//johnfitz -- warn about exceeding old limits
	//GLQuake limit was 64 textures of 128x128. Estimate how many 128x128 textures we would need
	//given that we are using lightmap_count of LMBLOCK_WIDTH x LMBLOCK_HEIGHT
	i = lightmap_count * ((LMBLOCK_WIDTH / 128) * (LMBLOCK_HEIGHT / 128));
	if (i > 64)
		Con_DWarning("%i lightmaps exceeds standard limit of 64.\n",i);
	//johnfitz
}

/*
=============================================================

	VBO support

=============================================================
*/

GLuint gl_bmodel_vbo = 0;
size_t gl_bmodel_vbo_size = 0;

GLuint gl_bmodel_ibo = 0;
size_t gl_bmodel_ibo_size = 0;
GLuint gl_bmodel_indirect_buffer = 0;
GLuint gl_bmodel_leaf_buffer = 0;
GLuint gl_bmodel_surf_buffer = 0;
GLuint gl_bmodel_marksurf_buffer = 0;

/*
==================
GL_DeleteBModelBuffers
==================
*/
void GL_DeleteBModelBuffers (void)
{
	GL_DeleteBuffer (gl_bmodel_vbo);
	GL_DeleteBuffer (gl_bmodel_ibo);
	GL_DeleteBuffer (gl_bmodel_indirect_buffer);
	GL_DeleteBuffer (gl_bmodel_leaf_buffer);
	GL_DeleteBuffer (gl_bmodel_surf_buffer);
	GL_DeleteBuffer (gl_bmodel_marksurf_buffer);
	gl_bmodel_vbo = 0;
	gl_bmodel_vbo_size = 0;
	gl_bmodel_ibo = 0;
	gl_bmodel_ibo_size = 0;
	gl_bmodel_indirect_buffer = 0;
	gl_bmodel_leaf_buffer = 0;
	gl_bmodel_surf_buffer = 0;
	gl_bmodel_marksurf_buffer = 0;
}

/*
==================
GL_BuildBModelVertexBuffer

Deletes gl_bmodel_vbo if it already exists, then rebuilds it with all
surfaces from world + all brush models
==================
*/
void GL_BuildBModelVertexBuffer (void)
{
	unsigned int	numverts, varray_bytes, varray_index;
	int		i, j;
	qmodel_t	*m;
	float		*varray;

// ask GL for a name for our VBO
	GL_DeleteBuffer (gl_bmodel_vbo);
	GL_GenBuffersFunc (1, &gl_bmodel_vbo);
	
// count all verts in all models
	numverts = 0;
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (i=0 ; i<m->numsurfaces ; i++)
		{
			numverts += m->surfaces[i].numedges;
		}
	}
	
// build vertex array
	varray_bytes = VERTEXSIZE * sizeof(float) * numverts;
	varray = (float *) malloc (varray_bytes);
	varray_index = 0;
	
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m || m->name[0] == '*' || m->type != mod_brush)
			continue;

		for (i=0 ; i<m->numsurfaces ; i++)
		{
			msurface_t *s = &m->surfaces[i];
			s->vbo_firstvert = varray_index;
			memcpy (&varray[VERTEXSIZE * varray_index], s->polys->verts, VERTEXSIZE * sizeof(float) * s->numedges);
			varray_index += s->numedges;
		}
	}

// upload to GPU
	gl_bmodel_vbo_size = varray_bytes;
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_BufferDataFunc (GL_ARRAY_BUFFER, varray_bytes, varray, GL_STATIC_DRAW);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_vbo, -1, "brushverts");
	free (varray);
}

/*
===============
GL_BuildBModelMarkBuffers
===============
*/
void GL_BuildBModelMarkBuffers (void)
{
	int			i, j, k, sum;
	int			numtex = 0, numtris = 0, maxnumtex = 0;
	int			*texidx = NULL;
	GLuint		*idx;
	bmodel_draw_indirect_t *cmds;
	bmodel_gpu_leaf_t *leafs;
	bmodel_gpu_surf_t *surfs;

	if (!cl.worldmodel)
		return;

	// count bmodel textures and triangles
	for (j = 1 ; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		if (!m || m->type != mod_brush)
			continue;
		m->firstcmd = numtex;
		numtex += m->numusedtextures;
		maxnumtex = q_max (maxnumtex, m->numtextures);
		for (i = 0; i < m->nummodelsurfaces; i++)
			numtris += m->surfaces[i + m->firstmodelsurface].numedges - 2;
	}

	// allocate cpu-side buffers
	gl_bmodel_ibo_size = numtris * 3 * sizeof(idx[0]);
	cmds = (bmodel_draw_indirect_t *) calloc (numtex, sizeof(cmds[0]));
	idx = (GLuint *) calloc (numtris * 3, sizeof(idx[0]));
	leafs = (bmodel_gpu_leaf_t *) calloc (cl.worldmodel->numleafs, sizeof(leafs[0]));
	surfs = (bmodel_gpu_surf_t *) calloc (cl.worldmodel->numsurfaces, sizeof(surfs[0]));
	texidx = (int *) calloc (maxnumtex, sizeof(texidx[0]));

	// fill worldmodel leaf data
	for (i = 0; i < cl.worldmodel->numleafs; i++)
	{
		mleaf_t *src = &cl.worldmodel->leafs[i + 1];
		bmodel_gpu_leaf_t *dst = &leafs[i];

		memcpy (dst->mins, src->minmaxs, 3 * sizeof(float));
		memcpy (dst->maxs, src->minmaxs + 3, 3 * sizeof(float));
		dst->firstsurf = src->firstmarksurface - cl.worldmodel->marksurfaces;
		dst->surfcountsky = (src->nummarksurfaces << 1) | (src->contents == CONTENTS_SKY);
	}

	for (i = 0; i < cl.worldmodel->numusedtextures; i++)
		texidx[cl.worldmodel->usedtextures[i]] = i;

	// fill worldmodel surface data
	for (i = 0; i < cl.worldmodel->numsurfaces; i++)
	{
		msurface_t *src = &cl.worldmodel->surfaces[i];
		bmodel_gpu_surf_t *dst = &surfs[i];
		float flip = (src->flags & SURF_PLANEBACK) ? -1.f : 1.f;

		if (src->texinfo->texnum < 0 || src->texinfo->texnum >= cl.worldmodel->numtextures)
			Sys_Error ("GL_BuildBModelMarkBuffers: bad texnum %d (total=%d)", src->texinfo->texnum, cl.worldmodel->numtextures);

		dst->plane[0] = src->plane->normal[0] * flip;
		dst->plane[1] = src->plane->normal[1] * flip;
		dst->plane[2] = src->plane->normal[2] * flip;
		dst->plane[3] = src->plane->dist * flip;
		dst->texnum = texidx[src->texinfo->texnum];
		dst->numedges = src->numedges;
		dst->firstvert = src->vbo_firstvert;
	}

	// count triangles for each model texture
	for (j = 1 ; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		msurface_t *s;
		if (!m || m->type != mod_brush)
			continue;

		memset (texidx, 0, sizeof(texidx[0]) * m->numtextures);
		for (i = 0; i < m->numusedtextures; i++)
			texidx[m->usedtextures[i]] = i;

		for (i = 0, s = m->surfaces + m->firstmodelsurface; i < m->nummodelsurfaces; i++, s++)
			cmds[m->firstcmd + texidx[s->texinfo->texnum]].count += (s->numedges - 2) * 3;
	}

	// compute per-drawcall index buffer offsets
	sum = 0;
	for (i = 0; i < numtex; i++)
	{
		cmds[i].firstIndex = sum;
		sum += cmds[i].count;
		cmds[i].instanceCount = 1;
	}

	// build index buffer
	for (j = 1 ; j < MAX_MODELS; j++)
	{
		qmodel_t *m = cl.model_precache[j];
		msurface_t *s;
		if (!m || m->type != mod_brush)
			continue;

		memset (texidx, 0, sizeof(texidx[0]) * m->numtextures);
		for (i = 0; i < m->numusedtextures; i++)
			texidx[m->usedtextures[i]] = i;

		for (i = 0, s = m->surfaces + m->firstmodelsurface; i < m->nummodelsurfaces; i++, s++)
		{
			bmodel_draw_indirect_t *draw = &cmds[m->firstcmd + texidx[s->texinfo->texnum]];
			for (k = 2; k < s->numedges; k++)
			{
				idx[draw->firstIndex++] = s->vbo_firstvert;
				idx[draw->firstIndex++] = s->vbo_firstvert + k - 1;
				idx[draw->firstIndex++] = s->vbo_firstvert + k;
			}
		}
	}

	// restore firstIndex values (they get shifted in the previous loop)
	sum = 0;
	for (i = 0; i < numtex; i++)
	{
		cmds[i].firstIndex = sum;
		sum += cmds[i].count;
	}

	// create gpu buffers
	GL_GenBuffersFunc (1, &gl_bmodel_indirect_buffer);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, gl_bmodel_indirect_buffer);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_indirect_buffer, -1, "bmodel indirect cmds");
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof(cmds[0]) * numtex, cmds, GL_DYNAMIC_DRAW);

	GL_GenBuffersFunc (1, &gl_bmodel_ibo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_ibo, -1, "bmodel indices");
	GL_BufferDataFunc (GL_ELEMENT_ARRAY_BUFFER, sizeof(idx[0]) * numtris * 3, idx, GL_DYNAMIC_DRAW);

	GL_GenBuffersFunc (1, &gl_bmodel_leaf_buffer);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, gl_bmodel_leaf_buffer);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_leaf_buffer, -1, "bmodel leafs");
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof(leafs[0]) * cl.worldmodel->numleafs, leafs, GL_STATIC_DRAW);

	GL_GenBuffersFunc (1, &gl_bmodel_surf_buffer);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, gl_bmodel_surf_buffer);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_surf_buffer, -1, "bmodel surfs");
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof(surfs[0]) * cl.worldmodel->numsurfaces, surfs, GL_STATIC_DRAW);

	GL_GenBuffersFunc (1, &gl_bmodel_marksurf_buffer);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, gl_bmodel_marksurf_buffer);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_marksurf_buffer, -1, "bmodel marksurfs");
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof(cl.worldmodel->marksurfaces[0]) * cl.worldmodel->nummarksurfaces, cl.worldmodel->marksurfaces, GL_STATIC_DRAW);

	// free cpu-side arrays
	free (texidx);
	free (surfs);
	free (leafs);
	free (idx);
	free (cmds);
}
