/* mg.dll - stand-in for MF.dll in Catherine Classic.
 *
 * The game plays movies through Media Foundation, rendering with the EVR into
 * a child window. Under Wine + DXVK on NVIDIA that second swapchain fights the
 * game's own and the movie flickers. This DLL replaces only
 * MFCreateVideoRendererActivate with a tiny video sink: decoded frames (RGB32)
 * are copied into the game's own back buffer right before it presents.
 * Everything else (session, audio, timing, skipping) is the real MF.dll.
 *
 * The exe's import "MF.dll" is renamed to "MG.dll" (see install.sh), so this
 * file is loaded instead. A log goes to videohook.log next to the exe.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <evr.h>
#include <stdio.h>
#include <stdarg.h>
#ifndef PRESENTATION_CURRENT_POSITION
#define PRESENTATION_CURRENT_POSITION 0x7fffffffffffffffLL
#endif
DEFINE_GUID(GUID_NULL_,0,0,0,0,0,0,0,0,0,0,0);
#define GUID_NULL GUID_NULL_

/* ------------------------------------------------------------------ log */
static FILE *logf;
static CRITICAL_SECTION loglock;
static void LOG(const char *fmt, ...)
{
    va_list a;
    EnterCriticalSection(&loglock);
    if (!logf) logf = fopen("videohook.log", "w");
    if (logf) {
        fprintf(logf, "[%lu] ", GetTickCount());
        va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a);
        fputc('\n', logf); fflush(logf);
    }
    LeaveCriticalSection(&loglock);
}

/* ------------------------------------------------------------ real MF.dll */
static HMODULE realmf;
static FARPROC realfn(const char *name)
{
    if (!realmf) realmf = LoadLibraryA("mf.dll");
    return realmf ? GetProcAddress(realmf, name) : NULL;
}
#define FWD(ret, name, decl, args) \
    __declspec(dllexport) ret WINAPI name decl { \
        static ret (WINAPI *p) decl; \
        if (!p) p = (void *)realfn(#name); \
        return p args; }
FWD(HRESULT, MFCreateTopology, (IMFTopology **t), (t))
FWD(HRESULT, MFCreateMediaSession, (IMFAttributes *c, IMFMediaSession **s), (c, s))
FWD(HRESULT, MFCreateAudioRendererActivate, (IMFActivate **a), (a))
FWD(HRESULT, MFCreateTopologyNode, (MF_TOPOLOGY_TYPE t, IMFTopologyNode **n), (t, n))

static void LOG(const char *fmt, ...);
static IMFGetService *cur_sink_gs; /* the live sink, for MR_VIDEO_RENDER_SERVICE */
__declspec(dllexport) HRESULT WINAPI MFGetService(IUnknown *o, REFGUID g, REFIID i, void **v)
{
    static HRESULT (WINAPI *p)(IUnknown *, REFGUID, REFIID, void **);
    if (!p) p = (void *)realfn("MFGetService");
    HRESULT hr = p(o, g, i, v);
    /* Wine's session only finds its own EVR; hand out ours */
    if (FAILED(hr) && cur_sink_gs && IsEqualGUID(g, &MR_VIDEO_RENDER_SERVICE))
        hr = IMFGetService_GetService(cur_sink_gs, g, i, v);
    return hr;
}

/* ------------------------------------------------------- shared frame state */
/* Written by the sink's pump thread, read on the game's render thread. */
static CRITICAL_SECTION framelock;
static BYTE *frame;            /* w*h*4, top-down BGRX */
static UINT fw, fh;
static volatile LONG frame_dirty, playing;
static IDirect3DSurface9 *surf; /* game thread only */
static HWND movie_hwnd;
static int passthrough;
static volatile LONG paused;
static volatile DWORD last_grab; /* game fetched a frame itself (GetCurrentImage) */
static LONGLONG frame_time; /* game paused the movie: show its own frame (skip prompt) */      /* the game's "WMV Playback" window */
static UINT sw, sh;

/* ----------------------------------------------------------- d3d9 hooks */
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *PresentEx_t)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (WINAPI *ResetEx_t)(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
typedef HRESULT (WINAPI *SCPresent_t)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static Present_t oPresent; static PresentEx_t oPresentEx; static Reset_t oReset;
static ResetEx_t oResetEx; static SCPresent_t oSCPresent;
static int logged_present;

static void release_surf(void)
{
    if (surf) { IDirect3DSurface9_Release(surf); surf = NULL; }
}

static int drawn_this_frame, trace_frames;
static void draw_video(IDirect3DDevice9 *dev, const char *via)
{
    if (!playing) { release_surf(); return; }
    if (!frame) return;
    /* the game hides its movie window to pause (skip prompt); show its frame then */
    if (movie_hwnd && !IsWindowVisible(movie_hwnd)) return;
    if (paused) return;
    /* the game is drawing the frames itself (skip prompt over the movie) */
    if (last_grab && GetTickCount() - last_grab < 300) return;
    if (logged_present < 3) { LOG("drawing via %s", via); logged_present++; }
    drawn_this_frame = 1;
    EnterCriticalSection(&framelock);
    if (surf && (sw != fw || sh != fh)) release_surf();
    if (!surf) {
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, fw, fh, D3DFMT_X8R8G8B8,
                                                              D3DPOOL_DEFAULT, &surf, NULL))) {
            LeaveCriticalSection(&framelock); LOG("CreateOffscreenPlainSurface failed"); return;
        }
        sw = fw; sh = fh; frame_dirty = 1;
    }
    if (frame_dirty) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(surf, &lr, NULL, D3DLOCK_DISCARD))) {
            for (UINT y = 0; y < fh; y++)
                memcpy((BYTE *)lr.pBits + y * lr.Pitch, frame + y * fw * 4, fw * 4);
            IDirect3DSurface9_UnlockRect(surf);
        }
        frame_dirty = 0;
    }
    LeaveCriticalSection(&framelock);

    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d; IDirect3DSurface9_GetDesc(bb, &d);
    /* letterbox: fit the movie into the back buffer, keep aspect */
    RECT r; UINT w = d.Width, h = d.Width * sh / sw;
    if (h > d.Height) { h = d.Height; w = d.Height * sw / sh; }
    r.left = (d.Width - w) / 2; r.top = (d.Height - h) / 2; r.right = r.left + w; r.bottom = r.top + h;
    IDirect3DDevice9_ColorFill(dev, bb, NULL, D3DCOLOR_XRGB(0, 0, 0));
    IDirect3DDevice9_StretchRect(dev, surf, NULL, bb, &r, D3DTEXF_LINEAR);
    IDirect3DSurface9_Release(bb);
}

