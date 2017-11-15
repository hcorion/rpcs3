#include "stdafx.h"
#include "gl_gs_frame.h"
#include "Emu/System.h"
#include <GL/Regal.h>
#include <OpenGL/OpenGL.h>
#include <QOpenGLContext>
#include <QWindow>

gl_gs_frame::gl_gs_frame(int w, int h, QIcon appIcon, bool disableMouse)
	: gs_frame("OpenGL", w, h, appIcon, disableMouse)
{
	setSurfaceType(QSurface::OpenGLSurface);

	m_format.setMajorVersion(4);
	m_format.setMinorVersion(1);
	m_format.setProfile(QSurfaceFormat::CoreProfile);
	m_format.setDepthBufferSize(16);
	m_format.setSwapBehavior(QSurfaceFormat::SwapBehavior::DoubleBuffer);
	if (g_cfg.video.debug_output)
	{
		m_format.setOption(QSurfaceFormat::FormatOption::DebugContext);
	}
	setFormat(m_format);
}

draw_context_t gl_gs_frame::make_context()
{
	auto context = new QOpenGLContext();
	context->setFormat(m_format);
	context->create();
	RegalMakeCurrent(context);
	return context;
}

void gl_gs_frame::set_current(draw_context_t ctx)
{
	if (!((QOpenGLContext*)ctx)->makeCurrent(this))
	{
		create();
		RegalMakeCurrent(ctx.get());
	}
}

void gl_gs_frame::delete_context(draw_context_t ctx)
{

	auto gl_ctx = (QOpenGLContext*)ctx;
	gl_ctx->doneCurrent();

#ifndef _WIN32
	delete gl_ctx;
#else
	//AMD driver crashes when executing wglDeleteContext
	//Catch with SEH
	__try
	{
		delete gl_ctx;
	}
	__except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		LOG_FATAL(RSX, "Your graphics driver just crashed whilst cleaning up. All consumed VRAM should have been released, but you may want to restart the emulator just in case");
	}
#endif
}

void gl_gs_frame::flip(draw_context_t context, bool skip_frame)
{
	gs_frame::flip(context);

	//Do not swap buffers if frame skip is active
	if (skip_frame) return;

	RegalMakeCurrent(context.get());
	((QOpenGLContext*)context)->swapBuffers(this);
}
