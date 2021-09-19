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
// r_main.c

#include "quakedef.h"

qboolean	r_cache_thrash;		// compatability

vec3_t		modelorg, r_entorigin;
entity_t	*currententity;

int			r_visframecount;	// bumped when going to a new PVS
int			r_framecount;		// used for dlight push checking

mplane_t	frustum[4];
float		r_matview[16];
float		r_matproj[16];
float		r_matviewproj[16];

//johnfitz -- rendering statistics
int rs_brushpolys, rs_aliaspolys, rs_skypolys, rs_particles, rs_fogpolys;
int rs_dynamiclightmaps, rs_brushpasses, rs_aliaspasses, rs_skypasses;
float rs_megatexels;

//
// view origin
//
vec3_t	vup;
vec3_t	vpn;
vec3_t	vright;
vec3_t	r_origin;

float r_fovx, r_fovy; //johnfitz -- rendering fov may be different becuase of r_waterwarp and r_stereo
qboolean water_warp;

//
// screen size info
//
refdef_t	r_refdef;

mleaf_t		*r_viewleaf, *r_oldviewleaf;

int		d_lightstylevalue[256];	// 8.8 fraction of base light value


cvar_t	r_norefresh = {"r_norefresh","0",CVAR_NONE};
cvar_t	r_drawentities = {"r_drawentities","1",CVAR_NONE};
cvar_t	r_drawviewmodel = {"r_drawviewmodel","1",CVAR_NONE};
cvar_t	r_speeds = {"r_speeds","0",CVAR_NONE};
cvar_t	r_pos = {"r_pos","0",CVAR_NONE};
cvar_t	r_fullbright = {"r_fullbright","0",CVAR_NONE};
cvar_t	r_lightmap = {"r_lightmap","0",CVAR_NONE};
cvar_t	r_shadows = {"r_shadows","0",CVAR_ARCHIVE};
cvar_t	r_wateralpha = {"r_wateralpha","1",CVAR_ARCHIVE};
cvar_t	r_dynamic = {"r_dynamic","1",CVAR_ARCHIVE};
cvar_t	r_novis = {"r_novis","0",CVAR_ARCHIVE};

cvar_t	gl_finish = {"gl_finish","0",CVAR_NONE};
cvar_t	gl_clear = {"gl_clear","1",CVAR_NONE};
cvar_t	gl_polyblend = {"gl_polyblend","1",CVAR_NONE};
cvar_t	gl_playermip = {"gl_playermip","0",CVAR_NONE};
cvar_t	gl_nocolors = {"gl_nocolors","0",CVAR_NONE};

//johnfitz -- new cvars
cvar_t	r_clearcolor = {"r_clearcolor","2",CVAR_ARCHIVE};
cvar_t	r_flatlightstyles = {"r_flatlightstyles", "0", CVAR_NONE};
cvar_t	gl_fullbrights = {"gl_fullbrights", "1", CVAR_ARCHIVE};
cvar_t	gl_farclip = {"gl_farclip", "16384", CVAR_ARCHIVE};
cvar_t	gl_overbright = {"gl_overbright", "1", CVAR_ARCHIVE};
cvar_t	gl_overbright_models = {"gl_overbright_models", "1", CVAR_ARCHIVE};
cvar_t	r_oldskyleaf = {"r_oldskyleaf", "0", CVAR_NONE};
cvar_t	r_drawworld = {"r_drawworld", "1", CVAR_NONE};
cvar_t	r_showtris = {"r_showtris", "0", CVAR_NONE};
cvar_t	r_showbboxes = {"r_showbboxes", "0", CVAR_NONE};
cvar_t	r_lerpmodels = {"r_lerpmodels", "1", CVAR_NONE};
cvar_t	r_lerpmove = {"r_lerpmove", "1", CVAR_NONE};
cvar_t	r_nolerp_list = {"r_nolerp_list", "progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,progs/flame_pyre.mdl,progs/v_saw.mdl,progs/v_xfist.mdl,progs/h2stuff/newfire.mdl", CVAR_NONE};
cvar_t	r_noshadow_list = {"r_noshadow_list", "progs/flame2.mdl,progs/flame.mdl,progs/bolt1.mdl,progs/bolt2.mdl,progs/bolt3.mdl,progs/laser.mdl", CVAR_NONE};

extern cvar_t	r_vfog;
//johnfitz

