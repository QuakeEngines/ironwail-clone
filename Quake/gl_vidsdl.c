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
// gl_vidsdl.c -- SDL GL vid component

#include "quakedef.h"
#include "cfgfile.h"
#include "bgmusic.h"
#include "resource.h"
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif

//ericw -- for putting the driver into multithreaded mode
#ifdef __APPLE__
#include <OpenGL/OpenGL.h>
#endif

#define MAX_MODE_LIST	600 //johnfitz -- was 30
#define MAX_BPPS_LIST	5
#define MAX_RATES_LIST	20
#define WARP_WIDTH		320
#define WARP_HEIGHT		200
#define MAXWIDTH		10000
#define MAXHEIGHT		10000

#define DEFAULT_SDL_FLAGS	SDL_OPENGL

#define DEFAULT_REFRESHRATE	60

typedef struct {
	int			width;
	int			height;
	int			refreshrate;
	int			bpp;
} vmode_t;

#define MAKE_GL_VERSION(major, minor)		(((major) << 16) | (minor))
#define MIN_GL_VERSION_MAJOR				4
#define MIN_GL_VERSION_MINOR				4
#define MIN_GL_VERSION						MAKE_GL_VERSION(MIN_GL_VERSION_MAJOR, MIN_GL_VERSION_MINOR)
#define MIN_GL_VERSION_STR					QS_STRINGIFY(MIN_GL_VERSION_MAJOR)"."QS_STRINGIFY(MIN_GL_VERSION_MINOR)

static const char *gl_vendor;
static const char *gl_renderer;
static const char *gl_version;
static int gl_version_major;
static int gl_version_minor;
static int gl_version_number;
static int gl_num_extensions;

static vmode_t	modelist[MAX_MODE_LIST];
static int		nummodes;

static qboolean	vid_initialized = false;

#if defined(USE_SDL2)
static SDL_Window	*draw_context;
static SDL_GLContext	gl_context;
#else
static SDL_Surface	*draw_context;
#endif

static qboolean	vid_locked = false; //johnfitz
static qboolean	vid_changed = false;

static void VID_Menu_Init (void); //johnfitz
static void VID_Menu_f (void); //johnfitz
static void VID_MenuDraw (void);
static void VID_MenuKey (int key);

static void ClearAllStates (void);
static void GL_Init (void);
static void GL_SetupState (void); //johnfitz

viddef_t	vid;				// global video state
modestate_t	modestate = MS_UNINIT;
qboolean	scr_skipupdate;

qboolean gl_swap_control = false; //johnfitz
qboolean gl_anisotropy_able = false; //johnfitz
qboolean gl_bindless_able = false;
qboolean gl_clipcontrol_able = false;
float gl_max_anisotropy; //johnfitz
int gl_stencilbits;

unsigned glstate;
GLint ssbo_align;
static GLuint globalvao;

#define QGL_DEFINE_FUNC(ret, name, args) ret (APIENTRYP GL_##name##Func) args = NULL;
QGL_ALL_FUNCTIONS(QGL_DEFINE_FUNC)
#undef QGL_DEFINE_FUNC

typedef struct glfunc_t {
	void**			ptr;
	const char*		name;
} glfunc_t;

#define QGL_REGISTER_NAMED_FUNC(ret, name, args) { (void**)&GL_##name##Func, "gl" #name },
static const glfunc_t gl_core_functions[] =
{
	QGL_CORE_FUNCTIONS(QGL_REGISTER_NAMED_FUNC)
	{NULL, NULL}
};

static const glfunc_t gl_arb_bindless_texture_functions[] =
{
	QGL_ARB_bindless_texture_FUNCTIONS(QGL_REGISTER_NAMED_FUNC)
	{NULL, NULL}
};

static const glfunc_t gl_arb_clip_control_functions[] =
{
	QGL_ARB_clip_control_FUNCTIONS(QGL_REGISTER_NAMED_FUNC)
	{NULL, NULL}
};
#undef QGL_REGISTER_NAMED_FUNC

//====================================

//johnfitz -- new cvars
cvar_t		vid_fullscreen = {"vid_fullscreen", "0", CVAR_ARCHIVE};	// QuakeSpasm, was "1"
cvar_t		vid_width = {"vid_width", "800", CVAR_ARCHIVE};		// QuakeSpasm, was 640
cvar_t		vid_height = {"vid_height", "600", CVAR_ARCHIVE};	// QuakeSpasm, was 480
cvar_t		vid_bpp = {"vid_bpp", "16", CVAR_ARCHIVE};
cvar_t		vid_refreshrate = {"vid_refreshrate", "60", CVAR_ARCHIVE};
cvar_t		vid_vsync = {"vid_vsync", "0", CVAR_ARCHIVE};
cvar_t		vid_fsaa = {"vid_fsaa", "0", CVAR_ARCHIVE}; // QuakeSpasm
cvar_t		vid_desktopfullscreen = {"vid_desktopfullscreen", "0", CVAR_ARCHIVE}; // QuakeSpasm
cvar_t		vid_borderless = {"vid_borderless", "0", CVAR_ARCHIVE}; // QuakeSpasm
//johnfitz

cvar_t		vid_gamma = {"gamma", "1", CVAR_ARCHIVE}; //johnfitz -- moved here from view.c
cvar_t		vid_contrast = {"contrast", "1", CVAR_ARCHIVE}; //QuakeSpasm, MarkV

//==========================================================================
//
//  HARDWARE GAMMA -- johnfitz
//
//==========================================================================

static int fsaa;

/*
================
VID_Gamma_Init -- call on init
================
*/
static void VID_Gamma_Init (void)
{
	Cvar_RegisterVariable (&vid_gamma);
	Cvar_RegisterVariable (&vid_contrast);
}

/*
======================
VID_GetCurrentWidth
======================
*/
static int VID_GetCurrentWidth (void)
{
#if defined(USE_SDL2)
	int w = 0, h = 0;
	SDL_GetWindowSize(draw_context, &w, &h);
	return w;
#else
	return draw_context->w;
#endif
}

/*
=======================
VID_GetCurrentHeight
=======================
*/
static int VID_GetCurrentHeight (void)
{
#if defined(USE_SDL2)
	int w = 0, h = 0;
	SDL_GetWindowSize(draw_context, &w, &h);
	return h;
#else
	return draw_context->h;
#endif
}

/*
====================
VID_GetCurrentRefreshRate
====================
*/
static int VID_GetCurrentRefreshRate (void)
{
#if defined(USE_SDL2)
	SDL_DisplayMode mode;
	int current_display;
	
	current_display = SDL_GetWindowDisplayIndex(draw_context);
	
	if (0 != SDL_GetCurrentDisplayMode(current_display, &mode))
		return DEFAULT_REFRESHRATE;
	
	return mode.refresh_rate;
#else
	// SDL1.2 doesn't support refresh rates
	return DEFAULT_REFRESHRATE;
#endif
}


/*
====================
VID_GetCurrentBPP
====================
*/
static int VID_GetCurrentBPP (void)
{
#if defined(USE_SDL2)
	const Uint32 pixelFormat = SDL_GetWindowPixelFormat(draw_context);
	return SDL_BITSPERPIXEL(pixelFormat);
#else
	return draw_context->format->BitsPerPixel;
#endif
}

/*
====================
VID_GetFullscreen
 
returns true if we are in regular fullscreen or "desktop fullscren"
====================
*/
static qboolean VID_GetFullscreen (void)
{
#if defined(USE_SDL2)
	return (SDL_GetWindowFlags(draw_context) & SDL_WINDOW_FULLSCREEN) != 0;
#else
	return (draw_context->flags & SDL_FULLSCREEN) != 0;
#endif
}

