#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libuvc/libuvc.h>
#include <libwebsockets.h>
#include <stdbool.h>
#include <time.h>
#include "common.h"
#include "motion_detector.h"

#define PORT 2138
#define MAX_FRAME_SIZE (2 * 1024 * 1024)
#define FPS 30
#define STREAM_FPS 5
#define FPS_INTERVAL (1000 / STREAM_FPS)
#define ANALYSE_INTERVAL (1000 / 15)
#define JSON_INTERVAL_MS 10000
#define LWS_TIMEOUT 100

// Globalne zmienne dla kamery
static uvc_device_handle_t *g_devHandler = NULL;
static uvc_stream_ctrl_t g_streamCtrl;
static volatile bool g_isStreaming = false;
static pthread_mutex_t g_streamMutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    volatile bool connectionEstablished;
    struct lws *wsi;  // Dodany wskaźnik do wsi
    struct timespec lastSentTime;
    struct timespec lastJsonSentTime;
    struct timespec lastFrameSentTime;
    unsigned char frameBuffer[MAX_FRAME_SIZE];
    size_t frameSize;
    volatile int hasNewFrame;
    volatile int frameCounter;
    volatile bool motionDetectedFlag;
    void* motionDetector;
    pthread_mutex_t mutex;
} AppState;

static long long timespec_diff_ms(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000LL + 
           (end->tv_nsec - start->tv_nsec) / 1000000LL;
}