/* Insert the movie right after the game's first draw onto the back buffer
 * (its background), so whatever it draws afterwards (skip prompt etc.)
 * stays on top. Present is only the fallback. */
typedef HRESULT (WINAPI *DP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (WINAPI *DIP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI *DPUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (WINAPI *DIPUP_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
static DP_t oDP; static DIP_t oDIP; static DPUP_t oDPUP; static DIPUP_t oDIPUP;
static int draw_idx;
static BOOL rt_is_backbuffer(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *rt = NULL, *bb = NULL; BOOL r = FALSE;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) &&
        SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) r = rt == bb;
    if (rt) IDirect3DSurface9_Release(rt);
    if (bb) IDirect3DSurface9_Release(bb);
    return r;
}
static void after_draw(IDirect3DDevice9 *dev, const char *kind, UINT prims)
{
    if (!playing || !frame) return;
    BOOL onbb = rt_is_backbuffer(dev);
    if (trace_frames < 4) {
        IDirect3DBaseTexture9 *t = NULL; D3DSURFACE_DESC d = {0};
        if (SUCCEEDED(IDirect3DDevice9_GetTexture(dev, 0, &t)) && t) {
            if (IDirect3DBaseTexture9_GetType(t) == D3DRTYPE_TEXTURE)
                IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)t, 0, &d);
            IDirect3DBaseTexture9_Release(t);
        }
        LOG("  frame %d draw %d: %s prims=%u rt=%s tex0=%ux%u", trace_frames, draw_idx, kind, prims,
            onbb ? "backbuffer" : "offscreen", d.Width, d.Height);
    }
    draw_idx++;
    (void)onbb;
}
static HRESULT WINAPI hDP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT n)
{ HRESULT hr = oDP(d, t, a, n); after_draw(d, "DP", n); return hr; }
static HRESULT WINAPI hDIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT n)
{ HRESULT hr = oDIP(d, t, a, b, c, e, n); after_draw(d, "DIP", n); return hr; }
static HRESULT WINAPI hDPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT st)
{ HRESULT hr = oDPUP(d, t, n, v, st); after_draw(d, "DPUP", n); return hr; }
static HRESULT WINAPI hDIPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT n, const void *i, D3DFORMAT f, const void *v, UINT st)
{ HRESULT hr = oDIPUP(d, t, a, b, n, i, f, v, st); after_draw(d, "DIPUP", n); return hr; }

static WNDPROC oMovieProc;
static LRESULT CALLBACK movie_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if ((m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEMOVE)
        || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_SHOWWINDOW || m == WM_INPUT)
        LOG("movie wnd msg %04x w=%lx", m, (long)w);
    return CallWindowProcA(oMovieProc, h, m, w, l);
}
static void end_frame(IDirect3DDevice9 *dev, const char *via)
{
    if (!drawn_this_frame) draw_video(dev, via);
    if (playing && frame && trace_frames < 4) LOG("  frame %d end (%d draws)", trace_frames++, draw_idx);
    drawn_this_frame = 0; draw_idx = 0;
}
static HRESULT WINAPI hPresent(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{ end_frame(dev, "Present"); return oPresent(dev, a, b, w, r); }
static HRESULT WINAPI hPresentEx(IDirect3DDevice9Ex *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f)
{ end_frame((IDirect3DDevice9 *)dev, "PresentEx"); return oPresentEx(dev, a, b, w, r, f); }
static HRESULT WINAPI hSCPresent(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f)
{
    IDirect3DDevice9 *dev;
    if (SUCCEEDED(IDirect3DSwapChain9_GetDevice(sc, &dev))) {
        end_frame(dev, "SwapChain::Present"); IDirect3DDevice9_Release(dev);
    }
    return oSCPresent(sc, a, b, w, r, f);
}
static HRESULT WINAPI hReset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *p)
{ release_surf(); return oReset(dev, p); }
static HRESULT WINAPI hResetEx(IDirect3DDevice9Ex *dev, D3DPRESENT_PARAMETERS *p, D3DDISPLAYMODEEX *m)
{ release_surf(); return oResetEx(dev, p, m); }

static void patch(void **slot, void *hook, void **orig)
{
    DWORD old;
    if (*slot == hook) return;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = *slot; *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static void hook_device(IDirect3DDevice9 *dev, BOOL ex)
{
    IDirect3DDevice9ExVtbl *vt = (IDirect3DDevice9ExVtbl *)dev->lpVtbl;
    patch((void **)&vt->Present, hPresent, (void **)&oPresent);
    patch((void **)&vt->Reset, hReset, (void **)&oReset);
    patch((void **)&vt->DrawPrimitive, hDP, (void **)&oDP);
    patch((void **)&vt->DrawIndexedPrimitive, hDIP, (void **)&oDIP);
    patch((void **)&vt->DrawPrimitiveUP, hDPUP, (void **)&oDPUP);
    patch((void **)&vt->DrawIndexedPrimitiveUP, hDIPUP, (void **)&oDIPUP);
    if (ex) {
        patch((void **)&vt->PresentEx, hPresentEx, (void **)&oPresentEx);
        patch((void **)&vt->ResetEx, hResetEx, (void **)&oResetEx);
    }
    IDirect3DSwapChain9 *sc;
    if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc))) {
        patch((void **)&sc->lpVtbl->Present, hSCPresent, (void **)&oSCPresent);
        IDirect3DSwapChain9_Release(sc);
    }
    LOG("hooked device %p (ex=%d)", dev, ex);
}

typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *CreateDeviceEx_t)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **);
static CreateDevice_t oCreateDevice; static CreateDeviceEx_t oCreateDeviceEx;
static HRESULT WINAPI hCreateDevice(IDirect3D9 *d, UINT a, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS *p, IDirect3DDevice9 **out)
{
    HRESULT hr = oCreateDevice(d, a, t, w, f, p, out);
    if (SUCCEEDED(hr)) hook_device(*out, FALSE);
    return hr;
}
static HRESULT WINAPI hCreateDeviceEx(IDirect3D9Ex *d, UINT a, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS *p, D3DDISPLAYMODEEX *m, IDirect3DDevice9Ex **out)
{
    HRESULT hr = oCreateDeviceEx(d, a, t, w, f, p, m, out);
    if (SUCCEEDED(hr)) hook_device((IDirect3DDevice9 *)*out, TRUE);
    return hr;
}

