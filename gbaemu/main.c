#include "mgba/core/core.h"
#include "mgba/core/input.h"
#include "mgba/core/thread.h"
#include "mgba/gba/core.h"
#include "mgba/internal/gba/gba.h"
#include "mgba/internal/gba/input.h" // For GBA key macro
#include "mgba/feature/commandline.h"

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define UNICODE
#define _UNICODE
#include <audiosessiontypes.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define AUDIO_LATENCY 20
#include <xaudio2.h>
#include <xinput.h>

#pragma comment(lib, "xaudio2")
#pragma comment(lib, "xinput")

#define TINYGBA_BINDING_KEY 0x22334455
#define LCD_WIDTH  240
#define LCD_HEIGHT 160

unsigned width, height;
struct mCoreThread m_thread;
struct mCore* m_core;
struct mCoreSync* m_sync; // audio sync
struct mInputMap m_inputMap;
color_t* m_outputBuffer;

static bool quit = false;

struct {
    int width;
    int height;
    uint32_t *pixels;
} frame = {0};

// input
uint8_t controller1 = 0xff;

XINPUT_STATE controllerState;
bool controllerActive;

LRESULT CALLBACK WindowProcessMessage(HWND, UINT, WPARAM, LPARAM);

static BITMAPINFO frame_bitmap_info;
static HBITMAP frame_bitmap = 0;
static HDC frame_device_context = 0;

IXAudio2* m_xAudio2;
IXAudio2MasteringVoice* m_masterVoice;

