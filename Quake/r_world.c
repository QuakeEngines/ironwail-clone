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
extern cvar_t gl_zfix; // QuakeSpasm z-fighting fix

extern gltexture_t *lightmap_texture;

extern GLuint gl_bmodel_vbo;
extern size_t gl_bmodel_vbo_size;
extern GLuint gl_bmodel_ibo;
extern size_t gl_bmodel_ibo_size;
extern GLuint gl_bmodel_indirect_buffer;
extern size_t gl_bmodel_indirect_buffer_size;
extern GLuint gl_bmodel_leaf_buffer;
extern GLuint gl_bmodel_surf_buffer;
extern GLuint gl_bmodel_marksurf_buffer;

static GLuint r_world_program;
static GLuint r_world_program_bindless;
static GLuint r_water_program;
static GLuint r_water_program_bindless;
static GLuint r_skystencil_program;
static GLuint r_skystencil_program_bindless;
static GLuint r_reset_indirect_draw_params_program;
static GLuint r_cull_leaves_program;
static GLuint r_gather_indirect_draw_params_program;

#define MAX_BMODEL_DRAWS		4096
#define MAX_BMODEL_INSTANCES	1024
static GLuint gl_bmodel_compacted_indirect_buffer;

typedef struct gpumark_frame_s {
	vec4_t		frustum[4];
	GLuint		oldskyleaf;
	float		vieworg[3];
	GLuint		framecount;
	GLuint		padding[3];
} gpumark_frame_t;

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);

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
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, gl_bmodel_indirect_buffer, 0, cl.worldmodel->texofs[TEXTYPE_COUNT] * sizeof(bmodel_draw_indirect_t));
	GL_DispatchComputeFunc ((cl.worldmodel->texofs[TEXTYPE_COUNT] + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	GL_UseProgram (r_cull_leaves_program);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, gl_bmodel_ibo, 0, gl_bmodel_ibo_size);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, vis, vissize, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, buf, (GLintptr)ofs, vissize);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, gl_bmodel_leaf_buffer, 0, cl.worldmodel->numleafs * sizeof(bmodel_gpu_leaf_t));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_marksurf_buffer, 0, cl.worldmodel->nummarksurfaces * sizeof(cl.worldmodel->marksurfaces[0]));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 6, gl_bmodel_surf_buffer, 0, cl.worldmodel->numsurfaces * sizeof(bmodel_gpu_surf_t));
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &frame, sizeof(frame), &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 7, buf, (GLintptr)ofs, sizeof(frame));

	GL_DispatchComputeFunc ((cl.worldmodel->numleafs + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);

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
GL_WaterAlphaForEntityTextureType
 
Returns the water alpha to use for the entity and texture type combination.
================
*/
float GL_WaterAlphaForEntityTextureType (entity_t *ent, textype_t type)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForTextureType(type);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}