/*
====================
VID_GetDesktopFullscreen
 
returns true if we are specifically in "desktop fullscreen" mode
====================
*/
static qboolean VID_GetDesktopFullscreen (void)
{
#if defined(USE_SDL2)
	return (SDL_GetWindowFlags(draw_context) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
#else
	return false;
#endif
}

/*
====================
VID_GetVSync
====================
*/
static qboolean VID_GetVSync (void)
{
#if defined(USE_SDL2)
	return SDL_GL_GetSwapInterval() == 1;
#else
	int swap_control;
	if (SDL_GL_GetAttribute(SDL_GL_SWAP_CONTROL, &swap_control) == 0)
		return swap_control > 0;
	return false;
#endif
}

/*
====================
VID_GetWindow

used by pl_win.c
====================
*/
void *VID_GetWindow (void)
{
#if defined(USE_SDL2)
	return draw_context;
#else
	return NULL;
#endif
}

/*
====================
VID_HasMouseOrInputFocus
====================
*/
qboolean VID_HasMouseOrInputFocus (void)
{
#if defined(USE_SDL2)
	return (SDL_GetWindowFlags(draw_context) & (SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS)) != 0;
#else
	return (SDL_GetAppState() & (SDL_APPMOUSEFOCUS | SDL_APPINPUTFOCUS)) != 0;
#endif
}

/*
====================
VID_IsMinimized
====================
*/
qboolean VID_IsMinimized (void)
{
#if defined(USE_SDL2)
	return !(SDL_GetWindowFlags(draw_context) & SDL_WINDOW_SHOWN);
#else
	/* SDL_APPACTIVE in SDL 1.x means "not minimized" */
	return !(SDL_GetAppState() & SDL_APPACTIVE);
#endif
}

#if defined(USE_SDL2)
/*
================
VID_SDL2_GetDisplayMode

Returns a pointer to a statically allocated SDL_DisplayMode structure
if there is one with the requested params on the default display.
Otherwise returns NULL.

This is passed to SDL_SetWindowDisplayMode to specify a pixel format
with the requested bpp. If we didn't care about bpp we could just pass NULL.
================
*/
static SDL_DisplayMode *VID_SDL2_GetDisplayMode(int width, int height, int refreshrate, int bpp)
{
	static SDL_DisplayMode mode;
	const int sdlmodes = SDL_GetNumDisplayModes(0);
	int i;

	for (i = 0; i < sdlmodes; i++)
	{
		if (SDL_GetDisplayMode(0, i, &mode) != 0)
			continue;
		
		if (mode.w == width && mode.h == height
			&& SDL_BITSPERPIXEL(mode.format) == bpp
			&& mode.refresh_rate == refreshrate)
		{
			return &mode;
		}
	}
	return NULL;
}
#endif /* USE_SDL2 */

/*
================
VID_ValidMode
================
*/
static qboolean VID_ValidMode (int width, int height, int refreshrate, int bpp, qboolean fullscreen)
{
// ignore width / height / bpp if vid_desktopfullscreen is enabled
	if (fullscreen && vid_desktopfullscreen.value)
		return true;

	if (width < 320)
		return false;

	if (height < 200)
		return false;

#if defined(USE_SDL2)
	if (fullscreen && VID_SDL2_GetDisplayMode(width, height, refreshrate, bpp) == NULL)
		bpp = 0;
#else
	{
		Uint32 flags = DEFAULT_SDL_FLAGS;
		if (fullscreen)
			flags |= SDL_FULLSCREEN;

		bpp = SDL_VideoModeOK(width, height, bpp, flags);
	}
#endif

	switch (bpp)
	{
	case 16:
	case 24:
	case 32:
		break;
	default:
		return false;
	}

	return true;
}

/*
================
VID_SetMode
================
*/
static qboolean VID_SetMode (int width, int height, int refreshrate, int bpp, qboolean fullscreen)
{
	int		temp;
	Uint32	flags;
	char		caption[50];
	int		depthbits, stencilbits;
#if defined(USE_SDL2)
	int		previous_display;
#endif

	// so Con_Printfs don't mess us up by forcing vid and snd updates
	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;

	CDAudio_Pause ();
	BGM_Pause ();

	/* z-buffer depth */
	if (bpp == 16)
		depthbits = 16;
	else
		depthbits = 24;
	stencilbits = 0;
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depthbits);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencilbits);

	q_snprintf(caption, sizeof(caption), "QuakeSpasm " QUAKESPASM_VER_STRING);

#if defined(USE_SDL2)
	/* Create the window if needed, hidden */
	if (!draw_context)
	{
		flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;

		if (vid_borderless.value)
			flags |= SDL_WINDOW_BORDERLESS;
		
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, MIN_GL_VERSION_MAJOR);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, MIN_GL_VERSION_MINOR);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifndef NDEBUG
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
		draw_context = SDL_CreateWindow (caption, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
		if (!draw_context) { // scale back SDL_GL_DEPTH_SIZE
			SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
			draw_context = SDL_CreateWindow (caption, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
		}
		if (!draw_context)
			Sys_Error ("Couldn't create window");

		previous_display = -1;
	}
	else
	{
		previous_display = SDL_GetWindowDisplayIndex(draw_context);
	}

	/* Ensure the window is not fullscreen */
	if (VID_GetFullscreen ())
	{
		if (SDL_SetWindowFullscreen (draw_context, 0) != 0)
			Sys_Error("Couldn't set fullscreen state mode");
	}

	/* Set window size and display mode */
	SDL_SetWindowSize (draw_context, width, height);
	if (previous_display >= 0)
		SDL_SetWindowPosition (draw_context, SDL_WINDOWPOS_CENTERED_DISPLAY(previous_display), SDL_WINDOWPOS_CENTERED_DISPLAY(previous_display));
	else
		SDL_SetWindowPosition(draw_context, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_SetWindowDisplayMode (draw_context, VID_SDL2_GetDisplayMode(width, height, refreshrate, bpp));
	SDL_SetWindowBordered (draw_context, vid_borderless.value ? SDL_FALSE : SDL_TRUE);

	/* Make window fullscreen if needed, and show the window */

	if (fullscreen) {
		const Uint32 flag = vid_desktopfullscreen.value ?
				SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
		if (SDL_SetWindowFullscreen (draw_context, flag) != 0)
			Sys_Error ("Couldn't set fullscreen state mode");
	}

	SDL_ShowWindow (draw_context);

	/* Create GL context if needed */
	if (!gl_context) {
		gl_context = SDL_GL_CreateContext(draw_context);
		if (!gl_context)
			Sys_Error("Couldn't create GL context");
	}

	gl_swap_control = true;
	if (SDL_GL_SetSwapInterval ((vid_vsync.value) ? 1 : 0) == -1)
		gl_swap_control = false;

#else /* !defined(USE_SDL2) */

	flags = DEFAULT_SDL_FLAGS;
	if (fullscreen)
		flags |= SDL_FULLSCREEN;
	if (vid_borderless.value)
		flags |= SDL_NOFRAME;
	
	gl_swap_control = true;
	if (SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, (vid_vsync.value) ? 1 : 0) == -1)
		gl_swap_control = false;

	bpp = SDL_VideoModeOK(width, height, bpp, flags);

	draw_context = SDL_SetVideoMode(width, height, bpp, flags);
	if (!draw_context) { // scale back SDL_GL_DEPTH_SIZE
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
		draw_context = SDL_SetVideoMode(width, height, bpp, flags);
	}
	if (!draw_context) { // scale back SDL_GL_STENCIL_SIZE
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
		draw_context = SDL_SetVideoMode(width, height, bpp, flags);
		if (!draw_context)
			Sys_Error ("Couldn't set video mode");
	}

	SDL_WM_SetCaption(caption, caption);
#endif /* !defined(USE_SDL2) */

	vid.width = VID_GetCurrentWidth();
	vid.height = VID_GetCurrentHeight();
	vid.refreshrate = VID_GetCurrentRefreshRate();
	vid.conwidth = vid.width & 0xFFFFFFF8;
	vid.conheight = vid.conwidth * vid.height / vid.width;
	vid.numpages = 2;

// read the obtained z-buffer depth
	if (SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthbits) == -1)
		depthbits = 0;

