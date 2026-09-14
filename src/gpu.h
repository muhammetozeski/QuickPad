#pragma once

#include <windows.h>

/*
 * Frames shown through the graphics card instead of GDI. A surface binds a DirectComposition
 * visual with a swap chain to a window and keeps a frame the program writes rows into from any
 * thread; presenting copies the frame to the swap chain on the graphics card and hands it to the
 * Desktop Window Manager. Nothing is copied on the processor, and no paint of the window's own
 * surface is involved. The frame is wider and taller than the window by the extra pixels asked
 * for, so composing may overshoot the window.
 */
typedef struct GpuSurface GpuSurface;

/* TRUE when Direct3D 11 and DirectComposition are usable; the shared device is made on the first call. */
BOOL GpuAvailable(void);

GpuSurface *GpuSurfaceCreate(HWND window, int width, int height, int extraWidth, int extraHeight);
BOOL GpuSurfaceResize(GpuSurface *surface, int width, int height);
void GpuSurfaceRelease(GpuSurface *surface);

/*
 * The frame's pixels for writing, stride pixels per row, until GpuSurfacePresent; NULL when the
 * surface stopped working. The frame keeps its content between presents; reading from it is slow.
 */
DWORD *GpuSurfaceMap(GpuSurface *surface, int *stride);

/* Ends the writing, copies the frame to the screen buffer and presents it; FALSE when the surface stopped working. */
BOOL GpuSurfacePresent(GpuSurface *surface);
