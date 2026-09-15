// ---------------------------------------------------------------------------
// d3d11_testbed.cpp
//
// A deliberately ORDINARY Direct3D 11 application, used as a test target for
// a multi-GPU DXVK fork.
//
// DESIGN RULE: this program must contain NO multi-GPU awareness whatsoever.
// It creates a device on the default adapter, exactly as a normal game would,
// and never enumerates, selects, or reasons about physical devices. Any
// splitting of work across GPUs must happen entirely below the D3D11 API,
// inside the DXVK fork. If this app ever needs to know about a second GPU,
// the fork has failed to be app-agnostic.
//
// FEATURE AXES
// ------------
// Every advanced technique below is behind its own flag and defaults OFF, so
// one build covers every combination without a rebuild. The axes were chosen
// from two sources: the structural shape of Superposition as seen in its DXVK
// logs (renders at full resolution, presents into a smaller window, keeps a
// halving pyramid of render targets, hundreds of draws across many passes),
// and the fork's own list of D3D11 entry points that are NOT mirrored to the
// worker device -- CopyResource, CopySubresourceRegion, ResolveSubresource,
// Dispatch and unordered access views. An application using any of those gets
// correct output on GPU0 and silently incomplete output on GPU1.
//
//   -renderscale F  scene renders at F x backbuffer size, blit rescales
//   -passes N       N chained full-res post passes (ping-pong)
//   -pyramid N      N halving downsample passes, Superposition-style
//   -msaa N         multisampled scene target + ResolveSubresource   [UNMIRRORED]
//   -depthread      sample the depth buffer as an SRV in a post pass
//   -compute        compute shader writing a UAV, sampled in post     [UNMIRRORED]
//   -copyres        CopyResource into a scratch texture before blit   [UNMIRRORED]
//   -copyregion     CopySubresourceRegion of a sub-rect               [UNMIRRORED]
//   -updatesub      constant buffer via UpdateSubresource, not Map
//
// Build (MSYS2 MinGW64 shell):
//   g++ -std=c++17 -O2 -I"/c/Program Files/RenderDoc" \
//       -static -static-libgcc -static-libstdc++ \
//       -o d3d11_testbed.exe d3d11_testbed.cpp -ld3d11 -ldxgi -luser32 -lgdi32
//
// d3dcompiler_47.dll is loaded at RUNTIME (LoadLibraryA/GetProcAddress), so
// there is no link-time dependency on libd3dcompiler.a.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcommon.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include "renderdoc_app.h"

static RENDERDOC_API_1_1_2* g_rdoc = nullptr;

static void RdocInit() {
    HMODULE mod = LoadLibraryA("C:\\Program Files\\RenderDoc\\renderdoc.dll");

    if (!mod) {
        printf("rdoc: LoadLibrary failed\n");
        return;
    }

    auto getApi = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");

    if (!getApi || !getApi(eRENDERDOC_API_Version_1_1_2, (void**)&g_rdoc)) {
        printf("rdoc: GetAPI failed\n");
        g_rdoc = nullptr;
        return;
    }

    printf("rdoc: ready\n");
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    int      width          = 1280;
    int      height         = 720;
    int      frames         = 600;    // 0 = run until closed
    int      vramMB         = 512;    // ballast texture pool size
    int      texSize        = 1024;   // ballast texture dimension
    int      psLoad         = 16;     // pixel shader inner loop iterations
    int      drawsPerFrame  = 32;     // draw calls per frame
    int      instances      = 64;     // instances per draw call
    bool     offscreen      = true;   // render to offscreen RT, then blit
    bool     clearOnly      = false;  // clear + present only, no geometry
    bool     vsync          = false;
    bool     debugLayer     = false;
    bool     quiet          = false;  // suppress console echo
    int      logCallFrames  = 2;      // log every API call for first N frames
    int      statsInterval  = 60;     // print frame stats every N frames
    std::string logPath     = "d3d11_testbed.log";
    std::string tag;                  // free-text label echoed into the log

    // --- feature axes, all default off ---
    float    renderScale    = 1.0f;   // scene resolution multiplier
    int      postPasses     = 0;      // chained full-res post passes
    int      pyramid        = 0;      // halving downsample passes
    int      msaa           = 1;      // scene target sample count
    bool     depthRead      = false;  // sample depth as SRV
    bool     compute        = false;  // CS writing a UAV, sampled in post
    bool     indirect       = false;  // CS writes draw args; scene drawn indirect
    bool     deferred       = false;  // scene pass recorded on a deferred context
    bool     copyRes        = false;  // CopyResource before the blit
    bool     copyRegion     = false;  // CopySubresourceRegion of a sub-rect
    bool     updateSub      = false;  // UpdateSubresource instead of Map
};

static Config g_cfg;

// ---------------------------------------------------------------------------
// Logging
//
// Every line is flushed immediately. A truncated log after a crash is worse
// than no log at all, and buffered output is exactly how you lose the last
// few lines before a device-lost or a driver hang.
// ---------------------------------------------------------------------------

class Logger {
public:
    bool open(const std::string& path) {
        m_file = fopen(path.c_str(), "w");
        if (!m_file) {
            fprintf(stderr, "[testbed] FATAL: cannot open log file '%s'\n", path.c_str());
            return false;
        }
        m_start = std::chrono::steady_clock::now();
        return true;
    }

    void close() {
        if (m_file) { fclose(m_file); m_file = nullptr; }
    }

    void write(const char* level, const char* fmt, ...) {
        char body[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);

        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - m_start).count();

        char line[4352];
        snprintf(line, sizeof(line), "[%10.3f] [%-5s] %s\n", ms, level, body);

        if (m_file) { fputs(line, m_file); fflush(m_file); }
        if (!g_cfg.quiet) { fputs(line, stdout); fflush(stdout); }
    }

private:
    FILE* m_file = nullptr;
    std::chrono::steady_clock::time_point m_start;
};

static Logger g_log;

#define LOG_INFO(...)  g_log.write("info",  __VA_ARGS__)
#define LOG_WARN(...)  g_log.write("warn",  __VA_ARGS__)
#define LOG_ERR(...)   g_log.write("error", __VA_ARGS__)

// Per-call trace. Enabled only for the first N frames so the log stays
// readable; set -logcalls 0 to disable entirely, or a large number to trace
// a whole run.
static int g_frameIndex = 0;
#define LOG_CALL(...) \
    do { if (g_frameIndex < g_cfg.logCallFrames) g_log.write("call", __VA_ARGS__); } while (0)

static bool checkHR(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        LOG_ERR("%s FAILED (hr=0x%08lX)", what, (unsigned long)hr);
        return false;
    }
    LOG_INFO("%s ok", what);
    return true;
}

// ---------------------------------------------------------------------------
// Minimal row-major 4x4 math
//
// Uploaded row-major and declared `row_major` in HLSL, so mul(v, m) uses the
// row-vector convention and no transpose is needed anywhere.
// ---------------------------------------------------------------------------

struct Mat4 { float m[4][4]; };

static Mat4 matIdentity() {
    Mat4 r{};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1.0f;
    return r;
}

static Mat4 matMul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

static Mat4 matPerspectiveLH(float fovY, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovY * 0.5f);
    Mat4 r{};
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = zf / (zf - zn);
    r.m[2][3] = 1.0f;
    r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}

static void vecNorm(float v[3]) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void vecCross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

static float vecDot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static Mat4 matLookAtLH(const float eye[3], const float at[3], const float up[3]) {
    float z[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] };
    vecNorm(z);
    float x[3]; vecCross(up, z, x); vecNorm(x);
    float y[3]; vecCross(z, x, y);

    Mat4 r = matIdentity();
    r.m[0][0] = x[0]; r.m[0][1] = y[0]; r.m[0][2] = z[0];
    r.m[1][0] = x[1]; r.m[1][1] = y[1]; r.m[1][2] = z[1];
    r.m[2][0] = x[2]; r.m[2][1] = y[2]; r.m[2][2] = z[2];
    r.m[3][0] = -vecDot(x, eye);
    r.m[3][1] = -vecDot(y, eye);
    r.m[3][2] = -vecDot(z, eye);
    return r;
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

struct PerFrameCB {
    Mat4  viewProj;
    float time;
    float psLoad;
    float instanceCount;
    float drawIndex;
};

struct PostCB {
    float auxWeight;
    float depthWeight;
    float passIndex;
    float pad;
};

static const char* kSceneShaderHLSL = R"(
cbuffer PerFrame : register(b0) {
    row_major float4x4 viewProj;
    float time;
    float psLoad;
    float instanceCount;
    float drawIndex;
};

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);

struct VSIn {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    uint   iid    : SV_InstanceID;
};

struct VSOut {
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    float  shade  : TEXCOORD1;
};

