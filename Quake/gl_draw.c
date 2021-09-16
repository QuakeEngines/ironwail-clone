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

// draw.c -- 2d drawing

#include "quakedef.h"

//extern unsigned char d_15to8table[65536]; //johnfitz -- never used

cvar_t		scr_conalpha = {"scr_conalpha", "0.5", CVAR_ARCHIVE}; //johnfitz

qpic_t		*draw_disc;
qpic_t		*draw_backtile;

gltexture_t *char_texture; //johnfitz
qpic_t		*pic_ovr, *pic_ins; //johnfitz -- new cursor handling
qpic_t		*pic_nul; //johnfitz -- for missing gfx, don't crash

//johnfitz -- new pics
byte pic_ovr_data[8][8] =
{
	{255,255,255,255,255,255,255,255},
	{255, 15, 15, 15, 15, 15, 15,255},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255,255,  2,  2,  2,  2,  2,  2},
};

byte pic_ins_data[9][8] =
{
	{ 15, 15,255,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{255,  2,  2,255,255,255,255,255},
};

byte pic_nul_data[8][8] =
{
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
};

byte pic_stipple_data[8][8] =
{
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
};

byte pic_crosshair_data[8][8] =
{
	{255,255,255,255,255,255,255,255},
	{255,255,255,  8,  9,255,255,255},
	{255,255,255,  6,  8,  2,255,255},
	{255,  6,  8,  8,  6,  8,  8,255},
	{255,255,  2,  8,  8,  2,  2,  2},
	{255,255,255,  7,  8,  2,255,255},
	{255,255,255,255,  2,  2,255,255},
	{255,255,255,255,255,255,255,255},
};
//johnfitz

static GLuint r_gui_program;

/*
=============
GLDraw_CreateShaders
=============
*/

void GLDraw_CreateShaders (void)
{
	const GLchar *vertSource = \
		"#version 430\n"
		"\n"
		"layout(location=0) uniform mat4 MVP;\n"
		"layout(location=1) uniform vec4 Color;\n"
		"\n"
		"layout(location=0) in vec4 in_pos;\n"
		"layout(location=1) in vec2 in_uv;\n"
		"\n"
		"layout(location=0) out vec2 out_uv;\n"
		"layout(location=1) out vec4 out_color;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = MVP * in_pos;\n"
		"	out_uv = in_uv;\n"
		"	out_color = Color;\n"
		"}\n";
	
	const GLchar *fragSource = \
		"#version 430\n"
		"\n"
		"layout(binding=0) uniform sampler2D Tex;\n"
		"\n"
		"layout(location=0) centroid in vec2 in_uv;\n"
		"layout(location=1) centroid in vec4 in_color;\n"
		"\n"
		"layout(location=0) out vec4 out_fragcolor;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	out_fragcolor = texture2D(Tex, in_uv) * in_color;\n"
		"}\n";

	r_gui_program = GL_CreateProgram (vertSource, fragSource, "gui");
}

typedef struct
{
	gltexture_t *gltexture;
	float		sl, tl, sh, th;
} glpic_t;

typedef struct guivertex_t {
	float		pos[2];
	float		uv[2];
} guivertex_t;

#define MAX_QUADS 256

static guivertex_t quadverts[4 * MAX_QUADS];
static unsigned short quadindices[6 * MAX_QUADS];

static canvastype currentcanvas = CANVAS_NONE; //johnfitz -- for GL_SetCanvas
static float canvasmatrix[16];
static vec4_t canvascolor = {1.f, 1.f, 1.f, 1.f};

//==============================================================================
//
//  PIC CACHING
//
//==============================================================================

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	qpic_t		pic;
	byte		padding[32];	// for appended glpic
} cachepic_t;

#define	MAX_CACHED_PICS		128
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;

byte		menuplyr_pixels[4096];

//  scrap allocation
//  Allocate all the little status bar obejcts into a single texture
//  to crutch up stupid hardware / drivers

#define	MAX_SCRAPS		2
#define	BLOCK_WIDTH		256
#define	BLOCK_HEIGHT	256

int			scrap_allocated[MAX_SCRAPS][BLOCK_WIDTH];
byte		scrap_texels[MAX_SCRAPS][BLOCK_WIDTH*BLOCK_HEIGHT]; //johnfitz -- removed *4 after BLOCK_HEIGHT
qboolean	scrap_dirty;
gltexture_t	*scrap_textures[MAX_SCRAPS]; //johnfitz


