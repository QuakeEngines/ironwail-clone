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

extern gltexture_t *lightmap_texture;

extern GLuint gl_bmodel_vbo;
extern size_t gl_bmodel_vbo_size;
extern GLuint gl_bmodel_ibo;
extern size_t gl_bmodel_ibo_size;
extern GLuint gl_bmodel_indirect_buffer;
extern GLuint gl_bmodel_leaf_buffer;
extern GLuint gl_bmodel_surf_buffer;
extern GLuint gl_bmodel_marksurf_buffer;

static GLuint r_world_program;
static GLuint r_water_program;
static GLuint r_reset_indirect_draw_params_program;
static GLuint r_cull_leaves_program;

typedef struct gpumark_frame_s {
	vec4_t		frustum[4];
	GLuint		oldskyleaf;
	float		vieworg[3];
	GLuint		framecount;
	GLuint		padding[3];
} gpumark_frame_t;

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

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

/*
===============
R_MarkVisSurfaces
===============
*/
static void R_MarkVisSurfaces (byte* vis)
{
	int			i;
	GLuint		buf;
	GLbyte*		ofs;
	size_t		vissize = (cl.worldmodel->numleafs + 7) >> 3;
	gpumark_frame_t frame;

	GL_BeginGroup ("Mark surfaces");

	for (i = 0; i < 4; i++)
	{
		frame.frustum[i][0] = frustum[i].normal[0];
		frame.frustum[i][1] = frustum[i].normal[1];
		frame.frustum[i][2] = frustum[i].normal[2];
		frame.frustum[i][3] = frustum[i].dist;
	}
	frame.oldskyleaf = r_oldskyleaf.value != 0.f;
	frame.vieworg[0] = r_refdef.vieworg[0];
	frame.vieworg[1] = r_refdef.vieworg[1];
	frame.vieworg[2] = r_refdef.vieworg[2];
	frame.framecount = r_framecount;

	vissize = (vissize + 3) & ~3; // round up to uint

	GL_UseProgram (r_reset_indirect_draw_params_program);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, gl_bmodel_indirect_buffer, 0, cl.worldmodel->numusedtextures * sizeof(bmodel_draw_indirect_t));
	GL_DispatchComputeFunc ((cl.worldmodel->numusedtextures + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	GL_UseProgram (r_cull_leaves_program);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_ibo, 0, gl_bmodel_ibo_size);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, vis, vissize, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, vissize);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, gl_bmodel_leaf_buffer, 0, cl.worldmodel->numleafs * sizeof(bmodel_gpu_leaf_t));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, gl_bmodel_marksurf_buffer, 0, cl.worldmodel->nummarksurfaces * sizeof(cl.worldmodel->marksurfaces[0]));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_surf_buffer, 0, cl.worldmodel->numsurfaces * sizeof(bmodel_gpu_surf_t));
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &frame, sizeof(frame), &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 6, buf, (GLintptr)ofs, sizeof(frame));

	GL_DispatchComputeFunc ((cl.worldmodel->numleafs + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);

	GL_EndGroup ();
}

#if defined(USE_SIMD)
/*
===============
R_AddStaticModelsSIMD
===============
*/
void R_AddStaticModelsSIMD (byte *vis)
{
	int			i, j;
	int			numleafs = cl.worldmodel->numleafs;
	soa_aabb_t	*leafbounds = cl.worldmodel->soa_leafbounds;

	for (i = 0; i < numleafs; i += 8)
	{
		mleaf_t *leaf;
		int mask = vis[i>>3];
		if (mask == 0)
			continue;

		mask = R_CullBoxSIMD (leafbounds[i>>3], mask);
		if (mask == 0)
			continue;

		for (j = 0, leaf = &cl.worldmodel->leafs[1 + i]; j < 8 && i + j < numleafs; j++, leaf++)
			if ((mask & (1 << j)) && leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
	}
}
#endif // defined(USE_SIMD)

/*
===============
R_AddStaticModels
===============
*/
void R_AddStaticModels (byte* vis)
{
	int			i;
	mleaf_t		*leaf;

	for (i = 0, leaf = &cl.worldmodel->leafs[1]; i < cl.worldmodel->numleafs; i++, leaf++)
		if (vis[i>>3] & (1<<(i&7)) && !R_CullBox(leaf->minmaxs, leaf->minmaxs + 3) && leaf->efrags)
			R_StoreEfrags (&leaf->efrags);
}

/*
===============
R_MarkSurfaces
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

	R_MarkVisSurfaces (vis);

#if defined(USE_SIMD)
	if (use_simd)
		R_AddStaticModelsSIMD (vis);
	else
#endif
		R_AddStaticModels (vis);
}

/*
================
GL_WaterAlphaForEntityTexture
 
Returns the water alpha to use for the entity and texture combination.
================
*/
float GL_WaterAlphaForEntityTexture (entity_t *ent, texture_t *t)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForTextureType(t->type);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}