cvar_t	gl_zfix = {"gl_zfix", "0", CVAR_NONE}; // QuakeSpasm z-fighting fix

cvar_t	r_lavaalpha = {"r_lavaalpha","0",CVAR_NONE};
cvar_t	r_telealpha = {"r_telealpha","0",CVAR_NONE};
cvar_t	r_slimealpha = {"r_slimealpha","0",CVAR_NONE};

float	map_wateralpha, map_lavaalpha, map_telealpha, map_slimealpha;

qboolean r_fullbright_cheatsafe, r_lightmap_cheatsafe, r_drawworld_cheatsafe; //johnfitz

cvar_t	r_scale = {"r_scale", "1", CVAR_ARCHIVE};

//==============================================================================
//
// GLSL GAMMA CORRECTION
//
//==============================================================================

static GLuint r_gamma_texture;
static GLuint r_gamma_program;
static int r_gamma_texture_width, r_gamma_texture_height;

/*
=============
GLSLGamma_DeleteTexture
=============
*/
void GLSLGamma_DeleteTexture (void)
{
	glDeleteTextures (1, &r_gamma_texture);
	r_gamma_texture = 0;
	r_gamma_program = 0; // deleted in R_DeleteShaders
}

/*
=============
GLSLGamma_CreateResources
=============
*/
void GLSLGamma_CreateResources (void)
{
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"void main(void) {\n"
		"	ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
		"	v.x ^= v.y; // fix winding order\n"
		"	gl_Position = vec4(vec2(v) * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";

	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D GammaTexture;\n"
		"\n"
		"layout(location=0) uniform vec2 GammaContrast;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main(void) {\n"
		"	  out_fragcolor = texelFetch(GammaTexture, ivec2(gl_FragCoord), 0);\n"
		"	  out_fragcolor.rgb *= GammaContrast.y;\n"
		"	  out_fragcolor = vec4(pow(out_fragcolor.rgb, vec3(GammaContrast.x)), 1.0);\n"
		"}\n";

	r_gamma_program = GL_CreateProgram (vertSource, fragSource, "postprocess");

	glGenTextures (1, &r_gamma_texture);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, r_gamma_texture);

	r_gamma_texture_width = vid.width;
	r_gamma_texture_height = vid.height;

	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, r_gamma_texture_width, r_gamma_texture_height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL_ObjectLabelFunc (GL_TEXTURE, r_gamma_texture, -1, "postprocess");
}

/*
=============
GLSLGamma_GammaCorrect
=============
*/
void GLSLGamma_GammaCorrect (void)
{
	if (vid_gamma.value == 1 && vid_contrast.value == 1)
		return;

	GL_BeginGroup ("Postprocess");

// copy the framebuffer to the texture
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, r_gamma_texture);
	glCopyTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, glx, gly, glwidth, glheight);

// draw the texture back to the framebuffer with a fragment shader
	GL_UseProgram (r_gamma_program);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(0));
	GL_Uniform2fFunc (0, vid_gamma.value, q_min(2.0, q_max(1.0, vid_contrast.value)));

	glViewport (glx, gly, glwidth, glheight);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	GL_EndGroup ();
}

/*
=================
R_CullBox -- johnfitz -- replaced with new function from lordhavoc

Returns true if the box is completely outside the frustum
=================
*/
qboolean R_CullBox (vec3_t emins, vec3_t emaxs)
{
	int i;
	mplane_t *p;
	byte signbits;
	float vec[3];
	for (i = 0;i < 4;i++)
	{
		p = frustum + i;
		signbits = p->signbits;
		vec[0] = ((signbits % 2)<1) ? emaxs[0] : emins[0];
		vec[1] = ((signbits % 4)<2) ? emaxs[1] : emins[1];
		vec[2] = ((signbits % 8)<4) ? emaxs[2] : emins[2];
		if (p->normal[0]*vec[0] + p->normal[1]*vec[1] + p->normal[2]*vec[2] < p->dist)
			return true;
	}
	return false;
}