XAUDIO2_BUFFER m_audioBuffer;
IXAudio2SourceVoice* m_sourceVoice;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow) {
    // CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // XAudio2Create(&m_xAudio2, 0, XAUDIO2_USE_DEFAULT_PROCESSOR);
    // IXAudio2_CreateMasteringVoice(m_xAudio2, &m_masterVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects );

    const char window_class_name[] = "TinyRetro_GBemu";
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

    RECT wr = {0, 0, LCD_WIDTH, LCD_HEIGHT};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    static HWND window_handle;
    window_handle = CreateWindow(window_class_name, "TinyGBA", WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, hInstance, NULL);
    if(window_handle == NULL) { return -1; }

    char* ROM_path;
    if (__argc < 2) {
        OPENFILENAME ofn;
        TCHAR szFile[260] = {0};

        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL; // Set owner window if needed
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = "GBA rom(*.gba)\0*.gba\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileName(&ofn) == TRUE) {
            // ROM_path = (char*)malloc(sizeof(szFile));
            // wcstombs(ROM_path, szFile, sizeof(szFile));
            ROM_path = szFile;
        } else {
            printf("File selection cancelled.\n");
            return 0;
        }
    } else {
        ROM_path = __argv[1];
    }

    // mgba init
    m_core = GBACoreCreate();
    m_core->init(m_core);
    mCoreInitConfig(m_core, NULL);

    // struct mCoreOptions opts = {0};
    // opts.useBios = true;
    // opts.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL;
    // mCoreConfigLoadDefaults(&m_core->config, &opts);
    // m_core->init(m_core);

    m_core->desiredVideoDimensions(m_core, &width, &height);
    m_outputBuffer = (color_t*)malloc(width * height * BYTES_PER_PIXEL);
    m_core->setVideoBuffer(m_core, m_outputBuffer, width);
    // m_core->setAudioBufferSize(m_core, SAMPLES);

    mCoreLoadFile(m_core, ROM_path);

    m_core->reset(m_core);

    m_thread.core = m_core;

    struct mInputPlatformInfo MyGBAInputInfo = {};
    MyGBAInputInfo.platformName = "gba";
    MyGBAInputInfo.nKeys = GBA_KEY_MAX;
    MyGBAInputInfo.hat.up = GBA_KEY_UP;
    MyGBAInputInfo.hat.down = GBA_KEY_DOWN;
    MyGBAInputInfo.hat.right = GBA_KEY_RIGHT;
    MyGBAInputInfo.hat.left = GBA_KEY_LEFT;

    mInputMapInit(&m_inputMap, &MyGBAInputInfo);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, 'X', GBA_KEY_A);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, 'Z', GBA_KEY_B);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, 'A', GBA_KEY_L);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, 'S', GBA_KEY_R);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, VK_RETURN, GBA_KEY_START);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, VK_BACK, GBA_KEY_SELECT);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, VK_UP, GBA_KEY_UP);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, VK_DOWN, GBA_KEY_DOWN);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, VK_LEFT, GBA_KEY_LEFT);
    mInputBindKey(&m_inputMap, TINYGBA_BINDING_KEY, VK_RIGHT, GBA_KEY_RIGHT);

    double dt = 0;

    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    LONGLONG prev_time = (LONGLONG)ft.dwLowDateTime + ((LONGLONG)(ft.dwHighDateTime) << 32LL);

    while(!quit) {
        GetSystemTimeAsFileTime(&ft);
        LONGLONG time = (LONGLONG)ft.dwLowDateTime + ((LONGLONG)(ft.dwHighDateTime) << 32LL);
        dt = (double)(time - prev_time) / 10000000.0;
        prev_time = time;

        static MSG message = { 0 };
        while(PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) { DispatchMessage(&message); }

        // // process xinput
        // ZeroMemory(&controllerState, sizeof(XINPUT_STATE));
        //
        // // Get the state of the controller.
        // DWORD result = XInputGetState(0, &controllerState);
        //
        // // Store whether the controller is currently connected or not.
        // if(result == ERROR_SUCCESS) {
        //     controllerActive = true;
        // } else {
        //     controllerActive = false;
        // }
        //
        // if (controllerActive) {
        //     bool buttonZ = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_A;
        //     bool buttonX = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_B;
        //     bool buttonDUP = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP;
        //     bool buttonDDOWN = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN;
        //     bool buttonDLEFT = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT;
        //     bool buttonDRIGHT = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT;
        //     bool buttonSelect = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK;
        //     bool buttonStart = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_START;
        //
        //     int thumbLeftX = (int)controllerState.Gamepad.sThumbLX;
        //     int thumbLeftY = (int)controllerState.Gamepad.sThumbLY;
        //     int magnitude = (int)sqrt((thumbLeftX * thumbLeftX) + (thumbLeftY * thumbLeftY));
        //     if(magnitude < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        //         thumbLeftX = 0;
        //         thumbLeftY = 0;
        //     }
        //     if (thumbLeftX < -1000) buttonDLEFT = true;
        //     if (thumbLeftX >  1000) buttonDRIGHT = true;
        //     if (thumbLeftY >  1000) buttonDUP = true;
        //     if (thumbLeftY < -1000) buttonDDOWN = true;
        //
        //     controller1 = 0xff;
        //     if (buttonZ)      controller1 &= ~0b00000001;
        //     if (buttonX)      controller1 &= ~0b00000010;
        //     if (buttonSelect) controller1 &= ~0b00000100;
        //     if (buttonStart)  controller1 &= ~0b00001000;
        //     if (buttonDRIGHT) controller1 &= ~0b00010000;
        //     if (buttonDLEFT)  controller1 &= ~0b00100000;
        //     if (buttonDUP)    controller1 &= ~0b01000000;
        //     if (buttonDDOWN)  controller1 &= ~0b10000000;
        // }

        // mgba step
        m_core->runFrame(m_core);

        // display video
        for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
            uint32_t* fbflat = m_outputBuffer;
            uint32_t c = fbflat[i];
            uint8_t b = (uint8_t)(c >> 16U);
            uint8_t g = (uint8_t)(c >> 8U);
            uint8_t r = (uint8_t)(c);
            frame.pixels[i] = (r << 16U) | (g << 8U) | b;
        }

        // // render audio
        // static bool start_audio = false;
        //
        // if (!start_audio) {
        //     WAVEFORMATEX waveFormat;
        //     ZeroMemory(&waveFormat, sizeof(waveFormat));
        //
        //     // Set the wave format for the buffer.
        //     waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        //     waveFormat.nSamplesPerSec = 32768;
        //     waveFormat.wBitsPerSample = 16;
        //     waveFormat.nChannels = 2;
        //     waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
        //     waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        //     waveFormat.cbSize = 0;
        //
        //     m_audioBuffer.AudioBytes = AUDIO_SAMPLES_TOTAL * sizeof(uint16_t) * AUDIO_LATENCY;
        //     m_audioBuffer.pAudioData = (BYTE*)tmp_apu_buffer;
        //
        //     HRESULT result = IXAudio2_CreateSourceVoice(m_xAudio2, &m_sourceVoice, &waveFormat, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
        //     if (FAILED(result)) {}
        //     else {
        //         IXAudio2SourceVoice_SubmitSourceBuffer(m_sourceVoice, &m_audioBuffer, NULL);
        //         IXAudio2SourceVoice_SetVolume(m_sourceVoice, 1.0f, 0);
        //         IXAudio2SourceVoice_SetFrequencyRatio(m_sourceVoice, 1.0f, 0);
        //         IXAudio2SourceVoice_Start(m_sourceVoice, 0, XAUDIO2_COMMIT_NOW);
        //     }
        //
        //     start_audio = true;
        // }
        //
        // // Fill in the audio buffer struct.
        // minigb_apu_audio_callback(&apu, tmp_apu_buffer + tmp_audio_pos * AUDIO_SAMPLES_TOTAL);
        // tmp_audio_pos++;
        //
        // if (tmp_audio_pos == AUDIO_LATENCY) {
        //     IXAudio2SourceVoice_Stop(m_sourceVoice, 0, XAUDIO2_COMMIT_NOW);
        //     IXAudio2SourceVoice_SubmitSourceBuffer(m_sourceVoice, &m_audioBuffer, NULL);
        //     IXAudio2SourceVoice_Start(m_sourceVoice, 0, XAUDIO2_COMMIT_NOW);
        //     tmp_audio_pos = 0;
        // }

        InvalidateRect(window_handle, NULL, FALSE);
        UpdateWindow(window_handle);

        double time_to_16ms = (1.0 / 60) - dt;
        if (time_to_16ms > 0)
            Sleep((int)(time_to_16ms * 1000));// NOLINT magic numbers
    }

    mInputMapDeinit(&m_inputMap);
    mCoreConfigDeinit(&m_core->config);
    m_core->deinit(m_core);
    free(m_outputBuffer);

    // if (m_sourceVoice) IXAudio2Voice_DestroyVoice(m_sourceVoice);
    // IXAudio2Voice_DestroyVoice(m_masterVoice);
    // IXAudio2_Release(m_xAudio2);
    // CoUninitialize();

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
            int key = mInputMapKey(&m_inputMap, TINYGBA_BINDING_KEY, wParam);
            m_core->addKeys(m_core, 1 << key);
        } break;

        case WM_KEYUP: {
            int key = mInputMapKey(&m_inputMap, TINYGBA_BINDING_KEY, wParam);
            m_core->clearKeys(m_core, 1 << key);
        } break;

        default: {
            return DefWindowProc(window_handle, message, wParam, lParam);
        }
    }
    return 0;
}