/*
=============
GLWorld_CreateResources
=============
*/
void GLWorld_CreateResources (void)
{
	GL_GenBuffersFunc (1, &gl_bmodel_compacted_indirect_buffer);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_compacted_indirect_buffer);
	GL_BufferDataFunc (GL_DRAW_INDIRECT_BUFFER, sizeof(bmodel_draw_indirect_t) * MAX_BMODEL_DRAWS, NULL, GL_DYNAMIC_DRAW);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, 0);

	#define FRAMEDATA_BUFFER\
		"struct Light\n"\
		"{\n"\
		"	vec3	origin;\n"\
		"	float	radius;\n"\
		"	vec3	color;\n"\
		"	float	minlight;\n"\
		"};\n"\
		"\n"\
		"layout(std430, binding=0) restrict readonly buffer FrameDataBuffer\n"\
		"{\n"\
		"	mat4	ViewProj;\n"\
		"	vec3	FogColor;\n"\
		"	float	FogDensity;\n"\
		"	float	Time;\n"\
		"	int		NumLights;\n"\
		"	float	padding_framedatabuffer[2];\n"\
		"	Light	lights[];\n"\
		"};\n"\

	#define WORLD_CALLDATA_BUFFER\
		"struct Call\n"\
		"{\n"\
		"	uvec2	txhandle;\n"\
		"	uvec2	fbhandle;\n"\
		"	int		flags;\n"\
		"	float	wateralpha;\n"\
		"};\n"\
		"const int\n"\
		"	CF_USE_ALPHA_TEST = 1,\n"\
		"	CF_USE_POLYGON_OFFSET = 2\n"\
		";\n"\
		"\n"\
		"layout(std430, binding=1) restrict readonly buffer CallBuffer\n"\
		"{\n"\
		"	Call call_data[];\n"\
		"};\n"\

	#define WORLD_INSTANCEDATA_BUFFER\
		"struct Instance\n"\
		"{\n"\
		"	vec4	mat[3];\n"\
		"	float	alpha;\n"\
		"	float	padding;\n"\
		"};\n"\
		"\n"\
		"layout(std430, binding=2) restrict readonly buffer InstanceBuffer\n"\
		"{\n"\
		"	Instance instance_data[];\n"\
		"};\n"\
		"\n"\

	#define WORLD_VERTEX_BUFFER\
		"struct PackedVertex\n"\
		"{\n"\
		"	float data[7];\n"\
		"};\n"\
		"\n"\
		"layout(std430, binding=3) restrict readonly buffer VertexBuffer\n"\
		"{\n"\
		"	PackedVertex vertices[];\n"\
		"};\n"\
		"\n"\

	#define WORLD_VERTEX_SHADER(bindless)\
		"#version 430\n"\
		"\n"\
		FRAMEDATA_BUFFER\
		WORLD_CALLDATA_BUFFER\
		WORLD_INSTANCEDATA_BUFFER\
		WORLD_VERTEX_BUFFER\
		"\n"\
		"#define USE_BINDLESS " QS_STRINGIFY(bindless) "\n"\
		"#if USE_BINDLESS\n"\
		"	#extension GL_ARB_shader_draw_parameters : require\n"\
		"	#define DRAW_ID			gl_DrawIDARB\n"\
		"	#define INSTANCE_ID		(gl_BaseInstanceARB + gl_InstanceID)\n"\
		"#else\n"\
		"	#define DRAW_ID			0\n"\
		"	#define INSTANCE_ID		gl_InstanceID\n"\
		"#endif\n"\
		"\n"\
		"layout(location=0) flat out ivec2 out_drawinstance; // x = draw; y = instance\n"\
		"layout(location=1) out vec3 out_pos;\n"\
		"layout(location=2) out vec4 out_uv;\n"\
		"layout(location=3) out float out_fogdist;\n"\
		"\n"\
		"void main()\n"\
		"{\n"\
		"	PackedVertex vert = vertices[gl_VertexID];\n"\
		"	Call call = call_data[DRAW_ID];\n"\
		"	Instance instance = instance_data[INSTANCE_ID];\n"\
		"	mat4x3 world = transpose(mat3x4(instance.mat[0], instance.mat[1], instance.mat[2]));\n"\
		"	out_pos = mat3(world[0], world[1], world[2]) * vec3(vert.data[0], vert.data[1], vert.data[2]) + world[3];\n"\
		"	gl_Position = ViewProj * vec4(out_pos, 1.0);\n"\
		"	if ((call.flags & CF_USE_POLYGON_OFFSET) != 0)\n"\
		"		gl_Position.z += 1./1024.;\n"\
		"	out_uv = vec4(vert.data[3], vert.data[4], vert.data[5], vert.data[6]);\n"\
		"	out_fogdist = gl_Position.w;\n"\
		"	out_drawinstance = ivec2(DRAW_ID, INSTANCE_ID);\n"\
		"}\n"\

	#define WORLD_FRAGMENT_SHADER(bindless)\
		"#version 430\n"\
		"\n"\
		"#define USE_BINDLESS " QS_STRINGIFY(bindless) "\n"\
		"#if USE_BINDLESS\n"\
		"	#extension GL_ARB_bindless_texture : require\n"\
		"	sampler2D Tex;\n"\
		"	sampler2D FullbrightTex;\n"\
		"#else\n"\
		"	layout(binding=0) uniform sampler2D Tex;\n"\
		"	layout(binding=1) uniform sampler2D FullbrightTex;\n"\
		"#endif\n"\
		"layout(binding=2) uniform sampler2D LMTex;\n"\
		"\n"\
		FRAMEDATA_BUFFER\
		WORLD_CALLDATA_BUFFER\
		WORLD_INSTANCEDATA_BUFFER\
		"\n"\
		"layout(location=0) flat in ivec2 in_drawinstance;\n"\
		"layout(location=1) in vec3 in_pos;\n"\
		"layout(location=2) in vec4 in_uv;\n"\
		"layout(location=3) in float in_fogdist;\n"\
		"\n"\
		"layout(location=0) out vec4 out_fragcolor;\n"\
		"\n"\
		"void main()\n"\
		"{\n"\
		"	Call call = call_data[in_drawinstance.x];\n"\
		"	Instance instance = instance_data[in_drawinstance.y];\n"\
		"#if USE_BINDLESS\n"\
		"	Tex = sampler2D(call.txhandle);\n"\
		"	FullbrightTex = sampler2D(call.fbhandle);\n"\
		"#endif\n"\
		"	vec4 result = texture(Tex, in_uv.xy);\n"\
		"	vec3 fullbright = texture(FullbrightTex, in_uv.xy).rgb;\n"\
		"	if ((call.flags & CF_USE_ALPHA_TEST) != 0 && result.a < 0.666)\n"\
		"		discard;\n"\
		"	vec3 total_light = texture(LMTex, in_uv.zw).rgb;\n"\
		"	int numlights = NumLights;\n"\
		"	if (numlights > 0)\n"\
		"	{\n"\
		"		int i;\n"\
		"		vec3 nor = normalize(cross(dFdx(in_pos), dFdy(in_pos)));\n"\
		"		float planedist = dot(in_pos, nor);\n"\
		"		for (i = 0; i < numlights; i++)\n"\
		"		{\n"\
		"			Light l = lights[i];\n"\
		"			// mimics R_AddDynamicLights, up to a point\n"\
		"			float rad = l.radius;\n"\
		"			float dist = dot(l.origin, nor) - planedist;\n"\
		"			rad -= abs(dist);\n"\
		"			float minlight = l.minlight;\n"\
		"			if (rad < minlight)\n"\
		"				continue;\n"\
		"			vec3 local_pos = l.origin - nor * dist;\n"\
		"			minlight = rad - minlight;\n"\
		"			dist = length(in_pos - local_pos);\n"\
		"			total_light += clamp((minlight - dist) / 16.0, 0.0, 1.0) * max(0., rad - dist) / 256. * l.color;\n"\
		"		}\n"\
		"	}\n"\
		"	result.rgb *= clamp(total_light, 0.0, 1.0) * 2.0;\n"\
		"	result.rgb += fullbright;\n"\
		"	result = clamp(result, 0.0, 1.0);\n"\
		"	float fog = exp2(-(FogDensity * in_fogdist) * (FogDensity * in_fogdist));\n"\
		"	fog = clamp(fog, 0.0, 1.0);\n"\
		"	result.rgb = mix(FogColor, result.rgb, fog);\n"\
		"	float alpha = instance.alpha;\n"\
		"	if (alpha < 0.0)\n"\
		"		alpha = 1.0;\n"\
		"	result.a = alpha; // FIXME: This will make almost transparent things cut holes though heavy fog\n"\
		"	out_fragcolor = vec4(1,0,1,1);\n"\
		"	out_fragcolor = result;\n"\
		"}\n"\

	r_world_program = GL_CreateProgram (WORLD_VERTEX_SHADER(0), WORLD_FRAGMENT_SHADER(0), "world");
	r_world_program_bindless = gl_bindless_able ? GL_CreateProgram (WORLD_VERTEX_SHADER(1), WORLD_FRAGMENT_SHADER(1), "world [bindless]") : 0;
	
	#define WATER_VERTEX_SHADER(bindless)\
		"#version 430\n"\
		"\n"\
		FRAMEDATA_BUFFER\
		WORLD_INSTANCEDATA_BUFFER\
		WORLD_VERTEX_BUFFER\
		"\n"\
		"#define USE_BINDLESS " QS_STRINGIFY(bindless) "\n"\
		"#if USE_BINDLESS\n"\
		"#extension GL_ARB_shader_draw_parameters : require\n"\
		"	#define DRAW_ID			gl_DrawIDARB\n"\
		"	#define INSTANCE_ID		(gl_BaseInstanceARB + gl_InstanceID)\n"\
		"#else\n"\
		"	#define DRAW_ID			0\n"\
		"	#define INSTANCE_ID		gl_InstanceID\n"\
		"#endif\n"\
		"\n"\
		"layout(location=0) flat out ivec2 out_drawinstance; // x = draw; y = instance\n"\
		"layout(location=1) out vec2 out_uv;\n"\
		"layout(location=2) out float out_fogdist;\n"\
		"\n"\
		"void main()\n"\
		"{\n"\
		"	PackedVertex vert = vertices[gl_VertexID];\n"\
		"	Instance instance = instance_data[INSTANCE_ID];\n"\
		"	mat4x3 world = transpose(mat3x4(instance.mat[0], instance.mat[1], instance.mat[2]));\n"\
		"	vec3 pos = mat3(world[0], world[1], world[2]) * vec3(vert.data[0], vert.data[1], vert.data[2]) + world[3];\n"\
		"	gl_Position = ViewProj * vec4(pos, 1.0);\n"\
		"	out_uv = vec2(vert.data[3], vert.data[4]);\n"\
		"	out_fogdist = gl_Position.w;\n"\
		"	out_drawinstance = ivec2(DRAW_ID, INSTANCE_ID);\n"\
		"}\n"\
	
	#define WATER_FRAGMENT_SHADER(bindless)\
		"#version 430\n"\
		"\n"\
		"#define USE_BINDLESS " QS_STRINGIFY(bindless) "\n"\
		"#if USE_BINDLESS\n"\
		"#extension GL_ARB_bindless_texture : require\n"\
		"sampler2D Tex;\n"\
		"#else\n"\
		"layout(binding=0) uniform sampler2D Tex;\n"\
		"#endif\n"\
		"\n"\
		FRAMEDATA_BUFFER\
		WORLD_CALLDATA_BUFFER\
		WORLD_INSTANCEDATA_BUFFER\
		"\n"\
		"layout(location=0) flat in ivec2 in_drawinstance;\n"\
		"layout(location=1) in vec2 in_uv;\n"\
		"layout(location=2) in float in_fogdist;\n"\
		"\n"\
		"layout(location=0) out vec4 out_fragcolor;\n"\
		"\n"\
		"void main()\n"\
		"{\n"\
		"	Call call = call_data[in_drawinstance.x];\n"\
		"	Instance instance = instance_data[in_drawinstance.y];\n"\
		"#if USE_BINDLESS\n"\
		"	Tex = sampler2D(call.txhandle);\n"\
		"#endif\n"\
		"	vec2 uv = in_uv * 2.0 + 0.125 * sin(in_uv.yx * (3.14159265 * 2.0) + Time);\n"\
		"	vec4 result = texture(Tex, uv);\n"\
		"	float fog = exp2(-(FogDensity * in_fogdist) * (FogDensity * in_fogdist));\n"\
		"	fog = clamp(fog, 0.0, 1.0);\n"\
		"	result.rgb = mix(FogColor, result.rgb, fog);\n"\
		"	float alpha = instance.alpha;\n"\
		"	if (alpha < 0.0)\n"\
		"		alpha = call.wateralpha;\n"\
		"	result.a *= alpha;\n"\
		"	out_fragcolor = result;\n"\
		"}\n"\

	r_water_program = GL_CreateProgram (WATER_VERTEX_SHADER(0), WATER_FRAGMENT_SHADER(0), "water");
	r_water_program_bindless = gl_bindless_able ? GL_CreateProgram (WATER_VERTEX_SHADER(1), WATER_FRAGMENT_SHADER(1), "water [bindless]") : 0;

	#define SKYSTENCIL_VERTEX_SHADER(bindless)\
		"#version 430\n"\
		"\n"\
		FRAMEDATA_BUFFER\
		WORLD_INSTANCEDATA_BUFFER\
		WORLD_VERTEX_BUFFER\
		"\n"\
		"#define USE_BINDLESS " QS_STRINGIFY(bindless) "\n"\
		"#if USE_BINDLESS\n"\
		"#extension GL_ARB_shader_draw_parameters : require\n"\
		"	#define DRAW_ID			gl_DrawIDARB\n"\
		"	#define INSTANCE_ID		(gl_BaseInstanceARB + gl_InstanceID)\n"\
		"#else\n"\
		"	#define DRAW_ID			0\n"\
		"	#define INSTANCE_ID		gl_InstanceID\n"\
		"#endif\n"\
		"\n"\
		"layout(location=0) flat out ivec2 out_drawinstance; // x = draw; y = instance\n"\
		"\n"\
		"void main()\n"\
		"{\n"\
		"	PackedVertex vert = vertices[gl_VertexID];\n"\
		"	Instance instance = instance_data[INSTANCE_ID];\n"\
		"	mat4x3 world = transpose(mat3x4(instance.mat[0], instance.mat[1], instance.mat[2]));\n"\
		"	vec3 pos = mat3(world[0], world[1], world[2]) * vec3(vert.data[0], vert.data[1], vert.data[2]) + world[3];\n"\
		"	gl_Position = ViewProj * vec4(pos, 1.0);\n"\
		"	out_drawinstance = ivec2(DRAW_ID, INSTANCE_ID);\n"\
		"}\n"\
	
	r_skystencil_program = GL_CreateProgram (SKYSTENCIL_VERTEX_SHADER(0), NULL, "skystencil");
	r_skystencil_program_bindless = gl_bindless_able ? GL_CreateProgram (SKYSTENCIL_VERTEX_SHADER(1), NULL, "skystencil [bindless]") : 0;

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
		"layout(std430, binding=1) buffer DrawIndirectBuffer\n"\
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
		"layout(std430, binding=2) restrict writeonly buffer IndexBuffer\n"
		"{\n"
		"	uint indices[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=3) restrict readonly buffer VisBuffer\n"
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
		"layout(std430, binding=4) restrict readonly buffer LeafBuffer\n"
		"{\n"
		"	Leaf leaves[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=5) restrict readonly buffer MarkSurfBuffer\n"
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
		"layout(std430, binding=6) restrict buffer SurfaceBuffer\n"
		"{\n"
		"	Surface surfaces[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=7) restrict readonly buffer FrameBuffer\n"
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

	computeSource = \
		"#version 430\n"
		"\n"
		"layout(local_size_x=64) in;\n"
		"\n"
		"struct DrawElementsIndirectCommand\n"
		"{\n"
		"	uint	count;\n"
		"	uint	instanceCount;\n"
		"	uint	firstIndex;\n"
		"	uint	baseVertex;\n"
		"	uint	baseInstance;\n"
		"};\n"
		"\n"
		"layout(std430, binding=5) restrict readonly buffer DrawIndirectSrcBuffer\n"
		"{\n"
		"	DrawElementsIndirectCommand src_cmds[];\n"
		"};\n"
		"\n"
		"layout(std430, binding=6) restrict writeonly buffer DrawIndirectDstBuffer\n"
		"{\n"
		"	DrawElementsIndirectCommand dst_cmds[];\n"
		"};\n"
		"\n"
		"struct DrawRemap\n"
		"{\n"
		"	uint src_call;\n"
		"	uint instance_data;\n"
		"};\n"
		"\n"
		"layout(std430, binding=7) restrict readonly buffer DrawRemapBuffer\n"
		"{\n"
		"	DrawRemap remap_data[];\n"
		"};\n"
		"\n"
		"#define MAX_INSTANCES " QS_STRINGIFY (MAX_BMODEL_INSTANCES) "u\n"
		"\n"
		"void main()\n"
		"{\n"
		"	uint thread_id = gl_GlobalInvocationID.x;\n"
		"	uint num_calls = remap_data.length();\n"
		"	if (thread_id >= num_calls)\n"
		"		return;\n"
		"	DrawRemap remap = remap_data[thread_id];\n"
		"	DrawElementsIndirectCommand cmd = src_cmds[remap.src_call];\n"
		"	cmd.baseInstance = remap.instance_data / MAX_INSTANCES;\n"
		"	cmd.instanceCount = (remap.instance_data % MAX_INSTANCES) + 1u;\n"
		"	if (cmd.count == 0u)\n"
		"		cmd.instanceCount = 0u;\n"
		"	dst_cmds[thread_id] = cmd;\n"
		"}\n";

	r_gather_indirect_draw_params_program = GL_CreateComputeProgram (computeSource, "indirect draw gather");
}