typedef struct {
	float	mvp[16];
	vec4_t	fog;
	int		use_alpha_test;
	float	alpha;
	float	time;
	int		padding;
} worlduniforms_t;

/*
=============
GLWorld_CreateShaders
=============
*/
void GLWorld_CreateShaders (void)
{
	#define WORLD_PARAM_BUFFER\
		"layout(std430, binding=1) restrict readonly buffer ParamBuffer\n"\
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
		"layout(std430, binding=2) restrict readonly buffer VertexBuffer\n"\
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
		"layout(location=0) out vec3 out_pos;\n"
		"layout(location=1) out vec4 out_uv;\n"
		"layout(location=2) out float out_fogdist;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	PackedVertex vert = vertices[gl_VertexID];\n"
		"	out_pos = vec3(vert.data[0], vert.data[1], vert.data[2]);\n"
		"	gl_Position = MVP * vec4(out_pos, 1.0);\n"
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
		"struct Light\n"
		"{\n"
		"	vec3	origin;\n"
		"	float	radius;\n"
		"	vec3	color;\n"
		"	float	minlight;\n"
		"};\n"
		"\n"
		"layout(std430, binding=0) restrict readonly buffer LightBuffer\n"
		"{\n"
		"	Light lights[];\n"
		"};\n"
		"\n"
		"layout(location=0) in vec3 in_pos;\n"
		"layout(location=1) in vec4 in_uv;\n"
		"layout(location=2) in float in_fogdist;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture(Tex, in_uv.xy);\n"
		"	if (UseAlphaTest && result.a < 0.666)\n"
		"		discard;\n"
		"	vec3 total_light = texture(LMTex, in_uv.zw).rgb;\n"
		"	int numlights = lights.length() - 1;\n"
		"	if (numlights > 0)\n"
		"	{\n"
		"		int i;\n"
		"		vec3 nor = normalize(cross(dFdx(in_pos), dFdy(in_pos)));\n"
		"		float planedist = dot(in_pos, nor);\n"
		"		for (i = 0; i < numlights; i++)\n"
		"		{\n"
		"			Light l = lights[i];\n"
		"			// mimics R_AddDynamicLights, up to a point\n"
		"			float rad = l.radius;\n"
		"			float dist = dot(l.origin, nor) - planedist;\n"
		"			rad -= abs(dist);\n"
		"			float minlight = l.minlight;\n"
		"			if (rad < minlight)\n"
		"				continue;\n"
		"			vec3 local_pos = l.origin - nor * dist;\n"
		"			minlight = rad - minlight;\n"
		"			dist = length(in_pos - local_pos);\n"
		"			total_light += clamp((minlight - dist) / 16.0, 0.0, 1.0) * (rad - dist) * l.color;\n"
		"		}\n"
		"	}\n"
		"	result.rgb *= clamp(total_light, 0.0, 1.0) * 2.0;\n"
		"	result.rgb += texture(FullbrightTex, in_uv.xy).rgb;\n"
		"	result = clamp(result, 0.0, 1.0);\n"
		"	float fog = exp2(-(Fog.w * in_fogdist) * (Fog.w * in_fogdist));\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result.rgb = mix(Fog.rgb, result.rgb, fog);\n"
		"	result.a = Alpha;\n" // FIXME: This will make almost transparent things cut holes though heavy fog
		"	out_fragcolor = result;\n"
		"}\n";

	r_world_program = GL_CreateProgram (vertSource, fragSource, "world");
	
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

	#define WORLD_DRAW_BUFFER\
		"struct DrawElementsIndirectCommand\n"\
		"{\n"\
		"	uint	count;\n"\
		"	uint	instanceCount;\n"\
		"	uint	firstIndex;\n"\
		"	uint	baseVertex;\n"\
		"	uint	baseInstance;\n"\
		"};\n"\
		"\n"\
		"layout(std430, binding=0) buffer DrawIndirectBuffer\n"\
		"{\n"\
		"	DrawElementsIndirectCommand cmds[];\n"\
		"};\n"\

	const char* computeSource = \
		"#version 430\n"
		"\n"
		"layout(local_size_x=64) in;\n"
		"\n"
		WORLD_DRAW_BUFFER
		"\n"
		"void main()\n"
		"{\n"
		"	uint thread_id = gl_GlobalInvocationID.x;\n"
		"	if (thread_id < cmds.length())\n"
		"		cmds[thread_id].count = 0u;\n"
		"}\n";

	r_reset_indirect_draw_params_program = GL_CreateComputeProgram (computeSource, "clear indirect draw params");

	computeSource = \
		"#version 430\n"
		"\n"
		"layout(local_size_x=64) in;\n"
		"\n"
		WORLD_DRAW_BUFFER
		"\n"
		"layout(std430, binding=1) restrict writeonly buffer IndexBuffer\n"
		"{\n"
		"	uint indices[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=2) restrict readonly buffer VisBuffer\n"
		"{\n"
		"	uint vis[];\n"
		"};\n"
		"\n"
		"struct Leaf\n"
		"{\n"
		"	float	mins[3];\n"
		"	float	maxs[3];\n"
		"	uint	firstsurf;\n"
		"	uint	surfcountsky; // bit 0=sky; bits 1..31=surfcount\n"
		"};\n"
		"\n"
		"layout(std430, binding=3) restrict readonly buffer LeafBuffer\n"
		"{\n"
		"	Leaf leaves[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=4) restrict readonly buffer MarkSurfBuffer\n"
		"{\n"
		"	int marksurf[];\n"
		"};\n"
		"\n"
		"struct Surface\n"
		"{\n"
		"	vec4	plane;\n"
		"	uint	framecount;\n"
		"	uint	texnum;\n"
		"	uint	numedges;\n"
		"	uint	firstvert;\n"
		"};\n"
		"\n"
		"layout(std430, binding=5) restrict buffer SurfaceBuffer\n"
		"{\n"
		"	Surface surfaces[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=6) restrict readonly buffer FrameBuffer\n"
		"{\n"
		"	vec4	frustum[4];\n"
		"	uint	oldskyleaf;\n"
		"	float	vieworg[3];\n"
		"	uint	framecount;\n"
		"	uint	padding[3];\n"
		"};\n"
		"\n"
		"void main()\n"
		"{\n"
		"	uint thread_id = gl_GlobalInvocationID.x;\n"
		"	if (thread_id >= leaves.length())\n"
		"		return;\n"
		"	uint visible = vis[thread_id >> 5u] & (1u << (thread_id & 31u));\n"
		"	if (visible == 0u)\n"
		"		return;\n"
		"\n"
		"	Leaf leaf = leaves[thread_id];\n"
		"	uint i, j;\n"
		"	for (i = 0u; i < 4u; i++)\n"
		"	{\n"
		"		vec4 plane = frustum[i];\n"
		"		vec3 v;\n"
		"		v.x = plane.x < 0.f ? leaf.mins[0] : leaf.maxs[0];\n"
		"		v.y = plane.y < 0.f ? leaf.mins[1] : leaf.maxs[1];\n"
		"		v.z = plane.z < 0.f ? leaf.mins[2] : leaf.maxs[2];\n"
		"		if (dot(plane.xyz, v) < plane.w)\n"
		"			return;\n"
		"	}\n"
		"\n"
		"	if ((leaf.surfcountsky & 1u) > oldskyleaf)\n"
		"		return;\n"
		"	uint surfcount = leaf.surfcountsky >> 1u;\n"
		"	vec3 campos = vec3(vieworg[0], vieworg[1], vieworg[2]);\n"
		"	for (i = 0u; i < surfcount; i++)\n"
		"	{\n"
		"		int surfindex = marksurf[leaf.firstsurf + i];\n"
		"		Surface surf = surfaces[surfindex];\n"
		"		if (dot(surf.plane.xyz, campos) < surf.plane.w)\n"
		"			continue;\n"
		"		if (atomicExchange(surfaces[surfindex].framecount, framecount) == framecount)\n"
		"			continue;\n"
		"		uint texnum = surf.texnum;\n"
		"		uint numedges = surf.numedges;\n"
		"		uint firstvert = surf.firstvert;\n"
		"		uint ofs = cmds[texnum].firstIndex + atomicAdd(cmds[texnum].count, 3u * (numedges - 2u));\n"
		"		for (j = 2u; j < numedges; j++)\n"
		"		{\n"
		"			indices[ofs++] = firstvert;\n"
		"			indices[ofs++] = firstvert + j - 1u;\n"
		"			indices[ofs++] = firstvert + j;\n"
		"		}\n"
		"	}\n"
		"}\n";

	#undef WORLD_DRAW_BUFFER

	r_cull_leaves_program = GL_CreateComputeProgram (computeSource, "mark");
}

