#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libuvc/libuvc.h>
#include <libwebsockets.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include "common.h"
#include "motion_detector.h"

#define PORT 2138
#define MAX_FRAME_SIZE (2 * 1024 * 1024)
#define FPS 30
#define STREAM_FPS 5
#define FPS_INTERVAL (1000 / STREAM_FPS)
#define JSON_INTERVAL_MS 10000
#define LWS_TIMEOUT 100
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MIN_INTERVAL MIN(FPS_INTERVAL, JSON_INTERVAL_MS)
// #define ANALYZE_INTERVAL (1000 / 15)

// Globalne zmienne dla kamery
static uvc_device_handle_t *g_devHandler = NULL;
static uvc_stream_ctrl_t g_streamCtrl;
static volatile bool g_isStreaming = false;
static pthread_mutex_t g_streamMutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    volatile bool connectionEstablished;
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
    
    if (!state->connectionEstablished)
    {
        return;
    }
    
    // if (!frame || frame->data_bytes == 0 || frame->data_bytes > MAX_FRAME_SIZE)
    // {
    //     return;
    // }
    if (!frame ||
        frame->data_bytes == 0 ||
        // frame->data_bytes < 100  ||
        ((unsigned char*)frame->data)[0] != 0xFF ||
        ((unsigned char*)frame->data)[1] != 0xD8 ||
        frame->data_bytes > MAX_FRAME_SIZE) {
        fprintf(stderr, "[callbackUVC] Odrzucono uszkodzoną klatkę JPEG\n");
        return;
    }
    struct timespec timeNow;
    clock_gettime(CLOCK_MONOTONIC, &timeNow);
    long long elapsedTime = timespec_diff_ms(&state->lastSentTime, &timeNow);
    if (elapsedTime < FPS_INTERVAL)
    {
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

    if(prevFrameSize > 0 && state->frameCounter % 2 == 0)
    {
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
    
    // POPRAWKA: usleep zamiast sleep(1000000)
    usleep(100000); // 100ms delay
    
    uvc_error_t res = uvc_start_streaming(g_devHandler, &g_streamCtrl, callbackUVC, state, 0);
    if (res < 0)
    {
        uvc_perror(res, "[CAM] Błąd uruchomienia streamu\n");
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
        state->hasNewFrame = 0;
        state->frameSize = 0;
        state->frameCounter = 0;
        state->motionDetectedFlag = false;
        
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        state->lastSentTime = timeNow;
        state->lastJsonSentTime = timeNow;
        state->lastFrameSentTime = timeNow;
        pthread_mutex_unlock(&state->mutex);
        
        lws_callback_on_writable(wsi);
        break;
    }
    
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        if(state->connectionEstablished)
        {
            pthread_mutex_lock(&state->mutex);
            if (!state->hasNewFrame || state->frameSize == 0)
            {
                pthread_mutex_unlock(&state->mutex);
                lws_callback_on_writable(wsi);
                break;
            }
            
            struct timespec timeNow;
            clock_gettime(CLOCK_MONOTONIC, &timeNow);
            long long elapsedJsonTime = timespec_diff_ms(&state->lastJsonSentTime, &timeNow);
            long long elapsedFrameTime = timespec_diff_ms(&state->lastFrameSentTime, &timeNow);
            
            // Wysyłaj JSON co 10 sekund
            if(elapsedJsonTime >= JSON_INTERVAL_MS)
            {
                bool motion = state->motionDetectedFlag;
                state->motionDetectedFlag = false;
                state->lastJsonSentTime = timeNow;
                pthread_mutex_unlock(&state->mutex);
                
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
                lws_callback_on_writable(wsi);
                break;
            }
            // Sprawdź czy minął czas na wysłanie ramki
            else if (elapsedFrameTime > FPS_INTERVAL)
            {
                unsigned char *buf = (unsigned char*)malloc(LWS_PRE + state->frameSize);
                if (buf)
                {
                    memcpy(buf + LWS_PRE, state->frameBuffer, state->frameSize);
                    size_t frameSize = state->frameSize;
                    state->hasNewFrame = 0;
                    state->lastFrameSentTime = timeNow;
                    printf("[callbackWs] WYSYŁAM KLATÓWE  frameSize  = %lu , TIME: = %ld\n", frameSize, timeNow.tv_sec);
                    pthread_mutex_unlock(&state->mutex);
                    
                    lws_write(wsi, buf + LWS_PRE, frameSize, LWS_WRITE_BINARY);
                    free(buf);
                }
                else
                {
                    pthread_mutex_unlock(&state->mutex);
                }
            } else 
            {
                pthread_mutex_unlock(&state->mutex);
                lws_callback_on_writable(wsi);
                break;
            }
            // else if(elapsedFrameTime < FPS_INTERVAL)
            // {
            //     // printf("[callbackWs]OMIJAM KALTÓWE BO elapsedFrameTime = %lld , FPS_INTERVAL: = %d\n", elapsedFrameTime, FPS_INTERVAL );
            //     pthread_mutex_unlock(&state->mutex);
            //     lws_callback_on_writable(wsi);
            //     break;
            // }

            lws_callback_on_writable(wsi);
        }
        break;
    }
    
    case LWS_CALLBACK_CLOSED:
    {
        fprintf(stderr, "[WS] Klient rozłączony\n");

        pthread_mutex_lock(&state->mutex);
        state->connectionEstablished = false;
        state->hasNewFrame = 0;
        state->frameSize = 0;
        state->frameCounter = 0;
        state->motionDetectedFlag = false;
        pthread_mutex_unlock(&state->mutex);
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
    if (logFile)
    {
        setvbuf(logFile, NULL, _IOLBF, 0);
        stderr = logFile;
    }

    MotionParams motionParams = {
        .motionThreshold = 20,
        .minArea = 200,
        .gaussBlur = 21
    };

    struct timespec timeNow;
    clock_gettime(CLOCK_MONOTONIC, &timeNow);

    // POPRAWKA: Inicjalizuj wszystkie pola czasu
    AppState state = {
        .connectionEstablished = false,
        .lastSentTime = timeNow,
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
    
    // Krótkie opóźnienie po otwarciu urządzenia
    usleep(300000); // 100ms
    
    res = uvc_get_stream_ctrl_format_size(g_devHandler, &g_streamCtrl, 
                                          UVC_FRAME_FORMAT_MJPEG, 640, 480, FPS);
    if (res < 0)
    {
        uvc_perror(res, "get_stream_ctrl");
        uvc_close(g_devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    // WebSocket - ustaw najpierw serwer
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

    fprintf(stderr, "Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    
    // POPRAWKA: Uruchom kamerę dopiero po inicjalizacji WebSocket
    if (!startCameraStream(&state))
    {
        fprintf(stderr, "Nie udało się uruchomić streamu kamery\n");
        lws_context_destroy(lwsContext);
        uvc_close(g_devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }
    
    // Główna pętla - tylko obsługa WebSocket
    while (!stopRequested)
    {
        lws_service(lwsContext, LWS_TIMEOUT);
        usleep(MIN_INTERVAL * 1000); // najmniejszy interwał
    }

    // Cleanup
    fprintf(stderr, "[SHUTDOWN] Rozpoczęcie zamykania programu...\n");
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