#include "mgba/core/core.h"
#include "mgba/core/input.h"
#include "mgba/core/thread.h"
#include "mgba/gba/core.h"
#include "mgba/internal/gba/gba.h"
#include "mgba/internal/gba/input.h" // For GBA key macro
#include "mgba/feature/commandline.h"
#include "mgba/core/blip_buf.h" // For blip_t audio buffers
#include "mgba/internal/gba/audio.h" // For GBA audio functions

#define UNICODE
#define _UNICODE
#include <audiosessiontypes.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include <xinput.h>

#pragma comment(lib, "xinput")
#pragma comment(lib, "kernel32")
#pragma comment(lib, "ole32")

#define TINYGBA_BINDING_KEY 0x22334455
#define LCD_WIDTH  240
#define LCD_HEIGHT 160

static const CLSID CLSID_MMDeviceEnumerator  = { 0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e} };
static const IID IID_IMMDeviceEnumerator     = { 0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6} };
static const IID IID_IAudioClient            = { 0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2} };
static const IID IID_IAudioRenderClient      = { 0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2} };

const static GUID SOUNDIO_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = { 0x00000003,0x0000,0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const static GUID SOUNDIO_KSDATAFORMAT_SUBTYPE_PCM        = { 0x00000001,0x0000,0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Audio constants
#define AUDIO_SAMPLE_RATE 32768
#define AUDIO_CHANNELS 2
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_BUFFER_DURATION_MS 30  // Increased buffer size for smoother audio
#define SAMPLES_PER_FRAME 512        // Reduced for more frequent updates

unsigned width, height;
struct mCoreThread m_thread;
struct mCore* m_core;
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

// WASAPI Audio variables
static IMMDeviceEnumerator* deviceEnumerator = NULL;
static IMMDevice* device = NULL;
static IAudioClient* audioClient = NULL;
static IAudioRenderClient* renderClient = NULL;
static HANDLE audioEvent = NULL;
static HANDLE audioThread = NULL;
static UINT32 bufferFrameCount = 0;
static bool audioInitialized = false;
static int16_t* audioBuffer = NULL;
static size_t audioBufferSize = 0;
static volatile size_t audioWritePos = 0;
static volatile size_t audioReadPos = 0;
static CRITICAL_SECTION audioLock;

// Audio ring buffer for mGBA audio data
static int16_t* audioRingBuffer = NULL;
static size_t ringBufferSize = 0;
static volatile size_t ringWritePos = 0;
static volatile size_t ringReadPos = 0;

// WASAPI audio thread
static DWORD WINAPI audioThreadProc(LPVOID lpParam) {
    HRESULT hr;
    UINT32 paddingFrames, availableFrames;
    BYTE* data;
    
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    while (!quit) {
        WaitForSingleObject(audioEvent, INFINITE);
        
        if (quit) break;
        
        hr = audioClient->lpVtbl->GetCurrentPadding(audioClient, &paddingFrames);
        if (FAILED(hr)) continue;
        
        availableFrames = bufferFrameCount - paddingFrames;
        
        if (availableFrames > 0) {
            hr = renderClient->lpVtbl->GetBuffer(renderClient, availableFrames, &data);
            if (SUCCEEDED(hr)) {
                EnterCriticalSection(&audioLock);
                
                // Copy samples from our ring buffer to WASAPI buffer
                int16_t* output = (int16_t*)data;
                static int16_t lastLeft = 0, lastRight = 0; // For smooth interpolation
                
                for (UINT32 i = 0; i < availableFrames * 2; i += 2) {
                    if (ringReadPos != ringWritePos) {
                        size_t readIndex = (ringReadPos * 2) % ringBufferSize;
                        int16_t currentLeft = audioRingBuffer[readIndex];
                        int16_t currentRight = audioRingBuffer[readIndex + 1];
                        
                        // Simple interpolation for smoother audio
                        output[i] = (currentLeft + lastLeft) / 2;     // Left
                        output[i + 1] = (currentRight + lastRight) / 2; // Right
                        
                        lastLeft = currentLeft;
                        lastRight = currentRight;
                        
                        ringReadPos = (ringReadPos + 1) % (ringBufferSize / 2);
                    } else {
                        // Buffer underrun - use last known values for smooth transition
                        output[i] = lastLeft / 2;     // Fade out gradually
                        output[i + 1] = lastRight / 2;
                        
                        lastLeft = lastLeft * 3 / 4;  // Gradual fade
                        lastRight = lastRight * 3 / 4;
                    }
                }
                
                LeaveCriticalSection(&audioLock);
                
                hr = renderClient->lpVtbl->ReleaseBuffer(renderClient, availableFrames, 0);
            }
        }
    }
    
    CoUninitialize();
    return 0;
}

// Audio callback function to be called when mGBA has audio data
static void processAudioSamples(int16_t* samples, size_t nSamples) {
    if (!audioInitialized || !audioRingBuffer) return;
    
    EnterCriticalSection(&audioLock);
    
    // Copy stereo samples to ring buffer with overflow protection
    for (size_t i = 0; i < nSamples; i++) {
        size_t writeIndex = (ringWritePos * 2) % ringBufferSize;
        size_t nextWritePos = (ringWritePos + 1) % (ringBufferSize / 2);
        
        // Check for buffer overflow - if so, skip oldest sample
        if (nextWritePos == ringReadPos) {
            // Buffer is full, advance read position to make room
            ringReadPos = (ringReadPos + 1) % (ringBufferSize / 2);
        }
        
        // Apply simple anti-aliasing filter to reduce noise
        static int16_t prevLeft = 0, prevRight = 0;
        int16_t currentLeft = samples[i * 2];
        int16_t currentRight = samples[i * 2 + 1];
        
        // Simple low-pass filter (50% current + 50% previous)
        int16_t filteredLeft = (currentLeft + prevLeft) / 2;
        int16_t filteredRight = (currentRight + prevRight) / 2;
        
        audioRingBuffer[writeIndex] = filteredLeft;
        audioRingBuffer[writeIndex + 1] = filteredRight;
        
        prevLeft = currentLeft;
        prevRight = currentRight;
        
        ringWritePos = nextWritePos;
    }
    
    LeaveCriticalSection(&audioLock);
}

// Initialize WASAPI audio
static bool initializeAudio(void) {
    HRESULT hr;
    WAVEFORMATEXTENSIBLE waveFormat = {0};
    
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    // Create device enumerator
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                         &IID_IMMDeviceEnumerator, (void**)&deviceEnumerator);
    if (FAILED(hr)) {
        printf("Failed to create device enumerator: 0x%08x\n", hr);
        return false;
    }
    
    // Get default audio endpoint
    hr = deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(deviceEnumerator, eRender, eConsole, &device);
    if (FAILED(hr)) {
        printf("Failed to get default audio endpoint: 0x%08x\n", hr);
        return false;
    }
    
    // Create audio client
    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&audioClient);
    if (FAILED(hr)) {
        printf("Failed to activate audio client: 0x%08x\n", hr);
        return false;
    }
    
    // Set up wave format
    // waveFormat.Format.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormat.Format.nChannels = AUDIO_CHANNELS;
    waveFormat.Format.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    waveFormat.Format.wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    waveFormat.Format.nBlockAlign = (waveFormat.Format.wBitsPerSample / 8) * waveFormat.Format.nChannels;
    waveFormat.Format.nAvgBytesPerSec = waveFormat.Format.nSamplesPerSec * waveFormat.Format.nBlockAlign;
    waveFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat.Samples.wValidBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    if (AUDIO_CHANNELS == 1) {
        waveFormat.dwChannelMask = SPEAKER_FRONT_CENTER;
    }
    else {
        waveFormat.dwChannelMask = SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
    }
    waveFormat.SubFormat = SOUNDIO_KSDATAFORMAT_SUBTYPE_PCM;
    
    // Initialize audio client
    hr = audioClient->lpVtbl->Initialize(audioClient, AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                AUDIO_BUFFER_DURATION_MS * 10000, // Convert ms to 100ns units
                                0, (WAVEFORMATEX*)&waveFormat, NULL);
    if (FAILED(hr)) {
        printf("Failed to initialize audio client: 0x%08x\n", hr);
        return false;
    }
    
    // Get buffer size
    hr = audioClient->lpVtbl->GetBufferSize(audioClient, &bufferFrameCount);
    if (FAILED(hr)) {
        printf("Failed to get buffer size: 0x%08x\n", hr);
        return false;
    }
    
    // Create event for audio callback
    audioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!audioEvent) {
        printf("Failed to create audio event\n");
        return false;
    }
    
    // Set event handle
    hr = audioClient->lpVtbl->SetEventHandle(audioClient, audioEvent);
    if (FAILED(hr)) {
        printf("Failed to set event handle: 0x%08x\n", hr);
        return false;
    }
    
    // Get render client
    hr = audioClient->lpVtbl->GetService(audioClient, &IID_IAudioRenderClient, (void**)&renderClient);
    if (FAILED(hr)) {
        printf("Failed to get render client: 0x%08x\n", hr);
        return false;
    }
    
    // Create audio ring buffer
    ringBufferSize = SAMPLES_PER_FRAME * 8 * 2; // 8 frames worth of stereo samples
    audioRingBuffer = (int16_t*)malloc(ringBufferSize * sizeof(int16_t));
    if (!audioRingBuffer) {
        printf("Failed to allocate audio ring buffer\n");
        return false;
    }
    memset(audioRingBuffer, 0, ringBufferSize * sizeof(int16_t));
    
    InitializeCriticalSection(&audioLock);
    
    // Create audio thread
    audioThread = CreateThread(NULL, 0, audioThreadProc, NULL, 0, NULL);
    if (!audioThread) {
        printf("Failed to create audio thread\n");
        return false;
    }
    
    // Start audio client
    hr = audioClient->lpVtbl->Start(audioClient);
    if (FAILED(hr)) {
        printf("Failed to start audio client: 0x%08x\n", hr);
        return false;
    }
    
    audioInitialized = true;
    printf("WASAPI audio initialized successfully\n");
    return true;
}