typedef struct bmodel_gpu_instance_s {
	float		world[12];	// world matrix (transposed mat4x3)
	float		alpha;
	float		padding[3];
} bmodel_gpu_instance_t;

typedef struct bmodel_gpu_call_s {
	GLuint64	texture;
	GLuint64	fullbright;
	GLuint		flags;
	GLfloat		alpha;
} bmodel_gpu_call_t;

typedef struct bmodel_gpu_call_remap_s {
	GLuint		src;
	GLuint		inst;
} bmodel_gpu_call_remap_t;

static bmodel_gpu_instance_t	bmodel_instances[MAX_VISEDICTS + 1]; // +1 for worldspawn
static bmodel_gpu_call_t		bmodel_calls[MAX_BMODEL_DRAWS];
static bmodel_gpu_call_remap_t	bmodel_call_remap[MAX_BMODEL_DRAWS];
static int						num_bmodel_calls;
static GLuint					bmodel_batch_program;

/*
=============
R_InitBModelInstance
=============
*/
static void R_InitBModelInstance (bmodel_gpu_instance_t *inst, entity_t *ent)
{
	vec3_t angles;
	float mat[16];

	angles[0] = -ent->angles[0];
	angles[1] =  ent->angles[1];
	angles[2] =  ent->angles[2];
	R_EntityMatrix (mat, ent->origin, angles);

	#define COPY_ROW(row)					\
		inst->world[row*4+0] = mat[row+0],	\
		inst->world[row*4+1] = mat[row+4],	\
		inst->world[row*4+2] = mat[row+8],	\
		inst->world[row*4+3] = mat[row+12]

	COPY_ROW (0);
	COPY_ROW (1);
	COPY_ROW (2);

	#undef COPY_ROW

	inst->alpha = ent->alpha == ENTALPHA_DEFAULT ? -1.f : ENTALPHA_DECODE (ent->alpha);
	memset (&inst->padding, 0, sizeof(inst->padding));
}

