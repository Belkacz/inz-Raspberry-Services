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
#define MAX_FRAME_SIZE (4 * 1024 * 1024)
#define FPS 30
#define PREVIEW_FPS 5           // FPS dla wysyłania klatek przez WebSocket
#define MOTION_CHECK_FPS 10      // FPS dla analizy ruchu
#define JSON_SEND_FPS 1          // FPS dla wysyłania JSONa z danymi ruchu
#define JSON_SEND_INTERVAL_MS 5000
#define FPS_INTERVAL (1000 / PREVIEW_FPS)

typedef struct {
    // WebSocket
    volatile bool connectionEstablished;
    struct lws *current_wsi;

    // Timery
    struct timespec lastPreviewTime;
    struct timespec lastMotionCheckTime;
    struct timespec lastJsonSendTime;
    struct timespec lastMotionTime;

    // Bufory ramek
    unsigned char frameBuffer[MAX_FRAME_SIZE];
    size_t frameSize;
    volatile int hasNewFrame;
    
    unsigned char prevFrameBuffer[MAX_FRAME_SIZE];
    size_t prevFrameSize;
    bool hasPrevFrame;
    
    // Detekcja ruchu
    void* motionDetector;
    bool motionDetectedFlag;
    int framesAnalyzed;
    int motionFramesCount;  // liczba klatek z ruchem od ostatniego JSONa
    
    // Bufory do wysyłania
    char jsonBuffer[512];
    bool hasJsonToSend;
} AppState;

static long long timespec_diff_ms(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000LL + 
           (end->tv_nsec - start->tv_nsec) / 1000000LL;
}

static void callbackUVC(uvc_frame_t *frame, void *ptr)
{
    AppState *state = (AppState*)ptr;
    
    if (!frame || frame->data_bytes == 0 || frame->data_bytes > MAX_FRAME_SIZE)
        return;
    
    // Zapisz poprzednią klatkę przed nadpisaniem
    if (state->frameSize > 0) {
        memcpy(state->prevFrameBuffer, state->frameBuffer, state->frameSize);
        state->prevFrameSize = state->frameSize;
        state->hasPrevFrame = true;
    }
    
    // Zapisz nową klatkę
    memcpy(state->frameBuffer, frame->data, frame->data_bytes);
    state->frameSize = frame->data_bytes;
    state->hasNewFrame = 1;
    
    // Sprawdź czy czas na detekcję ruchu
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsedMotion = timespec_diff_ms(&state->lastMotionCheckTime, &now);
    long long motionInterval = 1000 / MOTION_CHECK_FPS;
    
    if (elapsedMotion >= motionInterval && state->hasPrevFrame) {
        // Wywołaj detekcję ruchu (C++)
        bool motionNow = motion_detector_detect(
            state->motionDetector,
            state->frameBuffer, state->frameSize,
            state->prevFrameBuffer, state->prevFrameSize
        );
        
        state->framesAnalyzed++;
        
        if (motionNow) {
            state->motionDetectedFlag = true;
            state->motionFramesCount++;
            state->lastMotionTime = now;
        }
        
        state->lastMotionCheckTime = now;
    }
    
    if (state->current_wsi) {
        lws_callback_on_writable(state->current_wsi);
    }
}

