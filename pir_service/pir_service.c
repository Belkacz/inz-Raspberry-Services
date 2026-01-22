/* ws_server_pir.c
   Kompilacja:
   gcc ws_server_pir.c -o ws_server_pir -lwebsockets -lgpiod
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
static struct gpiod_line *line1 = NULL;
static struct gpiod_line *line2 = NULL;
static char payload[MAX_PAYLOAD];
static struct lws *globalWsi = NULL;
static bool connectionEstablished = false;
static pthread_mutex_t payloadMutex = PTHREAD_MUTEX_INITIALIZER;

/* callback WebSocket */
static int callbackWs(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    (void)user;
    (void)in;
    (void)len;
    
    switch (reason)
    {
        case LWS_CALLBACK_ESTABLISHED:
            fprintf(stderr, "[WS] Nowe połączenie WebSocket\n");
            globalWsi = wsi;
            connectionEstablished = true;
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
        // blokowanie mutex dla bezpieczeństwa
            pthread_mutex_lock(&payloadMutex);
            if (payload[0] != '\0' && globalWsi)
            {
                unsigned char buffer[LWS_PRE + MAX_PAYLOAD];
                size_t n = strlen(payload);
                // kopiowanie payload
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
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    // GPIO initialization
    chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip)
    {
        fprintf(stderr, "Błąd: nie można otworzyć gpiochip0\n");
        return 1;
    }
    // przypisanie czujników do zmiennych
    line1 = gpiod_chip_get_line(chip, PIR_PIN1);
    line2 = gpiod_chip_get_line(chip, PIR_PIN2);
    if (!line1 || !line2)
    {
        fprintf(stderr, "Błąd: nie można pobrać linii GPIO\n");
        gpiod_chip_close(chip);
        return 1;
    }

    if (gpiod_line_request_both_edges_events(line1, "pir_ws") < 0 ||
        gpiod_line_request_both_edges_events(line2, "pir_ws") < 0)
    {
        fprintf(stderr, "Błąd: nie można zarejestrować eventów GPIO\n");
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
        gpiod_chip_close(chip);
        return 1;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    // inicjalizcja linii
    struct gpiod_line_bulk bulk, activated;
    gpiod_line_bulk_init(&bulk);
    gpiod_line_bulk_add(&bulk, line1);
    gpiod_line_bulk_add(&bulk, line2);

    struct gpiod_line_event event;
    int counterRising[2] = {0, 0}; // [0] dla GPIO26, [0] dla GPIO16
    time_t lastServiceTime = 0;
    
    // Wyślij początkowy stan mutex dla bezpieczeństwa
    pthread_mutex_lock(&payloadMutex);
    snprintf(payload, sizeof(payload),
        "{\"pir%dRisingCounter\":0,\"pir%dRisingCounter\":0,\"time\":%ld}", 
        PIR_PIN1, PIR_PIN2, (long)time(NULL));
    pthread_mutex_unlock(&payloadMutex);
    
    while (!stopRequested)
    {
        time_t now = time(NULL);
        struct timespec timeout;
        
        // ustaw timeout w zależności od stanu połączenia
        if (connectionEstablished)
        {
            timeout.tv_sec = STD_DELAY;
        }
        else
        {
            timeout.tv_sec = DISCONNECTED_DELAY;
        }
        timeout.tv_nsec = 0;
        
        // czekaj na eventy GPIO
        int ret = gpiod_line_event_wait_bulk(&bulk, &timeout, &activated);
        if (ret > 0)
        {
            // iteracjac po liniach
            for (unsigned int i = 0; i < gpiod_line_bulk_num_lines(&activated); i++)
            {
                struct gpiod_line *line = gpiod_line_bulk_get_line(&activated, i);
                if (gpiod_line_event_read(line, &event) == 0)
                {
                    unsigned int offset = gpiod_line_offset(line);
                    
                    // sprawdzenie eventów
                    if (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)
                    {
                        // inkrementacja liczników
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
    gpiod_chip_close(chip);
    lws_context_destroy(lwsContext);
    
    if (logFile)
    {
        fclose(logFile);
    }

    return 0;
}