/*
=============
R_ResetBModelCalls
=============
*/
static void R_ResetBModelCalls (GLuint program)
{
	bmodel_batch_program = program;
	num_bmodel_calls = 0;
}

/*
=============
R_FlushBModelCalls
=============
*/
static void R_FlushBModelCalls (void)
{
	GLuint	buf;
	GLbyte	*ofs;

	if (!num_bmodel_calls)
		return;

	GL_UseProgram (r_gather_indirect_draw_params_program);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 5, gl_bmodel_indirect_buffer, 0, gl_bmodel_indirect_buffer_size);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 6, gl_bmodel_compacted_indirect_buffer, 0, sizeof(bmodel_draw_indirect_t) * num_bmodel_calls);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_call_remap, sizeof(bmodel_call_remap[0]) * num_bmodel_calls, &buf, &ofs);
	GL_BindBufferRange  (GL_SHADER_STORAGE_BUFFER, 7, buf, (GLintptr)ofs, sizeof(bmodel_call_remap[0]) * num_bmodel_calls);
	GL_DispatchComputeFunc ((num_bmodel_calls + 63) / 64, 1, 1);
	GL_MemoryBarrierFunc (GL_COMMAND_BARRIER_BIT);

	GL_UseProgram (bmodel_batch_program);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_calls, sizeof(bmodel_calls[0]) * num_bmodel_calls, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, buf, (GLintptr)ofs, sizeof(bmodel_calls[0]) * num_bmodel_calls);
	GL_BindBuffer (GL_DRAW_INDIRECT_BUFFER, gl_bmodel_compacted_indirect_buffer);
	GL_MultiDrawElementsIndirectFunc (GL_TRIANGLES, GL_UNSIGNED_INT, 0, num_bmodel_calls, sizeof (bmodel_draw_indirect_t));

	num_bmodel_calls = 0;
}