/*
================
R_DrawBrushFaces
================
*/
void R_DrawBrushFaces (qmodel_t *model, entity_t *ent)
{
	texture_t	*t;
	int			i;
	float		entalpha;
	unsigned	state;
	qboolean	setup = false;
	worlduniforms_t uniforms;

	entalpha = (ent != NULL) ? ENTALPHA_DECODE(ent->alpha) : 1.0f;

	for (i = 0; i < model->numusedtextures; i++)
	{
		gltexture_t	*fullbright = NULL;
		int			use_alpha_test;

		t = model->textures[model->usedtextures[i]];
		if (!t || !t->gltexture || t->type > TEXTYPE_CUTOUT)
			continue;

		use_alpha_test = (t->type == TEXTYPE_CUTOUT);

		t = R_TextureAnimation(t, ent != NULL ? ent->frame : 0);

		if (!gl_fullbrights.value || !(fullbright = t->fullbright))
			fullbright = blacktexture;

		if (!setup || uniforms.use_alpha_test != use_alpha_test)
		{
			GLuint buf;
			GLbyte *ofs;

			if (!setup)
			{
				state = GLS_CULL_BACK | GLS_ATTRIBS(0);
				if (entalpha < 1)
					state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
				else
					state |= GLS_BLEND_OPAQUE;
	
				GL_UseProgram (r_world_program);
				GL_SetState (state);
				GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);

				GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

				memcpy(uniforms.mvp, r_matviewproj, 16 * sizeof(float));
				memcpy(uniforms.fog, fog_data, 4 * sizeof(float));
				uniforms.alpha = entalpha;
				uniforms.time = cl.time;
				uniforms.padding = 0;
				uniforms.use_alpha_test = use_alpha_test;

				GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
				GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_indirect_buffer);

				setup = true;
			}
			else
			{
				uniforms.use_alpha_test = use_alpha_test;
			}

			GL_Upload (GL_SHADER_STORAGE_BUFFER, &uniforms, sizeof(uniforms), &buf, &ofs);
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(uniforms));
		}

		GL_Bind (GL_TEXTURE0, r_lightmap_cheatsafe ? greytexture : t->gltexture);
		GL_Bind (GL_TEXTURE1, r_lightmap_cheatsafe ? blacktexture : fullbright);
		GL_DrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, (void*)(sizeof(bmodel_draw_indirect_t) * (model->firstcmd + i)));
	}
}

