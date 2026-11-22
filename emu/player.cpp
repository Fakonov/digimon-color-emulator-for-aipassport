// ---------------------------------------------------------------------------
//  digimon-player - interactive front end for the PC side unSP emulator.
//
//  Renders the ST7735 visible area (96x96, MAME's visarea is x 0..95, y 18..113)
//  scaled 4x into a plain GDI window, maps the three face buttons to PORTA, runs
//  the SoC at real time and persists the game's flash save block.
//
//  Controls
//    J / Z / Left Ctrl .... button 1  (PORTA 0x0008, active low)
//    K / X ................ button 2  (PORTA 0x0010)
//    L / C ................ button 3  (PORTA 0x0020)
//    F1 ................... BMP screenshot into saves/
//    F2 ................... toggle fast forward (uncapped)
//    Esc .................. quit (flash save is written)
//
//  Same SoC/CPU bring-up as emu/main.cpp; see docs/EMU.md for the validation
//  numbers that back the models used here.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>

#include "unsp.h"
#include "gpce4.h"

namespace {

constexpr int        CPU_CLOCK    = 96000000;
constexpr int        FPS          = 60;
constexpr int64_t    FRAME_CYCLES = CPU_CLOCK / FPS;   // 1,600,000
constexpr int        SLICE_CYCLES = 2000;              // keeps every timer source exact
constexpr int        PANEL        = 128;               // GRAM window mirrored by St7735
constexpr int        VIS_X        = 0;                 // MAME visarea: 0..95
constexpr int        VIS_Y        = 18;                // MAME visarea: 18..113
constexpr int        VIS_W        = 96;
constexpr int        VIS_H        = 96;
constexpr int        SCALE        = 4;

std::vector<uint8_t> load_file(const char *path, const char *what)
{
	std::FILE *fp = std::fopen(path, "rb");
	if (!fp) { std::fprintf(stderr, "cannot open %s (%s)\n", path, what); std::exit(1); }
	std::fseek(fp, 0, SEEK_END);
	long len = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	std::vector<uint8_t> buf((size_t)len);
	if (len > 0 && std::fread(buf.data(), 1, (size_t)len, fp) != (size_t)len)
	{
		std::fprintf(stderr, "short read on %s\n", path);
		std::exit(1);
	}
	std::fclose(fp);
	return buf;
}

Gpce4          *g_soc = nullptr;
unsp_20_device *g_cpu = nullptr;
uint16_t        g_port_bits = 0xffff;   // PORTA is active low: 1 = released
bool            g_fast = false;
int             g_shots = 0;
HWND            g_wnd = nullptr;

void apply_port()
{
	g_soc->set_port(0, g_port_bits);
}

void screenshot()
{
	char name[64];
	std::snprintf(name, sizeof(name), "saves/shot_%03d.bmp", g_shots++);
	BITMAPINFOHEADER bih{};
	bih.biSize = sizeof(bih);
	bih.biWidth = VIS_W;
	bih.biHeight = -VIS_H;               // top-down
	bih.biPlanes = 1;
	bih.biBitCount = 24;
	bih.biCompression = BI_RGB;

	BITMAPINFO bi{};
	bi.bmiHeader = bih;
	uint8_t *bits = nullptr;
	HDC hdc = GetDC(g_wnd);
	HBITMAP dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
	if (dib && bits)
	{
		const int stride = (VIS_W * 3 + 3) & ~3;
		for (int y = 0; y < VIS_H; y++)
		{
			uint8_t *row = bits + (size_t)y * stride;
			const uint16_t *src = g_soc->lcd.fb + (size_t)(VIS_Y + y) * PANEL + VIS_X;
			for (int x = 0; x < VIS_W; x++)
			{
				const uint16_t c = src[x];
				const uint8_t r = (uint8_t)((c >> 11) & 0x1f), g = (uint8_t)((c >> 5) & 0x3f), b = (uint8_t)(c & 0x1f);
				row[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
				row[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
				row[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
			}
		}
		BITMAPFILEHEADER fh{};
		fh.bfType = 0x4d42;
		fh.bfOffBits = sizeof(fh) + sizeof(bih);
		fh.bfSize = fh.bfOffBits + (DWORD)stride * VIS_H;
		FILE *fp = std::fopen(name, "wb");
		if (fp)
		{
			std::fwrite(&fh, sizeof(fh), 1, fp);
			std::fwrite(&bih, sizeof(bih), 1, fp);
			std::fwrite(bits, 1, (size_t)stride * VIS_H, fp);
			std::fclose(fp);
			std::printf("[shot] %s\n", name);
		}
		GdiFlush();
		DeleteObject(dib);
	}
	ReleaseDC(g_wnd, hdc);
}

void set_button(uint16_t mask, bool down)
{
	if (down) g_port_bits &= (uint16_t)~mask;
	else      g_port_bits |= mask;
	apply_port();
}

bool is_down_key(WPARAM vk)
{
	return (GetKeyState((int)vk) & 0x8000) != 0;
}

void poll_keyboard()
{
	// Polled instead of tracked through WM_KEYDOWN so a lost WM_KEYUP (alt-tab,
	// focus change) can never leave a button stuck down.
	set_button(0x0008, is_down_key('J') || is_down_key('Z') || is_down_key(VK_LCONTROL));
	set_button(0x0010, is_down_key('K') || is_down_key('X'));
	set_button(0x0020, is_down_key('L') || is_down_key('C'));
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CLOSE:
		DestroyWindow(h);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_KEYDOWN:
		if (wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
		if (wp == VK_F1) { screenshot(); return 0; }
		if (wp == VK_F2) { g_fast = !g_fast; std::printf("[player] fast=%d\n", g_fast ? 1 : 0); return 0; }
		break;
	default:
		break;
	}
	return DefWindowProc(h, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
	std::vector<uint8_t> irom = load_file("roms/internal_rom_221017a.bin", "internal rom");
	std::vector<uint8_t> flash = load_file("roms/dmc_v1_base.bin", "spi flash");

	CreateDirectoryA("saves", nullptr);
	if (std::FILE *fp = std::fopen("saves/flash.bin", "rb"))
	{
		std::fseek(fp, 0, SEEK_END);
		long len = std::ftell(fp);
		std::fseek(fp, 0, SEEK_SET);
		if ((size_t)len == flash.size() && std::fread(flash.data(), 1, flash.size(), fp) == flash.size())
			std::printf("[player] loaded saves/flash.bin\n");
		else
			std::printf("[player] saves/flash.bin ignored (%ld bytes)\n", len);
		std::fclose(fp);
	}

	Gpce4 soc;
	soc.set_images(irom.data(), irom.size(), flash.data(), flash.size());
	soc.reset();

	unsp_20_device cpu;
	cpu.m_bus = &soc;
	soc.set_cpu(&cpu);
	cpu.reset_state();
	cpu.m_vectorbase = 0x4010;
	cpu.m_core->m_r[0] = 0;
	for (int r = 1; r < 16; r++) cpu.m_core->m_r[r] = 0;
	cpu.m_core->m_r[unsp_device::REG_PC] = soc.read16(0x4010 + 0x7);
	cpu.m_core->m_enable_irq = 0;
	cpu.m_core->m_enable_fiq = 0;
	cpu.m_core->m_fir_move = 1;
	cpu.m_core->m_pri = 8;
	cpu.m_core->m_divq_bit = 0xffffffffu;

	g_soc = &soc;
	g_cpu = &cpu;
	apply_port();

	WNDCLASSA wc{};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = "DigimonColorPlayer";
	RegisterClassA(&wc);

	const int win_w = VIS_W * SCALE;
	const int win_h = VIS_H * SCALE;
	RECT rc{0, 0, win_w, win_h};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	HWND wnd = CreateWindowA("DigimonColorPlayer", "Digimon Color - unSP emulator",
	                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                         rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
	if (!wnd) { std::fprintf(stderr, "CreateWindow failed\n"); return 1; }
	g_wnd = wnd;
	ShowWindow(wnd, SW_SHOW);

	// Panel framebuffer: VIS_W x VIS_H top-down 32-bit DIB, refreshed each frame.
	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = VIS_W;
	bi.bmiHeader.biHeight = -VIS_H;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	uint32_t *pixels = nullptr;
	HDC mem = CreateCompatibleDC(nullptr);
	HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&pixels, nullptr, 0);
	SelectObject(mem, dib);

	int64_t cycle = 0;
		int64_t frame = 0;
		auto t_start = std::chrono::steady_clock::now();
		auto last_report = t_start;
		bool key_f3_down = false;

		for (;;)
		{
			MSG msg;
			while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT) goto done;
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}

			// F3 toggles the game's 60x time-lapse flag (0x0D03). The handset has
			// no RTC chip and the boot path freezes its software clock at 0:00:00
			// (docs/RE_RTC.md), so the host seeds the clock at frame 120 and, when
			// time-lapse is on, the game's own 2 Hz handler adds +0x3C seconds per
			// tick -- one game minute per guest second.
			if (is_down_key(VK_F3))
			{
				if (!key_f3_down)
				{
					key_f3_down = true;
					const bool on = soc.ram[0x0D03] == 0;
					soc.ram[0x0D03] = on ? 1 : 0;
					soc.ram[0x0CB0] = 1;
					std::printf("[player] time-lapse=%d\n", on ? 1 : 0);
				}
			}
			else key_f3_down = false;

			poll_keyboard();

			if (frame == 120)
			{
				time_t now_t = time(nullptr);
				struct tm lt;
				localtime_s(&lt, &now_t);
				soc.ram[0x0CB0] = 1;
				soc.ram[0x0CAF] = (uint16_t)lt.tm_hour;
				soc.ram[0x0CAE] = (uint16_t)lt.tm_min;
				soc.ram[0x0CAD] = (uint16_t)lt.tm_sec;
				soc.ram[0x0CB1] = 0;
				soc.ram[0x0CB2] = 1;
				std::printf("[player] clock seeded %02d:%02d:%02d from host\n", lt.tm_hour, lt.tm_min, lt.tm_sec);
			}

		const int64_t frame_end = cycle + FRAME_CYCLES;
		while (cycle < frame_end)
		{
			const int64_t remain = frame_end - cycle;
			uint32_t slice = soc.cycles_to_next_event((uint32_t)(remain < SLICE_CYCLES ? remain : SLICE_CYCLES));
			if (slice == 0) slice = 1;
			cpu.m_core->m_icount = (int32_t)slice - 1;
			cpu.execute_run();
			const int64_t consumed = (int64_t)((int32_t)slice - 1) - (int64_t)(int32_t)cpu.m_core->m_icount;
			soc.tick((uint32_t)(consumed > 0 ? consumed : slice));
			cycle += consumed > 0 ? consumed : slice;
		}
		frame++;

		// blit: VISarea -> 32-bit DIB (XRGB), black outside the window
		for (int y = 0; y < VIS_H; y++)
		{
			const uint16_t *src = soc.lcd.fb + (size_t)(VIS_Y + y) * PANEL + VIS_X;
			uint32_t *dst = pixels + (size_t)y * VIS_W;
			for (int x = 0; x < VIS_W; x++)
			{
				const uint16_t c = src[x];
				const uint32_t r = (uint32_t)(((c >> 11) & 0x1f) << 3);
				const uint32_t g = (uint32_t)(((c >> 5) & 0x3f) << 2);
				const uint32_t b = (uint32_t)((c & 0x1f) << 3);
				dst[x] = (r << 16) | (g << 8) | b;
			}
		}

		HDC dc = GetDC(wnd);
		SetStretchBltMode(dc, COLORONCOLOR);
		StretchBlt(dc, 0, 0, VIS_W * SCALE, VIS_H * SCALE, mem, 0, 0, VIS_W, VIS_H, SRCCOPY);
		ReleaseDC(wnd, dc);

		if (!g_fast)
		{
			// real time pacing against the wall clock, so audio/video timing of the
			// guest stays in step with the host
			const auto now = std::chrono::steady_clock::now();
			const int64_t due_us = frame * 1000000 / FPS;
			const int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - t_start).count();
			if (elapsed_us < due_us)
				Sleep((DWORD)((due_us - elapsed_us) / 1000));
		}

		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count() >= 2000)
		{
			last_report = now;
			const double wall = std::chrono::duration<double>(now - t_start).count();
			char title[160];
			std::snprintf(title, sizeof(title), "Digimon Color - %d frames, %.2fs guest, %.0f fps, PC=%06X",
			              (int)frame, (double)cycle / CPU_CLOCK, frame / (wall > 0 ? wall : 1),
			              cpu.m_core->m_r[unsp_device::REG_PC] & 0xffff);
			SetWindowTextA(wnd, title);
		}
	}

done:
	if (FILE *fp = std::fopen("saves/flash.bin", "wb"))
	{
		std::fwrite(flash.data(), 1, flash.size(), fp);
		std::fclose(fp);
		std::printf("[player] flash saved to saves/flash.bin\n");
	}
	if (dib) DeleteObject(dib);
	if (mem) DeleteDC(mem);
	return 0;
}