static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len)
{
    (void)in;
    (void)len;
    (void)user;
    
    AppState *state = (AppState *)lws_context_user(lws_get_context(wsi));
    if (!state) {
        return -1;
    }
    
    switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
        fprintf(stderr, "[WS] Klient połączony\n");
        state->connectionEstablished = true;
        state->current_wsi = wsi;
        
        clock_gettime(CLOCK_MONOTONIC, &state->lastPreviewTime);
        clock_gettime(CLOCK_MONOTONIC, &state->lastMotionCheckTime);
        clock_gettime(CLOCK_MONOTONIC, &state->lastJsonSendTime);
        
        // Reset liczników
        state->framesAnalyzed = 0;
        state->motionFramesCount = 0;
        state->motionDetectedFlag = false;
        
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        
        // PRIORYTET 1: JSON z danymi ruchu
        long long elapsedJson = timespec_diff_ms(&state->lastJsonSendTime, &now);
        // long long jsonInterval = 1000 / JSON_SEND_FPS;
        
        if (elapsedJson >= JSON_SEND_INTERVAL_MS) {
            // Przygotuj JSON
            snprintf(state->jsonBuffer, sizeof(state->jsonBuffer),
                    "{\"motion\":%s,\"timestamp\":%ld,\"framesAnalyzed\":%d,\"motionFrames\":%d}",
                    state->motionDetectedFlag ? "true" : "false",
                    now.tv_sec,
                    state->framesAnalyzed,
                    state->motionFramesCount);
            
            size_t jsonLen = strlen(state->jsonBuffer);
            unsigned char *buf = (unsigned char*)malloc(LWS_PRE + jsonLen);
            
            if (buf) {
                memcpy(buf + LWS_PRE, state->jsonBuffer, jsonLen);
                lws_write(wsi, buf + LWS_PRE, jsonLen, LWS_WRITE_TEXT);
                free(buf);
                
                fprintf(stderr, "[JSON] Motion=%s, Analyzed=%d, MotionFrames=%d\n",
                       state->motionDetectedFlag ? "YES" : "NO",
                       state->framesAnalyzed,
                       state->motionFramesCount);
                
                // Reset flag i liczników po wysłaniu
                state->motionDetectedFlag = false;
                state->framesAnalyzed = 0;
                state->motionFramesCount = 0;
                state->lastJsonSendTime = now;
            }
            
            lws_callback_on_writable(wsi);
            break;
        }
        
        // PRIORYTET 2: Klatka preview
        long long elapsedPreview = timespec_diff_ms(&state->lastPreviewTime, &now);
        long long previewInterval = 1000 / PREVIEW_FPS;
        
        if (state->hasNewFrame && state->frameSize > 0 && elapsedPreview >= previewInterval) {
            unsigned char *buf = (unsigned char*)malloc(LWS_PRE + state->frameSize);
            
            if (buf) {
                memcpy(buf + LWS_PRE, state->frameBuffer, state->frameSize);
                lws_write(wsi, buf + LWS_PRE, state->frameSize, LWS_WRITE_BINARY);
                free(buf);
                state->hasNewFrame = 0;
                state->lastPreviewTime = now;
            }
        }
        
        lws_callback_on_writable(wsi);
        break;
    }
        
    case LWS_CALLBACK_CLOSED:
        fprintf(stderr, "[WS] Klient rozłączony\n");
        state->connectionEstablished = false;
        state->current_wsi = NULL;
        break;
        
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
    if (logFile) {
        setvbuf(logFile, NULL, _IOLBF, 0);
        stderr = logFile;
    }

    // Inicjalizacja parametrów detekcji ruchu
    MotionParams motionParams = {
        .motionThreshold = 20,
        .minArea = 200,
        .gaussBlur = 21
    };

    // ALOKUJ AppState NA STERCIE zamiast na stosie!
    AppState *state = (AppState*)calloc(1, sizeof(AppState));
    if (!state) {
        fprintf(stderr, "Błąd: nie udało się zaalokować pamięci dla AppState\n");
        return 1;
    }
    
    // Inicjalizuj pola
    state->frameSize = 0;
    state->hasNewFrame = 0;
    state->connectionEstablished = false;
    state->current_wsi = NULL;
    state->prevFrameSize = 0;
    state->hasPrevFrame = false;
    state->motionDetectedFlag = false;
    state->framesAnalyzed = 0;
    state->motionFramesCount = 0;
    state->hasJsonToSend = false;
    state->lastPreviewTime.tv_sec = 0;
    state->lastPreviewTime.tv_nsec = 0;
    state->lastMotionCheckTime.tv_sec = 0;
    state->lastMotionCheckTime.tv_nsec = 0;
    state->lastJsonSendTime.tv_sec = 0;
    state->lastJsonSendTime.tv_nsec = 0;
    state->lastMotionTime.tv_sec = 0;
    state->lastMotionTime.tv_nsec = 0;
    
    // Inicjalizuj detektor ruchu
    state->motionDetector = motion_detector_init(640, 480, motionParams);
    if (!state->motionDetector) {
        fprintf(stderr, "Błąd: nie udało się zainicjalizować detektora ruchu\n");
        free(state);
        return 1;
    }

    uvc_context_t *camContext;
    uvc_device_t *device;
    uvc_device_handle_t *devHandler;
    uvc_stream_ctrl_t streamCtrl;
    uvc_error_t res;
    bool isStreaming = false;

    // --- Inicjalizacja UVC ---
    res = uvc_init(&camContext, NULL);
    if (res < 0) {
        uvc_perror(res, "uvc_init");
        motion_detector_destroy(state->motionDetector);
        free(state);
        return 1;
    }

    res = uvc_find_device(camContext, &device, 0, 0, NULL);
    if (res < 0) {
        uvc_perror(res, "find_device");
        uvc_exit(camContext);
        motion_detector_destroy(state->motionDetector);
        free(state);
        return 1;
    }

    res = uvc_open(device, &devHandler);
    if (res < 0) {
        uvc_perror(res, "uvc_open");
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state->motionDetector);
        free(state);
        return 1;
    }

    res = uvc_get_stream_ctrl_format_size(devHandler, &streamCtrl, 
                                          UVC_FRAME_FORMAT_MJPEG, 
                                          640, 480, FPS);
    if (res < 0) {
        uvc_perror(res, "get_stream_ctrl");
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state->motionDetector);
        free(state);
        return 1;
    }

    // --- WebSocket ---
    struct lws_protocols protocols[] = {   
        { "cam-protocol", callbackWs, 0, MAX_FRAME_SIZE, 0, NULL, 0},
        { NULL, NULL, 0, 0, 0, NULL, 0 }
    };

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = PORT;
    info.protocols = protocols;
    info.user = state;  // Przekaż wskaźnik

    lwsContext = lws_create_context(&info);
    if (!lwsContext) {
        fprintf(stderr, "Błąd: nie udało się utworzyć kontekstu WebSocket\n");
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state->motionDetector);
        free(state);
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    printf("Preview FPS: %d, Motion check FPS: %d, JSON FPS: %d\n", 
           PREVIEW_FPS, MOTION_CHECK_FPS, JSON_SEND_FPS);
    
    while (!stopRequested)
    {
        if (state->connectionEstablished && !isStreaming) {
            res = uvc_start_streaming(devHandler, &streamCtrl, callbackUVC, state, 0);
            if (res < 0) {
                uvc_perror(res, "start_streaming");
            } else {
                printf("Stream uruchomiony\n");
                isStreaming = true;
            }
        }
        
        if (!state->connectionEstablished && isStreaming) {
            printf("Rozłączono – zatrzymuję stream\n");
            uvc_stop_streaming(devHandler);
            isStreaming = false;
            state->hasPrevFrame = false;
        }
        
        lws_service(lwsContext, FPS_INTERVAL);
    }

    // Cleanup
    if (isStreaming) {
        uvc_stop_streaming(devHandler);
    }
    
    motion_detector_destroy(state->motionDetector);
    uvc_close(devHandler);
    uvc_unref_device(device);
    uvc_exit(camContext);
    lws_context_destroy(lwsContext);
    
    free(state);  // Zwolnij pamięć

    if (logFile) {
        fclose(logFile);
    }

    return 0;
}