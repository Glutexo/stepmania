#include "stdafx.h"
/*
-----------------------------------------------------------------------------
 Class: RageDisplay

 Desc: See header.

 Copyright (c) 2001-2002 by the person(s) listed below.  All rights reserved.
	Chris Danford
-----------------------------------------------------------------------------
*/

#include "RageDisplay.h"
#include "RageDisplayInternal.h"

#include "RageUtil.h"
#include "RageLog.h"
#include "RageTimer.h"
#include "RageException.h"
#include "RageTexture.h"
#include "RageTextureManager.h"
#include "RageMath.h"
#include "RageTypes.h"
#include "GameConstantsAndTypes.h"

RageDisplay*		DISPLAY	= NULL;

////////////
// Globals
////////////
SDL_Surface			*g_screen = NULL;		// this class is a singleton, so there can be only one
int					g_flags = 0;		/* SDL video flags */
GLenum				g_vertMode = GL_TRIANGLES;
RageTimer			g_LastCheckTimer;
int					g_iNumVerts;
int					g_iFPS, g_iVPF, g_iCFPS;

int					g_PerspectiveMode = 0;

int					g_CurrentHeight, g_CurrentWidth, g_CurrentBPP;
int					g_ModelMatrixCnt=0;
int RageDisplay::GetFPS() const { return g_iFPS; }
int RageDisplay::GetVPF() const { return g_iVPF; }
int RageDisplay::GetCumFPS() const { return g_iCFPS; }

static int			g_iFramesRenderedSinceLastCheck,
					g_iFramesRenderedSinceLastReset,
					g_iVertsRenderedSinceLastCheck,
					g_iNumChecksSinceLastReset;

PWSWAPINTERVALEXTPROC GLExt::wglSwapIntervalEXT;
PFNGLCOLORTABLEPROC GLExt::glColorTableEXT;
PFNGLCOLORTABLEPARAMETERIVPROC GLExt::glGetColorTableParameterivEXT;

/* We don't actually use normals (we don't tunr on lighting), there's just
 * no GL_T2F_C4F_V3F. */
const GLenum RageVertexFormat = GL_T2F_C4F_N3F_V3F;

void GetGLExtensions(set<string> &ext)
{
    const char *buf = (const char *)glGetString(GL_EXTENSIONS);

	vector<CString> lst;
	split(buf, " ", lst);

	for(unsigned i = 0; i < lst.size(); ++i)
		ext.insert(lst[i]);
}

RageDisplay::RageDisplay( bool windowed, int width, int height, int bpp, int rate, bool vsync )
{
	LOG->Trace( "RageDisplay::RageDisplay()" );
	m_oglspecs = new oglspecs_t;
	
	SDL_InitSubSystem(SDL_INIT_VIDEO);

	SetVideoMode( windowed, width, height, bpp, rate, vsync );

	SetupOpenGL();

	// Log driver details
	LOG->Info("OGL Vendor: %s", glGetString(GL_VENDOR));
	LOG->Info("OGL Renderer: %s", glGetString(GL_RENDERER));
	LOG->Info("OGL Version: %s", glGetString(GL_VERSION));
	LOG->Info("OGL Extensions: %s", glGetString(GL_EXTENSIONS));
	if( IsSoftwareRenderer() )
		LOG->Warn("This is a software renderer!");


	/* Log this, so if people complain that the radar looks bad on their
	 * system we can compare them: */
	glGetFloatv(GL_LINE_WIDTH_RANGE, m_oglspecs->line_range);
	LOG->Trace("Line width range: %f, %f", m_oglspecs->line_range[0], m_oglspecs->line_range[1]);
	glGetFloatv(GL_LINE_WIDTH_GRANULARITY, &m_oglspecs->line_granularity);
	LOG->Trace("Line width granularity: %f", m_oglspecs->line_granularity);
	glGetFloatv(GL_POINT_SIZE_RANGE, m_oglspecs->point_range);
	LOG->Trace("Point size range: %f-%f", m_oglspecs->point_range[0], m_oglspecs->point_range[1]);
	glGetFloatv(GL_POINT_SIZE_GRANULARITY, &m_oglspecs->point_granularity);
	LOG->Trace("Point size granularity: %f", m_oglspecs->point_granularity);
}

bool RageDisplay::IsSoftwareRenderer()
{
	return 
		( stricmp((const char*)glGetString(GL_VENDOR),"Microsoft Corporation")==0 ) &&
		( stricmp((const char*)glGetString(GL_RENDERER),"GDI Generic")==0 );
}

