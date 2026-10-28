// Xenon 2 Megablast (CDTV) on the original Xbox.
//
// The game's own 68000 code, recompiled (build/m68k), runs on the runtime
// in src/runtime. D:\image.bin is the user's X2 already relocated at $400
// (tools/extract.py); D:\data holds the rest of their CDTV files.
#include <xtl.h>

#include "runtime/chipset.h"
#include "runtime/cpu.h"
#include "runtime/highscores.h"
#include "runtime/platform.h"
#include "platform/xbox/renderer.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr UINT kWidth = 640, kHeight = 480;
constexpr u32 kStack = 0xC5FF0;          // the top of the slave's base memory

IDirect3DDevice8 *g_device;
IDirect3DTexture8 *g_screen;
HANDLE g_pads[4];
DWORD g_connected;
XINPUT_POLLING_PARAMETERS g_polling;
LARGE_INTEGER g_frequency, g_next;
u32 g_frames;
bool g_reference;                        // Back held: the reference picture alone

} // namespace

#ifdef XENON2_DISPLAY_CHECK
extern "C" uint32_t xenon2_tv[640 * 480];
uint32_t xenon2_tv[640 * 480];
#endif

namespace {

void init_display() {
    IDirect3D8 *d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) platform_fatal("Direct3DCreate8 failed");
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device)))
        platform_fatal("CreateDevice failed");
    if (FAILED(g_device->CreateTexture(512, 256, 1, 0, D3DFMT_LIN_X8R8G8B8, 0, &g_screen)))
        platform_fatal("no memory for the screen texture");
    renderer_init(g_device);
}

void open_pads() {
    for (DWORD port = 0; port < 4; ++port)
        if ((g_connected & (1u << port)) && !g_pads[port])
            g_pads[port] = XInputOpen(XDEVICE_TYPE_GAMEPAD, port, XDEVICE_NO_SLOT, &g_polling);
}

void init_input() {
    XDEVICE_PREALLOC_TYPE types[] = {{XDEVICE_TYPE_GAMEPAD, 4}};
    XInitDevices(1, types);
    g_polling.fAutoPoll = TRUE;
    g_polling.fInterruptOut = TRUE;
    g_polling.bInputInterval = 8;
    g_polling.bOutputInterval = 8;
    // Give the controllers a moment to enumerate.
    for (int i = 0; i < 50 && !g_connected; ++i) {
        g_connected = XGetDevices(XDEVICE_TYPE_GAMEPAD);
        Sleep(20);
    }
    open_pads();
}

XINPUT_GAMEPAD read_pad() {
    DWORD inserted = 0, removed = 0;
    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &inserted, &removed)) {
        for (int port = 0; port < 4; ++port)
            if ((removed & (1u << port)) && g_pads[port]) {
                XInputClose(g_pads[port]);
                g_pads[port] = nullptr;
            }
        g_connected = (g_connected & ~removed) | inserted;
        open_pads();
    }
    XINPUT_GAMEPAD pad = {};
    for (HANDLE h : g_pads) {
        XINPUT_STATE state;
        if (h && XInputGetState(h, &state) == ERROR_SUCCESS) return state.Gamepad;
    }
    return pad;
}

void draw(const uint32_t *pixels, int width, int height, int top, int bottom) {
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(g_screen->LockRect(0, &locked, nullptr, 0))) {
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch, pixels + size_t(y) * width,
                        size_t(width) * 4);
        g_screen->UnlockRect(0);
    }
    // The whole frame at the same scale as before, moved so the display
    // window's rows sit in the middle of the screen.
    struct Vertex { float x, y, z, rhw, u, v; };
    const float w = float(width), h = float(height);
    const float scale = kHeight / h;
    const float y0 = kHeight * .5f - (top + bottom) * .5f * scale - .5f, y1 = y0 + kHeight;
    const Vertex quad[4] = {
        {-.5f, y0, 0, 1, 0, 0}, {kWidth - .5f, y0, 0, 1, w, 0},
        {-.5f, y1, 0, 1, 0, h}, {kWidth - .5f, y1, 0, 1, w, h},
    };
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    g_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetTexture(0, g_screen);
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    g_device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    g_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
    g_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    // The playfield natively over it (the score panel stays the picture's);
    // with Back held, the reference picture alone.
    if (!g_reference) renderer_draw_playfield(kWidth / w, y0, scale);
