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

int		gl_lightmap_format;
int		lightmap_bytes;

#define MAX_SANITY_LIGHTMAPS (1u<<20)
struct lightmap_s	*lightmaps;
int					lightmap_count;
int					last_lightmap_allocated;
int					allocated[LMBLOCK_WIDTH];
int					num_lightmap_samples;
unsigned			*lightmap_offsets;
unsigned			*lightmap_samples;
GLuint				lightmap_sample_buffer;
GLuint				lightmap_offsets_texture;
gltexture_t			*lightmap_texture;
int					lightmap_width;
int					lightmap_height;
int					cached_lightstyles[MAX_LIGHTSTYLES];
unsigned			*lightmap_block_offsets;


/*
===============
R_TextureAnimation

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

	LIGHTMAPS

=============================================================
*/

/*
========================
AllocBlock -- returns a texture number and the position inside it
========================
*/
static int AllocBlock (int w, int h, int *x, int *y)
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

/*
========================
GL_AllocSurfaceLightmap
========================
*/
static void GL_AllocSurfaceLightmap (msurface_t *surf)
{
	int smax = (surf->extents[0]>>4)+1;
	int tmax = (surf->extents[1]>>4)+1;
	surf->lightmaptexturenum = AllocBlock (smax, tmax, &surf->light_s, &surf->light_t);
	if (surf->samples)
	{
		int maps;
		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
			;
		num_lightmap_samples += maps * smax * tmax;
	}
}

/*
========================
GL_MarkSurfaceLightmap
========================
*/
static void GL_MarkSurfaceLightmap (msurface_t *surf, msurface_t **texelsurf)
{
	struct lightmap_s *lm;
	int			smax, tmax;
	int			xofs, yofs;
	int			s, t, maps;
	msurface_t	**dst;

	if (!cl.worldmodel->lightdata || !surf->samples)
		return;

	lm = &lightmaps[surf->lightmaptexturenum];
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	xofs = lm->xofs + surf->light_s;
	yofs = lm->yofs + surf->light_t;

	dst = texelsurf + yofs * lightmap_width + xofs;
	for (t = 0; t < tmax; t++, dst += lightmap_width)
		for (s = 0; s < smax; s++)
			dst[s] = surf;

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		lm->stylemask[surf->styles[maps] >> 5] |= 1 << (surf->styles[maps] & 31);
}

/*
==================
GL_FillLightmapSamples
==================
*/
static void GL_FillLightmapSamples (struct lightmap_s *lm, msurface_t **texelsurf, int *offset)
{
	int i;
	for (i = 0; i < LMBLOCK_WIDTH * LMBLOCK_HEIGHT; i++)
	{
		int			s, t, smax, tmax, maps;
		unsigned	*packedofscount;
		const byte	*src;
		msurface_t	*surf;

		DecodeMortonIndex (i, &s, &t);
		surf = texelsurf [(lm->yofs + t) * lightmap_width + lm->xofs + s];
		if (!surf || !surf->samples || surf->styles[0] == 255)
			continue;

		packedofscount = &lightmap_offsets[(lm->yofs + t) * lightmap_width + lm->xofs + s];
		smax = (surf->extents[0]>>4)+1;
		tmax = (surf->extents[1]>>4)+1;
		s -= surf->light_s;
		t -= surf->light_t;
		src = surf->samples + (t * smax + s) * 3;

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++, src += smax * tmax * 3)
		{
			if (*offset >= num_lightmap_samples)
				Sys_Error ("GL_FillLightmapSamples overflow");
			lightmap_samples[(*offset)++] = src[0] | (src[1] << 8) | (src[2] << 16) | (surf->styles[maps] << 24);
		}
		*packedofscount = ((*offset - maps) << 3) | maps;
	}
}

/*
==================
GL_DeleteLightmapResources
==================
*/
void GL_DeleteLightmapResources (void)
{
	if (lightmap_sample_buffer)
	{
		GL_DeleteBuffer (lightmap_sample_buffer);
		lightmap_sample_buffer = 0;
	}
	if (lightmap_offsets_texture)
	{
		GL_DeleteNativeTexture (lightmap_offsets_texture);
		lightmap_offsets_texture = 0;
	}

	R_InvalidateLightmaps ();
}