/*
===============
R_CullModelForEntity -- johnfitz -- uses correct bounds based on rotation
===============
*/
qboolean R_CullModelForEntity (entity_t *e)
{
	vec3_t mins, maxs;

	if (e->angles[0] || e->angles[2]) //pitch or roll
	{
		VectorAdd (e->origin, e->model->rmins, mins);
		VectorAdd (e->origin, e->model->rmaxs, maxs);
	}
	else if (e->angles[1]) //yaw
	{
		VectorAdd (e->origin, e->model->ymins, mins);
		VectorAdd (e->origin, e->model->ymaxs, maxs);
	}
	else //no rotation
	{
		VectorAdd (e->origin, e->model->mins, mins);
		VectorAdd (e->origin, e->model->maxs, maxs);
	}

	return R_CullBox (mins, maxs);
}

/*
===============
R_EntityMatrix
===============
*/
#define DEG2RAD( a ) ( (a) * M_PI_DIV_180 )
void R_EntityMatrix (float matrix[16], vec3_t origin, vec3_t angles)
{
	float yaw   = DEG2RAD(angles[YAW]);
	float pitch = angles[PITCH];
	float roll  = angles[ROLL];
	if (pitch == 0.f && roll == 0.f)
	{
		float sy = sin(yaw);
		float cy = cos(yaw);

		// First column
		matrix[ 0] = cy;
		matrix[ 1] = sy;
		matrix[ 2] = 0.f;
		matrix[ 3] = 0.f;

		// Second column
		matrix[ 4] = -sy;
		matrix[ 5] = cy;
		matrix[ 6] = 0.f;
		matrix[ 7] = 0.f;

		// Third column
		matrix[ 8] = 0.f;
		matrix[ 9] = 0.f;
		matrix[10] = 1.f;
		matrix[11] = 0.f;
	}
	else
	{
		pitch = DEG2RAD(pitch);
		roll = DEG2RAD(roll);
		float sy = sin(yaw);
		float sp = sin(pitch);
		float sr = sin(roll);
		float cy = cos(yaw);
		float cp = cos(pitch);
		float cr = cos(roll);

		// https://www.symbolab.com/solver/matrix-multiply-calculator FTW!

		// First column
		matrix[ 0] = cy*cp;
		matrix[ 1] = sy*cp;
		matrix[ 2] = sp;
		matrix[ 3] = 0.f;

		// Second column
		matrix[ 4] = -cy*sp*sr - cr*sy;
		matrix[ 5] = cr*cy - sy*sp*sr;
		matrix[ 6] = cp*sr;
		matrix[ 7] = 0.f;

		// Third column
		matrix[ 8] = sy*sr - cr*cy*sp;
		matrix[ 9] = -cy*sr - cr*sy*sp;
		matrix[10] = cr*cp;
		matrix[11] = 0.f;
	}

	// Fourth column
	matrix[12] = origin[0];
	matrix[13] = origin[1];
	matrix[14] = origin[2];
	matrix[15] = 1.f;
}

/*
=============
GL_PolygonOffset -- johnfitz

negative offset moves polygon closer to camera
=============
*/
void GL_PolygonOffset (int offset)
{
	if (offset > 0)
	{
		glEnable (GL_POLYGON_OFFSET_FILL);
		glEnable (GL_POLYGON_OFFSET_LINE);
		glPolygonOffset(1, offset);
	}
	else if (offset < 0)
	{
		glEnable (GL_POLYGON_OFFSET_FILL);
		glEnable (GL_POLYGON_OFFSET_LINE);
		glPolygonOffset(-1, offset);
	}
	else
	{
		glDisable (GL_POLYGON_OFFSET_FILL);
		glDisable (GL_POLYGON_OFFSET_LINE);
	}
}

//==============================================================================
//
// SETUP FRAME
//
//==============================================================================

int SignbitsForPlane (mplane_t *out)
{
	int	bits, j;

	// for fast box on planeside test

	bits = 0;
	for (j=0 ; j<3 ; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1<<j;
	}
	return bits;
}

/*
===============
TurnVector -- johnfitz

turn forward towards side on the plane defined by forward and side
if angle = 90, the result will be equal to side
assumes side and forward are perpendicular, and normalized
to turn away from side, use a negative angle
===============
*/
#define DEG2RAD( a ) ( (a) * M_PI_DIV_180 )
void TurnVector (vec3_t out, const vec3_t forward, const vec3_t side, float angle)
{
	float scale_forward, scale_side;

	scale_forward = cos( DEG2RAD( angle ) );
	scale_side = sin( DEG2RAD( angle ) );

	out[0] = scale_forward*forward[0] + scale_side*side[0];
	out[1] = scale_forward*forward[1] + scale_side*side[1];
	out[2] = scale_forward*forward[2] + scale_side*side[2];
}