void RageDisplay::SetupOpenGL()
{
	/*
	 * Set up OpenGL for 2D rendering.
	 */
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	/*
	 * Set state variables
	 */
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

	/* Use <= for depth testing.  This lets us set all components of an actor to the
	 * same depth. */
	glDepthFunc(GL_LEQUAL);

	/* Line antialiasing is fast on most hardware, and saying "don't care"
	 * should turn it off if it isn't. */
	glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
	glHint(GL_POINT_SMOOTH_HINT, GL_DONT_CARE);

	/* Initialize the default ortho projection. */
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho(0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, SCREEN_NEAR, SCREEN_FAR );
	glMatrixMode( GL_MODELVIEW );
}

RageDisplay::~RageDisplay()
{
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	delete m_oglspecs;
}

bool RageDisplay::HasExtension(CString ext) const
{
	return m_oglspecs->glExts.find(ext) != m_oglspecs->glExts.end();
}

extern HWND g_hWndMain;

void RageDisplay::SetupExtensions()
{
	double fGLVersion = atof( (const char *) glGetString(GL_VERSION) );
	m_oglspecs->glVersion = int(roundf(fGLVersion * 10));
	LOG->Trace( "OpenGL version %.1f", m_oglspecs->glVersion / 10.);
	GetGLExtensions(m_oglspecs->glExts);

	/* Check for extensions: */
	m_oglspecs->EXT_texture_env_combine = HasExtension("GL_EXT_texture_env_combine");
	m_oglspecs->WGL_EXT_swap_control = HasExtension("WGL_EXT_swap_control");
	m_oglspecs->EXT_paletted_texture = HasExtension("GL_EXT_paletted_texture");

	/* Find extension functions. */
	wglSwapIntervalEXT = (PWSWAPINTERVALEXTPROC) SDL_GL_GetProcAddress("wglSwapIntervalEXT");
	glColorTableEXT = (PFNGLCOLORTABLEPROC) SDL_GL_GetProcAddress("glColorTableEXT");
	glGetColorTableParameterivEXT = (PFNGLCOLORTABLEPARAMETERIVPROC) SDL_GL_GetProcAddress("glGetColorTableParameterivEXT");

	/* Make sure we have all components for detected extensions. */
	if(m_oglspecs->WGL_EXT_swap_control)
		ASSERT(wglSwapIntervalEXT);
	if(m_oglspecs->EXT_paletted_texture)
	{
		ASSERT(glColorTableEXT);
		ASSERT(glGetColorTableParameterivEXT);
	}
}

/* Set the video mode.  In some cases, changing the video mode will reset
 * the rendering context; returns true if we need to reload textures. */
bool RageDisplay::SetVideoMode( bool windowed, int width, int height, int bpp, int rate, bool vsync )
{
//	LOG->Trace( "RageDisplay::SetVideoMode( %d, %d, %d, %d, %d, %d )", windowed, width, height, bpp, rate, vsync );

	g_flags = 0;
	if( !windowed )
		g_flags |= SDL_FULLSCREEN;
	g_flags |= SDL_DOUBLEBUF;
	g_flags |= SDL_RESIZABLE;
	g_flags |= SDL_OPENGL;

	SDL_ShowCursor( ~g_flags & SDL_FULLSCREEN );

	ASSERT( bpp == 16 || bpp == 32 );
	switch( bpp )
	{
	case 16:
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
		break;
	case 32:
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	}

	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, TRUE);

#ifdef SDL_HAS_REFRESH_RATE
	if(rate == REFRESH_DEFAULT)
		SDL_SM_SetRefreshRate(0);
	else
		SDL_SM_SetRefreshRate(rate);
#endif

	bool NewOpenGLContext = false;
#ifdef SDL_HAS_CHANGEVIDEOMODE
	if(g_screen)
	{
		/* We can change the video mode without nuking the GL context. */
		NewOpenGLContext = !!SDL_SM_ChangeVideoMode_OpenGL(&g_screen, width, height, bpp, g_flags);
		ASSERT(g_screen);
	}
	else