// Cleanup audio
static void cleanupAudio(void) {
    quit = true;
    
    if (audioEvent) {
        SetEvent(audioEvent);
    }
    
    if (audioThread) {
        WaitForSingleObject(audioThread, 5000);
        CloseHandle(audioThread);
        audioThread = NULL;
    }
    
    if (audioClient) {
        audioClient->lpVtbl->Stop(audioClient);
        audioClient->lpVtbl->Release(audioClient);
        audioClient = NULL;
    }
    
    if (renderClient) {
        renderClient->lpVtbl->Release(renderClient);
        renderClient = NULL;
    }
    
    if (device) {
        device->lpVtbl->Release(device);
        device = NULL;
    }
    
    if (deviceEnumerator) {
        deviceEnumerator->lpVtbl->Release(deviceEnumerator);
        deviceEnumerator = NULL;
    }
    
    if (audioEvent) {
        CloseHandle(audioEvent);
        audioEvent = NULL;
    }
    
    if (audioRingBuffer) {
        free(audioRingBuffer);
        audioRingBuffer = NULL;
    }
    
    DeleteCriticalSection(&audioLock);
    CoUninitialize();
    audioInitialized = false;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow) {
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

    m_core->desiredVideoDimensions(m_core, &width, &height);
    m_outputBuffer = (color_t*)malloc(width * height * BYTES_PER_PIXEL);
    m_core->setVideoBuffer(m_core, m_outputBuffer, width);
    
    // Set up audio buffer size for mGBA
    m_core->setAudioBufferSize(m_core, SAMPLES_PER_FRAME);
    
    // Initialize WASAPI audio
    if (!initializeAudio()) {
        printf("Failed to initialize audio\n");
    }

    mCoreLoadFile(m_core, ROM_path);
    m_core->reset(m_core);
    
    // Don't use mCoreThread for now - use direct calls to avoid conflicts
    // This is simpler and more stable for our WASAPI integration

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

        // Run mGBA core directly
        m_core->runFrame(m_core);
        
        // Process audio for WASAPI output
        if (audioInitialized) {
            // Create a temporary buffer for audio data
            int16_t mgbaAudioSamples[SAMPLES_PER_FRAME * 2] = {0}; // Stereo buffer
            
            /*
             * OPTIMIZED mGBA AUDIO INTEGRATION
             * 
             * ✅ WASAPI audio system is fully functional
             * ✅ Direct audio reading from mGBA blip_t buffers
             * ✅ Noise reduction and audio smoothing
             */
            
            // Get mGBA audio channels (blip_t buffers)
            blip_t* left = NULL;
            blip_t* right = NULL;
            
            if (m_core) {
                left = m_core->getAudioChannel(m_core, 0);  // Left channel
                right = m_core->getAudioChannel(m_core, 1); // Right channel
            }
            
            bool gotRealAudio = false;
            
            if (left && right) {
                // Set blip buffer rates for proper sample rate conversion
                int32_t clockRate = m_core->frequency(m_core);
                blip_set_rates(left, clockRate, AUDIO_SAMPLE_RATE);
                blip_set_rates(right, clockRate, AUDIO_SAMPLE_RATE);
                
                // Check how many samples are available
                int available = blip_samples_avail(left);
                int rightAvailable = blip_samples_avail(right);
                
                // Use the minimum available samples to avoid desync
                if (rightAvailable < available) {
                    available = rightAvailable;
                }
                
                if (available > SAMPLES_PER_FRAME) {
                    available = SAMPLES_PER_FRAME;
                }
                
                if (available > 0) {
                    // Read stereo samples from mGBA's blip buffers
                    blip_read_samples(left, mgbaAudioSamples, available, 2); // Left into even indices
                    blip_read_samples(right, mgbaAudioSamples + 1, available, 2); // Right into odd indices
                    
                    // Apply simple volume normalization to reduce clipping noise
                    static const float volumeScale = 0.5f; // Reduce volume slightly to prevent clipping
                    for (int i = 0; i < available * 2; i++) {
                        mgbaAudioSamples[i] = (int16_t)(mgbaAudioSamples[i] * volumeScale);
                    }
                    
                    gotRealAudio = true;
                    
                    // Fill remaining buffer with silence to prevent noise
                    if (available < SAMPLES_PER_FRAME) {
                        memset(&mgbaAudioSamples[available * 2], 0, 
                               (SAMPLES_PER_FRAME - available) * 2 * sizeof(int16_t));
                    }
                } else {
                    // If no samples available, output silence to prevent noise
                    memset(mgbaAudioSamples, 0, SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
                    gotRealAudio = true; // Don't fall back to test tone
                }
            }
            
            // Only use test audio if mGBA audio channels aren't available at all
            if (!gotRealAudio) {
                // Output silence instead of test tone to reduce noise
                memset(mgbaAudioSamples, 0, SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
                
                // Optional: very quiet test tone only for debugging
                static bool enableTestTone = false; // Set to true for debugging
                if (enableTestTone) {
                    static uint32_t frameCounter = 0;
                    frameCounter++;
                    
                    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
                        double time = (double)(frameCounter * SAMPLES_PER_FRAME + i) / AUDIO_SAMPLE_RATE;
                        double baseFreq = 440.0;
                        double amplitude = 50.0; // Very quiet
                        
                        int16_t sample = (int16_t)(amplitude * sin(2.0 * 3.14159265359 * baseFreq * time));
                        
                        mgbaAudioSamples[i * 2] = sample;     // Left channel
                        mgbaAudioSamples[i * 2 + 1] = sample; // Right channel
                    }
                }
            }
            
            processAudioSamples(mgbaAudioSamples, SAMPLES_PER_FRAME);
        }

        // display video
        for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
            uint32_t* fbflat = m_outputBuffer;
            uint32_t c = fbflat[i];
            uint8_t b = (uint8_t)(c >> 16U);
            uint8_t g = (uint8_t)(c >> 8U);
            uint8_t r = (uint8_t)(c);
            frame.pixels[i] = (r << 16U) | (g << 8U) | b;
        }

        InvalidateRect(window_handle, NULL, FALSE);
        UpdateWindow(window_handle);

        double time_to_16ms = (1.0 / 60) - dt;
        if (time_to_16ms > 0)
            Sleep((int)(time_to_16ms * 1000));
    }

    // Cleanup
    cleanupAudio();
    
    mInputMapDeinit(&m_inputMap);
    mCoreConfigDeinit(&m_core->config);
    m_core->deinit(m_core);
    free(m_outputBuffer);

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