VSOut VSMain(VSIn i) {
    // Scatter instances over a rotating lattice. Done procedurally rather
    // than with an instance buffer purely to keep the vertex layout simple;
    // the vertex and index buffers are still real and still bound.
    uint  n     = (uint)max(instanceCount, 1.0);
    uint  side  = (uint)ceil(pow((float)n, 1.0 / 3.0));
    uint  ix    = i.iid % side;
    uint  iy    = (i.iid / side) % side;
    uint  iz    = (i.iid / (side * side)) % side;

    float spread = 3.0;
    float3 offset = float3(
        ((float)ix - (float)side * 0.5) * spread,
        ((float)iy - (float)side * 0.5) * spread,
        ((float)iz - (float)side * 0.5) * spread);

    float  a = time * 0.6 + (float)i.iid * 0.37 + drawIndex * 0.11;
    float  s = sin(a), c = cos(a);
    float3 p = i.pos;
    float3 rp = float3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    float3 rn = float3(i.normal.x * c - i.normal.z * s, i.normal.y,
                       i.normal.x * s + i.normal.z * c);

    VSOut o;
    o.pos    = mul(float4(rp + offset, 1.0), viewProj);
    o.normal = rn;
    o.uv     = i.uv;
    o.shade  = 0.35 + 0.65 * saturate(dot(normalize(rn), normalize(float3(0.4, 0.8, -0.3))));
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    // Configurable ALU + texture load. SampleLevel (not Sample) so the
    // compiler does not reject a gradient computed inside a loop.
    float4 acc = 0.0;
    int    n   = (int)max(psLoad, 1.0);

    [loop]
    for (int k = 0; k < n; k++) {
        float fk = (float)k;
        float2 uv = i.uv + float2(sin(fk * 0.13 + time), cos(fk * 0.17 + time)) * 0.02;
        acc += gTex.SampleLevel(gSamp, uv, 0.0);
        acc.rgb += 0.001 * float3(sin(fk), cos(fk * 1.3), sin(fk * 0.7));
    }

    acc /= (float)n;
    acc.rgb *= i.shade;
    acc.a = 1.0;
    return acc;
}
)";

// Post / blit shader. One shader serves the plain blit, the chained post
// passes and the downsample pyramid; the differences are all in what is bound
// and where it writes. Auxiliary inputs are weighted by a constant buffer so
// an unused input contributes nothing rather than needing a second shader.
static const char* kPostShaderHLSL = R"(
cbuffer PostParams : register(b0) {
    float auxWeight;
    float depthWeight;
    float passIndex;
    float pad;
};

Texture2D    gSrc   : register(t0);
Texture2D    gAux   : register(t1);
Texture2D    gDepth : register(t2);
SamplerState gSamp  : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle from SV_VertexID; no vertex buffer bound on purpose.
VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 c = gSrc.SampleLevel(gSamp, i.uv, 0.0).rgb;

    // Compute-shader output, when enabled. Kept as a mild modulation so the
    // image stays recognisable: the point is that the sample happens, not
    // that it dominates.
    float3 aux = gAux.SampleLevel(gSamp, i.uv, 0.0).rgb;
    c = lerp(c, c * (0.6 + 0.8 * aux), saturate(auxWeight));

    // Depth as a shader resource, when enabled.
    float d = gDepth.SampleLevel(gSamp, i.uv, 0.0).r;
    c = lerp(c, c * (0.7 + 0.3 * d), saturate(depthWeight));

    c = pow(saturate(c), 1.0 / 1.05);   // token post step, keeps the pass honest
    return float4(c, 1.0);
}
)";

// Compute shader writing an unordered access view. Dispatch and UAV binds are
// both on the fork's unmirrored list, which is exactly why this axis exists.
static const char* kComputeShaderHLSL = R"(
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float2 uv = (float2)id.xy / 256.0;
    float3 c = float3(
        0.5 + 0.5 * sin(uv.x * 12.0),
        0.5 + 0.5 * cos(uv.y * 12.0),
        0.5 + 0.5 * sin((uv.x + uv.y) * 8.0));
    gOut[id.xy] = float4(c, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Runtime-loaded D3DCompile
// ---------------------------------------------------------------------------

#ifndef D3DCOMPILE_DEBUG
#define D3DCOMPILE_DEBUG             (1 << 2)
#endif
#ifndef D3DCOMPILE_SKIP_OPTIMIZATION
#define D3DCOMPILE_SKIP_OPTIMIZATION (1 << 10)
#endif

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static HMODULE        g_compilerDll = nullptr;
static PFN_D3DCompile g_D3DCompile  = nullptr;

static bool loadCompiler() {
    static const char* kCandidates[] = {
        "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"
    };

    for (const char* name : kCandidates) {
        g_compilerDll = LoadLibraryA(name);
        if (g_compilerDll) {
            g_D3DCompile = (PFN_D3DCompile)GetProcAddress(g_compilerDll, "D3DCompile");
            if (g_D3DCompile) {
                LOG_INFO("Shader compiler: loaded %s", name);
                return true;
            }
            FreeLibrary(g_compilerDll);
            g_compilerDll = nullptr;
        }
    }

    LOG_ERR("Could not load any d3dcompiler DLL. Tried d3dcompiler_47/46/43.dll.");
    LOG_ERR("Place d3dcompiler_47.dll next to the exe, or check C:\\Windows\\System32.");
    return false;
}

static ID3DBlob* compileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errs = nullptr;

    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = g_D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                              entry, target, flags, 0, &code, &errs);

    if (errs) {
        LOG_WARN("Shader diagnostics (%s/%s): %s", entry, target,
                 (const char*)errs->GetBufferPointer());
        errs->Release();
    }

    if (FAILED(hr)) {
        LOG_ERR("Shader compilation failed: %s / %s (hr=0x%08lX)",
                entry, target, (unsigned long)hr);
        return nullptr;
    }

    LOG_INFO("Compiled shader %s (%s), %zu bytes of bytecode",
             entry, target, (size_t)code->GetBufferSize());
    return code;
}

// ---------------------------------------------------------------------------
// Cube geometry
// ---------------------------------------------------------------------------

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