#ifdef XENON2_DISPLAY_CHECK
    // The screen as drawn, for screenshots (xemu can't take them over its
    // monitor): now and then, as reading it back is slow.
    IDirect3DSurface8 *back = nullptr;
    if (g_frames % 25 == 0 && (g_device->BlockUntilIdle(), SUCCEEDED(g_device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)))) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(back->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            for (UINT y = 0; y < kHeight; ++y)
                std::memcpy(xenon2_tv + y * kWidth, static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch, kWidth * 4);
            back->UnlockRect();
        }
        back->Release();
    }
#endif
    g_device->SetTexture(0, nullptr);
    g_device->Present(nullptr, nullptr, nullptr, nullptr);
}

bool read_file(const char *path, std::vector<uint8_t> &out) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    out.resize(size);
    DWORD got = 0;
    const BOOL ok = ReadFile(f, out.data(), size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

} // namespace

void platform_log(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    OutputDebugStringA(line);
}

void platform_fatal(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    OutputDebugStringA("*** xenon2 fatal: ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    platform_log("68k: d0-d7 %08X %08X %08X %08X %08X %08X %08X %08X\n", R.d[0], R.d[1], R.d[2], R.d[3], R.d[4],
                 R.d[5], R.d[6], R.d[7]);
    platform_log("68k: a0-a7 %08X %08X %08X %08X %08X %08X %08X %08X pc %06X sr %04X\n", R.a[0], R.a[1], R.a[2],
                 R.a[3], R.a[4], R.a[5], R.a[6], R.a[7], R.pc, rt_get_sr());
    cpu_dump();
    for (;;) Sleep(1000);
}

bool platform_read(const char *name, std::vector<uint8_t> &out) {
    // The CD's file names are any case; the disc's are as in the install.
    static const char *const kNames[] = {"Intro", "Level1", "Level2", "Level3", "Level4", "Level5", "Piccode",
                                         "Picdata1", "Picdata2", "Picdata3", "Picdata4", "Picdata5", "Picdata6",
                                         "Picdata7", "Picdata8", "Picdata9", "Picdatab", "Picdatac", "X", "X2"};
    char path[128];
    for (const char *n : kNames)
        if (!_stricmp(n, name)) {
            std::snprintf(path, sizeof(path), "D:\\data\\%s", n);
            return read_file(path, out);
        }
    // Sounds00-09, Speech00-66: the same spelling with a capital first letter.
    char fixed[64];
    std::snprintf(fixed, sizeof(fixed), "%s", name);
    for (char *p = fixed; *p; ++p) *p = char(std::tolower(u8(*p)));
    fixed[0] = char(std::toupper(u8(fixed[0])));
    std::snprintf(path, sizeof(path), "D:\\data\\%s", fixed);
    return read_file(path, out);
}

// The save: a save game ("High Scores") on the hard disk's U:, so the
// dashboard lists it.
bool save_path(const char *name, char *path, size_t size) {
    char dir[MAX_PATH];
    if (XCreateSaveGame("U:\\", L"High Scores", OPEN_ALWAYS, 0, dir, sizeof(dir)) != ERROR_SUCCESS) return false;
    std::snprintf(path, size, "%s%s", dir, name);     // dir ends in a backslash
    return true;
}

bool platform_save_read(const char *name, std::vector<uint8_t> &out) {
    char path[MAX_PATH];
    return save_path(name, path, sizeof(path)) && read_file(path, out);
}