#endif
	{
		g_screen = SDL_SetVideoMode(width, height, bpp, g_flags);
		if(!g_screen)
			RageException::Throw("SDL_SetVideoMode failed: %s", SDL_GetError());

		NewOpenGLContext = true;
	}

	if(NewOpenGLContext)
	{
		LOG->Trace("New OpenGL context");

		/* We have a new OpenGL context, so we have to tell our textures that
		 * their OpenGL texture number is invalid. */
		if(TEXTUREMAN)
			TEXTUREMAN->InvalidateTextures();

		SetupOpenGL();

		SDL_WM_SetCaption("StepMania", "StepMania");
	}

	DumpOpenGLDebugInfo();

	/* Now that we've initialized, we can search for extensions (some of which
	 * we may need to set up the video mode). */
	SetupExtensions();

	/* Set vsync the Windows way, if we can.  (What other extensions are there
	 * to do this, for other archs?) */
	if(m_oglspecs->WGL_EXT_swap_control) {
	    wglSwapIntervalEXT(vsync);
	}
	
	g_CurrentWidth = g_screen->w;
	g_CurrentHeight = g_screen->h;
	g_CurrentBPP = bpp;

	{
		/* Find out what we really got. */
		int r,g,b,a, colorbits, depth, stencil;
		
		SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
		SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
		SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
		SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
		SDL_GL_GetAttribute(SDL_GL_BUFFER_SIZE, &colorbits);
		SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depth);
		SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);
		LOG->Info("Got %i bpp (%i%i%i%i), %i depth, %i stencil",
			colorbits, r, g, b, a, depth, stencil);
	}

	SetViewport(0,0);

	/* Clear any junk that's in the framebuffer. */
	Clear();
	Flip();

	return NewOpenGLContext;
}

void RageDisplay::DumpOpenGLDebugInfo()
{
#if defined(WIN32)
	/* Dump Windows pixel format data. */
	int Actual = GetPixelFormat(wglGetCurrentDC());

	PIXELFORMATDESCRIPTOR pfd;
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize=sizeof(pfd);
	pfd.nVersion=1;
  
	int pfcnt = DescribePixelFormat(GetDC(g_hWndMain),1,sizeof(pfd),&pfd);
	for (int i=1; i <= pfcnt; i++)
	{
		memset(&pfd, 0, sizeof(pfd));
		pfd.nSize=sizeof(pfd);
		pfd.nVersion=1;
		DescribePixelFormat(GetDC(g_hWndMain),i,sizeof(pfd),&pfd);

		bool skip = false;

		bool rgba = (pfd.iPixelType==PFD_TYPE_RGBA);

		bool mcd = ((pfd.dwFlags & PFD_GENERIC_FORMAT) && (pfd.dwFlags & PFD_GENERIC_ACCELERATED));
		bool soft = ((pfd.dwFlags & PFD_GENERIC_FORMAT) && !(pfd.dwFlags & PFD_GENERIC_ACCELERATED));
		bool icd = !(pfd.dwFlags & PFD_GENERIC_FORMAT) && !(pfd.dwFlags & PFD_GENERIC_ACCELERATED);
		bool opengl = !!(pfd.dwFlags & PFD_SUPPORT_OPENGL);
		bool window = !!(pfd.dwFlags & PFD_DRAW_TO_WINDOW);
		bool dbuff = !!(pfd.dwFlags & PFD_DOUBLEBUFFER);

		if(!rgba || soft || !opengl || !window || !dbuff)
			skip = true;

		/* Skip the above, unless it happens to be the one we chose. */
		if(skip && i != Actual)
			continue;

		CString str = ssprintf("Mode %i: ", i);
		if(i == Actual) str += "*** ";
		if(skip) str += "(BOGUS) ";
		if(soft) str += "software ";
		if(icd) str += "ICD ";
		if(mcd) str += "MCD ";
		if(!rgba) str += "indexed ";
		if(!opengl) str += "!OPENGL ";
		if(!window) str += "!window ";
		if(!dbuff) str += "!dbuff ";

		str += ssprintf("%i (%i%i%i) ", pfd.cColorBits, pfd.cRedBits, pfd.cGreenBits, pfd.cBlueBits);
		if(pfd.cAlphaBits) str += ssprintf("%i alpha ", pfd.cAlphaBits);
		if(pfd.cDepthBits) str += ssprintf("%i depth ", pfd.cDepthBits);
		if(pfd.cStencilBits) str += ssprintf("%i stencil ", pfd.cStencilBits);
		if(pfd.cAccumBits) str += ssprintf("%i accum ", pfd.cAccumBits);

		if(i == Actual && skip)
		{
			/* We chose a bogus format. */
			LOG->Warn("%s", str.GetString());
		} else
			LOG->Trace("%s", str.GetString());
	}
#endif
}

void RageDisplay::ResolutionChanged(int width, int height)
{
	g_CurrentWidth = width;
	g_CurrentHeight = height;

	SetViewport(0,0);

	/* Clear any junk that's in the framebuffer. */
	Clear();
	Flip();
}