// read stencil bits
	if (SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &gl_stencilbits) == -1)
		gl_stencilbits = 0;

	modestate = VID_GetFullscreen() ? MS_FULLSCREEN : MS_WINDOWED;

	CDAudio_Resume ();
	BGM_Resume ();
	scr_disabled_for_loading = temp;

// fix the leftover Alt from any Alt-Tab or the like that switched us away
	ClearAllStates ();

	Con_SafePrintf ("Video mode: %dx%dx%d %dHz\n",
				VID_GetCurrentWidth(),
				VID_GetCurrentHeight(),
				VID_GetCurrentBPP(),
				VID_GetCurrentRefreshRate());

	vid.recalc_refdef = 1;

// no pending changes
	vid_changed = false;

	return true;
}

/*
===================
VID_Changed_f -- kristian -- notify us that a value has changed that requires a vid_restart
===================
*/
void VID_Changed_f (cvar_t *var)
{
	if (vid_initialized)
		Con_SafePrintf ("%s will be applied after a vid_restart\n", var->name);
	vid_changed = true;
}

/*
===================
VID_Restart -- johnfitz -- change video modes on the fly
===================
*/
static void VID_Restart (void)
{
	int width, height, refreshrate, bpp;
	qboolean fullscreen;

	if (vid_locked || !vid_changed)
		return;

	width = (int)vid_width.value;
	height = (int)vid_height.value;
	refreshrate = (int)vid_refreshrate.value;
	bpp = (int)vid_bpp.value;
	fullscreen = vid_fullscreen.value ? true : false;

//
// validate new mode
//
	if (!VID_ValidMode (width, height, refreshrate, bpp, fullscreen))
	{
		Con_Printf ("%dx%dx%d %dHz %s is not a valid mode\n",
				width, height, bpp, refreshrate, fullscreen? "fullscreen" : "windowed");
		return;
	}
	
// ericw -- OS X, SDL1: textures, VBO's invalid after mode change
//          OS X, SDL2: still valid after mode change
// To handle both cases, delete all GL objects (textures, VBO, GLSL) now.
// We must not interleave deleting the old objects with creating new ones, because
// one of the new objects could be given the same ID as an invalid handle
// which is later deleted.

	TexMgr_DeleteTextureObjects ();
	GLLight_DeleteResources ();
	GL_DeleteFrameBuffers ();
	GL_DeleteShaders ();
	GL_DeleteBModelBuffers ();
	GL_DeleteLightmapResources ();
	GLMesh_DeleteVertexBuffers ();
	R_InvalidateLightmaps ();

//
// set new mode
//
	VID_SetMode (width, height, refreshrate, bpp, fullscreen);

	GL_Init ();
	TexMgr_ReloadImages ();
	GL_CreateLightmapResources ();
	GL_BuildBModelVertexBuffer ();
	GL_BuildBModelMarkBuffers ();
	GLMesh_LoadVertexBuffers ();
	GL_SetupState ();

	//conwidth and conheight need to be recalculated
	vid.conwidth = (scr_conwidth.value > 0) ? (int)scr_conwidth.value : (scr_conscale.value > 0) ? (int)(vid.width/scr_conscale.value) : vid.width;
	vid.conwidth = CLAMP (320, vid.conwidth, vid.width);
	vid.conwidth &= 0xFFFFFFF8;
	vid.conheight = vid.conwidth * vid.height / vid.width;
//
// keep cvars in line with actual mode
//
	VID_SyncCvars();
//
// update mouse grab
//
	if (key_dest == key_console || key_dest == key_menu)
	{
		if (modestate == MS_WINDOWED)
			IN_Deactivate(true);
		else if (modestate == MS_FULLSCREEN)
			IN_Activate();
	}
}

/*
================
VID_Test -- johnfitz -- like vid_restart, but asks for confirmation after switching modes
================
*/
static void VID_Test (void)
{
	int old_width, old_height, old_refreshrate, old_bpp, old_fullscreen;

	if (vid_locked || !vid_changed)
		return;
//
// now try the switch
//
	old_width = VID_GetCurrentWidth();
	old_height = VID_GetCurrentHeight();
	old_refreshrate = VID_GetCurrentRefreshRate();
	old_bpp = VID_GetCurrentBPP();
	old_fullscreen = VID_GetFullscreen() ? true : false;

	VID_Restart ();

	//pop up confirmation dialoge
	if (!SCR_ModalMessage("Would you like to keep this\nvideo mode? (y/n)\n", 5.0f))
	{
		//revert cvars and mode
		Cvar_SetValueQuick (&vid_width, old_width);
		Cvar_SetValueQuick (&vid_height, old_height);
		Cvar_SetValueQuick (&vid_refreshrate, old_refreshrate);
		Cvar_SetValueQuick (&vid_bpp, old_bpp);
		Cvar_SetQuick (&vid_fullscreen, old_fullscreen ? "1" : "0");
		VID_Restart ();
	}
}

/*
================
VID_Unlock -- johnfitz
================
*/
static void VID_Unlock (void)
{
	vid_locked = false;
	VID_SyncCvars();
}

/*
================
VID_Lock -- ericw

Subsequent changes to vid_* mode settings, and vid_restart commands, will
be ignored until the "vid_unlock" command is run.

Used when changing gamedirs so the current settings override what was saved
in the config.cfg.
================
*/
void VID_Lock (void)
{
	vid_locked = true;
}

//==============================================================================
//
//	OPENGL STUFF
//
//==============================================================================

/*
===============
GL_Info_f -- johnfitz
===============
*/
static void GL_Info_f (void)
{
	int i;
	Con_SafePrintf ("GL_VENDOR:     %s\n", gl_vendor);
	Con_SafePrintf ("GL_RENDERER:   %s\n", gl_renderer);
	Con_SafePrintf ("GL_VERSION:    %s\n", gl_version);

	Con_SafePrintf ("GL_EXTENSIONS: %d\n", gl_num_extensions);
	for (i = 0; i < gl_num_extensions; i++)
		Con_Printf("%3d. %s\n", i + 1, GL_GetStringiFunc (GL_EXTENSIONS, i));
}

/*
===============
GL_FindExtension
===============
*/
static qboolean GL_FindExtension (const char *name)
{
	int i;
	for (i = 0; i < gl_num_extensions; i++)
	{
		if (0 == strcmp(name, (const char*) GL_GetStringiFunc (GL_EXTENSIONS, i)))
		{
			if (Q_strncmp (name, "GL_", 3) == 0)
				name += 3;
			Con_SafePrintf ("FOUND: %s\n", name);
			return true;
		}
	}
	return false;
}

/*
===============
GL_FindRequiredExtension

Returns boolean instead of void to make it easier to replace with GL_FindExtension
if the functionality becomes optional
===============
*/
static qboolean GL_FindRequiredExtension (const char *name)
{
	if (!GL_FindExtension (name))
		Sys_Error ("Required extension %s not found.", name);
	return true;
}

