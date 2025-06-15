#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#define AUDIO_LATENCY 20
#include <xaudio2.h>
#include <xinput.h>

#pragma comment(lib, "xaudio2")
#pragma comment(lib, "xinput")

#define ENABLE_SOUND 1
#define ENABLE_LCD   1
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);
#include "peanut_gb.h"
#include "minigb_apu/minigb_apu.h"


struct priv_t
{
	uint8_t *rom;
	uint8_t *cart_ram;

    /* Frame buffer */
    uint32_t fb[LCD_HEIGHT][LCD_WIDTH];
};

uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
	const struct priv_t* const p = (const struct priv_t* const)gb->direct.priv;
	return p->rom[addr];
}

uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
	const struct priv_t* const p = (const struct priv_t* const)gb->direct.priv;
	return p->cart_ram[addr];
}

void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
	const struct priv_t* const p = (const struct priv_t* const)gb->direct.priv;
	p->cart_ram[addr] = val;
}

uint8_t* read_rom_to_ram(const char *file_name) {
	FILE *rom_file = fopen(file_name, "rb");
	size_t rom_size;
	uint8_t *rom = NULL;

	if(rom_file == NULL)
		return NULL;

	fseek(rom_file, 0, SEEK_END);
	rom_size = ftell(rom_file);
	rewind(rom_file);
	rom = (uint8_t*)malloc(rom_size);

	if(fread(rom, sizeof(uint8_t), rom_size, rom_file) != rom_size) {
		free(rom);
		fclose(rom_file);
		return NULL;
	}

	fclose(rom_file);
	return rom;
}

void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val) {
	const char* gb_err_str[GB_INVALID_MAX] = {
		"UNKNOWN",
		"INVALID OPCODE",
		"INVALID READ",
		"INVALID WRITE",
		"HALT FOREVER"
	};
	struct priv_t* priv = (struct priv_t*)gb->direct.priv;

	fprintf(stderr, "Error %d occurred: %s at %04X\n. Exiting.\n",
			gb_err, gb_err_str[gb_err], val);

	/* Free memory and then exit. */
	free(priv->cart_ram);
	free(priv->rom);
	exit(EXIT_FAILURE);
}

#if ENABLE_LCD
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
	struct priv_t* priv = (struct priv_t*)gb->direct.priv;
	const uint32_t palette[] = { 0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000 };

    for(unsigned int x = 0; x < LCD_WIDTH; x++)
        priv->fb[line][x] = palette[pixels[x] & 3];
}
#endif

static struct minigb_apu_ctx apu;

uint8_t audio_read(uint16_t addr) {
    return minigb_apu_audio_read(&apu, addr);
}

void audio_write(uint16_t addr, uint8_t val) {
    minigb_apu_audio_write(&apu, addr, val);
}

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

uint32_t tmp_audio_pos = 0;
audio_sample_t tmp_apu_buffer[AUDIO_LATENCY * AUDIO_SAMPLES_TOTAL];
XAUDIO2_BUFFER m_audioBuffer;
IXAudio2SourceVoice* m_sourceVoice;