/*
=============
R_AddBModelCall
=============
*/
static void R_AddBModelCall (int index, int first_instance, int num_instances, texture_t *t, qboolean zfix)
{
	bmodel_gpu_call_t *call;
	bmodel_gpu_call_remap_t *remap;
	gltexture_t *tx, *fb;

	if (num_bmodel_calls == MAX_BMODEL_DRAWS)
		R_FlushBModelCalls ();

	call = num_bmodel_calls + bmodel_calls;
	remap = num_bmodel_calls + bmodel_call_remap;
	num_bmodel_calls++;

	if (t)
	{
		tx = t->gltexture;
		fb = t->fullbright;
		if (r_lightmap_cheatsafe)
			tx = fb = NULL;
		if (!gl_fullbrights.value)
			fb = NULL;
	}
	else
	{
		tx = fb = whitetexture;
	}

	if (!gl_zfix.value)
		zfix = 0;

	call->texture = tx ? tx->bindless_handle : greytexture->bindless_handle;
	call->fullbright = fb ? fb->bindless_handle : blacktexture->bindless_handle;
	call->flags = (t != NULL && t->type == TEXTYPE_CUTOUT) | (zfix << 1);
	call->alpha = t ? GL_WaterAlphaForTextureType (t->type) : 1.f;

	SDL_assert (num_instances > 0);
	SDL_assert (num_instances <= MAX_BMODEL_INSTANCES);
	remap->src = index;
	remap->inst = first_instance * MAX_BMODEL_INSTANCES + (num_instances - 1);
}