/*
=============
GL_BeginGroup
=============
*/
static qboolean gldebug = false;
void GL_BeginGroup (const char *name)
{
	if (gldebug)
		GL_PushDebugGroupFunc (GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
}

/*
=============
GL_EndGroup
=============
*/
void GL_EndGroup (void)
{
	if (gldebug)
		GL_PopDebugGroupFunc ();
}

/*
===============
GL_DebugCallback
===============
*/
static void APIENTRY GL_DebugCallback (GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam)
{
	const char *str_source = "";
	const char *str_type = "";
	const char *str_severity = "";

	switch (source)
	{
		case GL_DEBUG_SOURCE_API:				str_source = "api"; break;
		case GL_DEBUG_SOURCE_WINDOW_SYSTEM:		str_source = "window system"; break;
		case GL_DEBUG_SOURCE_SHADER_COMPILER:	str_source = "shader compiler"; break;
		case GL_DEBUG_SOURCE_THIRD_PARTY:		str_source = "third party"; break;
		case GL_DEBUG_SOURCE_APPLICATION:		str_source = "application"; break;
		case GL_DEBUG_SOURCE_OTHER:				str_source = "other"; break;
		default:
			break;
	}

	switch (type)
	{
		case GL_DEBUG_TYPE_PUSH_GROUP:
		case GL_DEBUG_TYPE_POP_GROUP:
			return;
		case GL_DEBUG_TYPE_ERROR:				str_type = "error "; break;
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:	str_type = "deprecated "; break;
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:	str_type = "undefined "; break;
		case GL_DEBUG_TYPE_PORTABILITY:			str_type = "portability "; break;
		case GL_DEBUG_TYPE_PERFORMANCE:			str_type = "performance "; break;
		case GL_DEBUG_TYPE_MARKER:				str_type = "marker "; break;
		case GL_DEBUG_TYPE_OTHER:				str_type = ""; break;
		default:
			break;
	}

	switch (severity)
	{
		case GL_DEBUG_SEVERITY_NOTIFICATION:	str_severity = "info"; break;
		case GL_DEBUG_SEVERITY_LOW:				str_severity = "low"; break;
		case GL_DEBUG_SEVERITY_MEDIUM:			str_severity = "med"; break;
		case GL_DEBUG_SEVERITY_HIGH:			str_severity = "high"; break;
		default:
			break;
	}

	Con_SafePrintf ("\x02GL %s %s[#%u/%s]: ", str_source, str_type, id, str_severity);
	Con_SafePrintf ("%s\n", message);
}

/*
===============
GL_InitFunctions
===============
*/
qboolean GL_InitFunctions (const glfunc_t *funcs, qboolean required)
{
	qboolean ret = true;

	while (funcs->name)
	{
		if ((*funcs->ptr = SDL_GL_GetProcAddress (funcs->name)) == NULL)
		{
			if (required)
			{
				Sys_Error ("OpenGL function %s not found\n", funcs->name);
			}
			else
			{
				Con_Warning ("OpenGL function %s not found\n", funcs->name);
				ret = false;
			}
		}
		funcs++;
	}

	return ret;
}

/*
===============
GL_CheckExtensions
===============
*/
static void GL_CheckExtensions (void)
{
	int swap_control;

	GL_InitFunctions (gl_core_functions, true);

#ifdef NDEBUG
	if (COM_CheckParm("-gldebug"))
#endif
	{
		gldebug = true;
		GL_DebugMessageCallbackFunc (&GL_DebugCallback, NULL);
		glEnable (GL_DEBUG_OUTPUT);
		glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
	}

	// swap control
	//
	if (!gl_swap_control)
	{
#if defined(USE_SDL2)
		Con_Warning ("vertical sync not supported (SDL_GL_SetSwapInterval failed)\n");
#else
		Con_Warning ("vertical sync not supported (SDL_GL_SetAttribute failed)\n");
#endif
	}
#if defined(USE_SDL2)
	else if ((swap_control = SDL_GL_GetSwapInterval()) == -1)
#else
	else if (SDL_GL_GetAttribute(SDL_GL_SWAP_CONTROL, &swap_control) == -1)
#endif
	{
		gl_swap_control = false;
#if defined(USE_SDL2)
		Con_Warning ("vertical sync not supported (SDL_GL_GetSwapInterval failed)\n");
#else
		Con_Warning ("vertical sync not supported (SDL_GL_GetAttribute failed)\n");
#endif
	}
	else if ((vid_vsync.value && swap_control != 1) || (!vid_vsync.value && swap_control != 0))
	{
		gl_swap_control = false;
		Con_Warning ("vertical sync not supported (swap_control doesn't match vid_vsync)\n");
	}
	else
	{
#if defined(USE_SDL2)
		Con_SafePrintf("FOUND: SDL_GL_SetSwapInterval\n");
#else
		Con_SafePrintf("FOUND: SDL_GL_SWAP_CONTROL\n");
#endif
	}

	// anisotropic filtering
	//
	if (GL_FindExtension ("GL_EXT_texture_filter_anisotropic"))
	{
		float test1,test2;
		GLuint tex;

		// test to make sure we really have control over it
		// 1.0 and 2.0 should always be legal values
		glGenTextures(1, &tex);
		glBindTexture (GL_TEXTURE_2D, tex);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.0f);
		glGetTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &test1);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 2.0f);
		glGetTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &test2);
		glDeleteTextures(1, &tex);

		if (test1 == 1 && test2 == 2)
		{
			gl_anisotropy_able = true;
		}
		else
		{
			Con_Warning ("anisotropic filtering locked by driver. Current driver setting is %f\n", test1);
		}

		//get max value either way, so the menu and stuff know it
		glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl_max_anisotropy);
		if (gl_max_anisotropy < 2)
		{
			gl_anisotropy_able = false;
			gl_max_anisotropy = 1;
			Con_Warning ("anisotropic filtering broken: disabled\n");
		}
	}
	else
	{
		gl_max_anisotropy = 1;
		Con_Warning ("texture_filter_anisotropic not supported\n");
	}

	gl_bindless_able =
		GL_FindRequiredExtension ("GL_ARB_shader_draw_parameters") &&
		GL_FindRequiredExtension ("GL_ARB_bindless_texture") &&
		GL_InitFunctions (gl_arb_bindless_texture_functions, true)
	;

	gl_clipcontrol_able =
		GL_FindExtension ("GL_ARB_clip_control") &&
		GL_InitFunctions (gl_arb_clip_control_functions, false)
	;
}

/*
=============
GL_SetStateEx
=============
*/
static void GL_SetStateEx (unsigned mask, unsigned force)
{
	unsigned diff = (mask ^ glstate) | force;

	if (diff & GLS_MASK_BLEND)
	{
		switch (mask & GLS_MASK_BLEND)
		{
			default:
			case GLS_BLEND_OPAQUE:
				glBlendFunc(GL_ONE, GL_ZERO);
				break;
			case GLS_BLEND_ALPHA:
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				break;
			case GLS_BLEND_ADD:
				glBlendFunc(GL_ONE, GL_ONE);
				break;
		}
	}

	if (diff & GLS_MASK_CULL)
	{
		unsigned cull = mask & GLS_MASK_CULL;
		qboolean force_cull = (force & GLS_MASK_CULL) != 0;
		if (cull == GLS_CULL_NONE)
		{
			glDisable(GL_CULL_FACE);
		}
		else
		{
			if ((glstate & GLS_MASK_CULL) == GLS_CULL_NONE || (force & GLS_MASK_CULL) != 0)
				glEnable(GL_CULL_FACE);
			if (cull == GLS_CULL_FRONT)
				glCullFace(GL_FRONT);
			else
				glCullFace(GL_BACK);
		}
	}

	if (diff & GLS_NO_ZTEST)
	{
		if (mask & GLS_NO_ZTEST)
			glDisable(GL_DEPTH_TEST);
		else
			glEnable(GL_DEPTH_TEST);
	}

	if (diff & GLS_NO_ZWRITE)
		glDepthMask((mask & GLS_NO_ZWRITE) == 0);

	if (diff & GLS_MASK_ATTRIBS)
	{
		unsigned attribs = mask & GLS_MASK_ATTRIBS;
		int i;
		for (i = 0; i < GLS_MAX_ATTRIBS; i++)
		{
			int bit = GLS_ATTRIB0 << i;
			if (!(diff & bit))
				continue;
			if (mask & bit)
				GL_EnableVertexAttribArrayFunc(i);
			else
				GL_DisableVertexAttribArrayFunc(i);
		}
	}

	glstate = mask;
}

