/* pir_service.c
   Kompilacja:
   gcc pir_service.c -o pir_service -lwebsockets -lgpiod -lpthread
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>

#include <gpiod.h>
#include <libwebsockets.h>
#include <pthread.h>
#include "../common.h"

#define PIR_PIN1 26
#define PIR_PIN2 16
#define MAX_PAYLOAD 256
#define PORT 2137
#define STD_DELAY 10
#define DISCONNECTED_DELAY 1

static struct gpiod_chip *chip = NULL;
static struct gpiod_line_request *request  = NULL;
static struct gpiod_edge_event_buffer *eventBuffer = NULL;
static char payload[MAX_PAYLOAD];
static struct lws *globalWsi = NULL;
static bool connectionEstablished = false;
static pthread_mutex_t payloadMutex = PTHREAD_MUTEX_INITIALIZER;

/* callback WebSocket */
static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len)
{
    (void)user; (void)in; (void)len;

    switch (reason)
    {
        case LWS_CALLBACK_ESTABLISHED:
            fprintf(stderr, "[WS] Nowe połączenie WebSocket\n");
            globalWsi = wsi;
            connectionEstablished = true;
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            pthread_mutex_lock(&payloadMutex);
            if (payload[0] != '\0' && globalWsi)
            {
                unsigned char buffer[LWS_PRE + MAX_PAYLOAD];
                size_t n = strlen(payload);
                memcpy(&buffer[LWS_PRE], payload, n);
                lws_write(globalWsi, &buffer[LWS_PRE], n, LWS_WRITE_TEXT);
                payload[0] = '\0';
            }
            pthread_mutex_unlock(&payloadMutex);
            break;

        case LWS_CALLBACK_CLOSED:
            fprintf(stderr, "[WS] Połączenie zamknięte\n");
            globalWsi = NULL;
            connectionEstablished = false;
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {"pir-protocol", callbackWs, 0, MAX_PAYLOAD, 0, NULL, 0},
    {NULL, NULL, 0, 0, 0, NULL, 0}
};

int main(void)
{
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    FILE *logFile = fopen("/var/log/pirService.log", "a");
    if (logFile)
    {
        setvbuf(logFile, NULL, _IOLBF, 0);
        stderr = logFile;
    }

    // GPIO initialization, otwieramy chip przez ścieżkę
    chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip)
    {
        fprintf(stderr, "Błąd: nie można otworzyć /dev/gpiochip0\n");
        return 1;
    }

    // konfiguracja linii – nasłuchiwanie obu zboczy
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings)
    {
        fprintf(stderr, "Błąd: gpiod_line_settings_new\n");
        gpiod_chip_close(chip);
        return 1;
    }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);

    // budujemy konfigurację requestu
    struct gpiod_request_config *requestConfig = gpiod_request_config_new();
    if (!requestConfig)
    {
        fprintf(stderr, "Błąd: gpiod_request_config_new\n");
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }
    gpiod_request_config_set_consumer(requestConfig, "pir_ws");

    struct gpiod_line_config *lineConfig = gpiod_line_config_new();
    if (!lineConfig)
    {
        fprintf(stderr, "Błąd: gpiod_line_config_new\n");
        gpiod_request_config_free(requestConfig);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }

    unsigned int offsets[2] = {PIR_PIN1, PIR_PIN2};
    if (gpiod_line_config_add_line_settings(lineConfig, offsets, 2, settings) < 0)
    {
        fprintf(stderr, "Błąd: gpiod_line_config_add_line_settings\n");
        gpiod_line_config_free(lineConfig);
        gpiod_request_config_free(requestConfig);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return 1;
    }

    // żądanie dostępu do linii
    request = gpiod_chip_request_lines(chip, requestConfig, lineConfig);
    gpiod_line_config_free(lineConfig);
    gpiod_request_config_free(requestConfig);
    gpiod_line_settings_free(settings);

    if (!request)
    {
        fprintf(stderr, "Błąd: gpiod_chip_request_lines\n");
        gpiod_chip_close(chip);
        return 1;
    }

    // bufor na eventy (max 2 eventy naraz)
    eventBuffer = gpiod_edge_event_buffer_new(2);
    if (!eventBuffer)
    {
        fprintf(stderr, "Błąd: gpiod_edge_event_buffer_new\n");
        gpiod_line_request_release(request);
        gpiod_chip_close(chip);
        return 1;
    }

   // WebSocket initialization
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = PORT;
    info.protocols = protocols;

    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        fprintf(stderr, "Błąd: nie udało się utworzyć kontekstu LWS\n");
        gpiod_edge_event_buffer_free(eventBuffer);
        gpiod_line_request_release(request);
        gpiod_chip_close(chip);
        return 1;
    }

    fprintf(stderr, "Serwer WebSocket działa na ws://<IP>:%d\n", PORT);

    int counterRising[2] = {0, 0}; // [0] dla GPIO26, [0] dla GPIO16
    time_t lastServiceTime = 0;

    // Wyślij początkowy stan mutex dla bezpieczeństwa
    pthread_mutex_lock(&payloadMutex);
    snprintf(payload, sizeof(payload),
        "{\"pir%dRisingCounter\":0,\"pir%dRisingCounter\":0,\"time\":%ld}",
        PIR_PIN1, PIR_PIN2, (long)time(NULL));
    pthread_mutex_unlock(&payloadMutex);

    // główna pętla
    while (!stopRequested)
    {
        long long timeout;
        // ustaw timeout w zależności od stanu połączenia
        if (connectionEstablished)
        {
            timeout = (long long)STD_DELAY * 1000000000LL;
        }
        else
        {
            timeout = (long long)DISCONNECTED_DELAY * 1000000000LL;
        }

        // czekaj na eventy GPIO ( wait_edge_events zwraca liczbę eventów)
        int ret = gpiod_line_request_wait_edge_events(request, timeout);
        if (ret > 0)
        {
            int eventsNum = gpiod_line_request_read_edge_events(request, eventBuffer, 2);
            for (int i = 0; i < eventsNum; i++)
            {
                struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(eventBuffer, (unsigned long)i);
                if (event)
                {
                    enum gpiod_edge_event_type type = gpiod_edge_event_get_event_type(event);
                    unsigned int offset = gpiod_edge_event_get_line_offset(event);

                    // sprawdzenie eventów
                    if (type == GPIOD_EDGE_EVENT_RISING_EDGE)
                    {
                        if (offset == PIR_PIN1)
                        {
                            counterRising[0]++;
                        }
                        else if (offset == PIR_PIN2)
                        {
                            counterRising[1]++;
                        }
                    }
                }
            }
        }
        time_t now = time(NULL);
        // Określ czy wysłać dane
        int delay = connectionEstablished ? STD_DELAY : DISCONNECTED_DELAY;
        if (difftime(now, lastServiceTime) >= delay)
        {
            // Przygotuj payload
            pthread_mutex_lock(&payloadMutex);
            snprintf(payload, sizeof(payload),
                "{\"pir%dRisingCounter\":%d,\"pir%dRisingCounter\":%d,\"time\":%ld}",
                PIR_PIN1, counterRising[0], PIR_PIN2, counterRising[1], (long)now);
            pthread_mutex_unlock(&payloadMutex);
            // Powiadom WebSocket o danych do wysłania
            if (globalWsi)
            {
                lws_callback_on_writable(globalWsi);
            }

            lastServiceTime = now;

            // Reset liczników
            counterRising[0] = 0;
            counterRising[1] = 0;
        }

        lws_service(lwsContext, BASE_LWS_TIMEOUT);
    }

    // Cleanup
    gpiod_edge_event_buffer_free(eventBuffer);
    gpiod_line_request_release(request);
    gpiod_chip_close(chip);
    lws_context_destroy(lwsContext);

    if (logFile)
    {
        fclose(logFile);
    }

    return 0;
}