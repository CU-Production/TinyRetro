#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "NES.h"

static bool quit = false;
constexpr auto nes_width  = 256;
constexpr auto nes_height = 240;

struct {
    int width;
    int height;
    uint32_t *pixels;
} frame = {0};

// input
uint8_t controller1 = 0;

LRESULT CALLBACK WindowProcessMessage(HWND, UINT, WPARAM, LPARAM);

static BITMAPINFO frame_bitmap_info;
static HBITMAP frame_bitmap = 0;
static HDC frame_device_context = 0;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow) {
    const wchar_t window_class_name[] = L"TinyRetro_Nesemu";
    static WNDCLASS window_class = { 0 };
    window_class.lpfnWndProc = WindowProcessMessage;
    window_class.hInstance = hInstance;
    window_class.lpszClassName = window_class_name;
    RegisterClass(&window_class);

    frame_bitmap_info.bmiHeader.biSize = sizeof(frame_bitmap_info.bmiHeader);
    frame_bitmap_info.bmiHeader.biPlanes = 1;
    frame_bitmap_info.bmiHeader.biBitCount = 32;
    frame_bitmap_info.bmiHeader.biCompression = BI_RGB;
    frame_device_context = CreateCompatibleDC(0);

    RECT wr = {0, 0, nes_width, nes_height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    static HWND window_handle;
    window_handle = CreateWindow(window_class_name, L"TinyNES", WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, hInstance, NULL);
    if(window_handle == NULL) { return -1; }

    char* ROM_path;
    if (__argc < 2) {
        MessageBox(window_handle, L"Please pass ROM path as first parameter.", L"Error", MB_OK);
        std::cerr << "Please pass ROM path as first parameter.\n";
        // return EXIT_FAILURE;
    } else {
        ROM_path = __argv[1];
    }

    char* SRAM_path = new char[strlen(ROM_path) + 1];
    strcpy(SRAM_path, ROM_path);
    strcat(SRAM_path, ".srm");

    std::cout << "Initializing NES..." << std::endl;
    NES* nes = new NES(ROM_path, SRAM_path);
    if (!nes->initialized) return EXIT_FAILURE;



    double dt = 0;

    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    LONGLONG prev_time = (LONGLONG)ft.dwLowDateTime + ((LONGLONG)(ft.dwHighDateTime) << 32LL);

    while(!quit) {
        GetSystemTimeAsFileTime(&ft);
        LONGLONG time = (LONGLONG)ft.dwLowDateTime + ((LONGLONG)(ft.dwHighDateTime) << 32LL);
        dt = static_cast<double>(time - prev_time) / 10000000.0;
        prev_time = time;

        static MSG message = { 0 };
        while(PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) { DispatchMessage(&message); }

        // GetAsyncKeyState & 0x8000 keydown
        // GetAsyncKeyState & 0b1    keypress
        for (int i = 0; i < 256; i++) {
            if ((GetAsyncKeyState(i) & 0x8000) || (GetAsyncKeyState(i) & 0b1)) {
                controller1 |= (i == 'z' || i == 'Z');       // A
                controller1 |= (i == 'x' || i == 'X') << 1;  // B
                controller1 |= (i == VK_BACK)         << 2;  // Select
                controller1 |= (i == VK_RETURN)       << 3;  // Start
                controller1 |= (i == VK_UP)           << 4;
                controller1 |= (i == VK_DOWN)         << 5;
                controller1 |= (i == VK_LEFT)         << 6;
                controller1 |= (i == VK_RIGHT)        << 7;
            }
        }

        // processe input
        nes->controller1->buttons = controller1;
        nes->controller2->buttons = 0;

        // step the NES state forward by 'dt' seconds, or more if in fast-forward
        emulate(nes, dt);

        memcpy(frame.pixels, nes->ppu->front, sizeof(uint32_t) * nes_width * nes_height);
        for (int i = 0; i < nes_width * nes_height; i++) {
            uint32_t c = nes->ppu->front[i];
            uint8_t b = static_cast<uint8_t>(c >> 16U);
            uint8_t g = static_cast<uint8_t>(c >> 8U);
            uint8_t r = static_cast<uint8_t>(c);
            frame.pixels[i] = (r << 16U) | (g << 8U) | b;
        }

        nes->apu->stream.clear();

        InvalidateRect(window_handle, NULL, FALSE);
        UpdateWindow(window_handle);

        double time_to_16ms = (1.0 / 60) - dt;
        if (time_to_16ms > 0)
            Sleep(static_cast<int>(time_to_16ms * 1000));// NOLINT magic numbers
    }

    return 0;
}


LRESULT CALLBACK WindowProcessMessage(HWND window_handle, UINT message, WPARAM wParam, LPARAM lParam) {
    switch(message) {
        case WM_QUIT:
        case WM_DESTROY: {
            quit = true;
        } break;

        case WM_PAINT: {
            static PAINTSTRUCT paint;
            static HDC device_context;
            device_context = BeginPaint(window_handle, &paint);
            BitBlt(device_context,
                   paint.rcPaint.left, paint.rcPaint.top,
                   paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top,
                   frame_device_context,
                   paint.rcPaint.left, paint.rcPaint.top,
                   SRCCOPY);
            EndPaint(window_handle, &paint);
        } break;

        case WM_SIZE: {
            frame_bitmap_info.bmiHeader.biWidth  = LOWORD(lParam);
            frame_bitmap_info.bmiHeader.biHeight = -HIWORD(lParam);

            if(frame_bitmap) DeleteObject(frame_bitmap);
            frame_bitmap = CreateDIBSection(NULL, &frame_bitmap_info, DIB_RGB_COLORS, (void**)&frame.pixels, 0, 0);
            SelectObject(frame_device_context, frame_bitmap);

            frame.width =  LOWORD(lParam);
            frame.height = HIWORD(lParam);
        } break;

        case WM_KEYDOWN: {
            switch(wParam) {
                case 'Z':       controller1 |= 0b00000001; break;
                case 'X':       controller1 |= 0b00000010; break;
                case VK_BACK:   controller1 |= 0b00000100; break;
                case VK_RETURN: controller1 |= 0b00001000; break;
                case VK_UP:     controller1 |= 0b00010000; break;
                case VK_DOWN:   controller1 |= 0b00100000; break;
                case VK_LEFT:   controller1 |= 0b01000000; break;
                case VK_RIGHT:  controller1 |= 0b10000000; break;
            }
        } break;

        case WM_KEYUP: {
            switch(wParam) {
                case 'Z':       controller1 &= 0b11111110; break;
                case 'X':       controller1 &= 0b11111101; break;
                case VK_BACK:   controller1 &= 0b11111011; break;
                case VK_RETURN: controller1 &= 0b11110111; break;
                case VK_UP:     controller1 &= 0b11101111; break;
                case VK_DOWN:   controller1 &= 0b11011111; break;
                case VK_LEFT:   controller1 &= 0b10111111; break;
                case VK_RIGHT:  controller1 &= 0b01111111; break;
            }
        } break;

        default: {
            return DefWindowProc(window_handle, message, wParam, lParam);
        }
    }
    return 0;
}
