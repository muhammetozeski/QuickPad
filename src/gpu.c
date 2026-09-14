#include "gpu.h"
#include "quickpad.h"
#include "trace.h"

#include <d3d11.h>
#include <dxgi1_2.h>

/*
 * The DirectComposition interfaces, declared here because dcomp.h compiles only as C++. Only the
 * methods used are named; the others are placeholders that keep the vtable slots in place.
 */
typedef struct IDCompositionVisual IDCompositionVisual;
typedef struct IDCompositionTarget IDCompositionTarget;
typedef struct IDCompositionDevice IDCompositionDevice;

typedef struct IDCompositionVisualVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionVisual *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionVisual *);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionVisual *);
    void *offsetAndTransform[12];   /* SetOffsetX (2), SetOffsetY (2), SetTransform (2), SetTransformParent, SetEffect,
                                       SetBitmapInterpolationMode, SetBorderMode, SetClip (2) */
    HRESULT (STDMETHODCALLTYPE *SetContent)(IDCompositionVisual *, IUnknown *);
} IDCompositionVisualVtbl;

struct IDCompositionVisual {
    const IDCompositionVisualVtbl *lpVtbl;
};

typedef struct IDCompositionTargetVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionTarget *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionTarget *);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionTarget *);
    HRESULT (STDMETHODCALLTYPE *SetRoot)(IDCompositionTarget *, IDCompositionVisual *);
} IDCompositionTargetVtbl;

struct IDCompositionTarget {
    const IDCompositionTargetVtbl *lpVtbl;
};

typedef struct IDCompositionDeviceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionDevice *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionDevice *);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionDevice *);
    HRESULT (STDMETHODCALLTYPE *Commit)(IDCompositionDevice *);
    HRESULT (STDMETHODCALLTYPE *WaitForCommitCompletion)(IDCompositionDevice *);
    void *GetFrameStatistics;
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(IDCompositionDevice *, HWND, BOOL, IDCompositionTarget **);
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(IDCompositionDevice *, IDCompositionVisual **);
} IDCompositionDeviceVtbl;

struct IDCompositionDevice {
    const IDCompositionDeviceVtbl *lpVtbl;
};

/* The interface ids, so that no import library is needed for them. */
static const GUID gpuIID_IDXGIDevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID gpuIID_IDXGIFactory2 = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
static const GUID gpuIID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID gpuIID_IDCompositionDevice = { 0xC37EA93A, 0xE7AA, 0x450D, { 0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3 } };

typedef HRESULT (WINAPI *CreateDeviceFunction)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *CreateCompositionFunction)(IUnknown *, REFIID, void **);

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static IDXGIFactory2 *factory;
static IDCompositionDevice *composition;
static int state;    /* 0 not tried, 1 usable, -1 unusable */

struct GpuSurface {
    IDXGISwapChain1 *swapChain;
    IDCompositionTarget *target;
    IDCompositionVisual *visual;
    ID3D11Texture2D *frame;      /* staging texture the rows are written into; keeps its content between presents */
    int width;                   /* of the window */
    int height;
    int extraWidth;
    int extraHeight;
    BOOL mapped;
    DWORD *mappedPixels;
    int mappedStride;
    BOOL broken;
};

#define Release(object) ((object)->lpVtbl->Release(object))

BOOL GpuAvailable(void)
{
    if (state != 0) {
        return state > 0;
    }
    state = -1;

    HMODULE d3d = LoadLibraryExW(L"d3d11.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dcomp = LoadLibraryExW(L"dcomp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    CreateDeviceFunction createDevice = d3d != NULL ? (CreateDeviceFunction)GetProcAddress(d3d, "D3D11CreateDevice") : NULL;
    CreateCompositionFunction createComposition = dcomp != NULL ? (CreateCompositionFunction)GetProcAddress(dcomp, "DCompositionCreateDevice2") : NULL;
    if (createDevice == NULL || createComposition == NULL) {
        return FALSE;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(createDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context))) {
        return FALSE;
    }
    IDXGIDevice *dxgiDevice = NULL;
    IDXGIAdapter *adapter = NULL;
    BOOL usable = SUCCEEDED(device->lpVtbl->QueryInterface(device, &gpuIID_IDXGIDevice, (void **)&dxgiDevice))
        && SUCCEEDED(dxgiDevice->lpVtbl->GetAdapter(dxgiDevice, &adapter))
        && SUCCEEDED(adapter->lpVtbl->GetParent(adapter, &gpuIID_IDXGIFactory2, (void **)&factory))
        && SUCCEEDED(createComposition((IUnknown *)dxgiDevice, &gpuIID_IDCompositionDevice, (void **)&composition));
    if (adapter != NULL) {
        Release(adapter);
    }
    if (dxgiDevice != NULL) {
        Release(dxgiDevice);
    }
    if (!usable) {
        return FALSE;
    }
    state = 1;
    return TRUE;
}