/*
================
Scrap_AllocBlock

returns an index into scrap_texnums[] and the position inside it
================
*/
int Scrap_AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		texnum;

	for (texnum=0 ; texnum<MAX_SCRAPS ; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i=0 ; i<BLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (scrap_allocated[texnum][i+j] >= best)
					break;
				if (scrap_allocated[texnum][i+j] > best2)
					best2 = scrap_allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			scrap_allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Sys_Error ("Scrap_AllocBlock: full"); //johnfitz -- correct function name
	return 0; //johnfitz -- shut up compiler
}

/*
================
Scrap_Upload -- johnfitz -- now uses TexMgr
================
*/
void Scrap_Upload (void)
{
	char name[8];
	int	i;

	for (i=0; i<MAX_SCRAPS; i++)
	{
		sprintf (name, "scrap%i", i);
		scrap_textures[i] = TexMgr_LoadImage (NULL, name, BLOCK_WIDTH, BLOCK_HEIGHT, SRC_INDEXED, scrap_texels[i],
			"", (src_offset_t)scrap_texels[i], TEXPREF_ALPHA | TEXPREF_OVERWRITE | TEXPREF_NOPICMIP);
	}

	scrap_dirty = false;
}

/*
================
Draw_PicFromWad
================
*/
qpic_t *Draw_PicFromWad (const char *name)
{
	qpic_t	*p;
	glpic_t	gl;
	src_offset_t offset; //johnfitz

	p = (qpic_t *) W_GetLumpName (name);
	if (!p) return pic_nul; //johnfitz

	// load little ones into the scrap
	if (p->width < 64 && p->height < 64)
	{
		int		x, y;
		int		i, j, k;
		int		texnum;

		texnum = Scrap_AllocBlock (p->width, p->height, &x, &y);
		scrap_dirty = true;
		k = 0;
		for (i=0 ; i<p->height ; i++)
		{
			for (j=0 ; j<p->width ; j++, k++)
				scrap_texels[texnum][(y+i)*BLOCK_WIDTH + x + j] = p->data[k];
		}
		gl.gltexture = scrap_textures[texnum]; //johnfitz -- changed to an array
		//johnfitz -- no longer go from 0.01 to 0.99
		gl.sl = x/(float)BLOCK_WIDTH;
		gl.sh = (x+p->width)/(float)BLOCK_WIDTH;
		gl.tl = y/(float)BLOCK_WIDTH;
		gl.th = (y+p->height)/(float)BLOCK_WIDTH;
	}
	else
	{
		char texturename[64]; //johnfitz
		q_snprintf (texturename, sizeof(texturename), "%s:%s", WADFILENAME, name); //johnfitz

		offset = (src_offset_t)p - (src_offset_t)wad_base + sizeof(int)*2; //johnfitz

		gl.gltexture = TexMgr_LoadImage (NULL, texturename, p->width, p->height, SRC_INDEXED, p->data, WADFILENAME,
										  offset, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP); //johnfitz -- TexMgr
		gl.sl = 0;
		gl.sh = (float)p->width/(float)TexMgr_PadConditional(p->width); //johnfitz
		gl.tl = 0;
		gl.th = (float)p->height/(float)TexMgr_PadConditional(p->height); //johnfitz
	}

	memcpy (p->data, &gl, sizeof(glpic_t));

	return p;
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (const char *path)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	glpic_t		gl;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
	{
		if (!strcmp (path, pic->name))
			return &pic->pic;
	}
	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
	menu_numcachepics++;
	strcpy (pic->name, path);

//
// load the pic from disk
//
	dat = (qpic_t *)COM_LoadTempFile (path, NULL);
	if (!dat)
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	SwapPic (dat);

	// HACK HACK HACK --- we need to keep the bytes for
	// the translatable player picture just for the menu
	// configuration dialog
	if (!strcmp (path, "gfx/menuplyr.lmp"))
		memcpy (menuplyr_pixels, dat->data, dat->width*dat->height);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	gl.gltexture = TexMgr_LoadImage (NULL, path, dat->width, dat->height, SRC_INDEXED, dat->data, path,
									  sizeof(int)*2, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP); //johnfitz -- TexMgr
	gl.sl = 0;
	gl.sh = (float)dat->width/(float)TexMgr_PadConditional(dat->width); //johnfitz
	gl.tl = 0;
	gl.th = (float)dat->height/(float)TexMgr_PadConditional(dat->height); //johnfitz
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}