typedef HRESULT (WINAPI *D3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
typedef IDirect3D9 *(WINAPI *D3DCreate9_t)(UINT);
static D3DCreate9Ex_t oD3DCreate9Ex; static D3DCreate9_t oD3DCreate9;
static void hook_d3d(IDirect3D9Ex *d3d, BOOL ex)
{
    IDirect3D9ExVtbl *vt = (IDirect3D9ExVtbl *)d3d->lpVtbl;
    patch((void **)&vt->CreateDevice, hCreateDevice, (void **)&oCreateDevice);
    if (ex) patch((void **)&vt->CreateDeviceEx, hCreateDeviceEx, (void **)&oCreateDeviceEx);
}
static HRESULT WINAPI hD3DCreate9Ex(UINT v, IDirect3D9Ex **out)
{
    HRESULT hr = oD3DCreate9Ex(v, out);
    LOG("Direct3DCreate9Ex -> %08lx", hr);
    if (SUCCEEDED(hr)) hook_d3d(*out, TRUE);
    return hr;
}
static IDirect3D9 *WINAPI hD3DCreate9(UINT v)
{
    IDirect3D9 *d = oD3DCreate9(v);
    LOG("Direct3DCreate9 -> %p", d);
    if (d) hook_d3d((IDirect3D9Ex *)d, FALSE);
    return d;
}

static FARPROC WINAPI hGetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC p = GetProcAddress(m, name);
    if (p && (UINT_PTR)name > 0xffff) {
        if (!strcmp(name, "Direct3DCreate9Ex")) { oD3DCreate9Ex = (void *)p; return (FARPROC)hD3DCreate9Ex; }
        if (!strcmp(name, "Direct3DCreate9")) { oD3DCreate9 = (void *)p; return (FARPROC)hD3DCreate9; }
    }
    return p;
}

static void hook_exe_iat(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void *real = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir->VirtualAddress); d->Name; d++) {
        void **iat = (void **)(base + d->FirstThunk);
        for (; *iat; iat++)
            if (*iat == real) { void *o; patch(iat, hGetProcAddress, &o); LOG("hooked GetProcAddress"); return; }
    }
    LOG("GetProcAddress import not found!");
}

/* ------------------------------------------------------------ the sink */
#define MAXQ 8
typedef struct Sink {
    IMFMediaSink IMFMediaSink_iface;
    IMFStreamSink IMFStreamSink_iface;
    IMFMediaTypeHandler IMFMediaTypeHandler_iface;
    IMFClockStateSink IMFClockStateSink_iface;
    IMFGetService IMFGetService_iface;
    IMFRateSupport IMFRateSupport_iface;
    IMFVideoDisplayControl IMFVideoDisplayControl_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    IMFMediaEventQueue *events;
    IMFPresentationClock *clock;
    IMFMediaType *type;
    UINT w, h; LONG stride;
    HWND hwnd;
    IMFSample *q[MAXQ]; int nq;
    int outstanding;
    BOOL started, shutdown;
    BOOL marker_pending; PROPVARIANT marker_ctx;
    HANDLE thread, stop_evt;
} Sink;

#define IMPL(iface) static inline Sink *from_##iface(iface *p) { return CONTAINING_RECORD(p, Sink, iface##_iface); }
IMPL(IMFMediaSink) IMPL(IMFStreamSink) IMPL(IMFMediaTypeHandler) IMPL(IMFClockStateSink)
IMPL(IMFGetService) IMPL(IMFRateSupport) IMPL(IMFVideoDisplayControl)

static HRESULT sink_qi(Sink *s, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFMediaSink)) *out = &s->IMFMediaSink_iface;
    else if (IsEqualIID(riid, &IID_IMFStreamSink) || IsEqualIID(riid, &IID_IMFMediaEventGenerator)) *out = &s->IMFStreamSink_iface;
    else if (IsEqualIID(riid, &IID_IMFMediaTypeHandler)) *out = &s->IMFMediaTypeHandler_iface;
    else if (IsEqualIID(riid, &IID_IMFClockStateSink)) *out = &s->IMFClockStateSink_iface;
    else if (IsEqualIID(riid, &IID_IMFGetService)) *out = &s->IMFGetService_iface;
    else if (IsEqualIID(riid, &IID_IMFRateSupport)) *out = &s->IMFRateSupport_iface;
    else if (IsEqualIID(riid, &IID_IMFVideoDisplayControl)) *out = &s->IMFVideoDisplayControl_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}
static ULONG sink_addref(Sink *s) { return InterlockedIncrement(&s->ref); }
static void flush_locked(Sink *s)
{
    for (int i = 0; i < s->nq; i++) IMFSample_Release(s->q[i]);
    s->nq = 0;
}
static ULONG sink_release(Sink *s)
{
    ULONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        flush_locked(s);
        if (s->events) IMFMediaEventQueue_Release(s->events);
        if (s->type) IMFMediaType_Release(s->type);
        PropVariantClear(&s->marker_ctx);
        DeleteCriticalSection(&s->cs);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return r;
}
#define UNK(iface) \
    static HRESULT WINAPI uQI_##iface(iface *p, REFIID r, void **o) { return sink_qi(from_##iface(p), r, o); } \
    static ULONG WINAPI uAddRef_##iface(iface *p) { return sink_addref(from_##iface(p)); } \
    static ULONG WINAPI uRelease_##iface(iface *p) { return sink_release(from_##iface(p)); }
UNK(IMFMediaSink) UNK(IMFStreamSink) UNK(IMFMediaTypeHandler) UNK(IMFClockStateSink)
UNK(IMFGetService) UNK(IMFRateSupport) UNK(IMFVideoDisplayControl)