/*
=============
GL_SetState
=============
*/
void GL_SetState (unsigned mask)
{
	GL_SetStateEx (mask, 0);
}

/*
=============
GL_ResetState
=============
*/
void GL_ResetState (void)
{
	GL_SetStateEx (GLS_DEFAULT_STATE, (unsigned)(-1));
}

/*
===============
GL_SetupState -- johnfitz

does all the stuff from GL_Init that needs to be done every time a new GL render context is created
===============
*/
static void GL_SetupState (void)
{
	glClearColor (0.f, 0.f, 0.f, 0.f);
	if (gl_clipcontrol_able)
	{
		GL_ClipControlFunc (GL_LOWER_LEFT, GL_ZERO_TO_ONE);
		glClearDepth (0.f);
		glDepthFunc (GL_GEQUAL);
	}
	else
	{
		glClearDepth (1.f);
		glDepthFunc (GL_LEQUAL);
	}
	glFrontFace (GL_CW); //johnfitz -- glquake used CCW with backwards culling -- let's do it right
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_DepthRange (ZRANGE_FULL); //johnfitz -- moved here becuase gl_ztrick is gone.
	glEnable (GL_BLEND);

	GL_ResetState ();
}

/*
===============
GL_Init
===============
*/
static void GL_Init (void)
{
	gl_vendor = (const char *) glGetString (GL_VENDOR);
	gl_renderer = (const char *) glGetString (GL_RENDERER);
	gl_version = (const char *) glGetString (GL_VERSION);
	glGetIntegerv (GL_NUM_EXTENSIONS, &gl_num_extensions);

	Con_SafePrintf ("GL_VENDOR: %s\n", gl_vendor);
	Con_SafePrintf ("GL_RENDERER: %s\n", gl_renderer);
	Con_SafePrintf ("GL_VERSION: %s\n", gl_version);
	
	if (gl_version == NULL || sscanf(gl_version, "%d.%d", &gl_version_major, &gl_version_minor) < 2)
	{
		gl_version_major = 0;
		gl_version_minor = 0;
	}
	gl_version_number = MAKE_GL_VERSION(gl_version_major, gl_version_minor);
	if (gl_version_number < MIN_GL_VERSION)
		Sys_Error("OpenGL " MIN_GL_VERSION_STR " required, found %d.%d\n", gl_version_major, gl_version_minor);

	GL_CheckExtensions ();

	GL_GenVertexArraysFunc (1, &globalvao);
	GL_BindVertexArrayFunc (globalvao);

	glGetIntegerv (GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssbo_align);
	--ssbo_align;

#ifdef __APPLE__
	// ericw -- enable multi-threaded OpenGL, gives a decent FPS boost.
	// https://developer.apple.com/library/mac/technotes/tn2085/
	if (host_parms->numcpus > 1 &&
	    kCGLNoError != CGLEnable(CGLGetCurrentContext(), kCGLCEMPEngine))
	{
		Con_Warning ("Couldn't enable multi-threaded OpenGL");
	}
#endif

	//johnfitz -- intel video workarounds from Baker
	if (!strcmp(gl_vendor, "Intel"))
	{
		Con_Printf ("Intel Display Adapter detected, enabling gl_clear\n");
		Cbuf_AddText ("gl_clear 1");
	}
	//johnfitz

	GL_CreateShaders ();
	GL_CreateFrameBuffers ();
	GLWorld_CreateResources ();
	GLLight_CreateResources ();

	GL_ClearBufferBindings ();
	GL_InitDynamicBuffers ();
}

/*
=================
GL_BeginRendering -- sets values of glx, gly, glwidth, glheight
=================
*/
void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	qboolean postprocess = vid_gamma.value != 1.f || vid_contrast.value != 1.f;

	*x = *y = 0;
	*width = vid.width;
	*height = vid.height;

	GL_DynamicBuffersBeginFrame ();

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, postprocess ? framebufs.composite.fbo : 0);
}

/*
=================
GL_EndRendering
=================
*/
void GL_EndRendering (void)
{
	GLSLGamma_GammaCorrect ();
	GL_DynamicBuffersEndFrame ();

	if (!scr_skipupdate)
	{
#if defined(USE_SDL2)
		SDL_GL_SwapWindow(draw_context);
#else
		SDL_GL_SwapBuffers();
#endif
	}
}


void	VID_Shutdown (void)
{
	if (vid_initialized)
	{
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
		draw_context = NULL;
#if defined(USE_SDL2)
		gl_context = NULL;
#endif
		PL_VID_Shutdown();
	}
}

/*
===================================================================

MAIN WINDOW

===================================================================
*/

/*
================
ClearAllStates
================
*/
static void ClearAllStates (void)
{
	Key_ClearStates ();
	IN_ClearStates ();
}


//==========================================================================
//
//  COMMANDS
//
//==========================================================================

/*
=================
VID_DescribeCurrentMode_f
=================
*/
static void VID_DescribeCurrentMode_f (void)
{
	if (draw_context)
		Con_Printf("%dx%dx%d %dHz %s\n",
			VID_GetCurrentWidth(),
			VID_GetCurrentHeight(),
			VID_GetCurrentBPP(),
			VID_GetCurrentRefreshRate(),
			VID_GetFullscreen() ? "fullscreen" : "windowed");
}

/*
=================
VID_DescribeModes_f -- johnfitz -- changed formatting, and added refresh rates after each mode.
=================
*/
static void VID_DescribeModes_f (void)
{
	int	i;
	int	lastwidth, lastheight, lastbpp, count;

	lastwidth = lastheight = lastbpp = count = 0;

	for (i = 0; i < nummodes; i++)
	{
		if (lastwidth != modelist[i].width || lastheight != modelist[i].height || lastbpp != modelist[i].bpp)
		{
			if (count > 0)
				Con_SafePrintf ("\n");
			Con_SafePrintf ("   %4i x %4i x %i : %i", modelist[i].width, modelist[i].height, modelist[i].bpp, modelist[i].refreshrate);
			lastwidth = modelist[i].width;
			lastheight = modelist[i].height;
			lastbpp = modelist[i].bpp;
			count++;
		}
	}
	Con_Printf ("\n%i modes\n", count);
}

//==========================================================================
//
//  INIT
//
//==========================================================================