/*
===============
R_SetFrustum -- johnfitz -- rewritten
===============
*/
void R_SetFrustum (float fovx, float fovy)
{
	int		i;

	TurnVector(frustum[0].normal, vpn, vright, fovx/2 - 90); //right plane
	TurnVector(frustum[1].normal, vpn, vright, 90 - fovx/2); //left plane
	TurnVector(frustum[2].normal, vpn, vup, 90 - fovy/2); //bottom plane
	TurnVector(frustum[3].normal, vpn, vup, fovy/2 - 90); //top plane

	for (i=0 ; i<4 ; i++)
	{
		frustum[i].type = PLANE_ANYZ;
		frustum[i].dist = DotProduct (r_origin, frustum[i].normal); //FIXME: shouldn't this always be zero?
		frustum[i].signbits = SignbitsForPlane (&frustum[i]);
	}
}

/*
=============
GL_FrustumMatrix
=============
*/
#define NEARCLIP 4
static void GL_FrustumMatrix(float matrix[16], float fovx, float fovy)
{
	const float w = 1.0f / tanf(fovx * 0.5f);
	const float h = 1.0f / tanf(fovy * 0.5f);

	// reduce near clip distance at high FOV's to avoid seeing through walls
	const float d = 12.f * q_min(w, h);
	const float n = CLAMP(0.5f, d, NEARCLIP);
	const float f = gl_farclip.value;

	memset(matrix, 0, 16 * sizeof(float));

	// projection matrix with the coordinate system conversion baked in
	matrix[0*4 + 2] = (f + n) / (f - n);
	matrix[0*4 + 3] = 1.f;
	matrix[1*4 + 0] = -w;
	matrix[2*4 + 1] = h;
	matrix[3*4 + 2] = -2.f * f * n / (f - n);
}

/*
=============
GL_SetFrustum -- johnfitz -- written to replace MYgluPerspective
=============
*/
void GL_SetFrustum(float fovx, float fovy)
{
	GL_FrustumMatrix(r_matproj, fovx * (M_PI / 180.0), fovy * (M_PI / 180.0));
}

/*
=============
R_SetupGL
=============
*/
void R_SetupGL (void)
{
	int scale;
	float translation[16];
	float rotation[16];

	//johnfitz -- rewrote this section
	scale =  CLAMP(1, (int)r_scale.value, 4); // ericw -- see R_WarpScaleView
	glViewport (glx + r_refdef.vrect.x,
				gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height,
				r_refdef.vrect.width / scale,
				r_refdef.vrect.height / scale);
	//johnfitz

	GL_SetFrustum (r_fovx, r_fovy); //johnfitz -- use r_fov* vars

	// View matrix
	RotationMatrix(r_matview, DEG2RAD(-r_refdef.viewangles[ROLL]), 0);
	RotationMatrix(rotation, DEG2RAD(-r_refdef.viewangles[PITCH]), 1);
	MatrixMultiply(r_matview, rotation);
	RotationMatrix(rotation, DEG2RAD(-r_refdef.viewangles[YAW]), 2);
	MatrixMultiply(r_matview, rotation);

	TranslationMatrix(translation, -r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);
	MatrixMultiply(r_matview, translation);

	// View projection matrix
	memcpy(r_matviewproj, r_matproj, 16 * sizeof(float));
	MatrixMultiply(r_matviewproj, r_matview);
}

/*
=============
R_Clear -- johnfitz -- rewritten and gutted
=============
*/
void R_Clear (void)
{
	unsigned int clearbits;

	clearbits = GL_DEPTH_BUFFER_BIT;
	// from mh -- if we get a stencil buffer, we should clear it, even though we don't use it
	if (gl_stencilbits)
		clearbits |= GL_STENCIL_BUFFER_BIT;
	if (gl_clear.value)
		clearbits |= GL_COLOR_BUFFER_BIT;

	GL_SetState (glstate & ~GLS_NO_ZWRITE); // make sure depth writes are enabled
	glClear (clearbits);
}

/*
===============
R_SetupScene -- johnfitz -- this is the stuff that needs to be done once per eye in stereo mode
===============
*/
void R_SetupScene (void)
{
	R_SetupGL ();
}

