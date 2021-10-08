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
byte				*lightmap_data;
gltexture_t			*lightmap_texture;
int					lightmap_width;
int					lightmap_height;

unsigned	blocklights[LMBLOCK_WIDTH*LMBLOCK_HEIGHT*3]; //johnfitz -- was 18*18, added lit support (*3) and loosened surface extents maximum (LMBLOCK_WIDTH*LMBLOCK_HEIGHT)


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
	int			i, k;
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

// calculate dynamic lighting for bmodel if it's not an
// instanced model
	if (clmodel->firstmodelsurface != 0)
	{
		for (k=0 ; k<MAX_DLIGHTS ; k++)
		{
			if ((cl_dlights[k].die < cl.time) ||
				(!cl_dlights[k].radius))
				continue;

			R_MarkLights (&cl_dlights[k], k,
				clmodel->nodes + clmodel->hulls[0].firstclipnode);
		}
	}

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
				R_RenderDynamicLightmaps(psurf);
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

/*
================
R_RenderDynamicLightmaps
called during rendering
================
*/
void R_RenderDynamicLightmaps (msurface_t *fa)
{
	byte		*base;
	int			maps;
	glRect_t    *theRect;
	int smax, tmax;

	if (fa->flags & SURF_DRAWTILED) //johnfitz -- not a lightmapped surface
		return;

	// add to lightmap chain
	fa->polys->chain = lightmaps[fa->lightmaptexturenum].polys;
	lightmaps[fa->lightmaptexturenum].polys = fa->polys;

	// check for lightmap modification
	for (maps=0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
		if (d_lightstylevalue[fa->styles[maps]] != fa->cached_light[maps])
			goto dynamic;

	if (fa->dlightframe == r_framecount	// dynamic this frame
		|| fa->cached_dlight)			// dynamic previously
	{
dynamic:
		if (r_dynamic.value)
		{
			struct lightmap_s *lm = &lightmaps[fa->lightmaptexturenum];
			lm->modified = true;
			theRect = &lm->rectchange;
			if (fa->light_t < theRect->t) {
				if (theRect->h)
					theRect->h += theRect->t - fa->light_t;
				theRect->t = fa->light_t;
			}
			if (fa->light_s < theRect->l) {
				if (theRect->w)
					theRect->w += theRect->l - fa->light_s;
				theRect->l = fa->light_s;
			}
			smax = (fa->extents[0]>>4)+1;
			tmax = (fa->extents[1]>>4)+1;
			if ((theRect->w + theRect->l) < (fa->light_s + smax))
				theRect->w = (fa->light_s-theRect->l)+smax;
			if ((theRect->h + theRect->t) < (fa->light_t + tmax))
				theRect->h = (fa->light_t-theRect->t)+tmax;
			base = lm->data;
			base += (fa->light_t * lightmap_width + fa->light_s) * lightmap_bytes;
			R_BuildLightMap (fa, base, lightmap_width * lightmap_bytes);
		}
	}
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
	struct lightmap_s *lm = &lightmaps[surf->lightmaptexturenum];
	byte *base = lm->data + (surf->light_t * lightmap_width + surf->light_s) * lightmap_bytes;
	R_BuildLightMap (surf, base, lightmap_width * lightmap_bytes);
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
	if (lightmaps)
	{
		free (lightmaps);
		lightmaps = NULL;
	}
	lightmap_texture = NULL;
	last_lightmap_allocated = 0;
	lightmap_count = 0;
	lightmap_width = 0;
	lightmap_height = 0;
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
	lightmap_data = (byte *) calloc (lightmap_bytes, lightmap_width * lightmap_height);

	// compute offsets for each lightmap block
	for (i=0; i<lightmap_count; i++)
	{
		lm = &lightmaps[i];
		lm->xofs = (i % xblocks) * LMBLOCK_WIDTH;
		lm->yofs = (i / xblocks) * LMBLOCK_HEIGHT;
		lm->data = lightmap_data + (lm->yofs * lightmap_width + lm->xofs) * lightmap_bytes;
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

	lightmap_texture =
		TexMgr_LoadImage (cl.worldmodel, "lightmap", lightmap_width, lightmap_height,
			SRC_LIGHTMAP, lightmap_data, "", (src_offset_t)lightmap_data, TEXPREF_LINEAR | TEXPREF_NOPICMIP
		);

	for (i=0; i<lightmap_count; i++)
	{
		lm = &lightmaps[i];
		lm->texture = lightmap_texture;
		lm->modified = false;
		lm->rectchange.l = LMBLOCK_WIDTH;
		lm->rectchange.t = LMBLOCK_HEIGHT;
		lm->rectchange.w = 0;
		lm->rectchange.h = 0;
	}

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

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (msurface_t *surf)
{
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;
	//johnfitz -- lit support via lordhavoc
	float		cred, cgreen, cblue, brightness;
	unsigned	*bl;
	//johnfitz

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if (! (surf->dlightbits[lnum >> 5] & (1U << (lnum & 31))))
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		//johnfitz -- lit support via lordhavoc
		bl = blocklights;
		cred = cl_dlights[lnum].color[0] * 256.0f;
		cgreen = cl_dlights[lnum].color[1] * 256.0f;
		cblue = cl_dlights[lnum].color[2] * 256.0f;
		//johnfitz
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
				//johnfitz -- lit support via lordhavoc
				{
					brightness = rad - dist;
					bl[0] += (int) (brightness * cred);
					bl[1] += (int) (brightness * cgreen);
					bl[2] += (int) (brightness * cblue);
				}
				bl += 3;
				//johnfitz
			}
		}
	}
}