/*
================
Draw_MakePic -- johnfitz -- generate pics from internal data
================
*/
qpic_t *Draw_MakePic (const char *name, int width, int height, byte *data)
{
	int flags = TEXPREF_NEAREST | TEXPREF_ALPHA | TEXPREF_PERSIST | TEXPREF_NOPICMIP | TEXPREF_PAD;
	qpic_t		*pic;
	glpic_t		gl;

	pic = (qpic_t *) Hunk_Alloc (sizeof(qpic_t) - 4 + sizeof (glpic_t));
	pic->width = width;
	pic->height = height;

	gl.gltexture = TexMgr_LoadImage (NULL, name, width, height, SRC_INDEXED, data, "", (src_offset_t)data, flags);
	gl.sl = 0;
	gl.sh = (float)width/(float)TexMgr_PadConditional(width);
	gl.tl = 0;
	gl.th = (float)height/(float)TexMgr_PadConditional(height);
	memcpy (pic->data, &gl, sizeof(glpic_t));

	return pic;
}

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
===============
Draw_LoadPics -- johnfitz
===============
*/
void Draw_LoadPics (void)
{
	byte		*data;
	src_offset_t	offset;

	data = (byte *) W_GetLumpName ("conchars");
	if (!data) Sys_Error ("Draw_LoadPics: couldn't load conchars");
	offset = (src_offset_t)data - (src_offset_t)wad_base;
	char_texture = TexMgr_LoadImage (NULL, WADFILENAME":conchars", 128, 128, SRC_INDEXED, data,
		WADFILENAME, offset, TEXPREF_ALPHA | TEXPREF_NEAREST | TEXPREF_NOPICMIP | TEXPREF_CONCHARS);

	draw_disc = Draw_PicFromWad ("disc");
	draw_backtile = Draw_PicFromWad ("backtile");
}

/*
===============
Draw_NewGame -- johnfitz
===============
*/
void Draw_NewGame (void)
{
	cachepic_t	*pic;
	int			i;

	// empty scrap and reallocate gltextures
	memset(scrap_allocated, 0, sizeof(scrap_allocated));
	memset(scrap_texels, 255, sizeof(scrap_texels));

	Scrap_Upload (); //creates 2 empty gltextures

	// reload wad pics
	W_LoadWadFile (); //johnfitz -- filename is now hard-coded for honesty
	Draw_LoadPics ();
	SCR_LoadPics ();
	Sbar_LoadPics ();

	// empty lmp cache
	for (pic = menu_cachepics, i = 0; i < menu_numcachepics; pic++, i++)
		pic->name[0] = 0;
	menu_numcachepics = 0;
}

/*
===============
Draw_Init -- johnfitz -- rewritten
===============
*/
void Draw_Init (void)
{
	int i;

	Cvar_RegisterVariable (&scr_conalpha);

	// init quad indices
	for (i = 0; i < MAX_QUADS; i++)
	{
		quadindices[i*6 + 0] = i*4 + 0;
		quadindices[i*6 + 1] = i*4 + 1;
		quadindices[i*6 + 2] = i*4 + 2;
		quadindices[i*6 + 3] = i*4 + 0;
		quadindices[i*6 + 4] = i*4 + 2;
		quadindices[i*6 + 5] = i*4 + 3;
	}

	// clear scrap and allocate gltextures
	memset(scrap_allocated, 0, sizeof(scrap_allocated));
	memset(scrap_texels, 255, sizeof(scrap_texels));

	Scrap_Upload (); //creates 2 empty textures

	// create internal pics
	pic_ins = Draw_MakePic ("ins", 8, 9, &pic_ins_data[0][0]);
	pic_ovr = Draw_MakePic ("ovr", 8, 8, &pic_ovr_data[0][0]);
	pic_nul = Draw_MakePic ("nul", 8, 8, &pic_nul_data[0][0]);

	// load game pics
	Draw_LoadPics ();
}

//==============================================================================
//
//  2D DRAWING
//
//==============================================================================