/*
================
R_DrawBrushFaces_Water -- johnfitz
================
*/
void R_DrawBrushFaces_Water (qmodel_t *model, entity_t *ent)
{
	int			i;
	qboolean	setup = false;
	float		old_alpha;
	worlduniforms_t uniforms;

	for (i = 0; i < model->numusedtextures; i++)
	{
		float alpha;
		texture_t *t = model->textures[model->usedtextures[i]];
		if (!t || !t->gltexture || !TEXTYPE_ISLIQUID(t->type))
			continue;

		alpha = GL_WaterAlphaForEntityTexture (ent, t);

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
				GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
				GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_indirect_buffer);

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
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(uniforms));
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

			setup = true;
		}

		GL_Bind (GL_TEXTURE0, r_lightmap_cheatsafe ? whitetexture : t->gltexture);
		GL_DrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, (void*)(sizeof(bmodel_draw_indirect_t) * (model->firstcmd + i)));
	}
}

/*
================
R_DrawBrushFaces_ShowTris -- johnfitz
================
*/
void R_DrawBrushFaces_ShowTris (qmodel_t *model)
{
	int			i;
	texture_t	*t;
	qboolean	setup = false;

	for (i=0 ; i<model->numusedtextures ; i++)
	{
		t = model->textures[model->usedtextures[i]];
		if (!t)
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
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(uniforms));
			GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

			GL_Bind (GL_TEXTURE0, whitetexture);
			GL_Bind (GL_TEXTURE1, whitetexture);
			GL_Bind (GL_TEXTURE2, whitetexture);
		
			GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);
			GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_indirect_buffer);

			setup = true;
		}

		GL_DrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, (void*)(sizeof(bmodel_draw_indirect_t) * (model->firstcmd + i)));
	}
}

/*
=============
R_DrawWorld
=============
*/
void R_DrawWorld (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	GL_BeginGroup ("World");

	R_DrawBrushFaces (cl.worldmodel, NULL);

	GL_EndGroup ();
}

/*
=============
R_DrawWorld_Water
=============
*/
void R_DrawWorld_Water (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	GL_BeginGroup ("World water");

	R_DrawBrushFaces_Water (cl.worldmodel, NULL);

	GL_EndGroup ();
}

/*
=============
R_DrawWorld_ShowTris
=============
*/
void R_DrawWorld_ShowTris (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	GL_BeginGroup ("World tris");

	R_DrawBrushFaces_ShowTris (cl.worldmodel);

	GL_EndGroup ();
}
