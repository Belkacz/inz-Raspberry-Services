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

static unsigned char frameBuffer[MAX_FRAME_SIZE];
static size_t frameSize = 0;
static volatile int hasNewFrame = 0;
static volatile bool connectionEstablished = false;

static void callbackUVC(uvc_frame_t *frame, void *ptr)
{
    if (frame->data_bytes > MAX_FRAME_SIZE) return;
    
    memcpy(frameBuffer, frame->data, frame->data_bytes);
    frameSize = frame->data_bytes;
    hasNewFrame = 1;
}

static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len)
{
    switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
        // printf("✓ Klient połączony\n");
        lwsl_user("Nowe połączenie WebSocket\n");
        connectionEstablished = true;
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (hasNewFrame && frameSize > 0)
        {
            unsigned char *buf = malloc(LWS_PRE + frameSize);
            if (buf)
            {
                memcpy(buf + LWS_PRE, frameBuffer, frameSize);
                lws_write(wsi, buf + LWS_PRE, frameSize, LWS_WRITE_BINARY);
                free(buf);
                hasNewFrame = 0;
            }
        }
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_CLOSED:
        // printf("Klient rozłączony\n");
        lwsl_user("Połączenie WebSocket zakończone\n");
        connectionEstablished = false;
        break;
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { "cam-protocol", callbackWs, 0, MAX_FRAME_SIZE },
    { NULL, NULL, 0, 0 }
};

int main(void)
{
    FILE *logFile = fopen("/var/log/camService.log", "a");
    setvbuf(logFile, NULL, _IOLBF, 0);
    stderr = logFile;

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

    // WebSocket serwer
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = PORT;
    info.protocols = protocols;

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
        if (connectionEstablished && !isStreaming)
        {
            res = uvc_start_streaming(devHandler, &streamCtrl, callbackUVC, NULL, 0);
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
        if (!connectionEstablished && isStreaming)
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

    return 0;
}