void RageDisplay::SetViewport(int shift_left, int shift_down)
{
	/* left and down are on a 0..SCREEN_WIDTH, 0..SCREEN_HEIGHT scale.
	 * Scale them to the actual viewport range. */
	shift_left = int( shift_left * float(g_CurrentWidth) / SCREEN_WIDTH );
	shift_down = int( shift_down * float(g_CurrentWidth) / SCREEN_WIDTH );

	glViewport(shift_left, -shift_down, g_CurrentWidth, g_CurrentHeight);
}

int RageDisplay::GetMaxTextureSize() const
{
	GLint size;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
	return size;
}

void RageDisplay::Clear()
{
	glClearColor( 0,0,0,1 );
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

void RageDisplay::Flip()
{
	SDL_GL_SwapBuffers();
	g_iFramesRenderedSinceLastCheck++;
	g_iFramesRenderedSinceLastReset++;

	if( g_LastCheckTimer.PeekDeltaTime() >= 1.0f )	// update stats every 1 sec.
	{
		g_LastCheckTimer.GetDeltaTime();
		g_iNumChecksSinceLastReset++;
		g_iFPS = g_iFramesRenderedSinceLastCheck;
		g_iCFPS = g_iFramesRenderedSinceLastReset / g_iNumChecksSinceLastReset;
		g_iVPF = g_iVertsRenderedSinceLastCheck / g_iFPS;
		g_iFramesRenderedSinceLastCheck = g_iVertsRenderedSinceLastCheck = 0;
		LOG->Trace( "FPS: %d, CFPS %d, VPF: %d", g_iFPS, g_iCFPS, g_iVPF );
	}
}

void RageDisplay::ResetStats()
{
	g_iFPS = g_iVPF = 0;
	g_iFramesRenderedSinceLastCheck = g_iFramesRenderedSinceLastReset = 0;
	g_iNumChecksSinceLastReset = 0;
	g_iVertsRenderedSinceLastCheck = 0;
	g_LastCheckTimer.GetDeltaTime();
}

void RageDisplay::DisablePalettedTexture()
{
	m_oglspecs->EXT_paletted_texture = false;
}

bool RageDisplay::IsWindowed() const 
{
	return true; // FIXME
}
void RageDisplay::DrawQuad( const RageVertex v[4] )	// upper-left, upper-right, lower-left, lower-right
{
	DrawQuads( v, 4 );
}

void RageDisplay::DrawQuads( const RageVertex v[], int iNumVerts )
{
	ASSERT( (iNumVerts%4) == 0 );

	glInterleavedArrays( RageVertexFormat, sizeof(RageVertex), v );
	glDrawArrays( GL_QUADS, 0, iNumVerts );

	g_iVertsRenderedSinceLastCheck += iNumVerts;
}
void RageDisplay::DrawFan( const RageVertex v[], int iNumVerts )
{
	ASSERT( iNumVerts >= 3 );
	glInterleavedArrays( RageVertexFormat, sizeof(RageVertex), v );
	glDrawArrays( GL_TRIANGLE_FAN, 0, iNumVerts );
	g_iVertsRenderedSinceLastCheck += iNumVerts;
}

void RageDisplay::DrawStrip( const RageVertex v[], int iNumVerts )
{
	ASSERT( iNumVerts >= 3 );
	glInterleavedArrays( RageVertexFormat, sizeof(RageVertex), v );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, iNumVerts );
	g_iVertsRenderedSinceLastCheck += iNumVerts;
}