typedef enum {
	BP_NORMAL,
	BP_SKY,
	BP_SHOWTRIS,
} brushpass_t;

/*
=============
R_DrawBrushModels_Real
=============
*/
static void R_DrawBrushModels_Real (entity_t **ents, int count, brushpass_t pass)
{
	int i, j;
	int totalinst, baseinst;
	unsigned state;
	GLuint program;
	GLuint buf;
	GLbyte *ofs;
	textype_t texbegin, texend;

	if (!count)
		return;

	if (count > countof(bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, countof(bmodel_instances));
		count = countof(bmodel_instances);
	}

	switch (pass)
	{
	case BP_NORMAL:
	default:
		texbegin = 0;
		texend = TEXTYPE_CUTOUT + 1;
		program = gl_bindless_able ? r_world_program_bindless : r_world_program;
		break;
	case BP_SKY:
		texbegin = TEXTYPE_SKY;
		texend = TEXTYPE_SKY + 1;
		program = gl_bindless_able ? r_skystencil_program_bindless : r_skystencil_program;
		break;
	case BP_SHOWTRIS:
		texbegin = 0;
		texend = TEXTYPE_COUNT;
		program = gl_bindless_able ? r_world_program_bindless : r_world_program;
		break;
	}

	// fill instance data
	for (i = 0, totalinst = 0; i < count; i++)
		if (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin] > 0)
			R_InitBModelInstance (&bmodel_instances[totalinst++], ents[i]);

	if (!totalinst)
		return;

	// setup state
	state = GLS_CULL_BACK | GLS_ATTRIBS(0);
	if (ents[0] == cl_entities || ENTALPHA_OPAQUE (ents[0]->alpha))
		state |= GLS_BLEND_OPAQUE;
	else
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	
	R_ResetBModelCalls (program);
	GL_SetState (state);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);

	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * count, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * count);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

	// generate drawcalls
	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		int frame = isworld ? 0 : e->frame;
		int numtex = model->texofs[texend] - model->texofs[texbegin];

		if (!numtex)
			continue;

		for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
			numinst += (ents[i]->model->texofs[texend] - ents[i]->model->texofs[texbegin]) > 0;

		for (j = model->texofs[texbegin]; j < model->texofs[texend]; j++)
		{
			texture_t *t = model->textures[model->usedtextures[j]];
			R_AddBModelCall (model->firstcmd + j, baseinst, numinst, pass != BP_SHOWTRIS ? R_TextureAnimation (t, frame) : 0, !isworld);
		}

		baseinst += numinst;
	}

	R_FlushBModelCalls ();
}