/*
===============
R_AccumulateLightmap

Scales 'lightmap' contents (RGB8) by 'scale' and accumulates
the result in the 'blocklights' array (RGB32)
===============
*/
void R_AccumulateLightmap(byte* lightmap, unsigned scale, int texels)
{
	unsigned *bl = blocklights;
	int size = texels * 3;

#ifdef USE_SSE2
	if (use_simd && size >= 8)
	{
		__m128i vscale = _mm_set1_epi16(scale);
		__m128i vlo, vhi, vdst, vsrc, v;

		while (size >= 8)
		{
			vsrc = _mm_loadl_epi64((const __m128i*)lightmap);

			v = _mm_unpacklo_epi8(vsrc, _mm_setzero_si128());
			vlo = _mm_mullo_epi16(v, vscale);
			vhi = _mm_mulhi_epu16(v, vscale);

			vdst = _mm_loadu_si128((const __m128i*)bl);
			vdst = _mm_add_epi32(vdst, _mm_unpacklo_epi16(vlo, vhi));
			_mm_storeu_si128((__m128i*)bl, vdst);
			bl += 4;

			vdst = _mm_loadu_si128((const __m128i*)bl);
			vdst = _mm_add_epi32(vdst, _mm_unpackhi_epi16(vlo, vhi));
			_mm_storeu_si128((__m128i*)bl, vdst);
			bl += 4;

			lightmap += 8;
			size -= 8;
		}
	}
#endif // def USE_SSE2

	while (size-- > 0)
		*bl++ += *lightmap++ * scale;
}

/*
===============
R_StoreLightmap

Converts contiguous lightmap info accumulated in 'blocklights'
from RGB32 (with 8 fractional bits) to RGBA8, saturates and
stores the result in 'dest'
===============
*/
void R_StoreLightmap(byte* dest, int width, int height, int stride)
{
	unsigned *src = blocklights;

#ifdef USE_SSE2
	if (use_simd)
	{
		__m128i vzero = _mm_setzero_si128();

		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				__m128i v = _mm_srli_epi32(_mm_loadu_si128((const __m128i*)src), 8);
				v = _mm_packs_epi32(v, vzero);
				v = _mm_packus_epi16(v, vzero);
				((uint32_t*)dest)[i] = _mm_cvtsi128_si32(v) | 0xff000000;
				src += 3;
			}
			dest += stride;
		}
	}
	else
#endif // def USE_SSE2
	{
		stride -= width * 4;
		while (height-- > 0)
		{
			int i;
			for (i = 0; i < width; i++)
			{
				unsigned c;
				c = *src++ >> 8; *dest++ = q_min(c, 255);
				c = *src++ >> 8; *dest++ = q_min(c, 255);
				c = *src++ >> 8; *dest++ = q_min(c, 255);
				*dest++ = 255;
			}
			dest += stride;
		}
	}
}