/*
===============
R_SetupView -- johnfitz -- this is the stuff that needs to be done once per frame, even in stereo mode
===============
*/
void R_SetupView (void)
{
	// Need to do those early because we now update dynamic light maps during R_MarkSurfaces
	R_PushDlights ();
	R_AnimateLight ();
	r_framecount++;

	Fog_SetupFrame (); //johnfitz

// build the transformation matrix for the given view angles
	VectorCopy (r_refdef.vieworg, r_origin);
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

// current viewleaf
	r_oldviewleaf = r_viewleaf;
	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();

	r_cache_thrash = false;

	//johnfitz -- calculate r_fovx and r_fovy here
	r_fovx = r_refdef.fov_x;
	r_fovy = r_refdef.fov_y;
	water_warp = false;
	if (r_waterwarp.value)
	{
		int contents = Mod_PointInLeaf (r_origin, cl.worldmodel)->contents;
		if (contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA)
		{
			if (r_waterwarp.value > 1.f)
			{
				//variance is a percentage of width, where width = 2 * tan(fov / 2) otherwise the effect is too dramatic at high FOV and too subtle at low FOV.  what a mess!
				r_fovx = atan(tan(DEG2RAD(r_refdef.fov_x) / 2) * (0.97 + sin(cl.time * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
				r_fovy = atan(tan(DEG2RAD(r_refdef.fov_y) / 2) * (1.03 - sin(cl.time * 1.5) * 0.03)) * 2 / M_PI_DIV_180;
			}
			else
			{
				water_warp = true;
			}
		}
	}
	//johnfitz

	R_SetFrustum (r_fovx, r_fovy); //johnfitz -- use r_fov* vars

	R_MarkSurfaces (); //johnfitz -- create texture chains from PVS

	R_Clear ();

	//johnfitz -- cheat-protect some draw modes
	r_fullbright_cheatsafe = r_lightmap_cheatsafe = false;
	r_drawworld_cheatsafe = true;
	if (cl.maxclients == 1)
	{
		if (!r_drawworld.value) r_drawworld_cheatsafe = false;

		if (r_fullbright.value || !cl.worldmodel->lightdata) r_fullbright_cheatsafe = true;
		else if (r_lightmap.value) r_lightmap_cheatsafe = true;
	}
	//johnfitz
}

//==============================================================================
//
// RENDER VIEW
//
//==============================================================================

/*
=============
R_DrawEntitiesOnList
=============
*/
void R_DrawEntitiesOnList (qboolean alphapass) //johnfitz -- added parameter
{
	int		i;

	if (!r_drawentities.value)
		return;

	GL_BeginGroup (alphapass ? "Translucent entities" : "Opaque entities");

	//johnfitz -- sprites are not a special case
	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		currententity = cl_visedicts[i];

		//johnfitz -- if alphapass is true, draw only alpha entites this time
		//if alphapass is false, draw only nonalpha entities this time
		if ((ENTALPHA_DECODE(currententity->alpha) < 1 && !alphapass) ||
			(ENTALPHA_DECODE(currententity->alpha) == 1 && alphapass))
			continue;

		//johnfitz -- chasecam
		if (currententity == &cl_entities[cl.viewentity])
			currententity->angles[0] *= 0.3;
		//johnfitz

		switch (currententity->model->type)
		{
			case mod_alias:
				R_DrawAliasModel (currententity);
				break;
			case mod_brush:
				R_DrawBrushModel (currententity);
				break;
			case mod_sprite:
				R_DrawSpriteModel (currententity);
				break;
		}
	}

	GL_EndGroup ();
}

/*
=============
R_DrawViewModel -- johnfitz -- gutted
=============
*/
void R_DrawViewModel (void)
{
	if (!r_drawviewmodel.value || !r_drawentities.value || chase_active.value)
		return;

	if (cl.items & IT_INVISIBILITY || cl.stats[STAT_HEALTH] <= 0)
		return;

	currententity = &cl.viewent;
	if (!currententity->model)
		return;

	//johnfitz -- this fixes a crash
	if (currententity->model->type != mod_alias)
		return;
	//johnfitz

	GL_BeginGroup ("View model");

	// hack the depth range to prevent view model from poking into walls
	glDepthRange (0, 0.3);
	R_DrawAliasModel (currententity);
	glDepthRange (0, 1);

	GL_EndGroup ();
}

/*
================
R_ShowTris -- johnfitz
================
*/
void R_ShowTris (void)
{
	extern cvar_t r_particles;
	int i;

	if (r_showtris.value < 1 || r_showtris.value > 2 || cl.maxclients > 1)
		return;

	GL_BeginGroup ("Show tris");

	if (r_showtris.value == 1)
		glDepthRange(0.f, 0.f);
	glPolygonMode (GL_FRONT_AND_BACK, GL_LINE);
	GL_PolygonOffset (OFFSET_SHOWTRIS);

	R_DrawWorld_ShowTris ();

	if (r_drawentities.value)
	{
		for (i=0 ; i<cl_numvisedicts ; i++)
		{
			currententity = cl_visedicts[i];

			if (currententity == &cl_entities[cl.viewentity]) // chasecam
				currententity->angles[0] *= 0.3;

			switch (currententity->model->type)
			{
			case mod_brush:
				R_DrawBrushModel_ShowTris (currententity);
				break;
			case mod_alias:
				R_DrawAliasModel_ShowTris (currententity);
				break;
			case mod_sprite:
				R_DrawSpriteModel_ShowTris (currententity);
				break;
			default:
				break;
			}
		}

		// viewmodel
		currententity = &cl.viewent;
		if (r_drawviewmodel.value
			&& !chase_active.value
			&& cl.stats[STAT_HEALTH] > 0
			&& !(cl.items & IT_INVISIBILITY)
			&& currententity->model
			&& currententity->model->type == mod_alias)
		{
			if (r_showtris.value != 1.f)
				glDepthRange (0, 0.3);

			R_DrawAliasModel_ShowTris (currententity);

			if (r_showtris.value != 1.f)
				glDepthRange (0.f, 1.f);
		}
	}

	R_DrawParticles_ShowTris ();

	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_PolygonOffset (OFFSET_NONE);
	if (r_showtris.value == 1)
		glDepthRange (0.f, 1.f);

	Sbar_Changed (); //so we don't get dots collecting on the statusbar

	GL_EndGroup ();
}

/*
================
R_RenderScene
================
*/
void R_RenderScene (void)
{
	R_SetupScene (); //johnfitz -- this does everything that should be done once per call to RenderScene

	Fog_EnableGFog (); //johnfitz

	R_DrawViewModel (); //johnfitz -- moved here from R_RenderView

	R_DrawWorld ();

	S_ExtraUpdate (); // don't let sound get messed up if going slow

	R_DrawEntitiesOnList (false); //johnfitz -- false means this is the pass for nonalpha entities

	Sky_DrawSky (); //johnfitz

	R_DrawWorld_Water (); //johnfitz -- drawn here since they might have transparency

	R_DrawEntitiesOnList (true); //johnfitz -- true means this is the pass for alpha entities

	R_DrawParticles ();

	Fog_DisableGFog (); //johnfitz

	R_ShowTris (); //johnfitz
}

static GLuint r_warpscale_program;
static GLuint r_warpscale_texture;
static int r_warpscale_texture_width, r_warpscale_texture_height;

/*
=============
R_WarpScaleView_DeleteTexture
=============
*/
void R_WarpScaleView_DeleteTexture (void)
{
	glDeleteTextures (1, &r_warpscale_texture);
	r_warpscale_texture = 0;
	r_warpscale_program = 0; // deleted in R_DeleteShaders
}

/*
=============
R_WarpScaleView_CreateResources
=============
*/
void R_WarpScaleView_CreateResources (void)
{
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) out vec2 out_uv;\n"
		"\n"
		"void main(void) {\n"
		"	ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
		"	v.x ^= v.y; // fix winding order\n"
		"	out_uv = vec2(v);\n"
		"	gl_Position = vec4(out_uv * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";

	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"\n"
		"layout(location=0) uniform vec4 UVScaleWarpTime; // xy=Scale z=Warp w=Time\n"
		"\n"
		"layout(location=0) in vec2 in_uv;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main(void) {\n"
		"	vec2 uv = in_uv;\n"
		"	vec2 uv_scale = UVScaleWarpTime.xy;\n"
		"	if (UVScaleWarpTime.z > 0.0)\n"
		"	{\n"
		"		float time = UVScaleWarpTime.w;\n"
		"		float aspect = dFdy(uv.y) / dFdx(uv.x);\n"
		"		vec2 warp_amp = UVScaleWarpTime.zz;\n"
		"		warp_amp.y *= aspect;\n"
		"		uv = warp_amp + uv * (1.0 - 2.0 * warp_amp); // remap to safe area\n"
		"		uv += warp_amp * sin(vec2(uv.y / aspect, uv.x) * (3.14159265 * 8.0) + time);\n"
		"	}\n"
		"	out_fragcolor = texture2D(Tex, uv * uv_scale);\n"
		"}\n";

	r_warpscale_program = GL_CreateProgram (vertSource, fragSource, "view warp/scale");

	glGenTextures (1, &r_warpscale_texture);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, r_warpscale_texture);

	r_warpscale_texture_width = vid.width;
	r_warpscale_texture_height = vid.height;

	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, r_warpscale_texture_width, r_warpscale_texture_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GL_ObjectLabelFunc (GL_TEXTURE, r_warpscale_texture, -1, "warp/scale");
}