static void callbackUVC(uvc_frame_t *frame, void *ptr)
{
    AppState *state = (AppState*)ptr;
    
    if (!state->connectionEstablished || !state->wsi)
    {
        return;
    }
    
    if (!frame || frame->data_bytes == 0 || frame->data_bytes > MAX_FRAME_SIZE)
    {
        return;
    }

    struct timespec timeNow;
    clock_gettime(CLOCK_MONOTONIC, &timeNow);
    long long elapsedTime = timespec_diff_ms(&state->lastSentTime, &timeNow);
    if ((elapsedTime < FPS_INTERVAL) || (elapsedTime < ANALYSE_INTERVAL))
    {
        // printf("[callbackUVC]  escape from frame anaylyse \n");
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    unsigned char prevFrameBuffer[MAX_FRAME_SIZE];
    size_t prevFrameSize;
    
    pthread_mutex_lock(&state->mutex);
    memcpy(prevFrameBuffer, state->frameBuffer, state->frameSize);
    prevFrameSize = state->frameSize;
    
    memcpy(state->frameBuffer, frame->data, frame->data_bytes);
    state->frameSize = frame->data_bytes;
    state->hasNewFrame = 1;
    state->lastSentTime = timeNow;
    state->frameCounter++;
    if(state->frameCounter >= 30)
    {
        state->frameCounter = 0;
    }
    pthread_mutex_unlock(&state->mutex);

    // Detekcja ruchu co drugą klatkę
    // if(prevFrameSize > 0 && state->frameCounter % 2 == 0)
    // {
    bool motionNow = motion_detector_detect(
        state->motionDetector,
        state->frameBuffer, state->frameSize,
        prevFrameBuffer, prevFrameSize
    );
    if(motionNow)
    {
        pthread_mutex_lock(&state->mutex);
        state->motionDetectedFlag = true;
        pthread_mutex_unlock(&state->mutex);
    }
    // }

    // Sprawdź czy czas na wysłanie JSON lub ramki
    long long elapsedJsonTime = timespec_diff_ms(&state->lastJsonSentTime, &timeNow);
    long long elapsedFrameTime = timespec_diff_ms(&state->lastFrameSentTime, &timeNow);
    
    // printf("[callbackUVC] elapsedJsonTime = %llu, JSON_INTERVAL_MS=%u \n", elapsedJsonTime, JSON_INTERVAL_MS);
    // printf("[callbackUVC] elapsedFrameTime = %llu, FPS_INTERVAL=%u \n", elapsedFrameTime, FPS_INTERVAL);
    // Jeśli minął odpowiedni czas, oznacz wsi jako writable
    if (elapsedJsonTime >= JSON_INTERVAL_MS || elapsedFrameTime >= FPS_INTERVAL)
    {
        // printf("[callbackUVC] DO NAW lws_callback_on_writable(state->wsi) \n");
        pthread_mutex_lock(&state->mutex);
        lws_callback_on_writable(state->wsi);
        struct lws_context *context = lws_get_context(state->wsi);
        lws_cancel_service(context);
        // lws_service(context, 50);
        pthread_mutex_unlock(&state->mutex);
    }
}

// Funkcja do startowania streamu
static bool startCameraStream(AppState *state)
{
    pthread_mutex_lock(&g_streamMutex);
    
    if (g_isStreaming)
    {
        pthread_mutex_unlock(&g_streamMutex);
        return true;
    }
    
    if (!g_devHandler)
    {
        fprintf(stderr, "[CAM] Błąd: brak uchwytu kamery\n");
        pthread_mutex_unlock(&g_streamMutex);
        return false;
    }
    
    uvc_error_t res = uvc_start_streaming(g_devHandler, &g_streamCtrl, callbackUVC, state, 0);
    if (res < 0)
    {
        uvc_perror(res, "[CAM] Błąd uruchomienia streamu");
        pthread_mutex_unlock(&g_streamMutex);
        return false;
    }
    
    g_isStreaming = true;
    fprintf(stderr, "[CAM] Stream uruchomiony\n");
    pthread_mutex_unlock(&g_streamMutex);
    return true;
}

// Funkcja do zatrzymywania streamu
static void stopCameraStream(void)
{
    pthread_mutex_lock(&g_streamMutex);
    
    if (!g_isStreaming)
    {
        pthread_mutex_unlock(&g_streamMutex);
        return;
    }
    
    if (g_devHandler)
    {
        uvc_stop_streaming(g_devHandler);
        fprintf(stderr, "[CAM] Stream zatrzymany\n");
    }
    
    g_isStreaming = false;
    pthread_mutex_unlock(&g_streamMutex);
}

static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len)
{
    (void)in;
    (void)len;
    (void)user;
    
    AppState *state = (AppState *)lws_context_user(lws_get_context(wsi));
    if (!state)
    {
        return -1;
    }
    
    switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
    {
        fprintf(stderr, "[WS] Klient połączony\n");
        
        pthread_mutex_lock(&state->mutex);
        state->connectionEstablished = true;
        state->wsi = wsi;  // Zapisz wskaźnik do wsi
        state->hasNewFrame = 0;
        state->frameSize = 0;
        state->frameCounter = 0;
        state->motionDetectedFlag = false;
        
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        state->lastSentTime = timeNow;
        state->lastJsonSentTime = timeNow;
        state->lastFrameSentTime = timeNow;
        memset(state->frameBuffer, 0, MAX_FRAME_SIZE);
        pthread_mutex_unlock(&state->mutex);
        
        // Uruchom kamerę
        startCameraStream(state);
        break;
    }
    
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        
        pthread_mutex_lock(&state->mutex);
        
        // Sprawdź czy jest ramka do wysłania
        if (!state->hasNewFrame || state->frameSize == 0)
        {
            // printf("[WEBSOCKET] tate->hasNewFrame=%d\n", state->hasNewFrame);
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        
        // Oblicz czasy
        long long elapsedJsonTime = timespec_diff_ms(&state->lastJsonSentTime, &timeNow);
        long long elapsedFrameTime = timespec_diff_ms(&state->lastFrameSentTime, &timeNow);
        
        // printf("[WEBSOCKET] hasFrame=%d, frameSize=%zu, elapsedJson=%lld, elapsedFrame=%lld\n",
        //        state->hasNewFrame, state->frameSize, elapsedJsonTime, elapsedFrameTime);
        
        // Priorytet 1: Wyślij JSON z informacją o ruchu (co 10s)
        if(elapsedJsonTime >= JSON_INTERVAL_MS)
        {
            bool motion = state->motionDetectedFlag;
            state->motionDetectedFlag = false;
            state->lastJsonSentTime = timeNow;
            pthread_mutex_unlock(&state->mutex);
            
            // printf("[WEBSOCKET] Wysyłam JSON: motion=%d\n", motion);
            
            char jsonBuffer[512];
            snprintf(jsonBuffer, sizeof(jsonBuffer),
                "{\"motion\":%s,\"timestamp\":%ld}",
                motion ? "true" : "false",
                timeNow.tv_sec);
            
            size_t jsonLen = strlen(jsonBuffer);
            unsigned char *buf = (unsigned char*)malloc(LWS_PRE + jsonLen);
            if (buf)
            {
                memcpy(buf + LWS_PRE, jsonBuffer, jsonLen);
                lws_write(wsi, buf + LWS_PRE, jsonLen, LWS_WRITE_TEXT);
                free(buf);
            }
            
            // Po wysłaniu, pozwól callbackUVC zdecydować o kolejnym writable
            break;
        }
        //  printf("[WEBSOCKET] elapsedFrameTime = %llu, FPS_INTERVAL=%u \n", elapsedFrameTime, FPS_INTERVAL);
        // Priorytet 2: Wyślij ramkę video (co ~66ms dla 15 FPS)
        if(elapsedFrameTime >= FPS_INTERVAL)
        {
            // printf("[WEBSOCKET] Wysyłam ramkę: size=%zu\n", state->frameSize);
            
            size_t frameSize = state->frameSize;
            unsigned char *buf = (unsigned char*)malloc(LWS_PRE + frameSize);
            
            if (buf)
            {
                // Kopiuj POD mutexem
                memcpy(buf + LWS_PRE, state->frameBuffer, frameSize);
                
                // Aktualizuj state POD mutexem
                state->hasNewFrame = 0;
                state->lastFrameSentTime = timeNow;
                
                pthread_mutex_unlock(&state->mutex);
                
                // Wyślij POZA mutexem
                int written = lws_write(wsi, buf + LWS_PRE, frameSize, LWS_WRITE_BINARY);
                // printf("[callbackWs] Wysłano %d bajtów\n", written);
                free(buf);
            }
            else
            {
                pthread_mutex_unlock(&state->mutex);
            }
            
            // Po wysłaniu, pozwól callbackUVC zdecydować o kolejnym writable
            break;
        }
        
        // Jeśli żaden warunek nie został spełniony
        // printf("[callbackWs] Za wcześnie na wysłanie (json=%lld, frame=%lld)\n",
        //        elapsedJsonTime, elapsedFrameTime);
        pthread_mutex_unlock(&state->mutex);
        break;
    }
    
    case LWS_CALLBACK_CLOSED:
    {
        fprintf(stderr, "[WS] Klient rozłączony\n");

        pthread_mutex_lock(&state->mutex);
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        state->connectionEstablished = false;
        state->wsi = NULL;  // Wyczyść wskaźnik do wsi
        state->hasNewFrame = 0;
        state->frameSize = 0;
        state->frameCounter = 0;
        state->motionDetectedFlag = false;
        state->lastSentTime = timeNow;
        state->lastJsonSentTime = timeNow;
        state->lastFrameSentTime = timeNow;
        pthread_mutex_unlock(&state->mutex);
        
        // Zatrzymaj kamerę
        stopCameraStream();
        break;
    }
        
    default:
        break;
    }
    return 0;
}