/*
=================
VID_InitModelist
=================
*/
static void VID_InitModelist (void)
{
#if defined(USE_SDL2)
	const int sdlmodes = SDL_GetNumDisplayModes(0);
	int i;

	nummodes = 0;
	for (i = 0; i < sdlmodes; i++)
	{
		SDL_DisplayMode mode;

		if (nummodes >= MAX_MODE_LIST)
			break;
		if (SDL_GetDisplayMode(0, i, &mode) == 0)
		{
			modelist[nummodes].width = mode.w;
			modelist[nummodes].height = mode.h;
			modelist[nummodes].bpp = SDL_BITSPERPIXEL(mode.format);
			modelist[nummodes].refreshrate = mode.refresh_rate;
			nummodes++;
		}
	}
#else /* !defined(USE_SDL2) */
	SDL_PixelFormat	format;
	SDL_Rect	**modes;
	Uint32		flags;
	int		i, j, k, originalnummodes, existingmode;
	int		bpps[] = {16, 24, 32}; // enumerate >8 bpp modes

	originalnummodes = nummodes = 0;
	memset(&format, 0, sizeof(format));

	// enumerate fullscreen modes
	flags = DEFAULT_SDL_FLAGS | SDL_FULLSCREEN;
	for (i = 0; i < (int)(sizeof(bpps)/sizeof(bpps[0])); i++)
	{
		if (nummodes >= MAX_MODE_LIST)
			break;

		format.BitsPerPixel = bpps[i];
		modes = SDL_ListModes(&format, flags);

		if (modes == (SDL_Rect **)0 || modes == (SDL_Rect **)-1)
			continue;

		for (j = 0; modes[j]; j++)
		{
			if (modes[j]->w > MAXWIDTH || modes[j]->h > MAXHEIGHT || nummodes >= MAX_MODE_LIST)
				continue;

			modelist[nummodes].width = modes[j]->w;
			modelist[nummodes].height = modes[j]->h;
			modelist[nummodes].bpp = bpps[i];
			modelist[nummodes].refreshrate = DEFAULT_REFRESHRATE;

			for (k=originalnummodes, existingmode = 0 ; k < nummodes ; k++)
			{
				if ((modelist[nummodes].width == modelist[k].width)   &&
				    (modelist[nummodes].height == modelist[k].height) &&
				    (modelist[nummodes].bpp == modelist[k].bpp))
				{
					existingmode = 1;
					break;
				}
			}

			if (!existingmode)
			{
				nummodes++;
			}
		}
	}

	if (nummodes == originalnummodes)
		Con_SafePrintf ("No fullscreen DIB modes found\n");
#endif /* !defined(USE_SDL2) */
}

/*
===================
VID_Init
===================
*/
void	VID_Init (void)
{
	static char vid_center[] = "SDL_VIDEO_CENTERED=center";
	int		p, width, height, refreshrate, bpp;
	int		display_width, display_height, display_refreshrate, display_bpp;
	qboolean	fullscreen;
	const char	*read_vars[] = { "vid_fullscreen",
					 "vid_width",
					 "vid_height",
					 "vid_refreshrate",
					 "vid_bpp",
					 "vid_vsync",
					 "vid_fsaa",
					 "vid_desktopfullscreen",
					 "vid_borderless"};
#define num_readvars	( sizeof(read_vars)/sizeof(read_vars[0]) )

	Cvar_RegisterVariable (&vid_fullscreen); //johnfitz
	Cvar_RegisterVariable (&vid_width); //johnfitz
	Cvar_RegisterVariable (&vid_height); //johnfitz
	Cvar_RegisterVariable (&vid_refreshrate); //johnfitz
	Cvar_RegisterVariable (&vid_bpp); //johnfitz
	Cvar_RegisterVariable (&vid_vsync); //johnfitz
	Cvar_RegisterVariable (&vid_fsaa); //QuakeSpasm
	Cvar_RegisterVariable (&vid_desktopfullscreen); //QuakeSpasm
	Cvar_RegisterVariable (&vid_borderless); //QuakeSpasm
	Cvar_SetCallback (&vid_fullscreen, VID_Changed_f);
	Cvar_SetCallback (&vid_width, VID_Changed_f);
	Cvar_SetCallback (&vid_height, VID_Changed_f);
	Cvar_SetCallback (&vid_refreshrate, VID_Changed_f);
	Cvar_SetCallback (&vid_bpp, VID_Changed_f);
	Cvar_SetCallback (&vid_vsync, VID_Changed_f);
	Cvar_SetCallback (&vid_fsaa, VID_Changed_f);
	Cvar_SetCallback (&vid_desktopfullscreen, VID_Changed_f);
	Cvar_SetCallback (&vid_borderless, VID_Changed_f);
	
	Cmd_AddCommand ("vid_unlock", VID_Unlock); //johnfitz
	Cmd_AddCommand ("vid_restart", VID_Restart); //johnfitz
	Cmd_AddCommand ("vid_test", VID_Test); //johnfitz
	Cmd_AddCommand ("vid_describecurrentmode", VID_DescribeCurrentMode_f);
	Cmd_AddCommand ("vid_describemodes", VID_DescribeModes_f);

	putenv (vid_center);	/* SDL_putenv is problematic in versions <= 1.2.9 */

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
		Sys_Error("Couldn't init SDL video: %s", SDL_GetError());

#if defined(USE_SDL2)
	{
		SDL_DisplayMode mode;
		if (SDL_GetDesktopDisplayMode(0, &mode) != 0)
			Sys_Error("Could not get desktop display mode");

		display_width = mode.w;
		display_height = mode.h;
		display_refreshrate = mode.refresh_rate;
		display_bpp = SDL_BITSPERPIXEL(mode.format);
	}
#else
	{
		const SDL_VideoInfo *info = SDL_GetVideoInfo();
		display_width = info->current_w;
		display_height = info->current_h;
		display_refreshrate = DEFAULT_REFRESHRATE;
		display_bpp = info->vfmt->BitsPerPixel;
	}
#endif

	Cvar_SetValueQuick (&vid_bpp, (float)display_bpp);

	if (CFG_OpenConfig("config.cfg") == 0)
	{
		CFG_ReadCvars(read_vars, num_readvars);
		CFG_CloseConfig();
	}
	CFG_ReadCvarOverrides(read_vars, num_readvars);

	VID_InitModelist();

	width = (int)vid_width.value;
	height = (int)vid_height.value;
	refreshrate = (int)vid_refreshrate.value;
	bpp = (int)vid_bpp.value;
	fullscreen = (int)vid_fullscreen.value;
	fsaa = (int)vid_fsaa.value;

	if (COM_CheckParm("-current"))
	{
		width = display_width;
		height = display_height;
		refreshrate = display_refreshrate;
		bpp = display_bpp;
		fullscreen = true;
	}
	else
	{
		p = COM_CheckParm("-width");
		if (p && p < com_argc-1)
		{
			width = Q_atoi(com_argv[p+1]);

			if(!COM_CheckParm("-height"))
				height = width * 3 / 4;
		}

		p = COM_CheckParm("-height");
		if (p && p < com_argc-1)
		{
			height = Q_atoi(com_argv[p+1]);

			if(!COM_CheckParm("-width"))
				width = height * 4 / 3;
		}

		p = COM_CheckParm("-refreshrate");
		if (p && p < com_argc-1)
			refreshrate = Q_atoi(com_argv[p+1]);
		
		p = COM_CheckParm("-bpp");
		if (p && p < com_argc-1)
			bpp = Q_atoi(com_argv[p+1]);

		if (COM_CheckParm("-window") || COM_CheckParm("-w"))
			fullscreen = false;
		else if (COM_CheckParm("-fullscreen") || COM_CheckParm("-f"))
			fullscreen = true;
	}

	p = COM_CheckParm ("-fsaa");
	if (p && p < com_argc-1)
		fsaa = atoi(com_argv[p+1]);

	if (!VID_ValidMode(width, height, refreshrate, bpp, fullscreen))
	{
		width = (int)vid_width.value;
		height = (int)vid_height.value;
		refreshrate = (int)vid_refreshrate.value;
		bpp = (int)vid_bpp.value;
		fullscreen = (int)vid_fullscreen.value;
	}

	if (!VID_ValidMode(width, height, refreshrate, bpp, fullscreen))
	{
		width = 640;
		height = 480;
		refreshrate = display_refreshrate;
		bpp = display_bpp;
		fullscreen = false;
	}

	vid_initialized = true;

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

#if !defined(USE_SDL2)
	// set window icon
	PL_SetWindowIcon();
#endif

	VID_SetMode (width, height, refreshrate, bpp, fullscreen);

#if defined(USE_SDL2)
	// set window icon
	PL_SetWindowIcon();
#endif

	GL_Init ();
	GL_SetupState ();
	Cmd_AddCommand ("gl_info", GL_Info_f); //johnfitz

	//johnfitz -- removed code creating "glquake" subdirectory

	vid_menucmdfn = VID_Menu_f; //johnfitz
	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

	VID_Gamma_Init(); //johnfitz
	VID_Menu_Init(); //johnfitz

	//QuakeSpasm: current vid settings should override config file settings.
	//so we have to lock the vid mode from now until after all config files are read.
	vid_locked = true;
}