static void buildCube(std::vector<Vertex>& verts, std::vector<uint16_t>& indices) {
    const float n[6][3] = {
        { 0, 0,-1}, { 0, 0, 1}, {-1, 0, 0}, { 1, 0, 0}, { 0,-1, 0}, { 0, 1, 0}
    };
    const float f[6][4][3] = {
        {{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1},{ 1,-1,-1}},
        {{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},{-1,-1, 1}},
        {{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},{-1,-1,-1}},
        {{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1},{ 1,-1, 1}},
        {{-1,-1, 1},{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1}},
        {{-1, 1,-1},{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1}}
    };
    const float uv[4][2] = { {0,1}, {0,0}, {1,0}, {1,1} };

    for (int face = 0; face < 6; face++) {
        uint16_t base = (uint16_t)verts.size();
        for (int v = 0; v < 4; v++) {
            Vertex vert{};
            vert.pos[0] = f[face][v][0] * 0.8f;
            vert.pos[1] = f[face][v][1] * 0.8f;
            vert.pos[2] = f[face][v][2] * 0.8f;
            vert.normal[0] = n[face][0];
            vert.normal[1] = n[face][1];
            vert.normal[2] = n[face][2];
            vert.uv[0] = uv[v][0];
            vert.uv[1] = uv[v][1];
            verts.push_back(vert);
        }
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static bool g_running = true;

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_running = false;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { g_running = false; return 0; }
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND createWindow(int w, int h) {
    WNDCLASSA wc{};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "D3D11TestbedWindow";
    RegisterClassA(&wc);

    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowA(
        "D3D11TestbedWindow", "D3D11 Testbed",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

static void printUsage() {
    printf(
        "d3d11_testbed - ordinary D3D11 app for multi-GPU DXVK testing\n\n"
        "Base:\n"
        "  -w N            backbuffer width           (default 1280)\n"
        "  -h N            backbuffer height          (default 720)\n"
        "  -frames N       exit after N frames, 0=inf (default 600)\n"
        "  -vram N         ballast texture pool, MB   (default 512)\n"
        "  -texsize N      ballast texture dimension  (default 1024)\n"
        "  -psload N       PS inner loop iterations   (default 16)\n"
        "  -draws N        draw calls per frame       (default 32)\n"
        "  -instances N    instances per draw call    (default 64)\n"
        "  -offscreen 0|1  offscreen pass + blit      (default 1)\n"
        "  -clearonly      clear + present only, no geometry\n"
        "  -vsync 0|1      vsync                      (default 0)\n"
        "  -debug          request D3D11 debug layer\n"
        "  -logcalls N     trace every API call for first N frames (default 2)\n"
        "  -stats N        frame stats every N frames (default 60)\n"
        "  -log PATH       log file path              (default d3d11_testbed.log)\n"
        "  -tag TEXT       label echoed into the log, for sweep runs\n"
        "  -quiet          do not echo log to stdout\n"
        "  -help           this text\n\n"
        "Feature axes (all default off):\n"
        "  -renderscale F  scene renders at F x backbuffer size (default 1.0)\n"
        "  -passes N       N chained full-res post passes\n"
        "  -pyramid N      N halving downsample passes\n"
        "  -msaa N         scene sample count, uses ResolveSubresource\n"
        "  -depthread      bind the depth buffer as an SRV in post\n"
        "  -compute        compute shader writing a UAV, sampled in post\n"
        "  -indirect       compute writes draw args, scene drawn DrawIndexedInstancedIndirect\n"
        "  -deferred       scene pass recorded on a deferred context, ExecuteCommandList\n"
        "  -copyres        CopyResource into a scratch texture before blit\n"
        "  -copyregion     CopySubresourceRegion of a sub-rect\n"
        "  -updatesub      constant buffer via UpdateSubresource, not Map\n");
}

static bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](int& out) -> bool {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
            out = atoi(argv[++i]);
            return true;
        };
        auto nextF = [&](float& out) -> bool {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
            out = (float)atof(argv[++i]);
            return true;
        };

        if      (a == "-w")          { if (!next(g_cfg.width))         return false; }
        else if (a == "-h")          { if (!next(g_cfg.height))        return false; }
        else if (a == "-frames")     { if (!next(g_cfg.frames))        return false; }
        else if (a == "-vram")       { if (!next(g_cfg.vramMB))        return false; }
        else if (a == "-texsize")    { if (!next(g_cfg.texSize))       return false; }
        else if (a == "-psload")     { if (!next(g_cfg.psLoad))        return false; }
        else if (a == "-draws")      { if (!next(g_cfg.drawsPerFrame)) return false; }
        else if (a == "-instances")  { if (!next(g_cfg.instances))     return false; }
        else if (a == "-logcalls")   { if (!next(g_cfg.logCallFrames)) return false; }
        else if (a == "-stats")      { if (!next(g_cfg.statsInterval)) return false; }
        else if (a == "-passes")     { if (!next(g_cfg.postPasses))    return false; }
        else if (a == "-pyramid")    { if (!next(g_cfg.pyramid))       return false; }
        else if (a == "-msaa")       { if (!next(g_cfg.msaa))          return false; }
        else if (a == "-renderscale"){ if (!nextF(g_cfg.renderScale))  return false; }
        else if (a == "-offscreen")  { int v; if (!next(v)) return false; g_cfg.offscreen = v != 0; }
        else if (a == "-vsync")      { int v; if (!next(v)) return false; g_cfg.vsync     = v != 0; }
        else if (a == "-clearonly")  { g_cfg.clearOnly  = true; }
        else if (a == "-debug")      { g_cfg.debugLayer = true; }
        else if (a == "-quiet")      { g_cfg.quiet      = true; }
        else if (a == "-depthread")  { g_cfg.depthRead  = true; }
        else if (a == "-compute")    { g_cfg.compute    = true; }
        else if (a == "-indirect")   { g_cfg.indirect   = true; }
        else if (a == "-deferred")   { g_cfg.deferred   = true; }
        else if (a == "-copyres")    { g_cfg.copyRes    = true; }
        else if (a == "-copyregion") { g_cfg.copyRegion = true; }
        else if (a == "-updatesub")  { g_cfg.updateSub  = true; }
        else if (a == "-log")        {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for -log\n"); return false; }
            g_cfg.logPath = argv[++i];
        }
        else if (a == "-tag")        {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for -tag\n"); return false; }
            g_cfg.tag = argv[++i];
        }
        else if (a == "-help" || a == "--help") { printUsage(); return false; }
        else { fprintf(stderr, "unknown argument: %s\n", a.c_str()); printUsage(); return false; }
    }

    if (g_cfg.renderScale < 0.1f) g_cfg.renderScale = 0.1f;
    if (g_cfg.renderScale > 4.0f) g_cfg.renderScale = 4.0f;
    if (g_cfg.msaa < 1) g_cfg.msaa = 1;
    if (g_cfg.pyramid < 0) g_cfg.pyramid = 0;
    if (g_cfg.postPasses < 0) g_cfg.postPasses = 0;

    // Every axis below writes into or reads from the offscreen scene target,
    // so none of them mean anything without it. Turning it on silently is
    // friendlier than failing, and the log records that it happened.
    if (g_cfg.renderScale != 1.0f || g_cfg.postPasses > 0 || g_cfg.pyramid > 0
     || g_cfg.msaa > 1 || g_cfg.depthRead || g_cfg.copyRes || g_cfg.copyRegion)
        g_cfg.offscreen = true;

    return true;
}

// ---------------------------------------------------------------------------
// A render target with the three views we may need for it
// ---------------------------------------------------------------------------

struct RenderTarget {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int width  = 0;
    int height = 0;

    void release() {
        if (srv) { srv->Release(); srv = nullptr; }
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }
};

static bool createRenderTarget(ID3D11Device* device, RenderTarget& rt,
                               int w, int h, int sampleCount,
                               bool wantSRV, const char* what) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)w;
    td.Height           = (UINT)h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = (UINT)sampleCount;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET;
    if (wantSRV) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &rt.tex);
    if (FAILED(hr)) {
        LOG_ERR("CreateTexture2D(%s %dx%d samples=%d) FAILED (hr=0x%08lX)",
                what, w, h, sampleCount, (unsigned long)hr);
        return false;
    }

    hr = device->CreateRenderTargetView(rt.tex, nullptr, &rt.rtv);
    if (FAILED(hr)) {
        LOG_ERR("CreateRenderTargetView(%s) FAILED (hr=0x%08lX)", what, (unsigned long)hr);
        return false;
    }

    if (wantSRV) {
        hr = device->CreateShaderResourceView(rt.tex, nullptr, &rt.srv);
        if (FAILED(hr)) {
            LOG_ERR("CreateShaderResourceView(%s) FAILED (hr=0x%08lX)", what, (unsigned long)hr);
            return false;
        }
    }

    rt.width  = w;
    rt.height = h;

    LOG_INFO("%s: %dx%d samples=%d tex=%p rtv=%p srv=%p",
             what, w, h, sampleCount, (void*)rt.tex, (void*)rt.rtv, (void*)rt.srv);
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (!parseArgs(argc, argv))
        return 1;

    if (!g_log.open(g_cfg.logPath))
        return 1;

    LOG_INFO("=== d3d11_testbed starting ===");
    if (!g_cfg.tag.empty())
        LOG_INFO("tag: %s", g_cfg.tag.c_str());
    LOG_INFO("config: %dx%d frames=%d vram=%dMB texsize=%d psload=%d draws=%d instances=%d",
             g_cfg.width, g_cfg.height, g_cfg.frames, g_cfg.vramMB,
             g_cfg.texSize, g_cfg.psLoad, g_cfg.drawsPerFrame, g_cfg.instances);
    LOG_INFO("config: offscreen=%d clearonly=%d vsync=%d debug=%d logcalls=%d",
             (int)g_cfg.offscreen, (int)g_cfg.clearOnly, (int)g_cfg.vsync,
             (int)g_cfg.debugLayer, g_cfg.logCallFrames);
    LOG_INFO("features: renderscale=%.3f passes=%d pyramid=%d msaa=%d depthread=%d "
             "compute=%d copyres=%d copyregion=%d updatesub=%d indirect=%d deferred=%d",
             g_cfg.renderScale, g_cfg.postPasses, g_cfg.pyramid, g_cfg.msaa,
             (int)g_cfg.depthRead, (int)g_cfg.compute, (int)g_cfg.copyRes,
             (int)g_cfg.copyRegion, (int)g_cfg.updateSub, (int)g_cfg.indirect,
             (int)g_cfg.deferred);

    const int renderW = (int)(g_cfg.width  * g_cfg.renderScale) & ~1;
    const int renderH = (int)(g_cfg.height * g_cfg.renderScale) & ~1;
    LOG_INFO("scene renders at %dx%d, presents at %dx%d",
             renderW, renderH, g_cfg.width, g_cfg.height);

    HWND hwnd = createWindow(g_cfg.width, g_cfg.height);
    if (!hwnd) { LOG_ERR("CreateWindow failed"); return 1; }
    LOG_INFO("Window created (hwnd=%p)", (void*)hwnd);

    // -----------------------------------------------------------------------
    // Device + swapchain.
    //
    // pAdapter is nullptr and DriverType is HARDWARE: the completely default
    // path. No adapter enumeration, no device selection. Whatever the runtime
    // hands back is what this app uses.
    // -----------------------------------------------------------------------

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount        = 2;
    scd.BufferDesc.Width   = g_cfg.width;
    scd.BufferDesc.Height  = g_cfg.height;
    scd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow       = hwnd;
    scd.SampleDesc.Count   = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed           = TRUE;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    if (g_cfg.debugLayer) createFlags |= D3D11_CREATE_DEVICE_DEBUG;

    D3D_FEATURE_LEVEL wantLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };
    D3D_FEATURE_LEVEL gotLevel = D3D_FEATURE_LEVEL_11_0;

    IDXGISwapChain*      swapChain = nullptr;
    ID3D11Device*        device    = nullptr;
    ID3D11DeviceContext* ctx       = nullptr;

    LOG_CALL("D3D11CreateDeviceAndSwapChain(adapter=nullptr, HARDWARE, flags=0x%X)", createFlags);
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        wantLevels, (UINT)(sizeof(wantLevels) / sizeof(wantLevels[0])),
        D3D11_SDK_VERSION, &scd, &swapChain, &device, &gotLevel, &ctx);

    if (!checkHR(hr, "D3D11CreateDeviceAndSwapChain")) {
        g_log.close();
        return 1;
    }
    LOG_INFO("device=%p context=%p swapchain=%p featureLevel=0x%04X",
             (void*)device, (void*)ctx, (void*)swapChain, (unsigned)gotLevel);

    // Report the adapter the runtime gave us. This is descriptive only: a
    // normal game does exactly this to show a name in its settings screen.
    {
        IDXGIDevice*  dxgiDevice  = nullptr;
        IDXGIAdapter* dxgiAdapter = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
            if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter))) {
                DXGI_ADAPTER_DESC ad{};
                if (SUCCEEDED(dxgiAdapter->GetDesc(&ad))) {
                    char name[256]{};
                    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                    LOG_INFO("Adapter as reported to the app: '%s'", name);
                    LOG_INFO("  DedicatedVideoMemory  = %llu MB",
                             (unsigned long long)(ad.DedicatedVideoMemory / (1024ull * 1024ull)));
                    LOG_INFO("  VendorId=0x%04X DeviceId=0x%04X",
                             (unsigned)ad.VendorId, (unsigned)ad.DeviceId);
                }
                dxgiAdapter->Release();
            }
            dxgiDevice->Release();
        }
    }

    // MSAA support check. Asking for a sample count the device cannot do is a
    // configuration error, not a bug worth chasing later in a sweep log.
    if (g_cfg.msaa > 1) {
        UINT quality = 0;
        hr = device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)g_cfg.msaa, &quality);

        if (FAILED(hr) || quality == 0) {
            LOG_WARN("MSAA %dx not supported for RGBA8, falling back to 1x", g_cfg.msaa);
            g_cfg.msaa = 1;
        } else {
            LOG_INFO("MSAA %dx supported, %u quality levels", g_cfg.msaa, quality);
        }
    }

    // -----------------------------------------------------------------------
    // Backbuffer RTV
    // -----------------------------------------------------------------------

    ID3D11Texture2D*        backBuffer = nullptr;
    ID3D11RenderTargetView* backRTV    = nullptr;

    LOG_CALL("IDXGISwapChain::GetBuffer(0)");
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (!checkHR(hr, "SwapChain::GetBuffer")) return 1;

    LOG_CALL("CreateRenderTargetView(backbuffer=%p)", (void*)backBuffer);
    hr = device->CreateRenderTargetView(backBuffer, nullptr, &backRTV);
    if (!checkHR(hr, "CreateRenderTargetView(backbuffer)")) return 1;
    LOG_INFO("backbuffer RTV = %p", (void*)backRTV);

    // -----------------------------------------------------------------------
    // Clear-only mode: the minimal possible render loop.
    // -----------------------------------------------------------------------

    if (g_cfg.clearOnly) {
        LOG_INFO("=== clear-only mode: ClearRenderTargetView + Present, no geometry ===");

        auto lastStats = std::chrono::steady_clock::now();
        int  statFrames = 0;

        while (g_running) {
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!g_running) break;

            float t = (float)g_frameIndex * 0.02f;
            float color[4] = {
                0.5f + 0.5f * sinf(t),
                0.5f + 0.5f * sinf(t * 1.3f),
                0.5f + 0.5f * sinf(t * 1.7f),
                1.0f
            };

            LOG_CALL("ClearRenderTargetView(rtv=%p, rgba=[%.3f %.3f %.3f %.3f])",
                     (void*)backRTV, color[0], color[1], color[2], color[3]);
            ctx->ClearRenderTargetView(backRTV, color);

            LOG_CALL("IDXGISwapChain::Present(%u, 0)", g_cfg.vsync ? 1u : 0u);
            swapChain->Present(g_cfg.vsync ? 1 : 0, 0);

            g_frameIndex++;
            statFrames++;

            if (g_cfg.statsInterval > 0 && statFrames >= g_cfg.statsInterval) {
                auto now = std::chrono::steady_clock::now();
                double sec = std::chrono::duration<double>(now - lastStats).count();
                LOG_INFO("frame %d | %.1f fps | %.3f ms/frame",
                         g_frameIndex, statFrames / sec, (sec * 1000.0) / statFrames);
                lastStats = now;
                statFrames = 0;
            }

            if (g_cfg.frames > 0 && g_frameIndex >= g_cfg.frames) break;
        }

        LOG_INFO("=== clear-only run complete: %d frames ===", g_frameIndex);
        backRTV->Release();
        backBuffer->Release();
        ctx->Release();
        swapChain->Release();
        device->Release();
        DestroyWindow(hwnd);
        g_log.close();
        return 0;
    }

    // -----------------------------------------------------------------------
    // Shaders
    // -----------------------------------------------------------------------

    if (!loadCompiler()) { g_log.close(); return 1; }

    ID3DBlob* vsScene = compileShader(kSceneShaderHLSL, "VSMain", "vs_5_0");
    ID3DBlob* psScene = compileShader(kSceneShaderHLSL, "PSMain", "ps_5_0");
    ID3DBlob* vsPost  = compileShader(kPostShaderHLSL,  "VSMain", "vs_5_0");
    ID3DBlob* psPost  = compileShader(kPostShaderHLSL,  "PSMain", "ps_5_0");
    if (!vsScene || !psScene || !vsPost || !psPost) { g_log.close(); return 1; }

    ID3DBlob* csBlob = nullptr;
    if (g_cfg.compute) {
        csBlob = compileShader(kComputeShaderHLSL, "CSMain", "cs_5_0");
        if (!csBlob) { g_log.close(); return 1; }
    }

    ID3D11VertexShader* sceneVS = nullptr;
    ID3D11PixelShader*  scenePS = nullptr;
    ID3D11VertexShader* postVS  = nullptr;
    ID3D11PixelShader*  postPS  = nullptr;
    ID3D11ComputeShader* computeCS = nullptr;

    LOG_CALL("CreateVertexShader(scene)");
    hr = device->CreateVertexShader(vsScene->GetBufferPointer(), vsScene->GetBufferSize(), nullptr, &sceneVS);
    if (!checkHR(hr, "CreateVertexShader(scene)")) return 1;

    LOG_CALL("CreatePixelShader(scene)");
    hr = device->CreatePixelShader(psScene->GetBufferPointer(), psScene->GetBufferSize(), nullptr, &scenePS);
    if (!checkHR(hr, "CreatePixelShader(scene)")) return 1;

    LOG_CALL("CreateVertexShader(post)");
    hr = device->CreateVertexShader(vsPost->GetBufferPointer(), vsPost->GetBufferSize(), nullptr, &postVS);
    if (!checkHR(hr, "CreateVertexShader(post)")) return 1;

    LOG_CALL("CreatePixelShader(post)");
    hr = device->CreatePixelShader(psPost->GetBufferPointer(), psPost->GetBufferSize(), nullptr, &postPS);
    if (!checkHR(hr, "CreatePixelShader(post)")) return 1;

    if (csBlob) {
        LOG_CALL("CreateComputeShader(aux)");
        hr = device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &computeCS);
        if (!checkHR(hr, "CreateComputeShader(aux)")) return 1;
        csBlob->Release();
    }

    // -----------------------------------------------------------------------
    // Input layout
    // -----------------------------------------------------------------------

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    ID3D11InputLayout* inputLayout = nullptr;
    LOG_CALL("CreateInputLayout(3 elements)");
    hr = device->CreateInputLayout(layoutDesc, 3, vsScene->GetBufferPointer(),
                                   vsScene->GetBufferSize(), &inputLayout);
    if (!checkHR(hr, "CreateInputLayout")) return 1;

    vsScene->Release(); psScene->Release();
    vsPost->Release();  psPost->Release();

    // -----------------------------------------------------------------------
    // Vertex / index / constant buffers
    //
    // The scene constant buffer is DYNAMIC + Map/WRITE_DISCARD by default,
    // which is the path most games use and the one the fork mirrors through
    // Unmap. With -updatesub it becomes a DEFAULT buffer written by
    // UpdateSubresource instead, which is a different mirroring path entirely.
    // -----------------------------------------------------------------------

    std::vector<Vertex>   verts;
    std::vector<uint16_t> indices;
    buildCube(verts, indices);

    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* indexBuffer  = nullptr;
    ID3D11Buffer* constBuffer  = nullptr;
    ID3D11Buffer* postCB       = nullptr;

    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(verts.size() * sizeof(Vertex));
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = verts.data();

        LOG_CALL("CreateBuffer(VERTEX, %u bytes, %zu verts)", bd.ByteWidth, verts.size());
        hr = device->CreateBuffer(&bd, &sd, &vertexBuffer);
        if (!checkHR(hr, "CreateBuffer(vertex)")) return 1;
        LOG_INFO("vertexBuffer = %p", (void*)vertexBuffer);
    }

    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(indices.size() * sizeof(uint16_t));
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = indices.data();

        LOG_CALL("CreateBuffer(INDEX, %u bytes, %zu indices)", bd.ByteWidth, indices.size());
        hr = device->CreateBuffer(&bd, &sd, &indexBuffer);
        if (!checkHR(hr, "CreateBuffer(index)")) return 1;
        LOG_INFO("indexBuffer = %p", (void*)indexBuffer);
    }

    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(PerFrameCB);
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;

        if (g_cfg.updateSub) {
            bd.Usage          = D3D11_USAGE_DEFAULT;
            bd.CPUAccessFlags = 0;
        } else {
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        }

        LOG_CALL("CreateBuffer(CONSTANT, %u bytes, %s)", bd.ByteWidth,
                 g_cfg.updateSub ? "DEFAULT" : "DYNAMIC");
        hr = device->CreateBuffer(&bd, nullptr, &constBuffer);
        if (!checkHR(hr, "CreateBuffer(constant)")) return 1;
        LOG_INFO("constBuffer = %p", (void*)constBuffer);
    }

    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(PostCB);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        LOG_CALL("CreateBuffer(POST CONSTANT, %u bytes, DYNAMIC)", bd.ByteWidth);
        hr = device->CreateBuffer(&bd, nullptr, &postCB);
        if (!checkHR(hr, "CreateBuffer(post constant)")) return 1;
    }

    // -----------------------------------------------------------------------
    // Ballast textures
    // -----------------------------------------------------------------------

    std::vector<ID3D11Texture2D*>          ballastTex;
    std::vector<ID3D11ShaderResourceView*> ballastSRV;

    {
        const size_t bytesPer = (size_t)g_cfg.texSize * g_cfg.texSize * 4;
        const size_t budget   = (size_t)g_cfg.vramMB * 1024ull * 1024ull;
        int count = (int)(budget / bytesPer);
        if (count < 1) count = 1;

        LOG_INFO("Ballast: %d textures of %dx%d RGBA8 (%.2f MB each, %.2f MB total)",
                 count, g_cfg.texSize, g_cfg.texSize,
                 bytesPer / (1024.0 * 1024.0),
                 (count * bytesPer) / (1024.0 * 1024.0));

        std::vector<uint8_t> pixels(bytesPer);

        for (int i = 0; i < count; i++) {
            for (int y = 0; y < g_cfg.texSize; y++) {
                for (int x = 0; x < g_cfg.texSize; x++) {
                    size_t o = ((size_t)y * g_cfg.texSize + x) * 4;
                    pixels[o + 0] = (uint8_t)((x * 3 + i * 17) & 0xFF);
                    pixels[o + 1] = (uint8_t)((y * 5 + i * 29) & 0xFF);
                    pixels[o + 2] = (uint8_t)(((x ^ y) + i * 41) & 0xFF);
                    pixels[o + 3] = 0xFF;
                }
            }

            D3D11_TEXTURE2D_DESC td{};
            td.Width            = g_cfg.texSize;
            td.Height           = g_cfg.texSize;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sd{};
            sd.pSysMem     = pixels.data();
            sd.SysMemPitch = (UINT)(g_cfg.texSize * 4);

            ID3D11Texture2D* tex = nullptr;
            hr = device->CreateTexture2D(&td, &sd, &tex);
            if (FAILED(hr)) {
                LOG_WARN("Ballast texture %d failed (hr=0x%08lX) - stopping at %zu textures",
                         i, (unsigned long)hr, ballastTex.size());
                break;
            }

            ID3D11ShaderResourceView* srv = nullptr;
            hr = device->CreateShaderResourceView(tex, nullptr, &srv);
            if (FAILED(hr)) {
                LOG_WARN("Ballast SRV %d failed (hr=0x%08lX)", i, (unsigned long)hr);
                tex->Release();
                break;
            }

            ballastTex.push_back(tex);
            ballastSRV.push_back(srv);

            if ((i % 32) == 0)
                LOG_INFO("  ballast[%d] tex=%p srv=%p", i, (void*)tex, (void*)srv);
        }

        LOG_INFO("Ballast allocated: %zu textures, %.2f MB",
                 ballastTex.size(), (ballastTex.size() * bytesPer) / (1024.0 * 1024.0));
    }

    if (ballastSRV.empty()) {
        LOG_ERR("No ballast textures allocated; cannot render. Lower -vram or -texsize.");
        g_log.close();
        return 1;
    }

    // -----------------------------------------------------------------------
    // Depth buffer
    //
    // With -depthread the depth texture is created TYPELESS so it can carry
    // both a depth-stencil view and a shader resource view. Without it the
    // plain D32_FLOAT path is kept, since that is what an ordinary app does
    // and the difference itself is one of the axes being tested.
    // -----------------------------------------------------------------------

    ID3D11Texture2D*          depthTex = nullptr;
    ID3D11DepthStencilView*   depthDSV = nullptr;
    ID3D11ShaderResourceView* depthSRV = nullptr;

    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = (UINT)renderW;
        td.Height           = (UINT)renderH;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = g_cfg.depthRead ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = (UINT)g_cfg.msaa;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
        if (g_cfg.depthRead) td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

        LOG_CALL("CreateTexture2D(depth %dx%d samples=%d %s)", renderW, renderH,
                 g_cfg.msaa, g_cfg.depthRead ? "R32_TYPELESS" : "D32_FLOAT");
        hr = device->CreateTexture2D(&td, nullptr, &depthTex);
        if (!checkHR(hr, "CreateTexture2D(depth)")) return 1;

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
        dsvd.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = g_cfg.msaa > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                            : D3D11_DSV_DIMENSION_TEXTURE2D;

        LOG_CALL("CreateDepthStencilView(depth=%p)", (void*)depthTex);
        hr = device->CreateDepthStencilView(depthTex, &dsvd, &depthDSV);
        if (!checkHR(hr, "CreateDepthStencilView")) return 1;

        if (g_cfg.depthRead) {
            // A multisampled depth SRV would need a resolve of its own, which
            // is a separate axis; combining the two is left out deliberately.
            if (g_cfg.msaa > 1) {
                LOG_WARN("-depthread with MSAA is not supported here; depth SRV skipped");
            } else {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
                srvd.Format                    = DXGI_FORMAT_R32_FLOAT;
                srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvd.Texture2D.MipLevels       = 1;

                LOG_CALL("CreateShaderResourceView(depth=%p, R32_FLOAT)", (void*)depthTex);
                hr = device->CreateShaderResourceView(depthTex, &srvd, &depthSRV);
                if (!checkHR(hr, "CreateShaderResourceView(depth)")) return 1;
            }
        }

        LOG_INFO("depthTex=%p depthDSV=%p depthSRV=%p",
                 (void*)depthTex, (void*)depthDSV, (void*)depthSRV);
    }

    // -----------------------------------------------------------------------
    // Scene target, resolve target, ping-pong post targets, pyramid, scratch
    // -----------------------------------------------------------------------

    RenderTarget sceneRT;      // what the geometry pass writes to
    RenderTarget resolveRT;    // MSAA resolve destination, when msaa > 1
    RenderTarget postRT[2];    // ping-pong for chained post passes
    RenderTarget scratchRT;    // CopyResource / CopySubresourceRegion target
    std::vector<RenderTarget> pyramidRT;

    if (g_cfg.offscreen) {
        // With MSAA the scene target cannot also be a shader resource, so it
        // gets resolved into a second single-sampled target first. That
        // resolve is ResolveSubresource, which the fork does not mirror.
        if (!createRenderTarget(device, sceneRT, renderW, renderH,
                                g_cfg.msaa, g_cfg.msaa == 1, "sceneRT"))
            return 1;

        if (g_cfg.msaa > 1) {
            if (!createRenderTarget(device, resolveRT, renderW, renderH, 1, true, "resolveRT"))
                return 1;
        }

        if (g_cfg.postPasses > 0) {
            if (!createRenderTarget(device, postRT[0], renderW, renderH, 1, true, "postRT[0]"))
                return 1;
            if (!createRenderTarget(device, postRT[1], renderW, renderH, 1, true, "postRT[1]"))
                return 1;
        }

        for (int i = 0; i < g_cfg.pyramid; i++) {
            int pw = renderW >> (i + 1);
            int ph = renderH >> (i + 1);
            if (pw < 4 || ph < 4) {
                LOG_WARN("pyramid level %d would be %dx%d, stopping the chain there", i, pw, ph);
                break;
            }

            RenderTarget rt;
            char label[64];
            snprintf(label, sizeof(label), "pyramidRT[%d]", i);
            if (!createRenderTarget(device, rt, pw, ph, 1, true, label))
                return 1;
            pyramidRT.push_back(rt);
        }

        if (g_cfg.copyRes || g_cfg.copyRegion) {
            if (!createRenderTarget(device, scratchRT, renderW, renderH, 1, true, "scratchRT"))
                return 1;
        }
    }

    // Compute output. Also serves as the neutral texture bound to t1 when the
    // compute axis is off, so the post shader always has something valid in
    // every slot -- an unbound slot samples zero, which would be a second
    // variable in every run.
    ID3D11Texture2D*           auxTex = nullptr;
    ID3D11ShaderResourceView*  auxSRV = nullptr;
    ID3D11UnorderedAccessView* auxUAV = nullptr;

    {
        const int auxSize = 256;

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = auxSize;
        td.Height           = auxSize;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (g_cfg.compute) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

        std::vector<uint8_t> white((size_t)auxSize * auxSize * 4, 0xFF);
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem     = white.data();
        sd.SysMemPitch = auxSize * 4;

        LOG_CALL("CreateTexture2D(aux %dx%d RGBA8%s)", auxSize, auxSize,
                 g_cfg.compute ? ", SRV|UAV" : ", SRV");
        hr = device->CreateTexture2D(&td, &sd, &auxTex);
        if (!checkHR(hr, "CreateTexture2D(aux)")) return 1;

        hr = device->CreateShaderResourceView(auxTex, nullptr, &auxSRV);
        if (!checkHR(hr, "CreateShaderResourceView(aux)")) return 1;

        if (g_cfg.compute) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

            LOG_CALL("CreateUnorderedAccessView(aux=%p)", (void*)auxTex);
            hr = device->CreateUnorderedAccessView(auxTex, &ud, &auxUAV);
            if (!checkHR(hr, "CreateUnorderedAccessView(aux)")) return 1;
        }
    }

    // A 1x1 white texture for the depth slot when depth reading is off, for
    // the same reason as above: every slot always holds a real view.
    ID3D11Texture2D*          dummyTex = nullptr;
    ID3D11ShaderResourceView* dummySRV = nullptr;
    {
        uint8_t px[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = px; sd.SysMemPitch = 4;

        hr = device->CreateTexture2D(&td, &sd, &dummyTex);
        if (!checkHR(hr, "CreateTexture2D(dummy 1x1)")) return 1;

        hr = device->CreateShaderResourceView(dummyTex, nullptr, &dummySRV);
        if (!checkHR(hr, "CreateShaderResourceView(dummy)")) return 1;
    }

    ID3D11SamplerState* sampler = nullptr;
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;

        LOG_CALL("CreateSamplerState(LINEAR/CLAMP)");
        hr = device->CreateSamplerState(&sd, &sampler);
        if (!checkHR(hr, "CreateSamplerState")) return 1;
    }

    // The scene still wants wrapping, since its UVs are perturbed in the loop.
    ID3D11SamplerState* wrapSampler = nullptr;
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;

        hr = device->CreateSamplerState(&sd, &wrapSampler);
        if (!checkHR(hr, "CreateSamplerState(wrap)")) return 1;
    }

    ID3D11DepthStencilState* depthState = nullptr;
    {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable    = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc      = D3D11_COMPARISON_LESS;

        LOG_CALL("CreateDepthStencilState(depth test+write, LESS)");
        hr = device->CreateDepthStencilState(&dsd, &depthState);
        if (!checkHR(hr, "CreateDepthStencilState")) return 1;
    }

    ID3D11RasterizerState* rasterState = nullptr;
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode        = D3D11_FILL_SOLID;
        rd.CullMode        = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable = TRUE;
        rd.MultisampleEnable = g_cfg.msaa > 1 ? TRUE : FALSE;

        LOG_CALL("CreateRasterizerState(SOLID/CULL_BACK, msaa=%d)", g_cfg.msaa);
        hr = device->CreateRasterizerState(&rd, &rasterState);
        if (!checkHR(hr, "CreateRasterizerState")) return 1;
    }

    LOG_INFO("=== resource creation complete, entering render loop ===");

    RdocInit();

    // -----------------------------------------------------------------------
    // Render loop
    // -----------------------------------------------------------------------

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    const UINT indexCount = (UINT)indices.size();

    // -deferred: the scene pass is recorded on a deferred context and
    // replayed with ExecuteCommandList, the way Fire Strike renders.
    // Every worker mirror in the fork is immediate-context only, so on
    // GPU1 the scene never happens.
    ID3D11DeviceContext* dctx = nullptr;
    if (g_cfg.deferred) {
        LOG_CALL("CreateDeferredContext");
        hr = device->CreateDeferredContext(0, &dctx);
        if (!checkHR(hr, "CreateDeferredContext")) return 1;
    }

    // -indirect: a compute shader writes the scene draw's arguments
    // into a buffer every frame, and the draw reads them back with
    // DrawIndexedInstancedIndirect. On a device where Dispatch is not
    // mirrored the argument buffer never changes from its initial
    // zeroes and the draw does nothing -- Fire Strike's physics test.
    ID3D11ComputeShader*       argsCS  = nullptr;
    ID3D11Buffer*              argsBuf = nullptr;
    ID3D11UnorderedAccessView* argsUAV = nullptr;
    if (g_cfg.indirect) {
        char src[1024];
        snprintf(src, sizeof(src),
            "RWByteAddressBuffer gOut : register(u0);\n"
            "[numthreads(1, 1, 1)]\n"
            "void CSArgs(uint3 id : SV_DispatchThreadID) {\n"
            "    gOut.Store(0, %uu);\n"
            "    gOut.Store(4, %uu);\n"
            "    gOut.Store(8, 0u);\n"
            "    gOut.Store(12, 0u);\n"
            "    gOut.Store(16, 0u);\n"
            "}\n", indexCount, (unsigned)g_cfg.instances);
        ID3DBlob* blob = compileShader(src, "CSArgs", "cs_5_0");
        if (!blob) { g_log.close(); return 1; }
        LOG_CALL("CreateComputeShader(args)");
        hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &argsCS);
        blob->Release();
        if (!checkHR(hr, "CreateComputeShader(args)")) return 1;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 20;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        LOG_CALL("CreateBuffer(INDIRECT ARGS, 20 bytes)");
        hr = device->CreateBuffer(&bd, nullptr, &argsBuf);
        if (!checkHR(hr, "CreateBuffer(indirect args)")) return 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format              = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements  = 5;
        ud.Buffer.Flags        = D3D11_BUFFER_UAV_FLAG_RAW;
        LOG_CALL("CreateUnorderedAccessView(args)");
        hr = device->CreateUnorderedAccessView(argsBuf, &ud, &argsUAV);
        if (!checkHR(hr, "CreateUnorderedAccessView(args)")) return 1;
    }
    auto   runStart  = std::chrono::steady_clock::now();
    auto   lastStats = runStart;
    int    statFrames = 0;
    double worstFrameMs = 0.0;

    ID3D11ShaderResourceView* nullSRV3[3] = { nullptr, nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUAV1[1] = { nullptr };

    // Runs one fullscreen post pass. Every post-style pass in the frame goes
    // through here so they cannot drift apart in how they bind state.
    auto runPostPass = [&](ID3D11RenderTargetView* dstRTV,
                           ID3D11ShaderResourceView* srcSRV,
                           int dstW, int dstH,
                           float auxWeight, float depthWeight,
                           int passIndex,
                           const char* what) {
        LOG_CALL("OMSetRenderTargets(1, %s=%p, dsv=nullptr)", what, (void*)dstRTV);
        ctx->OMSetRenderTargets(1, &dstRTV, nullptr);

        D3D11_VIEWPORT vp{};
        vp.Width    = (float)dstW;
        vp.Height   = (float)dstH;
        vp.MaxDepth = 1.0f;
        LOG_CALL("RSSetViewports(1, %.0fx%.0f) [%s]", vp.Width, vp.Height, what);
        ctx->RSSetViewports(1, &vp);

        LOG_CALL("OMSetDepthStencilState(nullptr) [%s]", what);
        ctx->OMSetDepthStencilState(nullptr, 0);

        LOG_CALL("IASetInputLayout(nullptr) [fullscreen tri from SV_VertexID]");
        ctx->IASetInputLayout(nullptr);

        ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        LOG_CALL("VSSetShader(postVS=%p) [%s]", (void*)postVS, what);
        ctx->VSSetShader(postVS, nullptr, 0);

        LOG_CALL("PSSetShader(postPS=%p) [%s]", (void*)postPS, what);
        ctx->PSSetShader(postPS, nullptr, 0);

        ctx->PSSetSamplers(0, 1, &sampler);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(postCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            PostCB pc{};
            pc.auxWeight   = auxWeight;
            pc.depthWeight = depthWeight;
            pc.passIndex   = (float)passIndex;
            memcpy(m.pData, &pc, sizeof(pc));
            ctx->Unmap(postCB, 0);
        }
        ctx->PSSetConstantBuffers(0, 1, &postCB);

        ID3D11ShaderResourceView* srvs[3] = {
            srcSRV,
            auxSRV,
            depthSRV ? depthSRV : dummySRV
        };
        LOG_CALL("PSSetShaderResources(0, 3, src=%p aux=%p depth=%p) [%s]",
                 (void*)srvs[0], (void*)srvs[1], (void*)srvs[2], what);
        ctx->PSSetShaderResources(0, 3, srvs);

        LOG_CALL("Draw(3, 0) [%s]", what);
        ctx->Draw(3, 0);

        LOG_CALL("PSSetShaderResources(0, 3, nullptr) [release, %s]", what);
        ctx->PSSetShaderResources(0, 3, nullSRV3);
    };

    while (g_running) {
        auto frameStart = std::chrono::steady_clock::now();

        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        float t = std::chrono::duration<float>(frameStart - runStart).count();

        // Camera orbits the lattice.
        float radius = 6.0f + 2.5f * (float)ceil(pow((double)g_cfg.instances, 1.0 / 3.0));
        float eye[3] = { sinf(t * 0.2f) * radius, radius * 0.35f, cosf(t * 0.2f) * radius };
        float at[3]  = { 0.0f, 0.0f, 0.0f };
        float up[3]  = { 0.0f, 1.0f, 0.0f };

        Mat4 view = matLookAtLH(eye, at, up);
        Mat4 proj = matPerspectiveLH(1.0472f, (float)renderW / (float)renderH, 0.1f, 500.0f);
        Mat4 viewProj = matMul(view, proj);

        // --- compute pass ---------------------------------------------------
        // Dispatch and UAV binds are both unmirrored in the fork, so this runs
        // before anything samples auxSRV.
        if (g_cfg.compute && computeCS && auxUAV) {
            LOG_CALL("CSSetShader(computeCS=%p)", (void*)computeCS);
            ctx->CSSetShader(computeCS, nullptr, 0);

            LOG_CALL("CSSetUnorderedAccessViews(0, 1, auxUAV=%p)", (void*)auxUAV);
            ctx->CSSetUnorderedAccessViews(0, 1, &auxUAV, nullptr);

            LOG_CALL("Dispatch(32, 32, 1)");
            ctx->Dispatch(32, 32, 1);

            LOG_CALL("CSSetUnorderedAccessViews(0, 1, nullptr) [release UAV]");
            ctx->CSSetUnorderedAccessViews(0, 1, nullUAV1, nullptr);
            ctx->CSSetShader(nullptr, nullptr, 0);
        }
        if (g_cfg.indirect && argsCS && argsUAV) {
            LOG_CALL("CSSetShader(argsCS=%p)", (void*)argsCS);
            ctx->CSSetShader(argsCS, nullptr, 0);
            LOG_CALL("CSSetUnorderedAccessViews(0, 1, argsUAV=%p)", (void*)argsUAV);
            ctx->CSSetUnorderedAccessViews(0, 1, &argsUAV, nullptr);
            LOG_CALL("Dispatch(1, 1, 1) [indirect args]");
            ctx->Dispatch(1, 1, 1);
            ctx->CSSetUnorderedAccessViews(0, 1, nullUAV1, nullptr);
            ctx->CSSetShader(nullptr, nullptr, 0);
        }

        // --- scene pass -----------------------------------------------------
        ID3D11RenderTargetView* targetRTV = g_cfg.offscreen ? sceneRT.rtv : backRTV;
        const float clearColor[4] = { 0.06f, 0.07f, 0.10f, 1.0f };

        LOG_CALL("ClearRenderTargetView(rtv=%p)", (void*)targetRTV);
        ID3D11DeviceContext* sc = (g_cfg.deferred && dctx) ? dctx : ctx;
        sc->ClearRenderTargetView(targetRTV, clearColor);

        LOG_CALL("ClearDepthStencilView(dsv=%p, depth=1.0)", (void*)depthDSV);
        sc->ClearDepthStencilView(depthDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

        LOG_CALL("OMSetRenderTargets(1, rtv=%p, dsv=%p)", (void*)targetRTV, (void*)depthDSV);
        sc->OMSetRenderTargets(1, &targetRTV, depthDSV);

        D3D11_VIEWPORT vp{};
        vp.Width    = (float)(g_cfg.offscreen ? renderW : g_cfg.width);
        vp.Height   = (float)(g_cfg.offscreen ? renderH : g_cfg.height);
        vp.MaxDepth = 1.0f;
        LOG_CALL("RSSetViewports(1, %.0fx%.0f)", vp.Width, vp.Height);
        sc->RSSetViewports(1, &vp);

        LOG_CALL("RSSetState(%p)", (void*)rasterState);
        sc->RSSetState(rasterState);

        LOG_CALL("OMSetDepthStencilState(%p)", (void*)depthState);
        sc->OMSetDepthStencilState(depthState, 0);

        LOG_CALL("IASetInputLayout(%p)", (void*)inputLayout);
        sc->IASetInputLayout(inputLayout);

        LOG_CALL("IASetPrimitiveTopology(TRIANGLELIST)");
        sc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        LOG_CALL("IASetVertexBuffers(0, 1, vb=%p, stride=%u, offset=%u)",
                 (void*)vertexBuffer, stride, offset);
        sc->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

        LOG_CALL("IASetIndexBuffer(ib=%p, R16_UINT, 0)", (void*)indexBuffer);
        sc->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);

        LOG_CALL("VSSetShader(%p)", (void*)sceneVS);
        sc->VSSetShader(sceneVS, nullptr, 0);

        LOG_CALL("PSSetShader(%p)", (void*)scenePS);
        sc->PSSetShader(scenePS, nullptr, 0);

        LOG_CALL("PSSetSamplers(0, 1, %p)", (void*)wrapSampler);
        sc->PSSetSamplers(0, 1, &wrapSampler);

        for (int d = 0; d < g_cfg.drawsPerFrame; d++) {
            PerFrameCB cb{};
            cb.viewProj      = viewProj;
            cb.time          = t;
            cb.psLoad        = (float)g_cfg.psLoad;
            cb.instanceCount = (float)g_cfg.instances;
            cb.drawIndex     = (float)d;

            if (g_cfg.updateSub) {
                LOG_CALL("UpdateSubresource(constBuffer=%p)", (void*)constBuffer);
                sc->UpdateSubresource(constBuffer, 0, nullptr, &cb, 0, 0);
            } else {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                LOG_CALL("Map(constBuffer=%p, 0, WRITE_DISCARD)", (void*)constBuffer);
                hr = sc->Map(constBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(hr)) {
                    LOG_ERR("Map(constBuffer) failed (hr=0x%08lX)", (unsigned long)hr);
                    g_running = false;
                    break;
                }
                memcpy(mapped.pData, &cb, sizeof(cb));
                LOG_CALL("Unmap(constBuffer=%p, 0)", (void*)constBuffer);
                sc->Unmap(constBuffer, 0);
            }

            LOG_CALL("VSSetConstantBuffers(0, 1, %p)", (void*)constBuffer);
            sc->VSSetConstantBuffers(0, 1, &constBuffer);

            LOG_CALL("PSSetConstantBuffers(0, 1, %p)", (void*)constBuffer);
            sc->PSSetConstantBuffers(0, 1, &constBuffer);

            // Rotate through the ballast pool so every texture gets touched.
            ID3D11ShaderResourceView* srv =
                ballastSRV[(size_t)(d + g_frameIndex) % ballastSRV.size()];

            LOG_CALL("PSSetShaderResources(0, 1, srv=%p)", (void*)srv);
            sc->PSSetShaderResources(0, 1, &srv);

            LOG_CALL("DrawIndexedInstanced(indexCount=%u, instances=%d, 0, 0, 0)",
                     indexCount, g_cfg.instances);
            if (g_cfg.indirect && argsBuf) {
                LOG_CALL("DrawIndexedInstancedIndirect(argsBuf=%p, 0)", (void*)argsBuf);
                sc->DrawIndexedInstancedIndirect(argsBuf, 0);
            } else {
                sc->DrawIndexedInstanced(indexCount, (UINT)g_cfg.instances, 0, 0, 0);
            }
        }

        if (g_cfg.deferred && dctx) {
            ID3D11CommandList* cl = nullptr;
            hr = dctx->FinishCommandList(FALSE, &cl);
            if (!checkHR(hr, "FinishCommandList")) return 1;
            LOG_CALL("ExecuteCommandList(%p)", (void*)cl);
            ctx->ExecuteCommandList(cl, FALSE);
            cl->Release();
        }
        if (!g_running) break;
        // --- post chain -----------------------------------------------------
        if (g_cfg.offscreen) {
            ID3D11ShaderResourceView* nullSRV1[1] = { nullptr };
            LOG_CALL("PSSetShaderResources(0, 1, nullptr) [unbind before sampling scene]");
            ctx->PSSetShaderResources(0, 1, nullSRV1);

            // Whatever currently holds the frame. Each stage below updates it.
            ID3D11ShaderResourceView* currentSRV = sceneRT.srv;

            // MSAA resolve. ResolveSubresource is not mirrored by the fork.
            if (g_cfg.msaa > 1) {
                LOG_CALL("ResolveSubresource(dst=%p, src=%p, RGBA8)",
                         (void*)resolveRT.tex, (void*)sceneRT.tex);
                ctx->ResolveSubresource(resolveRT.tex, 0, sceneRT.tex, 0,
                                        DXGI_FORMAT_R8G8B8A8_UNORM);
                currentSRV = resolveRT.srv;
            }

            // Downsample pyramid, Superposition-style: each level is half the
            // previous one and reads what the level above produced.
            for (size_t i = 0; i < pyramidRT.size(); i++) {
                char label[64];
                snprintf(label, sizeof(label), "pyramid[%zu]", i);
                runPostPass(pyramidRT[i].rtv, currentSRV,
                            pyramidRT[i].width, pyramidRT[i].height,
                            0.0f, 0.0f, (int)i + 1, label);
                currentSRV = pyramidRT[i].srv;
            }

            // Chained full-res post passes. After the pyramid these read the
            // smallest level, which is exactly the kind of upscale-and-combine
            // a real post stack does.
            for (int i = 0; i < g_cfg.postPasses; i++) {
                RenderTarget& dst = postRT[i & 1];
                char label[64];
                snprintf(label, sizeof(label), "post[%d]", i);
                runPostPass(dst.rtv, currentSRV, dst.width, dst.height,
                            g_cfg.compute ? 1.0f : 0.0f,
                            (depthSRV && i == 0) ? 1.0f : 0.0f,
                            i, label);
                currentSRV = dst.srv;
            }

            // Copy paths. Both are unmirrored in the fork, and both leave the
            // frame in a texture the worker device has never been told about.
            if (g_cfg.copyRes) {
                ID3D11Resource* srcRes = nullptr;
                if (g_cfg.postPasses > 0)      srcRes = postRT[(g_cfg.postPasses - 1) & 1].tex;
                else if (!pyramidRT.empty())   srcRes = pyramidRT.back().tex;
                else if (g_cfg.msaa > 1)       srcRes = resolveRT.tex;
                else                           srcRes = sceneRT.tex;

                // CopyResource needs matching dimensions; the pyramid changes
                // them, so fall back to a region copy in that case.
                bool sameSize = (g_cfg.postPasses > 0) || pyramidRT.empty();

                if (sameSize) {
                    LOG_CALL("CopyResource(dst=%p, src=%p)", (void*)scratchRT.tex, (void*)srcRes);
                    ctx->CopyResource(scratchRT.tex, srcRes);
                    currentSRV = scratchRT.srv;
                } else if (g_frameIndex == 0) {
                    LOG_WARN("-copyres skipped: pyramid output is smaller than the scratch target");
                }
            }

            if (g_cfg.copyRegion) {
                // A sub-rect rather than the whole surface, since the region
                // form takes a different path through the fork than
                // CopyResource does.
                D3D11_BOX box{};
                box.left   = 0;
                box.top    = 0;
                box.front  = 0;
                box.right  = (UINT)(renderW / 2);
                box.bottom = (UINT)(renderH / 2);
                box.back   = 1;

                ID3D11Resource* srcRes = (g_cfg.postPasses > 0)
                    ? postRT[(g_cfg.postPasses - 1) & 1].tex
                    : sceneRT.tex;

                if (g_cfg.msaa == 1) {
                    LOG_CALL("CopySubresourceRegion(dst=%p, src=%p, %ux%u)",
                             (void*)scratchRT.tex, (void*)srcRes, box.right, box.bottom);
                    ctx->CopySubresourceRegion(scratchRT.tex, 0, 0, 0, 0, srcRes, 0, &box);
                } else if (g_frameIndex == 0) {
                    LOG_WARN("-copyregion skipped: source is multisampled");
                }
            }

            // Final blit into the back buffer. This is where the render
            // resolution meets the presentation resolution.
            runPostPass(backRTV, currentSRV, g_cfg.width, g_cfg.height,
                        g_cfg.compute ? 1.0f : 0.0f,
                        depthSRV ? 1.0f : 0.0f,
                        99, "final blit");
        }

        LOG_CALL("IDXGISwapChain::Present(%u, 0)", g_cfg.vsync ? 1u : 0u);
        swapChain->Present(g_cfg.vsync ? 1 : 0, 0);

        if (g_rdoc) {
            if (g_frameIndex == 100) {
                g_rdoc->StartFrameCapture(nullptr, nullptr);
                printf("rdoc: capture started\n");
            } else if (g_frameIndex == 101) {
                uint32_t ok = g_rdoc->EndFrameCapture(nullptr, nullptr);
                printf("rdoc: capture ended, ok=%u\n", ok);
            }
        }

        g_frameIndex++;
        statFrames++;

        double frameMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - frameStart).count();
        if (frameMs > worstFrameMs) worstFrameMs = frameMs;

        if (g_frameIndex == g_cfg.logCallFrames && g_cfg.logCallFrames > 0)
            LOG_INFO("--- per-call tracing ends here (frame %d) ---", g_frameIndex);

        if (g_cfg.statsInterval > 0 && statFrames >= g_cfg.statsInterval) {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - lastStats).count();
            LOG_INFO("frame %d | %.1f fps | %.3f ms avg | %.3f ms worst",
                     g_frameIndex, statFrames / sec, (sec * 1000.0) / statFrames, worstFrameMs);
            lastStats = now;
            statFrames = 0;
            worstFrameMs = 0.0;
        }

        if (g_cfg.frames > 0 && g_frameIndex >= g_cfg.frames) break;
    }

    double totalSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - runStart).count();

    // A single machine-readable line per run, so a sweep over many
    // configurations can be reduced to one grep.
    LOG_INFO("RESULT tag=%s renderscale=%.3f passes=%d pyramid=%d msaa=%d "
             "depthread=%d compute=%d copyres=%d copyregion=%d updatesub=%d "
             "frames=%d seconds=%.3f fps=%.1f",
             g_cfg.tag.empty() ? "-" : g_cfg.tag.c_str(),
             g_cfg.renderScale, g_cfg.postPasses, g_cfg.pyramid, g_cfg.msaa,
             (int)g_cfg.depthRead, (int)g_cfg.compute, (int)g_cfg.copyRes,
             (int)g_cfg.copyRegion, (int)g_cfg.updateSub,
             g_frameIndex, totalSec, g_frameIndex / (totalSec > 0 ? totalSec : 1.0));

    LOG_INFO("=== run complete: %d frames in %.2f s (%.1f fps average) ===",
             g_frameIndex, totalSec, g_frameIndex / (totalSec > 0 ? totalSec : 1.0));

    // -----------------------------------------------------------------------
    // Teardown
    // -----------------------------------------------------------------------

    LOG_INFO("Releasing resources");

    if (ctx) ctx->ClearState();

    for (auto* s : ballastSRV) if (s) s->Release();
    for (auto* t : ballastTex) if (t) t->Release();

    for (auto& rt : pyramidRT) rt.release();
    scratchRT.release();
    postRT[0].release();
    postRT[1].release();
    resolveRT.release();
    sceneRT.release();

    if (auxUAV)      auxUAV->Release();
    if (auxSRV)      auxSRV->Release();
    if (auxTex)      auxTex->Release();
    if (dummySRV)    dummySRV->Release();
    if (dummyTex)    dummyTex->Release();
    if (computeCS)   computeCS->Release();
    if (rasterState) rasterState->Release();
    if (depthState)  depthState->Release();
    if (wrapSampler) wrapSampler->Release();
    if (sampler)     sampler->Release();
    if (depthSRV)    depthSRV->Release();
    if (depthDSV)    depthDSV->Release();
    if (depthTex)    depthTex->Release();
    if (postCB)      postCB->Release();
    if (constBuffer) constBuffer->Release();
    if (indexBuffer) indexBuffer->Release();
    if (vertexBuffer) vertexBuffer->Release();
    if (inputLayout) inputLayout->Release();
    if (postPS)      postPS->Release();
    if (postVS)      postVS->Release();
    if (scenePS)     scenePS->Release();
    if (sceneVS)     sceneVS->Release();
    if (backRTV)     backRTV->Release();
    if (backBuffer)  backBuffer->Release();
    if (ctx)         ctx->Release();
    if (swapChain)   swapChain->Release();
    if (device)      device->Release();

    if (g_compilerDll) FreeLibrary(g_compilerDll);
    DestroyWindow(hwnd);

    LOG_INFO("=== d3d11_testbed exiting cleanly ===");
    g_log.close();
    return 0;
}