/*
=============
R_EntHasWater
=============
*/
static qboolean R_EntHasWater (entity_t *ent, qboolean translucent)
{
	int i;
	for (i = TEXTYPE_FIRSTLIQUID; i < TEXTYPE_LASTLIQUID+1; i++)
	{
		int numtex = ent->model->texofs[i+1] - ent->model->texofs[i];
		if (numtex && (GL_WaterAlphaForEntityTextureType (ent, (textype_t)i) < 1.f) == translucent)
			return true;
	}
	return false;
}

/*
=============
R_DrawBrushModels_Water
=============
*/
void R_DrawBrushModels_Water (entity_t **ents, int count, qboolean translucent)
{
	int i, j;
	int totalinst, baseinst;
	unsigned state;
	GLuint buf;
	GLbyte *ofs;

	if (count > countof(bmodel_instances))
	{
		Con_DWarning ("bmodel instance overflow: %d > %d\n", count, countof(bmodel_instances));
		count = countof(bmodel_instances);
	}

	// fill instance data
	for (i = 0, totalinst = 0; i < count; i++)
		if (R_EntHasWater (ents[i], translucent))
			R_InitBModelInstance (&bmodel_instances[totalinst++], ents[i]);

	if (!totalinst)
		return;

	GL_BeginGroup (translucent ? "Water (translucent)" : "Water (opaque)");

	// setup state
	state = GLS_CULL_BACK | GLS_ATTRIBS(0);
	if (translucent)
		state |= GLS_BLEND_ALPHA | GLS_NO_ZWRITE;
	else
		state |= GLS_BLEND_OPAQUE;
	
	R_ResetBModelCalls (gl_bindless_able ? r_water_program_bindless : r_water_program);
	GL_SetState (state);
	GL_Bind (GL_TEXTURE2, r_fullbright_cheatsafe ? greytexture : lightmap_texture);

	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl_bmodel_ibo);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, bmodel_instances, sizeof(bmodel_instances[0]) * totalinst, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof(bmodel_instances[0]) * count);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 3, gl_bmodel_vbo, 0, gl_bmodel_vbo_size);

	// generate drawcalls
	for (i = 0, baseinst = 0; i < count; /**/)
	{
		int numinst;
		entity_t *e = ents[i++];
		qmodel_t *model = e->model;
		qboolean isworld = (e == &cl_entities[0]);
		int frame = isworld ? 0 : e->frame;

		if (!R_EntHasWater (e, translucent))
			continue;

		for (numinst = 1; i < count && ents[i]->model == model && numinst < MAX_BMODEL_INSTANCES; i++)
			numinst += R_EntHasWater (ents[i], translucent);

		for (j = model->texofs[TEXTYPE_FIRSTLIQUID]; j < model->texofs[TEXTYPE_LASTLIQUID+1]; j++)
		{
			texture_t *t = model->textures[model->usedtextures[j]];
			R_AddBModelCall (model->firstcmd + j, baseinst, numinst, R_TextureAnimation (t, frame), !isworld);
		}

		baseinst += numinst;
	}

	R_FlushBModelCalls ();

	GL_EndGroup ();
}

/*
=============
R_DrawBrushModels
=============
*/
void R_DrawBrushModels (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_NORMAL);
}

/*
=============
R_DrawBrushModels_Sky
=============
*/
void R_DrawBrushModels_Sky (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SKY);
}

/*
=============
R_DrawBrushModels_ShowTris
=============
*/
void R_DrawBrushModels_ShowTris (entity_t **ents, int count)
{
	R_DrawBrushModels_Real (ents, count, BP_SHOWTRIS);
}
