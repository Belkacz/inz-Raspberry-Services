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
#define FRAME_ANALYZE_STEP 2

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

// funkcją do liczenia różnicy w czasie
static long long timespecDiffMs(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000LL + 
           (end->tv_nsec - start->tv_nsec) / 1000000LL;
}

static void callbackUVC(uvc_frame_t *frame, void *ptr)
{
    AppState *state = (AppState*)ptr;
    
    // brak analizy jeśli nie ma połączenia
    if (!state->connectionEstablished)
    {
        return;
    }
    
    // sprawdzanie poprawności klatki
    if (!frame ||
        frame->data_bytes == 0 ||
        ((unsigned char*)frame->data)[0] != 0xFF ||
        ((unsigned char*)frame->data)[1] != 0xD8 ||
        frame->data_bytes > MAX_FRAME_SIZE) {
        fprintf(stderr, "[callbackUVC] Odrzucono uszkodzoną klatkę JPEG\n");
        return;
    }
    
    struct timespec timeNow;
    clock_gettime(CLOCK_MONOTONIC, &timeNow);
    long long elapsedTime = timespecDiffMs(&state->lastSentTime, &timeNow);
    if (elapsedTime < FPS_INTERVAL)
    {
        return;
    }

    unsigned char prevFrameBuffer[MAX_FRAME_SIZE];
    size_t prevFrameSize;
    
    pthread_mutex_lock(&state->mutex);
    // kopiowanie danych poprzedniej klatki 
    memcpy(prevFrameBuffer, state->frameBuffer, state->frameSize);
    prevFrameSize = state->frameSize;
    
    // kopiowanie danych nowej klatki 
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

    // analiza co drugiej klatki
    if(prevFrameSize > 0 && state->frameCounter % FRAME_ANALYZE_STEP == 0)
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
        // zerowanie flag przy połączeniu
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
            long long elapsedJsonTime = timespecDiffMs(&state->lastJsonSentTime, &timeNow);
            long long elapsedFrameTime = timespecDiffMs(&state->lastFrameSentTime, &timeNow);
            
            // wysyłaj JSON co 10 sekund
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
            // sprawdź czy minął czas na wysłanie ramki
            else if (elapsedFrameTime > FPS_INTERVAL)
            {
                unsigned char *buf = (unsigned char*)malloc(LWS_PRE + state->frameSize);
                if (buf)
                {
                    memcpy(buf + LWS_PRE, state->frameBuffer, state->frameSize);
                    size_t frameSize = state->frameSize;
                    state->hasNewFrame = 0;
                    pthread_mutex_unlock(&state->mutex);
                    
                    lws_write(wsi, buf + LWS_PRE, frameSize, LWS_WRITE_BINARY);
                    free(buf);
                }
                else
                {
                    pthread_mutex_unlock(&state->mutex);
                }
            } 
            else 
            {
                pthread_mutex_unlock(&state->mutex);
                lws_callback_on_writable(wsi);
                break;
            }
            lws_callback_on_writable(wsi);
        }
        break;
    }
    
    case LWS_CALLBACK_CLOSED:
    {
        fprintf(stderr, "[WS] Klient rozłączony\n");
        // zerowanie flag przy rozłączeniu
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

    // inicjalizacja zminnych dla state
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

    // zmienne kammery UVC
    uvc_context_t *camContext;
    uvc_device_t *device;
    uvc_device_handle_t *devHandler = NULL;
    uvc_stream_ctrl_t streamCtrl;
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

    res = uvc_open(device, &devHandler);
    if (res < 0)
    {
        uvc_perror(res, "uvc_open");
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }
    
    usleep(300000); // 300ms delay
    
    res = uvc_get_stream_ctrl_format_size(devHandler, &streamCtrl, 
                                          UVC_FRAME_FORMAT_MJPEG, 640, 480, FPS);
    if (res < 0)
    {
        uvc_perror(res, "get_stream_ctrl");
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    // protokół WebSocket
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

    // tworzenie kontekstu WebSocket
    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        fprintf(stderr, "Błąd: nie udało się utworzyć kontekstu WebSocket\n");
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    fprintf(stderr, "Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    
    // start streamu z kamery
    usleep(100000); // 100ms delay
    res = uvc_start_streaming(devHandler, &streamCtrl, callbackUVC, &state, 0);
    if (res < 0)
    {
        uvc_perror(res, "[CAM] Błąd uruchomienia streamu");
        lws_context_destroy(lwsContext);
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }
    fprintf(stderr, "[CAM] Stream uruchomiony\n");
    
    // główna pętla dla obsługi WebSocket
    while (!stopRequested)
    {
        lws_service(lwsContext, LWS_TIMEOUT);
        usleep(MIN_INTERVAL * 1000);
    }

    // Cleanup
    fprintf(stderr, "[SHUTDOWN] Rozpoczęcie zamykania programu...\n");
    uvc_stop_streaming(devHandler);
    fprintf(stderr, "[CAM] Stream zatrzymany\n");
    uvc_close(devHandler);
    uvc_unref_device(device);
    uvc_exit(camContext);
    lws_context_destroy(lwsContext);
    motion_detector_destroy(state.motionDetector);
    pthread_mutex_destroy(&state.mutex);
    
    if (logFile)
    {
        fclose(logFile);
    }

    return 0;
}