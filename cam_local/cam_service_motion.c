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
#define STREAM_FPS 15  // Docelowy FPS dla WebSocket
#define FPS_INTERVAL (1000 / STREAM_FPS)
#define JSON_INTERVAL_MS 10000
#define LWS_TIMEOUT 100

typedef struct {
    // WebSocket
    volatile bool connectionEstablished;

    struct timespec lastSentTime;
    struct timespec lastJsonSentTime;
    struct timespec lastFameSentTime;

    // Kamera
    unsigned char frameBuffer[MAX_FRAME_SIZE];
    size_t frameSize;
    volatile int hasNewFrame;
    volatile int frameCounter;
    volatile bool motionDetectedFlag;

    void* motionDetector;
    pthread_mutex_t mutex;
} AppState;

// Pomocnicza funkcja do liczenia różnicy czasu w ms
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
        return;  // Nie przetwarzaj ramek gdy nikt nie słucha
    }
    // Podstawowe sprawdzenia
    if (!frame || frame->data_bytes == 0 || frame->data_bytes > MAX_FRAME_SIZE)
    {
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
    memcpy(prevFrameBuffer, state->frameBuffer, state->frameSize);
    prevFrameSize = state->frameSize;

    pthread_mutex_lock(&state->mutex);
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
        lwsl_user("Nowe połączenie WebSocket\n");
        pthread_mutex_lock(&state->mutex);  // Jeśli używasz
        state->connectionEstablished = true;
        
        // RESET wszystkiego na czysto
        state->hasNewFrame = 0;
        state->frameSize = 0;
        state->frameCounter = 0;
        state->motionDetectedFlag = false;
        // Zresetuj czas ostatniego wysłania
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        state->lastSentTime = timeNow;
        state->lastJsonSentTime = timeNow;
        state->lastFameSentTime = timeNow;
        pthread_mutex_unlock(&state->mutex);
        lws_callback_on_writable(wsi);
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
                pthread_mutex_lock(&state->mutex);
        if (!state->hasNewFrame || state->frameSize == 0)
        {
            pthread_mutex_unlock(&state->mutex);
            // Brak nowej ramki - poproś o kolejne wywołanie
            lws_callback_on_writable(wsi);
            break;
        }
        
        // Sprawdź czy minął wystarczający czas od ostatniego wysłania
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        long long elapesedJsonTime = timespec_diff_ms(&state->lastJsonSentTime, &timeNow);
        long long elapsedTime = timespec_diff_ms(&state->lastSentTime, &timeNow);
        if(elapesedJsonTime > JSON_INTERVAL_MS)
        {
            bool motion = state->motionDetectedFlag;
            // Zaktualizuj state
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
                lws_callback_on_writable(wsi);
                break;
            }
        } else if(elapsedTime < FPS_INTERVAL)
        {
            // Za wcześnie - pomiń tę ramkę
            // Poproś o kolejne wywołanie w następnym cyklu
            pthread_mutex_unlock(&state->mutex);
            lws_callback_on_writable(wsi);
            break;
        } else
        {
            unsigned char *buf = (unsigned char*)malloc(LWS_PRE + state->frameSize);
            if (buf)
            {
                pthread_mutex_unlock(&state->mutex);
                memcpy(buf + LWS_PRE, state->frameBuffer, state->frameSize);
                pthread_mutex_unlock(&state->mutex);

                lws_write(wsi, buf + LWS_PRE, state->frameSize, LWS_WRITE_BINARY);
                free(buf);
                state->hasNewFrame = 0;
            }
            state->lastFameSentTime = timeNow;
            pthread_mutex_unlock(&state->mutex);
        }

        lws_callback_on_writable(wsi);
        break;
    }
    case LWS_CALLBACK_CLOSED:
    {
        fprintf(stderr, "[WS] Klient rozłączony\n");
        lwsl_user("Połączenie WebSocket zakończone\n");

        pthread_mutex_lock(&state->mutex);  // Jeśli używasz mutex
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        state->connectionEstablished = false;
        state->hasNewFrame = 0;
        state->frameSize = 0;
        state->frameCounter = 0;
        state->motionDetectedFlag = false;
        
        //WAŻNE: Reset timerów
        state->lastSentTime = timeNow;
        state->lastJsonSentTime = timeNow;
        state->lastFameSentTime = timeNow;
        pthread_mutex_unlock(&state->mutex);  // Jeśli używasz mutex
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
        .lastSentTime = {0, 0},
        .lastJsonSentTime = timeNow,
        .lastFameSentTime = timeNow,
        .frameBuffer = {0},              // opcjonalne, ale usuwa warning
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
    uvc_device_handle_t *devHandler;
    uvc_stream_ctrl_t streamCtrl;
    uvc_error_t res;
    bool isStreaming = false;

    // --- Inicjalizacja UVC ---
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

    res = uvc_get_stream_ctrl_format_size(devHandler, &streamCtrl, UVC_FRAME_FORMAT_MJPEG, 
                                          640, 480, 0);
    if (res < 0)
    {
        uvc_perror(res, "get_stream_ctrl");
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    // --- WebSocket ---
    struct lws_protocols protocols[] =
    {   
        // const char *name, lws_callback_function *callback , size_t per_session_data_size, size_t rx_buffer_size, unsigned int id , void *user, size_t tx_packet_size
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
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
        motion_detector_destroy(state.motionDetector);
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    while (!stopRequested)
    {
        // Start streaming
        if (state.connectionEstablished && !isStreaming)
        {
            // uvc_device_handle_t *devh, uvc_stream_ctrl_t *ctrl, uvc_frame_callback_t *cb,void *user_ptr, uint8_t flags
            res = uvc_start_streaming(devHandler, &streamCtrl, callbackUVC, &state, 0);
            if (res < 0)
            {
                uvc_perror(res, "streaming error");
            } else
            {
                printf("Stream uruchomiony\n");
                isStreaming = true;
            }
        }
        
        // Stop isStreaming gdy klient się rozłączy
        if (!state.connectionEstablished && isStreaming) {
            printf("Rozłączono – zatrzymuję stream\n");
            uvc_stop_streaming(devHandler);
            isStreaming = false;
        }
        // Obsługa WebSocket
        lws_service(lwsContext, LWS_TIMEOUT);
        usleep(30000);
    }

    // Cleanup
    if (isStreaming)
    {
        uvc_stop_streaming(devHandler);
    }
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