/* copy a sample into the shared frame buffer (top-down BGRX) */
static void take_frame(Sink *s, IMFSample *smp)
{
    IMFMediaBuffer *buf; IMF2DBuffer *b2;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(smp, &buf))) return;
    BYTE *src = NULL; LONG pitch = s->stride; DWORD len;
    BOOL two = SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2));
    if (two && FAILED(IMF2DBuffer_Lock2D(b2, &src, &pitch))) { IMF2DBuffer_Release(b2); two = FALSE; }
    if (two) { static int l2; if (!l2++) LOG("2D buffer pitch %ld", pitch); }
    if (!two) {
        if (FAILED(IMFMediaBuffer_Lock(buf, &src, NULL, &len))) { IMFMediaBuffer_Release(buf); return; }
        /* Wine may report a stride in pixels; trust the buffer size instead */
        LONG need = (LONG)s->w * 4, sign = pitch < 0 ? -1 : 1;
        if (labs(pitch) < need) pitch = sign * (s->h ? (LONG)(len / s->h) : need);
        if (labs(pitch) < need || (DWORD)labs(pitch) * s->h > len) {
            static int warned; if (!warned++) LOG("bad buffer: len %lu, %ux%u", len, s->w, s->h);
            IMFMediaBuffer_Unlock(buf); IMFMediaBuffer_Release(buf); return;
        }
        if (pitch < 0) src += (s->h - 1) * -pitch; /* bottom-up: start at first visible row */
        static int logged; if (!logged++) LOG("buffer len %lu, pitch %ld", len, pitch);
    }
    EnterCriticalSection(&framelock);
    if (!frame || fw != s->w || fh != s->h) {
        HeapFree(GetProcessHeap(), 0, frame);
        frame = HeapAlloc(GetProcessHeap(), 0, s->w * s->h * 4);
        fw = s->w; fh = s->h;
    }
    for (UINT y = 0; y < s->h; y++) memcpy(frame + y * s->w * 4, src + (LONG)y * pitch, s->w * 4);
    frame_dirty = 1; playing = 1;
    LeaveCriticalSection(&framelock);
    if (two) { IMF2DBuffer_Unlock2D(b2); IMF2DBuffer_Release(b2); }
    else IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
}

/* pump: show due frames, keep samples coming, finish markers */
static DWORD WINAPI pump(void *arg)
{
    Sink *s = arg;
    while (WaitForSingleObject(s->stop_evt, 2) == WAIT_TIMEOUT) {
        EnterCriticalSection(&s->cs);
        if (s->started && !s->shutdown) {
            MFTIME now = 0, sys;
            if (s->clock) IMFPresentationClock_GetCorrelatedTime(s->clock, 0, &now, &sys);
            IMFSample *show = NULL;
            while (s->nq) {
                LONGLONG t = 0; IMFSample_GetSampleTime(s->q[0], &t);
                if (t > now) break;
                if (show) IMFSample_Release(show);
                show = s->q[0];
                memmove(s->q, s->q + 1, --s->nq * sizeof(*s->q));
            }
            if (show) { IMFSample_GetSampleTime(show, &frame_time); take_frame(s, show); IMFSample_Release(show); }
            while (!s->marker_pending && s->nq + s->outstanding < 4) {
                s->outstanding++;
                IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkRequestSample, &GUID_NULL, S_OK, NULL);
            }
            if (s->marker_pending && !s->nq) {
                s->marker_pending = FALSE;
                IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkMarker, &GUID_NULL, S_OK, &s->marker_ctx);
                PropVariantClear(&s->marker_ctx);
                LOG("end of segment");
            }
        }
        LeaveCriticalSection(&s->cs);
    }
    return 0;
}

/* --- IMFMediaSink */
static HRESULT WINAPI ms_GetCharacteristics(IMFMediaSink *p, DWORD *c) { *c = MEDIASINK_FIXED_STREAMS; return S_OK; }
static HRESULT WINAPI ms_AddStreamSink(IMFMediaSink *p, DWORD id, IMFMediaType *t, IMFStreamSink **o) { return MF_E_STREAMSINKS_FIXED; }
static HRESULT WINAPI ms_RemoveStreamSink(IMFMediaSink *p, DWORD id) { return MF_E_STREAMSINKS_FIXED; }
static HRESULT WINAPI ms_GetStreamSinkCount(IMFMediaSink *p, DWORD *n) { *n = 1; return S_OK; }
static HRESULT WINAPI ms_GetStreamSinkByIndex(IMFMediaSink *p, DWORD i, IMFStreamSink **o)
{
    if (i) return MF_E_INVALIDINDEX;
    *o = &from_IMFMediaSink(p)->IMFStreamSink_iface; IMFStreamSink_AddRef(*o); return S_OK;
}
static HRESULT WINAPI ms_GetStreamSinkById(IMFMediaSink *p, DWORD id, IMFStreamSink **o)
{
    if (id) return MF_E_INVALIDSTREAMNUMBER;
    *o = &from_IMFMediaSink(p)->IMFStreamSink_iface; IMFStreamSink_AddRef(*o); return S_OK;
}
static HRESULT WINAPI ms_SetPresentationClock(IMFMediaSink *p, IMFPresentationClock *c)
{
    Sink *s = from_IMFMediaSink(p);
    EnterCriticalSection(&s->cs);
    if (s->clock) {
        IMFPresentationClock_RemoveClockStateSink(s->clock, &s->IMFClockStateSink_iface);
        IMFPresentationClock_Release(s->clock);
    }
    s->clock = c;
    if (c) { IMFPresentationClock_AddRef(c); IMFPresentationClock_AddClockStateSink(c, &s->IMFClockStateSink_iface); }
    LeaveCriticalSection(&s->cs);
    return S_OK;
}
static HRESULT WINAPI ms_GetPresentationClock(IMFMediaSink *p, IMFPresentationClock **c)
{
    Sink *s = from_IMFMediaSink(p);
    if (!s->clock) return MF_E_NO_CLOCK;
    *c = s->clock; IMFPresentationClock_AddRef(*c); return S_OK;
}
static HRESULT WINAPI ms_Shutdown(IMFMediaSink *p)
{
    Sink *s = from_IMFMediaSink(p);
    LOG("sink shutdown");
    if (cur_sink_gs == &s->IMFGetService_iface) cur_sink_gs = NULL;
    SetEvent(s->stop_evt);
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread); CloseHandle(s->stop_evt);
    EnterCriticalSection(&s->cs);
    s->shutdown = TRUE; s->started = FALSE;
    flush_locked(s);
    if (s->clock) {
        IMFPresentationClock_RemoveClockStateSink(s->clock, &s->IMFClockStateSink_iface);
        IMFPresentationClock_Release(s->clock); s->clock = NULL;
    }
    IMFMediaEventQueue_Shutdown(s->events);
    LeaveCriticalSection(&s->cs);
    playing = 0;
    return S_OK;
}
static IMFMediaSinkVtbl ms_vtbl = {
    uQI_IMFMediaSink, uAddRef_IMFMediaSink, uRelease_IMFMediaSink,
    ms_GetCharacteristics, ms_AddStreamSink, ms_RemoveStreamSink, ms_GetStreamSinkCount,
    ms_GetStreamSinkByIndex, ms_GetStreamSinkById, ms_SetPresentationClock,
    ms_GetPresentationClock, ms_Shutdown,
};