/*
===============
R_BuildLightMap -- johnfitz -- revised for lit support via lordhavoc

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)
{
	int			smax, tmax;
	int			size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;

	surf->cached_dlight = (surf->dlightframe == r_framecount);

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	if (cl.worldmodel->lightdata)
	{
		// clear to no light
		memset (&blocklights[0], 0, size * 3 * sizeof (unsigned int)); //johnfitz -- lit support via lordhavoc

		// add all the lightmaps
		if (lightmap)
		{
			for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
				 maps++)
			{
				scale = d_lightstylevalue[surf->styles[maps]];
				surf->cached_light[maps] = scale;	// 8.8 fraction
				//johnfitz -- lit support via lordhavoc
				R_AccumulateLightmap(lightmap, scale, size);
				lightmap += size * 3;
				//johnfitz
			}
		}

		// add all the dynamic lights
		if (surf->dlightframe == r_framecount)
			R_AddDynamicLights (surf);
	}
	else
	{
		// set to full bright if no light data
		memset (&blocklights[0], 255, size * 3 * sizeof (unsigned int)); //johnfitz -- lit support via lordhavoc
	}

	R_StoreLightmap(dest, smax, tmax, stride);
}

/*
===============
R_UploadLightmap -- johnfitz -- uploads the modified lightmap to opengl if necessary

assumes lightmap texture is already bound
===============
*/
static void R_UploadLightmap(int lmap)
{
	struct lightmap_s *lm = &lightmaps[lmap];

	if (!lm->modified)
		return;

	lm->modified = false;

	glTexSubImage2D (GL_TEXTURE_2D, 0, lm->xofs + lm->rectchange.l, lm->yofs + lm->rectchange.t, lm->rectchange.w, lm->rectchange.h,
		gl_lightmap_format, GL_UNSIGNED_BYTE, lm->data + (lm->rectchange.t * lightmap_width + lm->rectchange.l) * lightmap_bytes);

	lm->rectchange.l = LMBLOCK_WIDTH;
	lm->rectchange.t = LMBLOCK_HEIGHT;
	lm->rectchange.h = 0;
	lm->rectchange.w = 0;

	rs_dynamiclightmaps++;
}

void R_UploadLightmaps (void)
{
	int lmap;
	qboolean anychange = false;

	for (lmap = 0; lmap < lightmap_count; lmap++)
	{
		if (!lightmaps[lmap].modified)
			continue;

		if (!anychange)
		{
			anychange = true;
			glPixelStorei (GL_UNPACK_ROW_LENGTH, lightmap_width);
		}

		GL_Bind (GL_TEXTURE0, lightmaps[lmap].texture);
		R_UploadLightmap(lmap);
	}

	if (anychange)
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
}

/*
================
R_RebuildAllLightmaps -- johnfitz -- called when gl_overbright gets toggled
================
*/
void R_RebuildAllLightmaps (void)
{
	int			i, j;
	qmodel_t	*mod;
	msurface_t	*fa;
	byte		*base;

	if (!cl.worldmodel) // is this the correct test?
		return;

	glPixelStorei (GL_UNPACK_ROW_LENGTH, lightmap_width);

	//for each surface in each model, rebuild lightmap with new scale
	for (i=1; i<MAX_MODELS; i++)
	{
		if (!(mod = cl.model_precache[i]))
			continue;
		fa = &mod->surfaces[mod->firstmodelsurface];
		for (j=0; j<mod->nummodelsurfaces; j++, fa++)
		{
			if (fa->flags & SURF_DRAWTILED)
				continue;
			base = lightmaps[fa->lightmaptexturenum].data;
			base += (fa->light_t * lightmap_width + fa->light_s) * lightmap_bytes;
			R_BuildLightMap (fa, base, lightmap_width * lightmap_bytes);
		}
	}

	//for each lightmap, upload it
	for (i=0; i<lightmap_count; i++)
	{
		struct lightmap_s *lm = &lightmaps[i];
		GL_Bind (GL_TEXTURE0, lm->texture);
		glTexSubImage2D (GL_TEXTURE_2D, 0, lm->xofs, lm->yofs, LMBLOCK_WIDTH, LMBLOCK_HEIGHT,
			gl_lightmap_format, GL_UNSIGNED_BYTE, lightmaps[i].data);
	}

	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
}