/*
================
R_WarpScaleView

The r_scale cvar allows rendering the 3D view at 1/2, 1/3, or 1/4 resolution.
This function scales the reduced resolution 3D view back up to fill 
r_refdef.vrect. This is for emulating a low-resolution pixellated look,
or possibly as a perforance boost on slow graphics cards.
================
*/
void R_WarpScaleView (void)
{
	float smax, tmax;
	int scale;
	int srcx, srcy, srcw, srch;

	// copied from R_SetupGL()
	scale = CLAMP(1, (int)r_scale.value, 4);
	srcx = glx + r_refdef.vrect.x;
	srcy = gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height;
	srcw = r_refdef.vrect.width / scale;
	srch = r_refdef.vrect.height / scale;

	if (scale == 1 && !water_warp)
		return;

	GL_BeginGroup ("Warp/scale view");

	// copy the framebuffer to the texture
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, r_warpscale_texture);
	glCopyTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, srcx, srcy, srcw, srch);

	glViewport (srcx, srcy, r_refdef.vrect.width, r_refdef.vrect.height);

	smax = srcw/(float)r_warpscale_texture_width;
	tmax = srch/(float)r_warpscale_texture_height;

	GL_UseProgram (r_warpscale_program);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(0));
	GL_Uniform4fFunc(0, smax, tmax, water_warp ? 1.f/256.f : 0.f, cl.time);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	GL_EndGroup ();
}