/* --- IMFStreamSink (+ IMFMediaEventGenerator) */
static HRESULT WINAPI ss_GetEvent(IMFStreamSink *p, DWORD f, IMFMediaEvent **e)
{ return IMFMediaEventQueue_GetEvent(from_IMFStreamSink(p)->events, f, e); }
static HRESULT WINAPI ss_BeginGetEvent(IMFStreamSink *p, IMFAsyncCallback *cb, IUnknown *st)
{ return IMFMediaEventQueue_BeginGetEvent(from_IMFStreamSink(p)->events, cb, st); }
static HRESULT WINAPI ss_EndGetEvent(IMFStreamSink *p, IMFAsyncResult *r, IMFMediaEvent **e)
{ return IMFMediaEventQueue_EndGetEvent(from_IMFStreamSink(p)->events, r, e); }
static HRESULT WINAPI ss_QueueEvent(IMFStreamSink *p, MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT *v)
{ return IMFMediaEventQueue_QueueEventParamVar(from_IMFStreamSink(p)->events, t, g, hr, v); }
static HRESULT WINAPI ss_GetMediaSink(IMFStreamSink *p, IMFMediaSink **o)
{ *o = &from_IMFStreamSink(p)->IMFMediaSink_iface; IMFMediaSink_AddRef(*o); return S_OK; }
static HRESULT WINAPI ss_GetIdentifier(IMFStreamSink *p, DWORD *id) { *id = 0; return S_OK; }
static HRESULT WINAPI ss_GetMediaTypeHandler(IMFStreamSink *p, IMFMediaTypeHandler **o)
{ *o = &from_IMFStreamSink(p)->IMFMediaTypeHandler_iface; IMFMediaTypeHandler_AddRef(*o); return S_OK; }
static HRESULT WINAPI ss_ProcessSample(IMFStreamSink *p, IMFSample *smp)
{
    Sink *s = from_IMFStreamSink(p);
    EnterCriticalSection(&s->cs);
    if (s->outstanding) s->outstanding--;
    if (s->nq < MAXQ) { s->q[s->nq++] = smp; IMFSample_AddRef(smp); }
    LeaveCriticalSection(&s->cs);
    return S_OK;
}
static HRESULT WINAPI ss_PlaceMarker(IMFStreamSink *p, MFSTREAMSINK_MARKER_TYPE t, const PROPVARIANT *val, const PROPVARIANT *ctx)
{
    Sink *s = from_IMFStreamSink(p);
    EnterCriticalSection(&s->cs);
    if (t == MFSTREAMSINK_MARKER_ENDOFSEGMENT && s->nq) {
        PropVariantClear(&s->marker_ctx);
        if (ctx) PropVariantCopy(&s->marker_ctx, ctx);
        s->marker_pending = TRUE;
    } else
        IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkMarker, &GUID_NULL, S_OK, ctx);
    LeaveCriticalSection(&s->cs);
    return S_OK;
}
static HRESULT WINAPI ss_Flush(IMFStreamSink *p)
{
    Sink *s = from_IMFStreamSink(p);
    EnterCriticalSection(&s->cs);
    flush_locked(s); s->outstanding = 0;
    if (s->marker_pending) {
        s->marker_pending = FALSE;
        IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkMarker, &GUID_NULL, E_ABORT, &s->marker_ctx);
        PropVariantClear(&s->marker_ctx);
    }
    LeaveCriticalSection(&s->cs);
    return S_OK;
}
static IMFStreamSinkVtbl ss_vtbl = {
    uQI_IMFStreamSink, uAddRef_IMFStreamSink, uRelease_IMFStreamSink,
    ss_GetEvent, ss_BeginGetEvent, ss_EndGetEvent, ss_QueueEvent,
    ss_GetMediaSink, ss_GetIdentifier, ss_GetMediaTypeHandler, ss_ProcessSample, ss_PlaceMarker, ss_Flush,
};

/* --- IMFMediaTypeHandler: accept uncompressed RGB32 only */
static HRESULT WINAPI th_IsMediaTypeSupported(IMFMediaTypeHandler *p, IMFMediaType *t, IMFMediaType **closest)
{
    GUID major, sub;
    if (closest) *closest = NULL;
    if (FAILED(IMFMediaType_GetGUID(t, &MF_MT_MAJOR_TYPE, &major)) || !IsEqualGUID(&major, &MFMediaType_Video)) return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub))) return MF_E_INVALIDMEDIATYPE;
    if (!IsEqualGUID(&sub, &MFVideoFormat_RGB32) && !IsEqualGUID(&sub, &MFVideoFormat_ARGB32)) return MF_E_INVALIDMEDIATYPE;
    return S_OK;
}
static HRESULT WINAPI th_GetMediaTypeCount(IMFMediaTypeHandler *p, DWORD *n) { *n = 1; return S_OK; }
static HRESULT WINAPI th_GetMediaTypeByIndex(IMFMediaTypeHandler *p, DWORD i, IMFMediaType **t)
{
    if (i) return MF_E_NO_MORE_TYPES;
    HRESULT hr = MFCreateMediaType(t);
    if (FAILED(hr)) return hr;
    IMFMediaType_SetGUID(*t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(*t, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    return S_OK;
}
static HRESULT WINAPI th_SetCurrentMediaType(IMFMediaTypeHandler *p, IMFMediaType *t)
{
    Sink *s = from_IMFMediaTypeHandler(p);
    HRESULT hr = th_IsMediaTypeSupported(p, t, NULL);
    if (FAILED(hr)) return hr;
    UINT64 sz = 0; UINT32 st = 0;
    IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &sz);
    EnterCriticalSection(&s->cs);
    if (s->type) IMFMediaType_Release(s->type);
    s->type = t; IMFMediaType_AddRef(t);
    s->w = (UINT)(sz >> 32); s->h = (UINT)sz;
    if (SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &st))) s->stride = (LONG)st;
    else s->stride = s->w * 4;
    LeaveCriticalSection(&s->cs);
    LOG("media type set: %ux%u stride %ld", s->w, s->h, s->stride);
    return S_OK;
}
static HRESULT WINAPI th_GetCurrentMediaType(IMFMediaTypeHandler *p, IMFMediaType **t)
{
    Sink *s = from_IMFMediaTypeHandler(p);
    if (!s->type) return MF_E_NOT_INITIALIZED;
    *t = s->type; IMFMediaType_AddRef(*t); return S_OK;
}
static HRESULT WINAPI th_GetMajorType(IMFMediaTypeHandler *p, GUID *g) { *g = MFMediaType_Video; return S_OK; }
static IMFMediaTypeHandlerVtbl th_vtbl = {
    uQI_IMFMediaTypeHandler, uAddRef_IMFMediaTypeHandler, uRelease_IMFMediaTypeHandler,
    th_IsMediaTypeSupported, th_GetMediaTypeCount, th_GetMediaTypeByIndex,
    th_SetCurrentMediaType, th_GetCurrentMediaType, th_GetMajorType,
};