// new proc by S.A., called by alt-return key binding.
void	VID_Toggle (void)
{
	// disabling the fast path completely because SDL_SetWindowFullscreen was changing
	// the window size on SDL2/WinXP and we weren't set up to handle it. --ericw
	//
	// TODO: Clear out the dead code, reinstate the fast path using SDL_SetWindowFullscreen
	// inside VID_SetMode, check window size to fix WinXP issue. This will
	// keep all the mode changing code in one place.
	static qboolean vid_toggle_works = false;
	qboolean toggleWorked;
#if defined(USE_SDL2)
	Uint32 flags = 0;
#endif

	S_ClearBuffer ();

	if (!vid_toggle_works)
		goto vrestart;

#if defined(USE_SDL2)
	if (!VID_GetFullscreen()) {
		flags = vid_desktopfullscreen.value ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
	}

	toggleWorked = SDL_SetWindowFullscreen(draw_context, flags) == 0;
#else
	toggleWorked = SDL_WM_ToggleFullScreen(draw_context) == 1;
#endif

	if (toggleWorked)
	{
		Sbar_Changed ();	// Sbar seems to need refreshing

		modestate = VID_GetFullscreen() ? MS_FULLSCREEN : MS_WINDOWED;

		VID_SyncCvars();

		// update mouse grab
		if (key_dest == key_console || key_dest == key_menu)
		{
			if (modestate == MS_WINDOWED)
				IN_Deactivate(true);
			else if (modestate == MS_FULLSCREEN)
				IN_Activate();
		}
	}
	else
	{
		vid_toggle_works = false;
		Con_DPrintf ("SDL_WM_ToggleFullScreen failed, attempting VID_Restart\n");
	vrestart:
		Cvar_SetQuick (&vid_fullscreen, VID_GetFullscreen() ? "0" : "1");
		Cbuf_AddText ("vid_restart\n");
	}
}

/*
================
VID_SyncCvars -- johnfitz -- set vid cvars to match current video mode
================
*/
void VID_SyncCvars (void)
{
	if (draw_context)
	{
		if (!VID_GetDesktopFullscreen())
		{
			Cvar_SetValueQuick (&vid_width, VID_GetCurrentWidth());
			Cvar_SetValueQuick (&vid_height, VID_GetCurrentHeight());
		}
		Cvar_SetValueQuick (&vid_refreshrate, VID_GetCurrentRefreshRate());
		Cvar_SetValueQuick (&vid_bpp, VID_GetCurrentBPP());
		Cvar_SetQuick (&vid_fullscreen, VID_GetFullscreen() ? "1" : "0");
		// don't sync vid_desktopfullscreen, it's a user preference that
		// should persist even if we are in windowed mode.
		Cvar_SetQuick (&vid_vsync, VID_GetVSync() ? "1" : "0");
	}

	vid_changed = false;
}

//==========================================================================
//
//  NEW VIDEO MENU -- johnfitz
//
//==========================================================================

enum {
	VID_OPT_MODE,
	VID_OPT_BPP,
	VID_OPT_REFRESHRATE,
	VID_OPT_FULLSCREEN,
	VID_OPT_VSYNC,
	VID_OPT_TEST,
	VID_OPT_APPLY,
	VIDEO_OPTIONS_ITEMS
};

static int	video_options_cursor = 0;

typedef struct {
	int width,height;
} vid_menu_mode;

//TODO: replace these fixed-length arrays with hunk_allocated buffers
static vid_menu_mode vid_menu_modes[MAX_MODE_LIST];
static int vid_menu_nummodes = 0;

static int vid_menu_bpps[MAX_BPPS_LIST];
static int vid_menu_numbpps = 0;

static int vid_menu_rates[MAX_RATES_LIST];
static int vid_menu_numrates=0;

/*
================
VID_Menu_Init
================
*/
static void VID_Menu_Init (void)
{
	int i, j, h, w;

	for (i = 0; i < nummodes; i++)
	{
		w = modelist[i].width;
		h = modelist[i].height;

		for (j = 0; j < vid_menu_nummodes; j++)
		{
			if (vid_menu_modes[j].width == w &&
				vid_menu_modes[j].height == h)
				break;
		}

		if (j == vid_menu_nummodes)
		{
			vid_menu_modes[j].width = w;
			vid_menu_modes[j].height = h;
			vid_menu_nummodes++;
		}
	}
}

/*
================
VID_Menu_RebuildBppList

regenerates bpp list based on current vid_width and vid_height
================
*/
static void VID_Menu_RebuildBppList (void)
{
	int i, j, b;

	vid_menu_numbpps = 0;

	for (i = 0; i < nummodes; i++)
	{
		if (vid_menu_numbpps >= MAX_BPPS_LIST)
			break;

		//bpp list is limited to bpps available with current width/height
		if (modelist[i].width != vid_width.value ||
			modelist[i].height != vid_height.value)
			continue;

		b = modelist[i].bpp;

		for (j = 0; j < vid_menu_numbpps; j++)
		{
			if (vid_menu_bpps[j] == b)
				break;
		}

		if (j == vid_menu_numbpps)
		{
			vid_menu_bpps[j] = b;
			vid_menu_numbpps++;
		}
	}

	//if there are no valid fullscreen bpps for this width/height, just pick one
	if (vid_menu_numbpps == 0)
	{
		Cvar_SetValueQuick (&vid_bpp, (float)modelist[0].bpp);
		return;
	}

	//if vid_bpp is not in the new list, change vid_bpp
	for (i = 0; i < vid_menu_numbpps; i++)
		if (vid_menu_bpps[i] == (int)(vid_bpp.value))
			break;

	if (i == vid_menu_numbpps)
		Cvar_SetValueQuick (&vid_bpp, (float)vid_menu_bpps[0]);
}

/*
================
VID_Menu_RebuildRateList

regenerates rate list based on current vid_width, vid_height and vid_bpp
================
*/
static void VID_Menu_RebuildRateList (void)
{
	int i,j,r;
	
	vid_menu_numrates=0;
	
	for (i=0;i<nummodes;i++)
	{
		//rate list is limited to rates available with current width/height/bpp
		if (modelist[i].width != vid_width.value ||
		    modelist[i].height != vid_height.value ||
		    modelist[i].bpp != vid_bpp.value)
			continue;
		
		r = modelist[i].refreshrate;
		
		for (j=0;j<vid_menu_numrates;j++)
		{
			if (vid_menu_rates[j] == r)
				break;
		}
		
		if (j==vid_menu_numrates)
		{
			vid_menu_rates[j] = r;
			vid_menu_numrates++;
		}
	}
	
	//if there are no valid fullscreen refreshrates for this width/height, just pick one
	if (vid_menu_numrates == 0)
	{
		Cvar_SetValue ("vid_refreshrate",(float)modelist[0].refreshrate);
		return;
	}
	
	//if vid_refreshrate is not in the new list, change vid_refreshrate
	for (i=0;i<vid_menu_numrates;i++)
		if (vid_menu_rates[i] == (int)(vid_refreshrate.value))
			break;
	
	if (i==vid_menu_numrates)
		Cvar_SetValue ("vid_refreshrate",(float)vid_menu_rates[0]);
}