/*
================
GL_SetCanvasColor
================
*/
void GL_SetCanvasColor (float r, float g, float b, float a)
{
	canvascolor[0] = r;
	canvascolor[1] = g;
	canvascolor[2] = b;
	canvascolor[3] = a;
}

/*
================
Draw_FillCharacterQuad -- johnfitz -- seperate function to spit out verts
================
*/
void Draw_FillCharacterQuad (int x, int y, char num, guivertex_t *verts)
{
	int				row, col;
	float			frow, fcol, size;

	row = num>>4;
	col = num&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	#define ADD_VERTEX(uvx, uvy, px, py)\
		verts->pos[0] = px; verts->pos[1] = py; verts->uv[0] = uvx; verts->uv[1] = uvy; ++verts

	ADD_VERTEX(fcol, frow, x, y);
	ADD_VERTEX(fcol + size, frow, x+8, y);
	ADD_VERTEX(fcol + size, frow + size, x+8, y+8);
	ADD_VERTEX(fcol, frow + size, x, y+8);

	#undef ADD_VERTEX
}

/*
================
Draw_Character -- johnfitz -- modified to call Draw_FillCharacterQuad
================
*/
void Draw_Character (int x, int y, int num)
{
	guivertex_t verts[4];

	if (y <= -8)
		return;			// totally off screen

	num &= 255;

	if (num == 32)
		return; //don't waste verts on spaces

	Draw_FillCharacterQuad (x, y, (char) num, verts);

	GL_UseProgram (r_gui_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, canvasmatrix);
	GL_Uniform4fvFunc (1, 1, canvascolor);

	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof(verts[0]), &verts[0].pos);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(verts[0]), &verts[0].uv);

	GL_Bind (GL_TEXTURE0, char_texture);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/*
================
Draw_String -- johnfitz -- modified to call Draw_FillCharacterQuad
================
*/
void Draw_String (int x, int y, const char *str)
{
	int numverts = 0;

	if (y <= -8)
		return;			// totally off screen

	GL_UseProgram (r_gui_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, canvasmatrix);
	GL_Uniform4fvFunc (1, 1, canvascolor);

	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(quadverts[0]), &quadverts[0].uv);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof(quadverts[0]), &quadverts[0].pos);

	GL_Bind (GL_TEXTURE0, char_texture);

	while (*str)
	{
		if (numverts == countof(quadverts))
		{
			glDrawElements(GL_TRIANGLES, numverts + (numverts >> 1), GL_UNSIGNED_SHORT, quadindices);
			numverts = 0;
		}
		if (*str != 32) //don't waste verts on spaces
		{
			Draw_FillCharacterQuad (x, y, *str, quadverts + numverts);
			numverts += 4;
		}
		str++;
		x += 8;
	}

	if (numverts)
		glDrawElements(GL_TRIANGLES, numverts + (numverts >> 1), GL_UNSIGNED_SHORT, quadindices);
}

/*
=============
Draw_Pic -- johnfitz -- modified
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	glpic_t		*gl;
	float		quad_pos[4 * 2];
	float		quad_uv[4 * 2];
	float		*pos = quad_pos;
	float		*uv = quad_uv;

	if (scrap_dirty)
		Scrap_Upload ();
	gl = (glpic_t *)pic->data;

	#define ADD_VERTEX(uvx, uvy, px, py)\
		*pos++ = px; *pos++ = py; *uv++ = uvx; *uv++ = uvy

	ADD_VERTEX(gl->sl, gl->tl, x, y);
	ADD_VERTEX(gl->sh, gl->tl, x+pic->width, y);
	ADD_VERTEX(gl->sh, gl->th, x+pic->width, y+pic->height);
	ADD_VERTEX(gl->sl, gl->th, x, y+pic->height);

	#undef ADD_VERTEX

	GL_UseProgram (r_gui_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, canvasmatrix);
	GL_Uniform4fvFunc (1, 1, canvascolor);

	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, 0, quad_pos);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, 0, quad_uv);

	GL_Bind (GL_TEXTURE0, gl->gltexture);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/*
=============
Draw_TransPicTranslate -- johnfitz -- rewritten to use texmgr to do translation

Only used for the player color selection menu
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, int top, int bottom)
{
	static int oldtop = -2;
	static int oldbottom = -2;

	if (top != oldtop || bottom != oldbottom)
	{
		glpic_t *p = (glpic_t *)pic->data;
		gltexture_t *glt = p->gltexture;
		oldtop = top;
		oldbottom = bottom;
		TexMgr_ReloadImage (glt, top, bottom);
	}
	Draw_Pic (x, y, pic);
}

/*
================
Draw_ConsoleBackground -- johnfitz -- rewritten
================
*/
void Draw_ConsoleBackground (void)
{
	qpic_t *pic;
	float alpha;

	pic = Draw_CachePic ("gfx/conback.lmp");
	pic->width = vid.conwidth;
	pic->height = vid.conheight;

	alpha = (con_forcedup) ? 1.0 : scr_conalpha.value;

	GL_SetCanvas (CANVAS_CONSOLE); //in case this is called from weird places

	if (alpha > 0.0)
	{
		GL_SetCanvasColor (1.f, 1.f, 1.f, alpha);
		Draw_Pic (0, 0, pic);
		GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
	}
}