/* --- IMFClockStateSink */

static HRESULT WINAPI cs_OnClockStart(IMFClockStateSink *p, MFTIME sys, LONGLONG off)
{
    Sink *s = from_IMFClockStateSink(p);
    EnterCriticalSection(&s->cs);
    if (off != PRESENTATION_CURRENT_POSITION) flush_locked(s);
    s->started = TRUE; s->outstanding = 0;
    IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkStarted, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&s->cs);
    movie_hwnd = s->hwnd; paused = 0;
    trace_frames = 4; /* draw tracing off */
    LOG("clock start");
    return S_OK;
}
static HRESULT WINAPI cs_OnClockStop(IMFClockStateSink *p, MFTIME sys)
{
    Sink *s = from_IMFClockStateSink(p);
    EnterCriticalSection(&s->cs);
    s->started = FALSE; flush_locked(s); s->outstanding = 0;
    IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkStopped, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&s->cs);
    playing = 0;
    LOG("clock stop");
    return S_OK;
}
static HRESULT WINAPI cs_OnClockPause(IMFClockStateSink *p, MFTIME sys)
{
    LOG("clock pause");
    paused = 1;
    Sink *s = from_IMFClockStateSink(p);
    EnterCriticalSection(&s->cs);
    s->started = FALSE;
    IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkPaused, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&s->cs);
    return S_OK;
}
static HRESULT WINAPI cs_OnClockRestart(IMFClockStateSink *p, MFTIME sys)
{
    LOG("clock restart");
    paused = 0;
    Sink *s = from_IMFClockStateSink(p);
    EnterCriticalSection(&s->cs);
    s->started = TRUE;
    IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkStarted, &GUID_NULL, S_OK, NULL);
    LeaveCriticalSection(&s->cs);
    return S_OK;
}
static HRESULT WINAPI cs_OnClockSetRate(IMFClockStateSink *p, MFTIME sys, float rate)
{
    Sink *s = from_IMFClockStateSink(p);
    IMFMediaEventQueue_QueueEventParamVar(s->events, MEStreamSinkRateChanged, &GUID_NULL, S_OK, NULL);
    return S_OK;
}
static IMFClockStateSinkVtbl cs_vtbl = {
    uQI_IMFClockStateSink, uAddRef_IMFClockStateSink, uRelease_IMFClockStateSink,
    cs_OnClockStart, cs_OnClockStop, cs_OnClockPause, cs_OnClockRestart, cs_OnClockSetRate,
};

/* --- IMFGetService */
static HRESULT WINAPI gs_GetService(IMFGetService *p, REFGUID svc, REFIID riid, void **o)
{
    Sink *s = from_IMFGetService(p);
    if (IsEqualIID(riid, &IID_IMFVideoDisplayControl) || IsEqualIID(riid, &IID_IMFRateSupport))
        return sink_qi(s, riid, o);
    *o = NULL;
    return MF_E_UNSUPPORTED_SERVICE;
}
static IMFGetServiceVtbl gs_vtbl = { uQI_IMFGetService, uAddRef_IMFGetService, uRelease_IMFGetService, gs_GetService };

/* --- IMFRateSupport */
static HRESULT WINAPI rs_GetSlowestRate(IMFRateSupport *p, MFRATE_DIRECTION d, BOOL thin, float *r) { *r = 0.0f; return S_OK; }
static HRESULT WINAPI rs_GetFastestRate(IMFRateSupport *p, MFRATE_DIRECTION d, BOOL thin, float *r) { *r = d == MFRATE_FORWARD ? 64.0f : -64.0f; return S_OK; }
static HRESULT WINAPI rs_IsRateSupported(IMFRateSupport *p, BOOL thin, float r, float *n) { if (n) *n = r; return S_OK; }
static IMFRateSupportVtbl rs_vtbl = {
    uQI_IMFRateSupport, uAddRef_IMFRateSupport, uRelease_IMFRateSupport,
    rs_GetSlowestRate, rs_GetFastestRate, rs_IsRateSupported,
};