/*
==================
GL_CreateLightmapResources
==================
*/
void GL_CreateLightmapResources (void)
{
	glGenTextures (1, &lightmap_offsets_texture);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, lightmap_offsets_texture);
	GL_ObjectLabelFunc (GL_TEXTURE, lightmap_offsets_texture, -1, "lightmap sample offsets");
	glTexImage2D (GL_TEXTURE_2D, 0, GL_R32UI, lightmap_width, lightmap_height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, lightmap_offsets);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, 0);

	GL_GenBuffersFunc (1, &lightmap_sample_buffer);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, lightmap_sample_buffer);
	GL_ObjectLabelFunc (GL_BUFFER, lightmap_sample_buffer, -1, "lightmap sample buffer");
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, num_lightmap_samples * sizeof (GLuint), lightmap_samples, GL_STATIC_DRAW);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, 0);
}

/*
==================
GL_FreeLightmapData
==================
*/
static void GL_FreeLightmapData (void)
{
	GL_DeleteLightmapResources ();

	if (lightmap_block_offsets)
	{
		free (lightmap_block_offsets);
		lightmap_block_offsets = NULL;
	}
	if (lightmap_samples)
	{
		free (lightmap_samples);
		lightmap_samples = NULL;
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

	lightmap_texture = NULL; // freed by the texture manager
	last_lightmap_allocated = 0;
	lightmap_count = 0;
	lightmap_width = 0;
	lightmap_height = 0;
	num_lightmap_samples = 0;
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
				lightmap_block_offsets[count++] = lm->xofs | (lm->yofs << 16);
				break;
			}
		}
	}

	if (!count)
		return;

	GL_BeginGroup ("Lightmap update");

	GL_UseProgram (glprogs.update_lightmap);
	GL_BindImageTextureFunc (0, lightmap_offsets_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
	GL_BindImageTextureFunc (1, lightmap_texture->texnum, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, d_lightstylevalue, sizeof(d_lightstylevalue), &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, buf, (GLintptr)ofs, sizeof(d_lightstylevalue));
	GL_Upload (GL_SHADER_STORAGE_BUFFER, lightmap_block_offsets, sizeof(GLuint) * count, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(GLuint) * count);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, lightmap_sample_buffer, 0, num_lightmap_samples * sizeof(GLuint));

	GL_DispatchComputeFunc (LMBLOCK_WIDTH / 64, LMBLOCK_HEIGHT / 1, count);
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
	int			i, j, xblocks, yblocks;
	int			numsamples;
	struct lightmap_s *lm;
	qmodel_t	*m;
	msurface_t	**texelsurf;

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

	texelsurf              = (msurface_t **) calloc (sizeof(*texelsurf),              lightmap_width * lightmap_height);
	lightmap_offsets       = (unsigned    *) calloc (sizeof(*lightmap_offsets),       lightmap_width * lightmap_height);
	lightmap_samples       = (unsigned    *) calloc (sizeof(*lightmap_samples),       num_lightmap_samples);
	lightmap_block_offsets = (unsigned    *) calloc (sizeof(*lightmap_block_offsets), lightmap_count);

	// compute offsets for each lightmap block
	for (i=0; i<lightmap_count; i++)
	{
		lm = &lightmaps[i];
		lm->xofs = (i % xblocks) * LMBLOCK_WIDTH;
		lm->yofs = (i / xblocks) * LMBLOCK_HEIGHT;
	}

	// create texel -> surface map
	for (j=1 ; j<MAX_MODELS ; j++)
	{
		m = cl.model_precache[j];
		if (!m)
			break;
		if (m->name[0] == '*')
			continue;
		for (i=0 ; i<m->numsurfaces ; i++)
		{
			//johnfitz -- rewritten to use SURF_DRAWTILED instead of the sky/water flags
			if (m->surfaces[i].flags & SURF_DRAWTILED)
				continue;
			GL_MarkSurfaceLightmap (m->surfaces + i, texelsurf);
			//johnfitz
		}
	}

	// fill sample buffer
	numsamples = 0;
	for (i=0; i<lightmap_count; i++)
		GL_FillLightmapSamples (lightmaps + i, texelsurf, &numsamples);
	free (texelsurf);
	texelsurf = NULL;

	// create GPU resources
	GL_CreateLightmapResources ();
	lightmap_texture =
		TexMgr_LoadImage (cl.worldmodel, "lightmap", lightmap_width, lightmap_height, SRC_LIGHTMAP, NULL, "", 0, TEXPREF_LINEAR | TEXPREF_NOPICMIP);

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
size_t gl_bmodel_indirect_buffer_size = 0;
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
	gl_bmodel_indirect_buffer_size = 0;
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
	int			i, j, k;
	qmodel_t	*m;
	float		*varray;
	float		lmscalex = 1.f / 16.f / lightmap_width;
	float		lmscaley = 1.f / 16.f / lightmap_height;

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

		for (i = 0; i < m->numsurfaces; i++)
		{
			msurface_t	*fa = &m->surfaces[i];
			texture_t	*texture = m->textures[fa->texinfo->texnum];
			float		*verts = &varray[VERTEXSIZE * varray_index];
			float		texscalex, texscaley, useofs;
			medge_t		*r_pedge;
			struct lightmap_s *lm;

			if (fa->flags & SURF_DRAWTILED)
			{
				// match old Mod_PolyForUnlitSurface
				if (fa->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
					texscalex = 1.f / 128.f; //warp animation repeats every 128
				else
					texscalex = 32.f; //to match r_notexture_mip
				texscaley = texscalex;
				useofs = 0.f; //unlit surfaces don't use the texture offset
				lm = NULL;
			}
			else
			{
				texscalex = 1.f / texture->width;
				texscaley = 1.f / texture->height;
				useofs = 1.f;
				lm = &lightmaps[fa->lightmaptexturenum];
			}

			fa->vbo_firstvert = varray_index;
			varray_index += fa->numedges;

			for (k = 0; k < fa->numedges; k++, verts += VERTEXSIZE)
			{
				float	*vec;
				float	s, t;
				int		lindex;

				lindex = m->surfedges[fa->firstedge + k];
				if (lindex > 0)
				{
					r_pedge = &m->edges[lindex];
					vec = m->vertexes[r_pedge->v[0]].position;
				}
				else
				{
					r_pedge = &m->edges[-lindex];
					vec = m->vertexes[r_pedge->v[1]].position;
				}

				s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3] * useofs;
				s *= texscalex;

				t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3] * useofs;
				t *= texscaley;

				VectorCopy (vec, verts);
				verts[3] = s;
				verts[4] = t;

				if (!(fa->flags & SURF_DRAWTILED))
				{
					// match old BuildSurfaceDisplayList

					// Q64 RERELEASE texture shift
					if (texture->shift > 0)
					{
						verts[3] /= (2 * texture->shift);
						verts[4] /= (2 * texture->shift);
					}

					//
					// lightmap texture coordinates
					//
					s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
					s -= fa->texturemins[0];
					s += (fa->light_s + lm->xofs) * 16;
					s += 8;
					s *= lmscalex;

					t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
					t -= fa->texturemins[1];
					t += (fa->light_t + lm->yofs) * 16;
					t += 8;
					t *= lmscaley;

					verts[5] = s;
					verts[6] = t;
				}
			}
		}
	}

// upload to GPU
	gl_bmodel_vbo_size = varray_bytes;
	GL_BindBuffer (GL_ARRAY_BUFFER, gl_bmodel_vbo);
	GL_ObjectLabelFunc (GL_BUFFER, gl_bmodel_vbo, -1, "brushverts");
	GL_BufferDataFunc (GL_ARRAY_BUFFER, varray_bytes, varray, GL_STATIC_DRAW);
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
		numtex += m->texofs[TEXTYPE_COUNT];
		maxnumtex = q_max (maxnumtex, m->numtextures);
		for (i = 0; i < m->nummodelsurfaces; i++)
			numtris += m->surfaces[i + m->firstmodelsurface].numedges - 2;
	}

	// allocate cpu-side buffers
	gl_bmodel_ibo_size = numtris * 3 * sizeof(idx[0]);
	gl_bmodel_indirect_buffer_size = numtex * sizeof(cmds[0]);
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

	for (i = 0; i < cl.worldmodel->texofs[TEXTYPE_COUNT]; i++)
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
		for (i = 0; i < m->texofs[TEXTYPE_COUNT]; i++)
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
		for (i = 0; i < m->texofs[TEXTYPE_COUNT]; i++)
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
