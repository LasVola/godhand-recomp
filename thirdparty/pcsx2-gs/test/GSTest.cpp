// Phase 1 test (docs/GS_PCSX2.md): open PCSX2's GS with the DX11 renderer on a window, send a GIF packet (a blue
// background and two sprites) and present it for a few seconds.
//   pcsx2_gs_test.exe [scale] [seconds]
#include "GS/GS.h"
#include "pcsx2gs_host.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

extern Pcsx2Config EmuConfig;

namespace
{
    LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        std::fprintf(stderr, "crash: exception %08lx at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
        for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                              SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i)
        {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
            const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line);
            std::fprintf(stderr, "  %s+0x%llx %s:%lu\n", named ? symbol->Name : "?", static_cast<unsigned long long>(displacement),
                         lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
        }
        std::fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    constexpr int kWidth = 1280;
    constexpr int kHeight = 896;

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (msg == WM_CLOSE)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    HWND CreateTestWindow()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"pcsx2_gs_test";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        RECT rect{0, 0, kWidth, kHeight};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowW(L"pcsx2_gs_test", L"pcsx2_gs_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        return hwnd;
    }

    // A+D packed GIF data.
    struct Packet
    {
        std::vector<u64> words;
        void ad(u8 reg, u64 value)
        {
            words.push_back(value);
            words.push_back(reg);
        }
        std::vector<u64> finish()
        {
            const u64 nloop = words.size() / 2u;
            std::vector<u64> out{nloop | (1ull << 15) | (1ull << 60), 0xEull}; // NLOOP, EOP, PACKED, NREG=1, REGS=A+D
            out.insert(out.end(), words.begin(), words.end());
            return out;
        }
    };

    u64 Rgbaq(u8 r, u8 g, u8 b)
    {
        return r | (u64(g) << 8) | (u64(b) << 16) | (0x80ull << 24) | (u64(0x3F800000u) << 32);
    }

    u64 Xyz(int x, int y)
    {
        return u64(x * 16) | (u64(y * 16) << 16);
    }

    void Sprite(Packet &p, int x0, int y0, int x1, int y1, u64 color)
    {
        p.ad(0x00, 6);     // PRIM: sprite
        p.ad(0x01, color); // RGBAQ
        p.ad(0x05, Xyz(x0, y0));
        p.ad(0x05, Xyz(x1, y1));
    }
}

int main(int argc, char *argv[])
{
    const int scale = argc > 1 ? std::atoi(argv[1]) : 2;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 3;
    SetUnhandledExceptionFilter(CrashFilter);
    HWND hwnd = CreateTestWindow();
    if (!hwnd)
        return 1;

    WindowInfo wi;
    wi.type = WindowInfo::Type::Win32;
    wi.window_handle = hwnd;
    wi.surface_width = kWidth;
    wi.surface_height = kHeight;
    pcsx2gs::SetRenderWindow(wi);
    const std::filesystem::path exeDir = std::filesystem::current_path();
    pcsx2gs::SetFolders("H:/JOGOS/pcsx2-ref/bin/resources", (exeDir / "gs_cache").string());
    std::filesystem::create_directories(exeDir / "gs_cache");

    // Privileged registers, PS2 layout. Urban Reign's 480p display: PMODE circuit 1, SMODE1 CMOD 0 / LC 32 (480p),
    // DISPLAY1 as the game sets it, DISPFB1 = frame buffer 0, 640 wide, PSMCT32.
    alignas(16) static u8 regs[0x2000] = {};
    auto reg = [](u32 offset, u64 value) { std::memcpy(regs + offset, &value, 8); };
    reg(0x0000, 0xFF21ull);            // PMODE: EN1, MMOD, ALP=FF
    reg(0x0010, 32ull << 3);           // SMODE1: LC=32, CMOD=0
    reg(0x0020, 0ull);                 // SMODE2: progressive
    reg(0x0070, 10ull << 9);           // DISPFB1: FBP 0, FBW 10, PSMCT32
    reg(0x0080, 0x1BF4FF00834138ull); // DISPLAY1

    Pcsx2Config::GSOptions options;
    options.Renderer = GSRendererType::DX11;
    options.UpscaleMultiplier = static_cast<float>(scale);
    EmuConfig.GS = options;
    if (!GSopen(options, GSRendererType::DX11, regs, GSVSyncMode::FIFO, false))
    {
        std::fprintf(stderr, "GSopen failed\n");
        return 2;
    }
    std::fprintf(stderr, "GS open: renderer %d, scale %d\n", static_cast<int>(GSGetCurrentRenderer()), scale);

    Packet p;
    p.ad(0x4C, 10ull << 16);                                   // FRAME_1: FBP 0, FBW 10, PSMCT32
    p.ad(0x4E, 1ull << 32);                                    // ZBUF_1: ZMSK
    p.ad(0x40, 639ull << 16 | 0ull << 32 | 447ull << 48);      // SCISSOR_1
    p.ad(0x18, 0ull);                                          // XYOFFSET_1
    p.ad(0x47, (1ull << 16) | (1ull << 17));                   // TEST_1: ZTE, ZTST always
    p.ad(0x1A, 1ull);                                          // PRMODECONT
    Sprite(p, 0, 0, 640, 448, Rgbaq(0x10, 0x30, 0x90));        // background
    Sprite(p, 100, 100, 300, 250, Rgbaq(0xE0, 0x20, 0x20));    // red
    Sprite(p, 340, 180, 600, 400, Rgbaq(0x20, 0xC0, 0x40));    // green
    const std::vector<u64> packet = p.finish();

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    u32 field = 0;
    bool running = true;
    while (running && std::chrono::steady_clock::now() < end)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        GSgifTransfer(reinterpret_cast<const u8 *>(packet.data()), static_cast<u32>(packet.size() / 2u));
        GSvsync(field, true);
        field ^= 1u;
    }
    int w = 0, h = 0;
    GSgetInternalResolution(&w, &h);
    std::fprintf(stderr, "internal resolution %dx%d\n", w, h);
    GSclose();
    return 0;
}