/* --- IMFVideoDisplayControl: enough for the game's calls */
static HRESULT WINAPI dc_GetNativeVideoSize(IMFVideoDisplayControl *p, SIZE *v, SIZE *ar)
{
    Sink *s = from_IMFVideoDisplayControl(p);
    LOG("DC GetNativeVideoSize -> %ux%u (ar %p)", s->w, s->h, ar);
    if (v) { v->cx = s->w; v->cy = s->h; }
    if (ar) { ar->cx = s->w; ar->cy = s->h; }
    return S_OK;
}
static HRESULT WINAPI dc_GetIdealVideoSize(IMFVideoDisplayControl *p, SIZE *mn, SIZE *mx)
{ LOG("DC GetIdealVideoSize");
    Sink *s = from_IMFVideoDisplayControl(p);
    if (mn) { mn->cx = 1; mn->cy = 1; }
    if (mx) { mx->cx = s->w; mx->cy = s->h; }
    return S_OK;
}
static HRESULT WINAPI dc_SetVideoPosition(IMFVideoDisplayControl *p, const MFVideoNormalizedRect *n, RECT *const r)
{ if (r) LOG("DC SetVideoPosition %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom); else LOG("DC SetVideoPosition NULL"); return S_OK; }
static HRESULT WINAPI dc_GetVideoPosition(IMFVideoDisplayControl *p, MFVideoNormalizedRect *n, RECT *r)
{ LOG("DC GetVideoPosition");
    Sink *s = from_IMFVideoDisplayControl(p);
    if (n) { n->left = n->top = 0; n->right = n->bottom = 1; }
    if (r) SetRect(r, 0, 0, s->w, s->h);
    return S_OK;
}
static HRESULT WINAPI dc_SetAspectRatioMode(IMFVideoDisplayControl *p, DWORD m) { LOG("DC SetAspectRatioMode"); return S_OK; }
static HRESULT WINAPI dc_GetAspectRatioMode(IMFVideoDisplayControl *p, DWORD *m) { *m = MFVideoARMode_PreservePicture; return S_OK; }
static HRESULT WINAPI dc_SetVideoWindow(IMFVideoDisplayControl *p, HWND w) { LOG("DC SetVideoWindow"); from_IMFVideoDisplayControl(p)->hwnd = w; return S_OK; }
static HRESULT WINAPI dc_GetVideoWindow(IMFVideoDisplayControl *p, HWND *w) { *w = from_IMFVideoDisplayControl(p)->hwnd; return S_OK; }
static HRESULT WINAPI dc_RepaintVideo(IMFVideoDisplayControl *p) { LOG("DC RepaintVideo"); return S_OK; }
static HRESULT WINAPI dc_GetCurrentImage(IMFVideoDisplayControl *p, BITMAPINFOHEADER *b, BYTE **d, DWORD *n, LONGLONG *t)
{
    static int logged;
    if (!b || !d || !n) return E_POINTER;
    EnterCriticalSection(&framelock);
    if (!frame) { LeaveCriticalSection(&framelock); return MF_E_INVALIDREQUEST; }
    DWORD size = fw * fh * 4;
    BYTE *out = CoTaskMemAlloc(size);
    if (!out) { LeaveCriticalSection(&framelock); return E_OUTOFMEMORY; }
    /* bottom-up 32-bit DIB, like a GDI bitmap */
    for (UINT y = 0; y < fh; y++) memcpy(out + (fh - 1 - y) * fw * 4, frame + y * fw * 4, fw * 4);
    memset(b, 0, sizeof(*b));
    b->biSize = sizeof(*b); b->biWidth = fw; b->biHeight = fh; b->biPlanes = 1;
    b->biBitCount = 32; b->biCompression = BI_RGB; b->biSizeImage = size;
    *d = out; *n = size; if (t) *t = frame_time;
    LeaveCriticalSection(&framelock);
    last_grab = GetTickCount();
    if (logged++ < 3) LOG("DC GetCurrentImage %ux%u", fw, fh);
    return S_OK;
}
static HRESULT WINAPI dc_SetBorderColor(IMFVideoDisplayControl *p, COLORREF c) { LOG("DC SetBorderColor"); return S_OK; }
static HRESULT WINAPI dc_GetBorderColor(IMFVideoDisplayControl *p, COLORREF *c) { *c = 0; return S_OK; }
static HRESULT WINAPI dc_SetRenderingPrefs(IMFVideoDisplayControl *p, DWORD f) { LOG("DC SetRenderingPrefs"); return S_OK; }
static HRESULT WINAPI dc_GetRenderingPrefs(IMFVideoDisplayControl *p, DWORD *f) { *f = 0; return S_OK; }
static HRESULT WINAPI dc_SetFullscreen(IMFVideoDisplayControl *p, BOOL f) { LOG("DC SetFullscreen"); return S_OK; }
static HRESULT WINAPI dc_GetFullscreen(IMFVideoDisplayControl *p, BOOL *f) { *f = FALSE; return S_OK; }
static IMFVideoDisplayControlVtbl dc_vtbl = {
    uQI_IMFVideoDisplayControl, uAddRef_IMFVideoDisplayControl, uRelease_IMFVideoDisplayControl,
    dc_GetNativeVideoSize, dc_GetIdealVideoSize, dc_SetVideoPosition, dc_GetVideoPosition,
    dc_SetAspectRatioMode, dc_GetAspectRatioMode, dc_SetVideoWindow, dc_GetVideoWindow,
    dc_RepaintVideo, dc_GetCurrentImage, dc_SetBorderColor, dc_GetBorderColor,
    dc_SetRenderingPrefs, dc_GetRenderingPrefs, dc_SetFullscreen, dc_GetFullscreen,
};

static Sink *sink_create(HWND hwnd)
{
    Sink *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    s->IMFMediaSink_iface.lpVtbl = &ms_vtbl;
    s->IMFStreamSink_iface.lpVtbl = &ss_vtbl;
    s->IMFMediaTypeHandler_iface.lpVtbl = &th_vtbl;
    s->IMFClockStateSink_iface.lpVtbl = &cs_vtbl;
    s->IMFGetService_iface.lpVtbl = &gs_vtbl;
    s->IMFRateSupport_iface.lpVtbl = &rs_vtbl;
    s->IMFVideoDisplayControl_iface.lpVtbl = &dc_vtbl;
    s->ref = 1; s->hwnd = hwnd;
    InitializeCriticalSection(&s->cs);
    MFCreateEventQueue(&s->events);
    s->stop_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
    s->thread = CreateThread(NULL, 0, pump, s, 0, NULL);
    cur_sink_gs = &s->IMFGetService_iface;
    return s;
}

/* ------------------------------------------------------- IMFActivate */
typedef struct Activate {
    IMFActivate IMFActivate_iface;
    IMFGetService IMFGetService_iface;
    LONG ref;
    IMFAttributes *attrs;
    HWND hwnd;
    Sink *sink;
} Activate;
static inline Activate *act(IMFActivate *p) { return CONTAINING_RECORD(p, Activate, IMFActivate_iface); }
static HRESULT WINAPI a_QI(IMFActivate *p, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IMFGetService)) { *o = &act(p)->IMFGetService_iface; IMFActivate_AddRef(p); return S_OK; }
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IMFAttributes) || IsEqualIID(r, &IID_IMFActivate)) {
        *o = p; IMFActivate_AddRef(p); return S_OK;
    }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI a_AddRef(IMFActivate *p) { return InterlockedIncrement(&act(p)->ref); }