void RageDisplay::DrawLoop( const RageVertex v[], int iNumVerts, float LineWidth )
{
	ASSERT( iNumVerts >= 3 );

	glEnable(GL_LINE_SMOOTH);

	/* Our line width is wrt the regular internal SCREEN_WIDTHxSCREEN_HEIGHT screen,
	 * but these width functions actually want raster sizes (that is, actual pixels).
	 * Scale the line width and point size by the average ratio of the scale. */
	float WidthVal = float(g_CurrentWidth) / SCREEN_WIDTH;
	float HeightVal = float(g_CurrentHeight) / SCREEN_HEIGHT;
	LineWidth *= (WidthVal + HeightVal) / 2;

	/* Clamp the width to the hardware max for both lines and points (whichever
	 * is more restrictive). */
	LineWidth = clamp(LineWidth, m_oglspecs->line_range[0], m_oglspecs->line_range[1]);
	LineWidth = clamp(LineWidth, m_oglspecs->point_range[0], m_oglspecs->point_range[1]);

	/* Hmm.  The granularity of lines and points might be different; for example,
	 * if lines are .5 and points are .25, we might want to snap the width to the
	 * nearest .5, so the hardware doesn't snap them to different sizes.  Does it
	 * matter? */

	glLineWidth(LineWidth);

	/* Draw the line loop: */
	glInterleavedArrays( RageVertexFormat, sizeof(RageVertex), v );
	glDrawArrays( GL_LINE_LOOP, 0, iNumVerts );

	glDisable(GL_LINE_SMOOTH);

	/* Round off the corners.  This isn't perfect; the point is sometimes a little
	 * larger than the line, causing a small bump on the edge.  Not sure how to fix
	 * that. */
	glPointSize(LineWidth);

	/* Hack: if the points will all be the same, we don't want to draw
	 * any points at all, since there's nothing to connect.  That'll happen
	 * if both scale factors in the matrix are ~0.  (Actually, I think
	 * it's true if two of the three scale factors are ~0, but we don't
	 * use this for anything 3d at the moment anyway ...) */
	RageMatrix mat;
	glGetFloatv( GL_MODELVIEW_MATRIX, (float*)mat );

	if(mat.m[0][0] < 1e-5 && mat.m[1][1] < 1e-5) 
	    return;

	glEnable(GL_POINT_SMOOTH);

	glInterleavedArrays( RageVertexFormat, sizeof(RageVertex), v );
	glDrawArrays( GL_POINTS, 0, iNumVerts );

	glDisable(GL_POINT_SMOOTH);
}

void RageDisplay::PushMatrix() 
{ 
	glPushMatrix();
	ASSERT(++g_ModelMatrixCnt<20);
}

void RageDisplay::PopMatrix() 
{ 
	glPopMatrix();
	ASSERT(g_ModelMatrixCnt-->0);
}

/* Switch from orthogonal to perspective view.
 *
 * Tricky: we want to maintain all of the zooms, rotations and translations
 * that have been applied already.  They're in our internal screen space (that
 * is, 640x480 ortho).  We can't simply leave them where they are, since they'll
 * be applied before the perspective transform, which means they'll be in the
 * wrong coordinate space.
 *
 * Handle translations (the right column of the transform matrix) to the viewport.
 * Move rotations and scaling (0,0 through 1,1) to after the perspective transform.
 * Actually, those might be able to stay where they are, I'm not sure; it's translations
 * that are annoying.  So, XXX: see if rots and scales can be left on the modelview
 * matrix instead of messing with the projection matrix.
 *
 * When finished, the current position will be the "viewpoint" (at 0,0).  negative
 * Z goes into the screen, positive X and Y is right and down.
 */
void RageDisplay::EnterPerspective(float fov, bool preserve_loc)
{
	g_PerspectiveMode++;
	if(g_PerspectiveMode > 1) {
		/* havn't yet worked out the details of this */
		LOG->Trace("EnterPerspective called when already in perspective mode");
		g_PerspectiveMode++;
		return;
	}

	/* Save the old matrices. */
	DISPLAY->PushMatrix();
	glMatrixMode( GL_PROJECTION );
	glPushMatrix();
	glLoadIdentity();
	float aspect = SCREEN_WIDTH/(float)SCREEN_HEIGHT;
	gluPerspective(fov, aspect, 1.000f, 1000.0f);

	/* Flip the Y coordinate, so positive numbers go down. */
	glScalef(1, -1, 1);

	if(!preserve_loc)
	{
		glMatrixMode( GL_MODELVIEW );
		return;
	}

	RageMatrix matTop;
	glGetFloatv( GL_MODELVIEW_MATRIX, (float*)matTop );

	{
		/* Pull out the 2d translation. */
		float x = matTop.m[3][0], y = matTop.m[3][1];

		/* These values are where the viewpoint should be.  By default, it's in the
		* center of the screen, and these values are from the top-left, so subtract
		* the difference. */
		x -= SCREEN_WIDTH/2;
		y -= SCREEN_HEIGHT/2;
		DISPLAY->SetViewport(int(x), int(y));
	}

	/* Pull out the 2d rotations and scales. */
	{
		RageMatrix mat;
		RageMatrixIdentity(&mat);
		mat.m[0][0] = matTop.m[0][0];
		mat.m[0][1] = matTop.m[0][1];
		mat.m[1][0] = matTop.m[1][0];
		mat.m[1][1] = matTop.m[1][1];
		glMultMatrixf((float *) mat);
	}

	/* We can't cope with perspective matrices or things that touch Z.  (We shouldn't
	* have touched those while in 2d, anyway.) */
	ASSERT(matTop.m[0][2] == 0.f &&	matTop.m[0][3] == 0.f && matTop.m[1][2] == 0.f &&
		matTop.m[1][3] == 0.f && matTop.m[2][0] == 0.f && matTop.m[2][1] == 0.f &&
		matTop.m[2][2] == 1.f && matTop.m[3][2] == 0.f && matTop.m[3][3] == 1.f);

	/* Reset the matrix back to identity. */
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
}