bool platform_save_write(const char *name, const void *data, size_t bytes) {
    char path[MAX_PATH];
    if (!save_path(name, path, sizeof(path))) return false;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, data, DWORD(bytes), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == bytes;
}

// A test input script (D:\inputs.txt, from build/inputs.txt): lines of
// "<from frame> <to frame> <buttons>", buttons any of U D L R F G (fire 1, 2).
struct ScriptLine { u32 from, to; Joystick j; };
std::vector<ScriptLine> g_script;

void load_script() {
    std::vector<uint8_t> text;
    if (!read_file("D:\\inputs.txt", text)) return;
    text.push_back(0);
    const char *p = reinterpret_cast<const char *>(text.data());
    while (*p) {
        ScriptLine l = {};
        char buttons[16] = {};
        if (std::sscanf(p, "%u %u %15s", &l.from, &l.to, buttons) == 3) {
            for (const char *b = buttons; *b; ++b) {
                l.j.up |= *b == 'U'; l.j.down |= *b == 'D'; l.j.left |= *b == 'L'; l.j.right |= *b == 'R';
                l.j.fire |= *b == 'F'; l.j.fire2 |= *b == 'G';
            }
            g_script.push_back(l);
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    platform_log("xenon2: input script, %u lines\n", unsigned(g_script.size()));
}

Joystick platform_joystick() {
    if (!g_script.empty()) {
        Joystick j = {};
        for (const ScriptLine &l : g_script)
            if (g_frames >= l.from && g_frames <= l.to) j = l.j;
        return j;
    }
    const XINPUT_GAMEPAD pad = read_pad();
    g_reference = pad.wButtons & XINPUT_GAMEPAD_BACK;
    const SHORT x = pad.sThumbLX, y = pad.sThumbLY;
    const auto analog = [&](int index) { return pad.bAnalogButtons[index] > 30; };
    Joystick j;
    j.left = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || x < -12000;
    j.right = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || x > 12000;
    j.up = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) || y > 12000;
    j.down = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || y < -12000;
    j.fire = analog(XINPUT_GAMEPAD_A) || analog(XINPUT_GAMEPAD_RIGHT_TRIGGER);
    j.fire2 = analog(XINPUT_GAMEPAD_B) || analog(XINPUT_GAMEPAD_X);
    return j;
}

void platform_frame(const uint32_t *pixels, int width, int height, int top, int bottom) {
    draw(pixels, width, height, top, bottom);
    static int shown_top = -1, shown_bottom = -1, changes;
    if ((top != shown_top || bottom != shown_bottom) && changes++ < 50)
        platform_log("xenon2: picture rows %d-%d\n", top, bottom);
    shown_top = top;
    shown_bottom = bottom;
    // PAL: 50 frames a second.
    const LONGLONG period = g_frequency.QuadPart / 50;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < g_next.QuadPart) {
        const DWORD ms = DWORD((g_next.QuadPart - now.QuadPart) * 1000 / g_frequency.QuadPart);
        if (ms) Sleep(ms);
        g_next.QuadPart += period;
    } else {
        g_next.QuadPart = now.QuadPart + period;     // behind: don't try to catch up
    }
    if (++g_frames % 250 == 0) platform_log("frame %u\n", g_frames);
}

void __cdecl main() {
    platform_log("xenon2: starting\n");
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_next);
    init_display();
    init_input();
    platform_audio_start();
    load_script();

    std::vector<uint8_t> image;
    if (!read_file("D:\\image.bin", image) || image.size() > kChipSize)
        platform_fatal("D:\\image.bin missing: build with tools/build.py and your Xenon 2 CDTV install");
    std::memcpy(MEM, image.data(), image.size());

    chipset_reset();
    cpu_reset(0x400, kStack);
    wl(0x6C, kNativeVblank);
    highscores_load();
    platform_log("xenon2: running\n");
    cpu_run();
}
