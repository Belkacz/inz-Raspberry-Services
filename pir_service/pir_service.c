/* ws_server_pir.c
   Kompilacja:
   gcc ws_server_pir.c -o ws_server_pir -lwebsockets -lgpiod
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <gpiod.h>
#include <libwebsockets.h>

#include "../common.h"

#define PIR_PIN1 26
#define PIR_PIN2 16
#define MAX_PAYLOAD 256
#define PORT 2137
#define STD_DEPLAY 10

static struct gpiod_chip *chip;
static struct gpiod_line *line1, *line2;
static char payload[MAX_PAYLOAD];
// static struct lws_context *lwsContext;
static struct lws *globalWsi = NULL;
int fastRetry = 3;
// volatile bool stopRequested = false;

static bool connectionEstablished = false;

/* callback WebSocket */
static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    switch (reason)
    {
        case LWS_CALLBACK_ESTABLISHED:
            lwsl_user("Nowe połączenie WebSocket\n");
            globalWsi = wsi;
            connectionEstablished = true;
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (payload[0] != '\0' && globalWsi)
            {
                unsigned char buffer[LWS_PRE + MAX_PAYLOAD];
                size_t n = strlen(payload);
                memcpy(&buffer[LWS_PRE], payload, n);
                lws_write(globalWsi, &buffer[LWS_PRE], n, LWS_WRITE_TEXT);
                payload[0] = '\0';
            }
            break;

        case LWS_CALLBACK_CLOSED:
            lwsl_user("Połączenie zamknięte\n");
            globalWsi = NULL;
            fastRetry = 3;
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {"pir-protocol", callbackWs, 0, MAX_PAYLOAD},
    {NULL, NULL, 0, 0}
};

int main(void)
{
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    FILE *logFile = fopen("/var/log/pirService.log", "a");
    setvbuf(logFile, NULL, _IOLBF, 0);
    stderr = logFile;
    setvbuf(stderr, NULL, _IONBF, 0); // stderr niebuforowany

    // pirs
    chip = gpiod_chip_open_by_name("gpiochip0");
    bool serviceJustStarted = false;
    if (!chip)
    {
        fprintf(stderr, "gpiochip0");
        return 1;
    }

    line1 = gpiod_chip_get_line(chip, PIR_PIN1);
    line2 = gpiod_chip_get_line(chip, PIR_PIN2);
    if (!line1 || !line2)
    {
        fprintf(stderr, "linie");
        gpiod_chip_close(chip);
        return 1;
    }

    if (gpiod_line_request_both_edges_events(line1, "pir_ws") < 0 ||
        gpiod_line_request_both_edges_events(line2, "pir_ws") < 0)
    {
        fprintf(stderr, "request events");
        gpiod_chip_close(chip);
        return 1;
    }

    // websocket
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = PORT;
    info.protocols = protocols;

    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        serviceJustStarted = true;
        fprintf(stderr, "Nie udało się utworzyć kontekstu LWS\n");
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);

    struct gpiod_line_bulk bulk, activated;
    gpiod_line_bulk_init(&bulk);
    gpiod_line_bulk_add(&bulk, line1);
    gpiod_line_bulk_add(&bulk, line2);

    struct gpiod_line_event event;

    static int counterRising[2] = {0, 0}; // [0] dla GPIO26, [1] dla GPIO16
    time_t lastServiceTime = 0;
    snprintf(payload, sizeof(payload),
        "{\"pir%dRisingCounter\":0,\"pir%dRisingCounter\":0, \"time\":%ld}", PIR_PIN1, PIR_PIN2, time(NULL));
    while (!stopRequested)
    {
        time_t now = time(NULL);  // timestamp na początku pętli
        struct timespec timeout;
        if(!globalWsi && fastRetry > 0)
        {
            timeout.tv_sec = 1;
        } else
        {
            timeout.tv_sec = STD_DEPLAY;
        }
        timeout.tv_sec = 1;

        int ret = gpiod_line_event_wait_bulk(&bulk, &timeout, &activated);
        if (ret > 0 && !serviceJustStarted)
        {
            for (int i = 0; i < gpiod_line_bulk_num_lines(&activated); i++)
            {
                struct gpiod_line *line = gpiod_line_bulk_get_line(&activated, i);
                if (gpiod_line_event_read(line, &event) == 0)
                {
                    int offset = gpiod_line_offset(line);

                    // jeśli event RISING to inkrementuj licznik
                    if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
                    {
                        if (offset == PIR_PIN1)
                        {
                            counterRising[0]++;
                        } else if(offset == PIR_PIN2)
                        {
                            counterRising[1]++;
                        }
                    }
                    // snprintf(payload, sizeof(payload),
                    //         "{\"gpio\":%d,\"type\":\"%s\",\"ts\":%ld,"
                    //         "\"counter_26_rising\":%d,\"counter_16_rising\":%d}",
                    //         offset,
                    //         (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) ? "RISING" : "FALLING",
                    //         (long)now,
                    //         counterRising[0],  // GPIO26
                    //         counterRising[1]); // GPIO16

                    // if (globalWsi)
                    //     lws_callback_on_writable(globalWsi);

                    // print tylko RISING
                    // if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
                    // {
                    //     printf("EVENT = %s, GIPIO = %d \n", (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) ? "RISING" : "FALLING", offset);
                    // }
                    // if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE && globalWsi)
                    // {
                    //     lws_callback_on_writable(globalWsi);
                    // }
                }
            }
        }

        if(globalWsi)
            lws_callback_on_writable(globalWsi);
        if (difftime(now, lastServiceTime) >= STD_DEPLAY || (!globalWsi && fastRetry > 0))
        {
            snprintf(payload, sizeof(payload),
                "{\"pir%dRisingCounter\":%d,\"pir%dRisingCounter\":%d, \"time\":%ld}",
                PIR_PIN1, counterRising[0], PIR_PIN2, counterRising[1], now);
            lws_service(lwsContext, 10);
            lastServiceTime = now;
            if(fastRetry > 0)
                fastRetry--;
            counterRising[0] = 0;
            counterRising[1] = 0;
        }
    }

    gpiod_chip_close(chip);
    lws_context_destroy(lwsContext);
    return 0;
}