/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	float quad_pos[4 * 2];
	float quad_uv[4 * 2];
	float *pos = quad_pos;
	float *uv = quad_uv;

	glpic_t	*gl;

	gl = (glpic_t *)draw_backtile->data;

	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);

	#define ADD_VERTEX(uvx, uvy, px, py)\
		*pos++ = px; *pos++ = py; *uv++ = uvx; *uv++ = uvy

	ADD_VERTEX(x/64.0, y/64.0, x, y);
	ADD_VERTEX((x+w)/64.0, y/64.0, x+w, y);
	ADD_VERTEX((x+w)/64.0, (y+h)/64.0, x+w, y+h);
	ADD_VERTEX(x/64.0, (y+h)/64.0, x, y+h);

	#undef ADD_VERTEX

	GL_UseProgram (r_gui_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, canvasmatrix);
	GL_Uniform4fvFunc (1, 1, canvascolor);

	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, 0, quad_pos);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, 0, quad_uv);

	GL_Bind (GL_TEXTURE0, gl->gltexture);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c, float alpha) //johnfitz -- added alpha
{
	byte *pal = (byte *)d_8to24table; //johnfitz -- use d_8to24table instead of host_basepal

	float quad_pos[4 * 2];
	float quad_uv[4 * 2] = { 0 };
	float *pos = quad_pos;

	GL_SetCanvasColor (pal[c*4]/255.0, pal[c*4+1]/255.0, pal[c*4+2]/255.0, alpha); //johnfitz -- added alpha

	#define ADD_VERTEX(px, py)\
		*pos++ = px; *pos++ = py

	ADD_VERTEX(x, y);
	ADD_VERTEX(x+w, y);
	ADD_VERTEX(x+w, y+h);
	ADD_VERTEX(x, y+h);

	#undef ADD_VERTEX

	GL_UseProgram (r_gui_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, canvasmatrix);
	GL_Uniform4fvFunc (1, 1, canvascolor);

	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, 0, quad_pos);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, 0, quad_uv);

	GL_Bind (GL_TEXTURE0, whitetexture);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
}

/*
================
Draw_FadeScreen -- johnfitz -- revised
================
*/
void Draw_FadeScreen (void)
{
	float quad_pos[4 * 2];
	float quad_uv[4 * 2] = { 0 };
	float *pos = quad_pos;

	GL_SetCanvas (CANVAS_DEFAULT);
	GL_SetCanvasColor (0.f, 0.f, 0.f, 0.5f);

	#define ADD_VERTEX(px, py)\
		*pos++ = px; *pos++ = py

	ADD_VERTEX(0, 0);
	ADD_VERTEX(glwidth, 0);
	ADD_VERTEX(glwidth, glheight);
	ADD_VERTEX(0, glheight);

	#undef ADD_VERTEX

	GL_UseProgram (r_gui_program);
	GL_UniformMatrix4fvFunc (0, 1, GL_FALSE, canvasmatrix);
	GL_Uniform4fvFunc (1, 1, canvascolor);

	GL_SetState (GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(2));
	GL_BindBuffer (GL_ARRAY_BUFFER, 0);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, 0, quad_pos);
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, 0, quad_uv);

	GL_Bind (GL_TEXTURE0, whitetexture);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);

	Sbar_Changed();
}

