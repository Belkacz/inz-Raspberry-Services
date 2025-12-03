#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libuvc/libuvc.h>
#include <libwebsockets.h>
#include <stdbool.h>
#include <time.h>
#include "common.h"

#define PORT 2138
#define MAX_FRAME_SIZE (2 * 1024 * 1024)
#define FPS 30
#define STREAM_FPS 15  // Docelowy FPS dla WebSocket
#define LWS_TIMEOUT_MS (1000 / STREAM_FPS)
#define FPS_INTERVAL (1000 / STREAM_FPS)

typedef struct {
    // WebSocket
    volatile bool connectionEstablished;

    struct timespec lastSentTime;

    // Kamera
    unsigned char frameBuffer[MAX_FRAME_SIZE];
    size_t frameSize;
    volatile int hasNewFrame;
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
    
    // PROSTY MODEL: Po prostu nadpisz bufor najnowszą ramką
    // Jeśli poprzednia nie została wysłana - trudno, nadpisujemy
    memcpy(state->frameBuffer, frame->data, frame->data_bytes);
    state->frameSize = frame->data_bytes;
    state->hasNewFrame = 1;
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
        lwsl_user("Nowe połączenie WebSocket\n");
        state->connectionEstablished = true;
        
        // Zresetuj czas ostatniego wysłania
        clock_gettime(CLOCK_MONOTONIC, &state->lastSentTime);
        
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        if (!state->hasNewFrame || state->frameSize == 0) {
            // Brak nowej ramki - poproś o kolejne wywołanie
            lws_callback_on_writable(wsi);
            break;
        }
        
        // Sprawdź czy minął wystarczający czas od ostatniego wysłania
        struct timespec timeNow;
        clock_gettime(CLOCK_MONOTONIC, &timeNow);
        long long elapsedTime = timespec_diff_ms(&state->lastSentTime, &timeNow);

        if (elapsedTime < FPS_INTERVAL)
        {
            // Za wcześnie - pomiń tę ramkę
            // Poproś o kolejne wywołanie w następnym cyklu
            lws_callback_on_writable(wsi);
            break;
        }
        // Wysyłanie klatki
        unsigned char *buf = malloc(LWS_PRE + state->frameSize);
        if (buf)
        {
            memcpy(buf + LWS_PRE, state->frameBuffer, state->frameSize);
            lws_write(wsi, buf + LWS_PRE, state->frameSize, LWS_WRITE_BINARY);
            free(buf);
            state->hasNewFrame = 0;
            state->lastSentTime = timeNow;
        }
        // Poproś o kolejne wywołanie
        lws_callback_on_writable(wsi);
        break;
    }
        
    case LWS_CALLBACK_CLOSED:
        fprintf(stderr, "[WS] Klient rozłączony\n");
        lwsl_user("Połączenie WebSocket zakończone\n");
        state->connectionEstablished = false;
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
    setvbuf(logFile, NULL, _IOLBF, 0);
    stderr = logFile;

    AppState state = {
        .frameSize = 0,
        .hasNewFrame = 0,
        .connectionEstablished = false,
        .lastSentTime = {0, 0}
    };

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
        return 1;
    }

    res = uvc_find_device(camContext, &device, 0, 0, NULL);
    if (res < 0)
    {
        uvc_perror(res, "find_device");
        uvc_exit(camContext);
        return 1;
    }

    res = uvc_open(device, &devHandler);
    if (res < 0)
    {
        uvc_perror(res, "uvc_open");
        uvc_unref_device(device);
        uvc_exit(camContext);
        return 1;
    }

    res = uvc_get_stream_ctrl_format_size(devHandler, &streamCtrl, UVC_FRAME_FORMAT_MJPEG, 
                                          640, 480, FPS);
    if (res < 0)
    {
        uvc_perror(res, "get_stream_ctrl");
        uvc_close(devHandler);
        uvc_unref_device(device);
        uvc_exit(camContext);
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
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    while (!stopRequested)
    {
        // Start streaming
        if (state.connectionEstablished && !isStreaming) {
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
        lws_service(lwsContext, LWS_TIMEOUT_MS);
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

    if (logFile)
    {
        fclose(logFile);
    }

    return 0;
}