#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libuvc/libuvc.h>
#include <libwebsockets.h>
#include <stdbool.h>
#include "../common.h"

#define PORT 2138
#define MAX_FRAME_SIZE (4 * 1024 * 1024)
#define FPS 30

typedef struct {
    unsigned char frameBuffer[MAX_FRAME_SIZE];
    size_t frameSize;
    volatile int hasNewFrame;
    volatile bool connectionEstablished;
} AppState;

static void callbackUVC(uvc_frame_t *frame, void *ptr)
{
    AppState *state = (AppState*)ptr;
    if (frame->data_bytes > MAX_FRAME_SIZE)
    {
        return;
    }
    
    memcpy(state->frameBuffer, frame->data, frame->data_bytes);
    state->frameSize = frame->data_bytes;
    state->hasNewFrame = 1;
}

static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len)
{
    in=in;   // Wycisz ostrzeżenie
    len=len;
    user=user;
    AppState *state = (AppState *)lws_context_user(lws_get_context(wsi));
    switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
        // printf("✓ Klient połączony\n");
        lwsl_user("Nowe połączenie WebSocket\n");
        state->connectionEstablished = true;
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (state->hasNewFrame && state->frameSize > 0)
        {
            unsigned char *buf = malloc(LWS_PRE + state->frameSize);
            if (buf)
            {
                memcpy(buf + LWS_PRE, state->frameBuffer, state->frameSize);
                lws_write(wsi, buf + LWS_PRE, state->frameSize, LWS_WRITE_BINARY);
                free(buf);
                state->hasNewFrame = 0;
            }
        }
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_CLOSED:
        // printf("Klient rozłączony\n");
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
        .connectionEstablished = false
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

    struct lws_protocols protocols[] =
    {   
        // const char *name, lws_callback_function *callback , size_t per_session_data_size, size_t rx_buffer_size, unsigned int id , void *user, size_t tx_packet_size
        { "cam-protocol", callbackWs, 0, MAX_FRAME_SIZE, 0, NULL, 0},
        { NULL, NULL, 0, 0, 0, NULL, 0 }
    };

    // WebSocket serwer
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = PORT;
    info.protocols = protocols;
    info.user = &state;

    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        fprintf(stderr, "Błąd: nie udało się utworzyć kontekstu WebSocket\n");
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    while (!stopRequested)
    {
        // Start isStreaming gdy klient się połączy
        if (state.connectionEstablished && !isStreaming)
        {
            // uvc_device_handle_t *devh, uvc_stream_ctrl_t *ctrl, uvc_frame_callback_t *cb,void *user_ptr, uint8_t flags
            res = uvc_start_streaming(devHandler, &streamCtrl, callbackUVC, &state, 0);
            if (res < 0)
            {
                uvc_perror(res, "start_streaming");
            } else
            {
                printf("Stream uruchomiony\n");
                isStreaming = true;
            }
        }
        
        // Stop isStreaming gdy klient się rozłączy
        if (!state.connectionEstablished && isStreaming)
        {
            printf("Rozłączono – zatrzymuję stream\n");
            uvc_stop_streaming(devHandler);
            isStreaming = false;
        }
        
        lws_service(lwsContext, 10);
    }

    //  Cleanup
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