/*
================
GL_OrthoMatrix
================
*/
static void GL_OrthoMatrix(float left, float right, float bottom, float top, float n, float f)
{
	float tx = -(right + left) / (right - left);
	float ty = -(top + bottom) / (top - bottom);
	float tz = -(f + n) / (f - n);

	memset(&canvasmatrix, 0, sizeof(canvasmatrix));

	// First column
	canvasmatrix[0*4 + 0] = 2.0f / (right-left);

	// Second column
	canvasmatrix[1*4 + 1] = 2.0f / (top-bottom);
	
	// Third column
	canvasmatrix[2*4 + 2] = 2.0f / (f-n);

	// Fourth column
	canvasmatrix[3*4 + 0] = tx;
	canvasmatrix[3*4 + 1] = ty;
	canvasmatrix[3*4 + 2] = tz;
	canvasmatrix[3*4 + 3] = 1.0f;
}

/*
================
GL_SetCanvas -- johnfitz -- support various canvas types
================
*/
void GL_SetCanvas (canvastype newcanvas)
{
	extern vrect_t scr_vrect;
	float s;
	int lines;

	if (newcanvas == currentcanvas)
		return;

	currentcanvas = newcanvas;

	switch(newcanvas)
	{
	case CANVAS_DEFAULT:
		GL_OrthoMatrix (0, glwidth, glheight, 0, -99999, 99999);
		glViewport (glx, gly, glwidth, glheight);
		break;
	case CANVAS_CONSOLE:
		lines = vid.conheight - (scr_con_current * vid.conheight / glheight);
		GL_OrthoMatrix (0, vid.conwidth, vid.conheight + lines, lines, -99999, 99999);
		glViewport (glx, gly, glwidth, glheight);
		break;
	case CANVAS_MENU:
		s = q_min((float)glwidth / 320.0, (float)glheight / 200.0);
		s = CLAMP (1.0, scr_menuscale.value, s);
		// ericw -- doubled width to 640 to accommodate long keybindings
		GL_OrthoMatrix (0, 640, 200, 0, -99999, 99999);
		glViewport (glx + (glwidth - 320*s) / 2, gly + (glheight - 200*s) / 2, 640*s, 200*s);
		break;
	case CANVAS_SBAR:
		s = CLAMP (1.0, scr_sbarscale.value, (float)glwidth / 320.0);
		if (cl.gametype == GAME_DEATHMATCH)
		{
			GL_OrthoMatrix (0, glwidth / s, 48, 0, -99999, 99999);
			glViewport (glx, gly, glwidth, 48*s);
		}
		else
		{
			GL_OrthoMatrix (0, 320, 48, 0, -99999, 99999);
			glViewport (glx + (glwidth - 320*s) / 2, gly, 320*s, 48*s);
		}
		break;
	case CANVAS_CROSSHAIR: //0,0 is center of viewport
		s = CLAMP (1.0, scr_crosshairscale.value, 10.0);
		GL_OrthoMatrix (scr_vrect.width/-2/s, scr_vrect.width/2/s, scr_vrect.height/2/s, scr_vrect.height/-2/s, -99999, 99999);
		glViewport (scr_vrect.x, glheight - scr_vrect.y - scr_vrect.height, scr_vrect.width & ~1, scr_vrect.height & ~1);
		break;
	case CANVAS_BOTTOMLEFT: //used by devstats
		s = (float)glwidth/vid.conwidth; //use console scale
		GL_OrthoMatrix (0, 320, 200, 0, -99999, 99999);
		glViewport (glx, gly, 320*s, 200*s);
		break;
	case CANVAS_BOTTOMRIGHT: //used by fps/clock
		s = (float)glwidth/vid.conwidth; //use console scale
		GL_OrthoMatrix (0, 320, 200, 0, -99999, 99999);
		glViewport (glx+glwidth-320*s, gly, 320*s, 200*s);
		break;
	case CANVAS_TOPRIGHT: //used by disc
		s = 1;
		GL_OrthoMatrix (0, 320, 200, 0, -99999, 99999);
		glViewport (glx+glwidth-320*s, gly+glheight-200*s, 320*s, 200*s);
		break;
	default:
		Sys_Error ("GL_SetCanvas: bad canvas type");
	}
}

/*
================
GL_Set2D
================
*/
void GL_Set2D (void)
{
	currentcanvas = CANVAS_INVALID;
	GL_SetCanvas (CANVAS_DEFAULT);
	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
}