/*
================
VID_Menu_ChooseNextMode

chooses next resolution in order, then updates vid_width and
vid_height cvars, then updates bpp and refreshrate lists
================
*/
static void VID_Menu_ChooseNextMode (int dir)
{
	int i;

	if (vid_menu_nummodes)
	{
		for (i = 0; i < vid_menu_nummodes; i++)
		{
			if (vid_menu_modes[i].width == vid_width.value &&
				vid_menu_modes[i].height == vid_height.value)
				break;
		}

		if (i == vid_menu_nummodes) //can't find it in list, so it must be a custom windowed res
		{
			i = 0;
		}
		else
		{
			i += dir;
			if (i >= vid_menu_nummodes)
				i = 0;
			else if (i < 0)
				i = vid_menu_nummodes-1;
		}

		Cvar_SetValueQuick (&vid_width, (float)vid_menu_modes[i].width);
		Cvar_SetValueQuick (&vid_height, (float)vid_menu_modes[i].height);
		VID_Menu_RebuildBppList ();
		VID_Menu_RebuildRateList ();
	}
}

/*
================
VID_Menu_ChooseNextBpp

chooses next bpp in order, then updates vid_bpp cvar
================
*/
static void VID_Menu_ChooseNextBpp (int dir)
{
	int i;

	if (vid_menu_numbpps)
	{
		for (i = 0; i < vid_menu_numbpps; i++)
		{
			if (vid_menu_bpps[i] == vid_bpp.value)
				break;
		}

		if (i == vid_menu_numbpps) //can't find it in list
		{
			i = 0;
		}
		else
		{
			i += dir;
			if (i >= vid_menu_numbpps)
				i = 0;
			else if (i < 0)
				i = vid_menu_numbpps-1;
		}

		Cvar_SetValueQuick (&vid_bpp, (float)vid_menu_bpps[i]);
	}
}

/*
================
VID_Menu_ChooseNextRate

chooses next refresh rate in order, then updates vid_refreshrate cvar
================
*/
static void VID_Menu_ChooseNextRate (int dir)
{
	int i;
	
	for (i=0;i<vid_menu_numrates;i++)
	{
		if (vid_menu_rates[i] == vid_refreshrate.value)
			break;
	}
	
	if (i==vid_menu_numrates) //can't find it in list
	{
		i = 0;
	}
	else
	{
		i+=dir;
		if (i>=vid_menu_numrates)
			i = 0;
		else if (i<0)
			i = vid_menu_numrates-1;
	}
	
	Cvar_SetValue ("vid_refreshrate",(float)vid_menu_rates[i]);
}

/*
================
VID_MenuKey
================
*/
static void VID_MenuKey (int key)
{
	switch (key)
	{
	case K_ESCAPE:
	case K_BBUTTON:
		VID_SyncCvars (); //sync cvars before leaving menu. FIXME: there are other ways to leave menu
		S_LocalSound ("misc/menu1.wav");
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		video_options_cursor--;
		if (video_options_cursor < 0)
			video_options_cursor = VIDEO_OPTIONS_ITEMS-1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		video_options_cursor++;
		if (video_options_cursor >= VIDEO_OPTIONS_ITEMS)
			video_options_cursor = 0;
		break;

	case K_LEFTARROW:
		S_LocalSound ("misc/menu3.wav");
		switch (video_options_cursor)
		{
		case VID_OPT_MODE:
			VID_Menu_ChooseNextMode (1);
			break;
		case VID_OPT_BPP:
			VID_Menu_ChooseNextBpp (1);
			break;
		case VID_OPT_REFRESHRATE:
			VID_Menu_ChooseNextRate (1);
			break;
		case VID_OPT_FULLSCREEN:
			Cbuf_AddText ("toggle vid_fullscreen\n");
			break;
		case VID_OPT_VSYNC:
			Cbuf_AddText ("toggle vid_vsync\n"); // kristian
			break;
		default:
			break;
		}
		break;

	case K_RIGHTARROW:
		S_LocalSound ("misc/menu3.wav");
		switch (video_options_cursor)
		{
		case VID_OPT_MODE:
			VID_Menu_ChooseNextMode (-1);
			break;
		case VID_OPT_BPP:
			VID_Menu_ChooseNextBpp (-1);
			break;
		case VID_OPT_REFRESHRATE:
			VID_Menu_ChooseNextRate (-1);
			break;
		case VID_OPT_FULLSCREEN:
			Cbuf_AddText ("toggle vid_fullscreen\n");
			break;
		case VID_OPT_VSYNC:
			Cbuf_AddText ("toggle vid_vsync\n");
			break;
		default:
			break;
		}
		break;

	case K_ENTER:
	case K_KP_ENTER:
	case K_ABUTTON:
		m_entersound = true;
		switch (video_options_cursor)
		{
		case VID_OPT_MODE:
			VID_Menu_ChooseNextMode (1);
			break;
		case VID_OPT_BPP:
			VID_Menu_ChooseNextBpp (1);
			break;
		case VID_OPT_REFRESHRATE:
			VID_Menu_ChooseNextRate (1);
			break;
		case VID_OPT_FULLSCREEN:
			Cbuf_AddText ("toggle vid_fullscreen\n");
			break;
		case VID_OPT_VSYNC:
			Cbuf_AddText ("toggle vid_vsync\n");
			break;
		case VID_OPT_TEST:
			Cbuf_AddText ("vid_test\n");
			break;
		case VID_OPT_APPLY:
			Cbuf_AddText ("vid_restart\n");
			key_dest = key_game;
			m_state = m_none;
			IN_Activate();
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}
}

/*
================
VID_MenuDraw
================
*/
static void VID_MenuDraw (void)
{
	int i, y;
	qpic_t *p;
	const char *title;

	y = 4;

	// plaque
	p = Draw_CachePic ("gfx/qplaque.lmp");
	M_DrawTransPic (16, y, p);

	//p = Draw_CachePic ("gfx/vidmodes.lmp");
	p = Draw_CachePic ("gfx/p_option.lmp");
	M_DrawPic ( (320-p->width)/2, y, p);

	y += 28;

	// title
	title = "Video Options";
	M_PrintWhite ((320-8*strlen(title))/2, y, title);

	y += 16;

	// options
	for (i = 0; i < VIDEO_OPTIONS_ITEMS; i++)
	{
		switch (i)
		{
		case VID_OPT_MODE:
			M_Print (16, y, "        Video mode");
			M_Print (184, y, va("%ix%i", (int)vid_width.value, (int)vid_height.value));
			break;
		case VID_OPT_BPP:
			M_Print (16, y, "       Color depth");
			M_Print (184, y, va("%i", (int)vid_bpp.value));
			break;
		case VID_OPT_REFRESHRATE:
			M_Print (16, y, "      Refresh rate");
			M_Print (184, y, va("%i", (int)vid_refreshrate.value));
			break;
		case VID_OPT_FULLSCREEN:
			M_Print (16, y, "        Fullscreen");
			M_DrawCheckbox (184, y, (int)vid_fullscreen.value);
			break;
		case VID_OPT_VSYNC:
			M_Print (16, y, "     Vertical sync");
			if (gl_swap_control)
				M_DrawCheckbox (184, y, (int)vid_vsync.value);
			else
				M_Print (184, y, "N/A");
			break;
		case VID_OPT_TEST:
			y += 8; //separate the test and apply items
			M_Print (16, y, "      Test changes");
			break;
		case VID_OPT_APPLY:
			M_Print (16, y, "     Apply changes");
			break;
		}

		if (video_options_cursor == i)
			M_DrawCharacter (168, y, 12+((int)(realtime*4)&1));

		y += 8;
	}
}

/*
================
VID_Menu_f
================
*/
static void VID_Menu_f (void)
{
	IN_Deactivate(modestate == MS_WINDOWED);
	key_dest = key_menu;
	m_state = m_video;
	m_entersound = true;

	//set all the cvars to match the current mode when entering the menu
	VID_SyncCvars ();

	//set up bpp and rate lists based on current cvars
	VID_Menu_RebuildBppList ();
	VID_Menu_RebuildRateList ();
}