void RageDisplay::ExitPerspective()
{
	g_PerspectiveMode--;
	if(g_PerspectiveMode) return;

	/* Restore the old matrices. */
	glMatrixMode( GL_PROJECTION );
	glPopMatrix();
	glMatrixMode( GL_MODELVIEW );
	DISPLAY->PopMatrix();

	/* Restore the viewport. */
	DISPLAY->SetViewport(0, 0);
}

/* gluLookAt.  The result is post-multiplied to the matrix (M = L * M) instead of
 * pre-multiplied. */
void RageDisplay::LookAt(const RageVector3 &Eye, const RageVector3 &At, const RageVector3 &Up)
{
	glMatrixMode(GL_MODELVIEW);

	glPushMatrix();
	glLoadIdentity();
	gluLookAt(Eye.x, Eye.y, Eye.z, At.x, At.y, At.z, Up.x, Up.y, Up.z);
	RageMatrix view;
	glGetFloatv( GL_MODELVIEW_MATRIX, (float*)view ); /* cheesy :) */
	glPopMatrix();

	smPostMultMatrixf(view);
}

void RageDisplay::Translate( float x, float y, float z )
{
	glTranslatef(x, y, z);
}


void RageDisplay::TranslateLocal( float x, float y, float z )
{
	RageMatrix matTemp;
	RageMatrixTranslation( &matTemp, x, y, z );

	smPostMultMatrixf(matTemp);
}

void RageDisplay::Scale( float x, float y, float z )
{
	glScalef(x, y, z);
}

void RageDisplay::RotateX( float r )
{
	glRotatef(r* 180/PI, 1, 0, 0);
}

void RageDisplay::RotateY( float r )
{
	glRotatef(r* 180/PI, 0, 1, 0);
}

void RageDisplay::RotateZ( float r )
{
	glRotatef(r* 180/PI, 0, 0, 1);
}

void RageDisplay::smPostMultMatrixf( const RageMatrix &f )
{
	RageMatrix m;
	glGetFloatv( GL_MODELVIEW_MATRIX, (float*)m );
	RageMatrixMultiply( &m, &f, &m );
	glLoadMatrixf((const float*) m);
}

void RageDisplay::SetTexture( RageTexture* pTexture )
{
	glBindTexture( GL_TEXTURE_2D, pTexture? pTexture->GetGLTextureID() : 0 );
}
void RageDisplay::SetTextureModeModulate()
{
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

/* Set the blend mode for both texture and alpha.  This is all that's
 * available pre-OpenGL 1.4. */
void RageDisplay::SetBlendMode(int src, int dst)
{
	glBlendFunc( src, dst );
}

void RageDisplay::SetTextureModeGlow()
{
	if(!m_oglspecs->EXT_texture_env_combine) {
		SetBlendMode( GL_SRC_ALPHA, GL_ONE );
		return;
	}

	/* Source color is the diffuse color only: */
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PRIMARY_COLOR_EXT);

	/* Source alpha is texture alpha * diffuse alpha: */
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_EXT, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA_EXT, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_PRIMARY_COLOR_EXT);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA_EXT, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_EXT, GL_TEXTURE);
}

void RageDisplay::SetBlendModeNormal()
{
	SetBlendMode( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
}
void RageDisplay::SetBlendModeAdd()
{
	SetBlendMode( GL_ONE, GL_ONE );
}

bool RageDisplay::ZBufferEnabled() const
{
	bool a;
	glGetBooleanv( GL_DEPTH_TEST, (unsigned char*)&a );
	return a;
}

void RageDisplay::EnableZBuffer()
{
	glEnable( GL_DEPTH_TEST );
}
void RageDisplay::DisableZBuffer()
{
	glDisable( GL_DEPTH_TEST );
}
void RageDisplay::EnableTextureWrapping()
{
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
}
void RageDisplay::DisableTextureWrapping()
{
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP );
}