int main(void)
{
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    signal(SIGSEGV, handleSignal);
    signal(SIGABRT, handleSignal);

    FILE *logFile = fopen("/var/log/camService.log", "a");
    setvbuf(logFile, NULL, _IOLBF, 0);
    stderr = logFile;

    MotionParams motionParams = {
        .motionThreshold = 20,
        .minArea = 200,
        .gaussBlur = 21
    };

    struct timespec timeNow;
    clock_gettime(CLOCK_MONOTONIC, &timeNow);

    AppState state = {
        .connectionEstablished = false,
        .wsi = NULL,
        .lastSentTime = {0, 0},
        .lastJsonSentTime = timeNow,
        .lastFrameSentTime = timeNow,
        .frameBuffer = {0},
        .frameSize = 0,
        .hasNewFrame = 0,
        .frameCounter = 0,
        .motionDetectedFlag = false,
        .motionDetector = motion_detector_init(640, 480, motionParams)
    };
    pthread_mutex_init(&state.mutex, NULL);

    if (!state.motionDetector)
    {
        fprintf(stderr, "Błąd: nie udało się zainicjalizować detektora ruchu\n");
        return 1;
    }

    uvc_context_t *camContext;
    uvc_device_t *device;
    uvc_error_t res;

    // Inicjalizacja UVC
    res = uvc_init(&camContext, NULL);
    if (res < 0)
    {
        uvc_perror(res, "uvc_init");
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    res = uvc_find_device(camContext, &device, 0, 0, NULL);
    if (res < 0)
    {
        uvc_perror(res, "find_device");
        motion_detector_destroy(state.motionDetector);
        uvc_exit(camContext);
        return 1;
    }

    res = uvc_open(device, &g_devHandler);
    if (res < 0)
    {
        uvc_perror(res, "uvc_open");
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    res = uvc_get_stream_ctrl_format_size(g_devHandler, &g_streamCtrl, 
                                          UVC_FRAME_FORMAT_MJPEG, 640, 480, 0);
    if (res < 0)
    {
        uvc_perror(res, "get_stream_ctrl");
        uvc_close(g_devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    // WebSocket
    struct lws_protocols protocols[] =
    {   
        { "cam-protocol", callbackWs, 0, MAX_FRAME_SIZE, 0, NULL, 0},
        { NULL, NULL, 0, 0, 0, NULL, 0 }
    };

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = PORT;
    info.protocols = protocols;
    info.user = &state;

    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        fprintf(stderr, "Błąd: nie udało się utworzyć kontekstu WebSocket\n");
        uvc_close(g_devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    
    // Główna pętla - tylko obsługa WebSocket
    while (!stopRequested)
    {
        lws_service(lwsContext, 30);
        usleep(60000);
    }

    // Cleanup
    stopCameraStream();
    uvc_close(g_devHandler);
    uvc_unref_device(device);
    uvc_exit(camContext);
    lws_context_destroy(lwsContext);
    motion_detector_destroy(state.motionDetector);
    pthread_mutex_destroy(&state.mutex);
    pthread_mutex_destroy(&g_streamMutex);
    
    if (logFile)
    {
        fclose(logFile);
    }

    return 0;
}