static struct gb_s gb;
static struct priv_t priv;
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    XAudio2Create(&m_xAudio2, 0, XAUDIO2_USE_DEFAULT_PROCESSOR);
    IXAudio2_CreateMasteringVoice(m_xAudio2, &m_masterVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects );

    const wchar_t window_class_name[] = L"TinyRetro_GBemu";
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
    window_handle = CreateWindow(window_class_name, L"TinyGB", WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU | WS_VISIBLE,
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
        ofn.lpstrFilter = L"GB rom(*.gb)\0*.gb\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileName(&ofn) == TRUE) {
            ROM_path = (char*)malloc(sizeof(szFile));
            wcstombs(ROM_path, szFile, sizeof(szFile));
        } else {
            printf("File selection cancelled.\n");
            return 0;
        }
    } else {
        ROM_path = __argv[1];
    }

    /* Must be freed */
    char *rom_file_name = ROM_path;
    enum gb_init_error_e ret;

    if((priv.rom = read_rom_to_ram(rom_file_name)) == NULL) {
        printf("%d: %s\n", __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write, &gb_error, &priv);

    if(ret != GB_INIT_NO_ERROR) {
        printf("Error: %d\n", ret);
        exit(EXIT_FAILURE);
    }

    priv.cart_ram = (uint8_t*)malloc(gb_get_save_size(&gb));

#if ENABLE_LCD
    gb_init_lcd(&gb, &lcd_draw_line);
    //    gb.direct.interlace = 1;
#endif

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO bufferInfo;
    GetConsoleScreenBufferInfo(output, &bufferInfo);

    minigb_apu_audio_init(&apu);

    controllerActive = false;


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

        // process xinput
        ZeroMemory(&controllerState, sizeof(XINPUT_STATE));

        // Get the state of the controller.
        DWORD result = XInputGetState(0, &controllerState);

        // Store whether the controller is currently connected or not.
        if(result == ERROR_SUCCESS) {
            controllerActive = true;
        } else {
            controllerActive = false;
        }

        if (controllerActive) {
            bool buttonZ = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_A;
            bool buttonX = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_B;
            bool buttonDUP = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP;
            bool buttonDDOWN = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN;
            bool buttonDLEFT = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT;
            bool buttonDRIGHT = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT;
            bool buttonSelect = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK;
            bool buttonStart = controllerState.Gamepad.wButtons & XINPUT_GAMEPAD_START;

            int thumbLeftX = (int)controllerState.Gamepad.sThumbLX;
            int thumbLeftY = (int)controllerState.Gamepad.sThumbLY;
            int magnitude = (int)sqrt((thumbLeftX * thumbLeftX) + (thumbLeftY * thumbLeftY));
            if(magnitude < XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
                thumbLeftX = 0;
                thumbLeftY = 0;
            }
            if (thumbLeftX < -1000) buttonDLEFT = true;
            if (thumbLeftX >  1000) buttonDRIGHT = true;
            if (thumbLeftY >  1000) buttonDUP = true;
            if (thumbLeftY < -1000) buttonDDOWN = true;

            controller1 = 0xff;
            if (buttonZ)      controller1 &= ~0b00000001;
            if (buttonX)      controller1 &= ~0b00000010;
            if (buttonSelect) controller1 &= ~0b00000100;
            if (buttonStart)  controller1 &= ~0b00001000;
            if (buttonDRIGHT) controller1 &= ~0b00010000;
            if (buttonDLEFT)  controller1 &= ~0b00100000;
            if (buttonDUP)    controller1 &= ~0b01000000;
            if (buttonDDOWN)  controller1 &= ~0b10000000;
        }

        // processe input
        gb.direct.joypad = controller1;

        // step the NES state forward by 'dt' seconds, or more if in fast-forward
        gb_run_frame(&gb);

        // display video
        for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
            uint32_t* fbflat = priv.fb;
            uint32_t c = fbflat[i];
            uint8_t b = (uint8_t)(c >> 16U);
            uint8_t g = (uint8_t)(c >> 8U);
            uint8_t r = (uint8_t)(c);
            frame.pixels[i] = (r << 16U) | (g << 8U) | b;
        }

        // render audio
        static bool start_audio = false;

        if (!start_audio) {
            WAVEFORMATEX waveFormat;
            ZeroMemory(&waveFormat, sizeof(waveFormat));

            // Set the wave format for the buffer.
            waveFormat.wFormatTag = WAVE_FORMAT_PCM;
            waveFormat.nSamplesPerSec = 32768;
            waveFormat.wBitsPerSample = 16;
            waveFormat.nChannels = 2;
            waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
            waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
            waveFormat.cbSize = 0;

            m_audioBuffer.AudioBytes = AUDIO_SAMPLES_TOTAL * sizeof(uint16_t) * AUDIO_LATENCY;
            m_audioBuffer.pAudioData = (BYTE*)tmp_apu_buffer;

            HRESULT result = IXAudio2_CreateSourceVoice(m_xAudio2, &m_sourceVoice, &waveFormat, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
            if (FAILED(result)) {}
            else {
                IXAudio2SourceVoice_SubmitSourceBuffer(m_sourceVoice, &m_audioBuffer, NULL);
                IXAudio2SourceVoice_SetVolume(m_sourceVoice, 1.0f, 0);
                IXAudio2SourceVoice_SetFrequencyRatio(m_sourceVoice, 1.0f, 0);
                IXAudio2SourceVoice_Start(m_sourceVoice, 0, XAUDIO2_COMMIT_NOW);
            }

            start_audio = true;
        }

        // Fill in the audio buffer struct.
        minigb_apu_audio_callback(&apu, tmp_apu_buffer + tmp_audio_pos * AUDIO_SAMPLES_TOTAL);
        tmp_audio_pos++;

        if (tmp_audio_pos == AUDIO_LATENCY) {
            IXAudio2SourceVoice_Stop(m_sourceVoice, 0, XAUDIO2_COMMIT_NOW);
            IXAudio2SourceVoice_SubmitSourceBuffer(m_sourceVoice, &m_audioBuffer, NULL);
            IXAudio2SourceVoice_Start(m_sourceVoice, 0, XAUDIO2_COMMIT_NOW);
            tmp_audio_pos = 0;
        }

        InvalidateRect(window_handle, NULL, FALSE);
        UpdateWindow(window_handle);

        double time_to_16ms = (1.0 / 60) - dt;
        if (time_to_16ms > 0)
            Sleep((int)(time_to_16ms * 1000));// NOLINT magic numbers
    }

    if (m_sourceVoice) IXAudio2Voice_DestroyVoice(m_sourceVoice);
    IXAudio2Voice_DestroyVoice(m_masterVoice);
    IXAudio2_Release(m_xAudio2);
    CoUninitialize();

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
                case 'Z':       controller1 &= 0b11111110; break;
                case 'X':       controller1 &= 0b11111101; break;
                case VK_BACK:   controller1 &= 0b11111011; break;
                case VK_RETURN: controller1 &= 0b11110111; break;
                case VK_RIGHT:  controller1 &= 0b11101111; break;
                case VK_LEFT:   controller1 &= 0b11011111; break;
                case VK_UP:     controller1 &= 0b10111111; break;
                case VK_DOWN:   controller1 &= 0b01111111; break;
            }
        } break;

        case WM_KEYUP: {
            switch(wParam) {
                case 'Z':       controller1 |= 0b00000001; break;
                case 'X':       controller1 |= 0b00000010; break;
                case VK_BACK:   controller1 |= 0b00000100; break;
                case VK_RETURN: controller1 |= 0b00001000; break;
                case VK_RIGHT:  controller1 |= 0b00010000; break;
                case VK_LEFT:   controller1 |= 0b00100000; break;
                case VK_UP:     controller1 |= 0b01000000; break;
                case VK_DOWN:   controller1 |= 0b10000000; break;
            }
        } break;

        default: {
            return DefWindowProc(window_handle, message, wParam, lParam);
        }
    }
    return 0;
}