static ULONG WINAPI a_Release(IMFActivate *p)
{
    Activate *a = act(p); ULONG r = InterlockedDecrement(&a->ref);
    if (!r) {
        if (a->sink) IMFMediaSink_Release(&a->sink->IMFMediaSink_iface);
        IMFAttributes_Release(a->attrs); HeapFree(GetProcessHeap(), 0, a);
    }
    return r;
}
#define A(name, decl, args) static HRESULT WINAPI a_##name decl { return IMFAttributes_##name args; }
#define AT act(p)->attrs
A(GetItem, (IMFActivate *p, REFGUID k, PROPVARIANT *v), (AT, k, v))
A(GetItemType, (IMFActivate *p, REFGUID k, MF_ATTRIBUTE_TYPE *t), (AT, k, t))
A(CompareItem, (IMFActivate *p, REFGUID k, REFPROPVARIANT v, BOOL *r), (AT, k, v, r))
A(Compare, (IMFActivate *p, IMFAttributes *o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL *r), (AT, o, t, r))
A(GetUINT32, (IMFActivate *p, REFGUID k, UINT32 *v), (AT, k, v))
A(GetUINT64, (IMFActivate *p, REFGUID k, UINT64 *v), (AT, k, v))
A(GetDouble, (IMFActivate *p, REFGUID k, double *v), (AT, k, v))
A(GetGUID, (IMFActivate *p, REFGUID k, GUID *v), (AT, k, v))
A(GetStringLength, (IMFActivate *p, REFGUID k, UINT32 *v), (AT, k, v))
A(GetString, (IMFActivate *p, REFGUID k, LPWSTR v, UINT32 n, UINT32 *l), (AT, k, v, n, l))
A(GetAllocatedString, (IMFActivate *p, REFGUID k, LPWSTR *v, UINT32 *l), (AT, k, v, l))
A(GetBlobSize, (IMFActivate *p, REFGUID k, UINT32 *v), (AT, k, v))
A(GetBlob, (IMFActivate *p, REFGUID k, UINT8 *b, UINT32 n, UINT32 *l), (AT, k, b, n, l))
A(GetAllocatedBlob, (IMFActivate *p, REFGUID k, UINT8 **b, UINT32 *l), (AT, k, b, l))
A(GetUnknown, (IMFActivate *p, REFGUID k, REFIID r, LPVOID *v), (AT, k, r, v))
A(SetItem, (IMFActivate *p, REFGUID k, REFPROPVARIANT v), (AT, k, v))
A(DeleteItem, (IMFActivate *p, REFGUID k), (AT, k))
A(DeleteAllItems, (IMFActivate *p), (AT))
A(SetUINT32, (IMFActivate *p, REFGUID k, UINT32 v), (AT, k, v))
A(SetUINT64, (IMFActivate *p, REFGUID k, UINT64 v), (AT, k, v))
A(SetDouble, (IMFActivate *p, REFGUID k, double v), (AT, k, v))
A(SetGUID, (IMFActivate *p, REFGUID k, REFGUID v), (AT, k, v))
A(SetString, (IMFActivate *p, REFGUID k, LPCWSTR v), (AT, k, v))
A(SetBlob, (IMFActivate *p, REFGUID k, const UINT8 *b, UINT32 n), (AT, k, b, n))
A(SetUnknown, (IMFActivate *p, REFGUID k, IUnknown *v), (AT, k, v))
A(LockStore, (IMFActivate *p), (AT))
A(UnlockStore, (IMFActivate *p), (AT))
A(GetCount, (IMFActivate *p, UINT32 *n), (AT, n))
A(GetItemByIndex, (IMFActivate *p, UINT32 i, GUID *k, PROPVARIANT *v), (AT, i, k, v))
A(CopyAllItems, (IMFActivate *p, IMFAttributes *d), (AT, d))
static HRESULT WINAPI a_ActivateObject(IMFActivate *p, REFIID r, void **o)
{
    Activate *a = act(p);
    if (!a->sink) a->sink = sink_create(a->hwnd);
    LOG("ActivateObject");
    return sink_qi(a->sink, r, o);
}
static HRESULT WINAPI a_ShutdownObject(IMFActivate *p)
{
    Activate *a = act(p);
    if (a->sink) { IMFMediaSink_Release(&a->sink->IMFMediaSink_iface); a->sink = NULL; }
    return S_OK;
}
static HRESULT WINAPI a_DetachObject(IMFActivate *p) { return a_ShutdownObject(p); }
static IMFActivateVtbl a_vtbl = {
    a_QI, a_AddRef, a_Release,
    a_GetItem, a_GetItemType, a_CompareItem, a_Compare, a_GetUINT32, a_GetUINT64, a_GetDouble,
    a_GetGUID, a_GetStringLength, a_GetString, a_GetAllocatedString, a_GetBlobSize, a_GetBlob,
    a_GetAllocatedBlob, a_GetUnknown, a_SetItem, a_DeleteItem, a_DeleteAllItems, a_SetUINT32,
    a_SetUINT64, a_SetDouble, a_SetGUID, a_SetString, a_SetBlob, a_SetUnknown, a_LockStore,
    a_UnlockStore, a_GetCount, a_GetItemByIndex, a_CopyAllItems,
    a_ActivateObject, a_ShutdownObject, a_DetachObject,
};

/* the session asks the activate object for MR_VIDEO_RENDER_SERVICE */
static inline Activate *act_gs(IMFGetService *p) { return CONTAINING_RECORD(p, Activate, IMFGetService_iface); }
static HRESULT WINAPI ags_QI(IMFGetService *p, REFIID r, void **o) { return a_QI(&act_gs(p)->IMFActivate_iface, r, o); }
static ULONG WINAPI ags_AddRef(IMFGetService *p) { return a_AddRef(&act_gs(p)->IMFActivate_iface); }
static ULONG WINAPI ags_Release(IMFGetService *p) { return a_Release(&act_gs(p)->IMFActivate_iface); }
static HRESULT WINAPI ags_GetService(IMFGetService *p, REFGUID svc, REFIID r, void **o)
{
    Activate *a = act_gs(p);
    if (!a->sink) a->sink = sink_create(a->hwnd);
    HRESULT hr = gs_GetService(&a->sink->IMFGetService_iface, svc, r, o);
    LOG("activate GetService -> %08lx", hr);
    return hr;
}
static IMFGetServiceVtbl ags_vtbl = { ags_QI, ags_AddRef, ags_Release, ags_GetService };

__declspec(dllexport) HRESULT WINAPI MFCreateVideoRendererActivate(HWND hwnd, IMFActivate **out)
{
    if (GetFileAttributesA("videohook.passthrough") != INVALID_FILE_ATTRIBUTES) {
        static HRESULT (WINAPI *real)(HWND, IMFActivate **);
        if (!real) real = (void *)realfn("MFCreateVideoRendererActivate");
        movie_hwnd = hwnd; passthrough = 1; playing = 0;
        LOG("PASSTHROUGH: real EVR, hwnd=%p", hwnd);
        return real(hwnd, out);
    }
    passthrough = 0;
    Activate *a = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*a));
    a->IMFActivate_iface.lpVtbl = &a_vtbl;
    a->IMFGetService_iface.lpVtbl = &ags_vtbl;
    a->ref = 1; a->hwnd = hwnd;
    MFCreateAttributes(&a->attrs, 0);
    *out = &a->IMFActivate_iface;
    LOG("MFCreateVideoRendererActivate hwnd=%p", hwnd);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *res)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&loglock);
        InitializeCriticalSection(&framelock);
        LOG("mg.dll loaded");
        hook_exe_iat();
    }
    return TRUE;
}