/*
================
R_RenderView
================
*/
void R_RenderView (void)
{
	double	time1, time2;

	if (r_norefresh.value)
		return;

	if (!cl.worldmodel)
		Sys_Error ("R_RenderView: NULL worldmodel");

	time1 = 0; /* avoid compiler warning */
	if (r_speeds.value)
	{
		glFinish ();
		time1 = Sys_DoubleTime ();

		//johnfitz -- rendering statistics
		rs_brushpolys = rs_aliaspolys = rs_skypolys = rs_particles = rs_fogpolys = rs_megatexels =
		rs_dynamiclightmaps = rs_aliaspasses = rs_skypasses = rs_brushpasses = 0;
	}
	else if (gl_finish.value)
		glFinish ();

	R_SetupView (); //johnfitz -- this does everything that should be done once per frame
	R_RenderScene ();
	R_WarpScaleView ();

	//johnfitz -- modified r_speeds output
	time2 = Sys_DoubleTime ();
	if (r_pos.value)
		Con_Printf ("x %i y %i z %i (pitch %i yaw %i roll %i)\n",
			(int)cl_entities[cl.viewentity].origin[0],
			(int)cl_entities[cl.viewentity].origin[1],
			(int)cl_entities[cl.viewentity].origin[2],
			(int)cl.viewangles[PITCH],
			(int)cl.viewangles[YAW],
			(int)cl.viewangles[ROLL]);
	else if (r_speeds.value == 2)
		Con_Printf ("%3i ms  %4i/%4i wpoly %4i/%4i epoly %3i lmap %4i/%4i sky %1.1f mtex\n",
					(int)((time2-time1)*1000),
					rs_brushpolys,
					rs_brushpasses,
					rs_aliaspolys,
					rs_aliaspasses,
					rs_dynamiclightmaps,
					rs_skypolys,
					rs_skypasses,
					TexMgr_FrameUsage ());
	else if (r_speeds.value)
		Con_Printf ("%3i ms  %4i wpoly %4i epoly %3i lmap\n",
					(int)((time2-time1)*1000),
					rs_brushpolys,
					rs_aliaspolys,
					rs_dynamiclightmaps);
	//johnfitz
}