static BOOL MakeFrame(GpuSurface *surface)
{
    D3D11_TEXTURE2D_DESC description = { 0 };
    description.Width = (UINT)(surface->width + surface->extraWidth);
    description.Height = (UINT)(surface->height + surface->extraHeight);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    /*
     * Staging keeps the frame's content between presents and maps in less time than a dynamic
     * texture on the drivers measured; either way the driver copies the frame when it is unmapped.
     */
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->lpVtbl->CreateTexture2D(device, &description, NULL, &surface->frame));
}

GpuSurface *GpuSurfaceCreate(HWND window, int width, int height, int extraWidth, int extraHeight)
{
    if (!GpuAvailable() || width <= 0 || height <= 0) {
        return NULL;
    }
    GpuSurface *surface = MemAllocZero(sizeof *surface);
    if (surface == NULL) {
        return NULL;
    }
    surface->width = width;
    surface->height = height;
    surface->extraWidth = extraWidth;
    surface->extraHeight = extraHeight;

    DXGI_SWAP_CHAIN_DESC1 description = { 0 };
    description.Width = (UINT)width;
    description.Height = (UINT)height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    BOOL made = SUCCEEDED(factory->lpVtbl->CreateSwapChainForComposition(factory, (IUnknown *)device, &description, NULL, &surface->swapChain))
        && SUCCEEDED(composition->lpVtbl->CreateTargetForHwnd(composition, window, TRUE, &surface->target))
        && SUCCEEDED(composition->lpVtbl->CreateVisual(composition, &surface->visual))
        && SUCCEEDED(surface->visual->lpVtbl->SetContent(surface->visual, (IUnknown *)surface->swapChain))
        && SUCCEEDED(surface->target->lpVtbl->SetRoot(surface->target, surface->visual))
        && SUCCEEDED(composition->lpVtbl->Commit(composition))
        && MakeFrame(surface);
    if (!made) {
        GpuSurfaceRelease(surface);
        return NULL;
    }
    return surface;
}

BOOL GpuSurfaceResize(GpuSurface *surface, int width, int height)
{
    if (surface->broken || width <= 0 || height <= 0) {
        return FALSE;
    }
    if (width == surface->width && height == surface->height) {
        return TRUE;
    }
    if (surface->mapped) {
        context->lpVtbl->Unmap(context, (ID3D11Resource *)surface->frame, 0);
        surface->mapped = FALSE;
    }
    Release(surface->frame);
    surface->frame = NULL;
    surface->width = width;
    surface->height = height;
    if (FAILED(surface->swapChain->lpVtbl->ResizeBuffers(surface->swapChain, 0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0)) || !MakeFrame(surface)) {
        surface->broken = TRUE;
        return FALSE;
    }
    return TRUE;
}

void GpuSurfaceRelease(GpuSurface *surface)
{
    if (surface == NULL) {
        return;
    }
    if (surface->mapped) {
        context->lpVtbl->Unmap(context, (ID3D11Resource *)surface->frame, 0);
    }
    if (surface->target != NULL) {
        surface->target->lpVtbl->SetRoot(surface->target, NULL);
        composition->lpVtbl->Commit(composition);
        Release(surface->target);
    }
    if (surface->visual != NULL) {
        Release(surface->visual);
    }
    if (surface->frame != NULL) {
        Release(surface->frame);
    }
    if (surface->swapChain != NULL) {
        Release(surface->swapChain);
    }
    MemFree(surface);
}

DWORD *GpuSurfaceMap(GpuSurface *surface, int *stride)
{
    if (surface->broken) {
        return NULL;
    }
    if (!surface->mapped) {
        D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
        if (FAILED(context->lpVtbl->Map(context, (ID3D11Resource *)surface->frame, 0, D3D11_MAP_WRITE, 0, &mapped))) {
            surface->broken = TRUE;
            return NULL;
        }
        surface->mapped = TRUE;
        surface->mappedPixels = mapped.pData;
        surface->mappedStride = (int)(mapped.RowPitch / sizeof(DWORD));
    }
    *stride = surface->mappedStride;
    return surface->mappedPixels;
}

BOOL GpuSurfacePresent(GpuSurface *surface)
{
    if (surface->broken) {
        return FALSE;
    }
    if (surface->mapped) {
        context->lpVtbl->Unmap(context, (ID3D11Resource *)surface->frame, 0);
        surface->mapped = FALSE;
    }
    TRACE("unmapped");
    ID3D11Texture2D *back = NULL;
    if (FAILED(surface->swapChain->lpVtbl->GetBuffer(surface->swapChain, 0, &gpuIID_ID3D11Texture2D, (void **)&back))) {
        surface->broken = TRUE;
        return FALSE;
    }
    D3D11_BOX box = { 0, 0, 0, (UINT)surface->width, (UINT)surface->height, 1 };
    context->lpVtbl->CopySubresourceRegion(context, (ID3D11Resource *)back, 0, 0, 0, 0, (ID3D11Resource *)surface->frame, 0, &box);
    Release(back);
    TRACE("copied to back buffer");
    HRESULT result = surface->swapChain->lpVtbl->Present(surface->swapChain, 0, 0);
    if (FAILED(result)) {
        surface->broken = TRUE;
        return FALSE;
    }
    return TRUE;
}
