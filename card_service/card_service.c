/* ws_server_pir_mt.c
   Kompilacja:
   gcc ws_server_pir_mt.c -o ws_server_pir_mt -lwebsockets -lgpiod -lpthread
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <libwebsockets.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>
// #include <LCD1602.h
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include "common.h"
#include "card_service_fn.h"

#define MAX_PAYLOAD 1024
#define CARD_INPUT "/dev/hidraw0"
#define CARD_NUMBER 24
#define MAX_CARD_LEN 32
#define PORT 2139

// callback WebSocket
static int callbackLWS(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    user=user;
    in=in;
    len=len;
    AppState *state = (AppState *)lws_context_user(lws_get_context(wsi));
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
        {
            lwsl_user("Nowe połączenie WebSocket\n");
            pthread_mutex_lock(&state->mutex);
            state->connectionEstablished = true;
            state->wsi = wsi;
            buildPayloud(state);
            lws_callback_on_writable(state->wsi);
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        case LWS_CALLBACK_SERVER_WRITEABLE:
        {
            char message[MAX_PAYLOAD] = "\0";
            pthread_mutex_lock(&state->mutex);
            if(!(state->payload[0] == '\0'))
            {
                len = strlen(state->payload);
                memcpy(message, state->payload, len);
                message[len]= '\0';
            }
            pthread_mutex_unlock(&state->mutex);

            if (message[0] == '\0')
            {
                strcpy(message, "{}");
            }

            unsigned char buffer[LWS_PRE + MAX_PAYLOAD];
            size_t messageLen = strlen(message);
            memcpy(&buffer[LWS_PRE], message, messageLen);
            int ret = lws_write(wsi, &buffer[LWS_PRE], messageLen, LWS_WRITE_TEXT);
            if (ret < 0)
                lwsl_err("lws_write failed: %d\n", ret);
            break;
        }

        case LWS_CALLBACK_CLOSED:
        {
            lwsl_user("Połączenie zamknięte\n");
            pthread_mutex_lock(&state->mutex);
            state->wsi = NULL;
            state->connectionEstablished = false;
            pthread_mutex_unlock(&state->mutex);
            break;
        }

        default:
            break;
    }
    return 0;
}

// wątek WebSocket (LWS)
void *wsThread(void *arg)
{
    struct lws_protocols protocols[] =
    {   
        // const char *name, lws_callback_function *callback , size_t per_session_data_size, size_t rx_buffer_size, unsigned int id , void *user, size_t tx_packet_size
        { "card-protocol", callbackLWS, 0, MAX_PAYLOAD, 0, NULL, 0},
        { NULL, NULL, 0, 0, 0, NULL, 0 }
    };

    AppState *state  = (AppState*)arg;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = PORT;
    info.protocols = protocols;
    info.user = state;

    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        fprintf(stderr, "Nie udało się utworzyć kontekstu LWS\n");
        return NULL;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    while (!stopRequested)
    {
        lws_service(lwsContext, BASE_LWS_TIMEOUT);
        pthread_mutex_lock(&state->mutex);
        if (state->connectionEstablished && state->requestSend && state->wsi)
        {
            lws_callback_on_writable(state->wsi);
            state->requestSend = false;
        }
        
        // Jeśli połączony - czekaj na sygnał (z timeoutem)
        if (state->connectionEstablished && state->wsi)
        {
            struct timespec delay;
            clock_gettime(CLOCK_MONOTONIC, &delay);

            unsigned long hundredMilliseconds = 100 * 1000000UL;
            delay.tv_nsec += hundredMilliseconds;

            if (delay.tv_nsec >= 1000000000L)
            {
                delay.tv_sec += 1;
                delay.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&state->payloadCond, &state->mutex, &delay);
        }
        
        pthread_mutex_unlock(&state->mutex);
        if (!state->connectionEstablished)
        {
            usleep(BASE_LWS_TIMEOUT); // 100ms
        }
    }

    lws_context_destroy(lwsContext);
    lwsContext = NULL;
    return NULL;
}

// wątek do wczytania kart
void *cardThread(void *arg)
{
    AppState *state  = (AppState*)arg;
    state->currentCardListSize = CARD_NUMBER;
    state->cardList = calloc(state->currentCardListSize, sizeof(char*));

    if (!state->cardList)
    {
        fprintf(stderr, "[cardThread] cardList calloc failed\n");
        return NULL;
    }
    for (int i = 0; i < state->currentCardListSize; i++)
    {
        state->cardList[i] = calloc(MAX_CARD_LEN, sizeof(char));
        if (!state->cardList[i])
        {
            fprintf(stderr, "failed calloc for cardList item no %d \n", i);
            for (int j = 0; j < i; j++) free(state->cardList[j]);
            free(state->cardList);
            return NULL;
        }
    }

    char cardBuf[MAX_CARD_LEN] = {0};
    size_t cardPos = 0;

    int file = open(CARD_INPUT, O_RDONLY | O_NONBLOCK);
    if (file < 0)
    {
        fprintf(stderr, "Open Card Input Error");
        for (int i = 0; i < state->currentCardListSize; i++) free(state->cardList[i]);
        free(state->cardList);
        return NULL;
    }

    const char *keycodes[] = {
        "", "", "", "", "a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
        "1","2","3","4","5","6","7","8","9","0"
    };

    while (!stopRequested)
    {
        unsigned char buf[8];
        ssize_t n = read(file, buf, sizeof(buf));
        if (n <= 0)
        {
            usleep(1000000);
            // sleep(1);
        } else
        {
            int code = buf[2];
            if (code == 0x28)
            {
                if (cardPos > 0)
                {
                    cardBuf[cardPos] = '\0';
                    pthread_mutex_lock(&state->mutex);
                    
                    // Logika dodawania/usuwania karty
                    int ret = findCardInList(cardBuf, (const char**)state->cardList, state->currentCardListSize);
                    if (ret == -1)
                    {
                        addCardToList(cardBuf, &state->cardList, state->currentCardListSize);
                    } else {
                        removeCardFromList(ret, &state->cardList, state->currentCardListSize);
                    }

                    // Budowanie payload
                    state->payload[0] = '\0';
                    buildPayloud(state);

                    // przekazanie informacji o wysyłce
                    state->requestSend = true;
                    if (state->connectionEstablished && state->wsi)
                    {
                        lws_callback_on_writable(state->wsi);
                        lws_cancel_service(lwsContext);
                    }
                    cardPos = 0;
                    pthread_cond_signal(&state->payloadCond);
                    pthread_mutex_unlock(&state->mutex);
                }
            } else if (code > 0x03 && code < 0x28)
            {
                if (cardPos < MAX_CARD_LEN - 1)
                {
                    char c = keycodes[code][0];
                    cardBuf[cardPos] = c;
                    cardPos++;
                }
            }
        }
    }

    close(file);
    for (int i = 0; i < state->currentCardListSize; i++) free(state->cardList[i]);
    free(state->cardList);
    return NULL;
}

#ifndef UNIT_TEST
int main(void)
{
    // Rejestracja handlerów sygnałów
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    signal(SIGSEGV, handleSignal);
    signal(SIGABRT, handleSignal);

    FILE *logFile = fopen("/var/log/cardService.log", "a");

    // Inicjalizacja AppState
    AppState state = {
        .payload = "\0",
        .wsi = NULL,
        .connectionEstablished = false,
        .currentCardListSize = CARD_NUMBER,
        .cardList = NULL,
        .requestSend = false
    };

    // Inicjalizacja mutex i condition variable
    pthread_mutex_init(&state.mutex, NULL);
    pthread_cond_init(&state.payloadCond, NULL);

    setvbuf(logFile, NULL, _IOLBF, 0);
    stderr = logFile;

    pthread_t thread_ws, thread_card;
    int ret;

    ret = pthread_create(&thread_ws, NULL, wsThread, &state);
    if (ret != 0)
    {
        fprintf(stderr, "pthread_create wsThread failed: %s\n", strerror(ret));
        return 1;
    }

    sleep(1);

    ret = pthread_create(&thread_card, NULL, cardThread, &state);
    if (ret != 0)
    {
        fprintf(stderr, "pthread_create cardThread failed: %s\n", strerror(ret));
        return 1;
    }

    pthread_join(thread_card, NULL);
    pthread_join(thread_ws, NULL);

    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.payloadCond);
    
    if (logFile)
    {
        fclose(logFile);
    }
    return 0;
}
#endif