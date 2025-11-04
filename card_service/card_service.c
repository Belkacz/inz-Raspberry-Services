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
#include <pthread.h>
#include <errno.h>
// #include <LCD1602.h
#include <signal.h>
#include <time.h>
#include <sys/time.h>

#define PIR_PIN1 26
#define PIR_PIN2 16
#define MAX_PAYLOAD 1024
#define CARD_INPUT "/dev/hidraw0"
#define CARD_NUMBER 24
#define MAX_CARD_LEN 32
#define PORT 2139

/* Wspólne zmienne */
static char payload[MAX_PAYLOAD];
static struct lws_context *lwsContext = NULL;
static struct lws *globalWsi = NULL;
static bool connectionEstablished = false;
static bool requestSend = false;
static volatile bool stopRequested = false;

int currentCardListSizeGlobal = CARD_NUMBER; // zmienna list kart dla callbackLWS
char **cardListGlobal = NULL;

/* mutex */
static pthread_mutex_t mutexState = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t payloadCond = PTHREAD_COND_INITIALIZER;

void handleSignal(int sig)
{
    printf("\n[!] Otrzymano sygnał %d — zatrzymywanie programu...\n", sig);
    stopRequested = true;
    if (lwsContext)
    {
        lws_cancel_service(lwsContext);  // przerywa lws_service()
    }
}

int findCardInList(const char *card, const char** cardList, int cardListLen)
{
    for (int i = 0; i < cardListLen; i++)
        if (strcmp(cardList[i], card) == 0)
            return i;
    return -1;
}
int checkEmptySlots(char** cardList, int cardListLen)
{
    int emptyCount = 0;
    for (int i = 0; i < cardListLen; i++)
    {
        if(cardList[i][0] == '\0') // pusty string
            emptyCount++;
    }
    return emptyCount;
}

int resizeBuffer(char*** cardList, int cardListLen, int emptySlots) // niepotrzebne w aktualnym staium rozwoju
{
    if (emptySlots == 0)
    {
        int newLen = cardListLen + CARD_NUMBER;
        char **tmpCardList = realloc(*cardList, newLen * sizeof(char *));
        if (!tmpCardList)
        {
            perror("realloc failed (expand)");
            return cardListLen; // zachowujemy starą długość
        }
        for (int i = cardListLen; i < newLen; i++)
        {
            tmpCardList[i] = calloc(MAX_CARD_LEN, sizeof(char));
            if (!tmpCardList[i])
            {
                fprintf(stderr, "calloc failed for new slot no %d\n", i);
            }
        }
        *cardList = tmpCardList;
        printf("[resizeBuffer] Expanded to %d slots\n", newLen);
        return newLen;
    } else if (emptySlots > CARD_NUMBER)
        {
            int newLen = cardListLen - (emptySlots - CARD_NUMBER);
            if (newLen < CARD_NUMBER)
                newLen = CARD_NUMBER;

            // zwolnij nadmiarowe elementy
            for (int i = newLen; i < cardListLen; i++)
                free((*cardList)[i]);

            char **tmp = realloc(*cardList, newLen * sizeof(char *));
            if (!tmp)
            {
                perror("realloc failed when downgrading the buffer size");
                return cardListLen;
            }

            *cardList = tmp;
            printf("[resizeBuffer] Downgraded Buffer card to %d slots\n", newLen);
            return newLen;
        }

        printf("[resizeBuffer] No resize needed (%d free slots)\n", emptySlots);
        return cardListLen;
}

int addCardToList(const char *card, char*** cardList, int cardListLen)
{
    for (int i = 0; i < cardListLen; i++)
    {
        if ((*cardList)[i][0] == '\0')
        {
            strncpy((*cardList)[i], card, MAX_CARD_LEN - 1);
            (*cardList)[i][MAX_CARD_LEN - 1] = '\0';
            return 1;
        }
    }
    return -1;
}

int removeCardFromList(const char *card, int cardIdx, char ***cardList, int cardListLen)
{
    if (cardIdx < 0 || cardIdx >= cardListLen)
        return -1;

    (*cardList)[cardIdx][0] = '\0';
    for (int i = cardIdx; i < cardListLen - 1; i++)
    {
        strcpy((*cardList)[i], (*cardList)[i + 1]);
        (*cardList)[i + 1][0] = '\0';
    }

    return 0;
}

void buildPayloud(char ** cardList, int currentCardListSize)
{
    payload[0] = '\0';
    strncat(payload, "{", MAX_PAYLOAD - 1);

    int cardCount = 0;
    for (int i = 0; i < currentCardListSize; i++)
    {
        if (cardList[i][0] != '\0')
        {
            char tmpCardStr[24]; // tymczasowy bufor dla jednej pary // 3473788805 to 9 + 6
            snprintf(tmpCardStr, sizeof(tmpCardStr),"\"card%d\":%s,", i, cardList[i]);

            strncat(payload, tmpCardStr, MAX_PAYLOAD - strlen(payload) - 1);
            cardCount++;
        }
    }
    snprintf(payload + strlen(payload),  // wrzucenie na koniec countera
         MAX_PAYLOAD - strlen(payload), 
         "\"cardCounter\":%d}", cardCount);
}

// callback WebSocket
static int callbackLWS(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            lwsl_user("Nowe połączenie WebSocket\n");
            pthread_mutex_lock(&mutexState);
            connectionEstablished = true;
            globalWsi = wsi;
            buildPayloud(cardListGlobal, currentCardListSizeGlobal);
            lws_callback_on_writable(globalWsi);
            pthread_mutex_unlock(&mutexState);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
        {
            char message[MAX_PAYLOAD] = "\0";
            printf("payload = %s \n");
            pthread_mutex_lock(&mutexState);
            if(!(payload[0] == '\0'))
            {
                strncpy(message, payload, MAX_PAYLOAD-1);
                message[MAX_PAYLOAD-1] = '\0';
                // payload[0] = '\0';
            }
            pthread_mutex_unlock(&mutexState);

            if (message[0] == '\0')
            {
                strncpy(message, "{}", sizeof(message)-1);
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
            lwsl_user("Połączenie zamknięte\n");
            pthread_mutex_lock(&mutexState);
            globalWsi = NULL;
            connectionEstablished = false;
            pthread_mutex_unlock(&mutexState);
            break;

        default:
            break;
    }
    return 0;
}

// protokoły LWS
static const struct lws_protocols protocols[] = {
    {"card-protocol", callbackLWS, 0, MAX_PAYLOAD},
    {NULL, NULL, 0, 0}
};

// wątek WebSocket (LWS)
void *wsThread(void *arg)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = PORT;
    info.protocols = protocols;

    lwsContext = lws_create_context(&info);
    if (!lwsContext)
    {
        fprintf(stderr, "Nie udało się utworzyć kontekstu LWS\n");
        return NULL;
    }

    printf("Serwer WebSocket działa na ws://<IP>:%d\n", PORT);
    while (!stopRequested)
    {
        lws_service(lwsContext, 10);
        pthread_mutex_lock(&mutexState);
        if (connectionEstablished && requestSend && globalWsi)
        {
            lws_callback_on_writable(globalWsi);
            requestSend = false;
        }
        
        // Jeśli połączony - czekaj na sygnał (z timeoutem)
        if (connectionEstablished && globalWsi)
        {
            struct timespec delay;
            clock_gettime(CLOCK_REALTIME, &delay);

            unsigned long hundredMilliseconds = 100 * 1000000UL;
            delay.tv_nsec += hundredMilliseconds;

            if (delay.tv_nsec >= 1000000000L)
            {
                delay.tv_sec += 1;
                delay.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&payloadCond, &mutexState, &delay);
        }
        
        pthread_mutex_unlock(&mutexState);
        if (!connectionEstablished)
        {
            usleep(100000); // 100ms
        }
    }

    lws_context_destroy(lwsContext);
    lwsContext = NULL;
    return NULL;
}

// wątek do wczytania kart
void *cardThread(void *arg)
{
    int currentCardListSize = CARD_NUMBER;
    char **cardList = calloc(currentCardListSize, sizeof(char*));

    if (!cardList)
    {
        fprintf(stderr, "cardList calloc failed\n");
        return NULL;
    }
    for (int i = 0; i < currentCardListSize; i++)
    {
        cardList[i] = calloc(MAX_CARD_LEN, sizeof(char));
        cardListGlobal = calloc(MAX_CARD_LEN, sizeof(char));
        if (!cardList[i])
        {
            fprintf(stderr, "failed calloc for cardList item no %d \n", i);
            for (int j = 0; j < i; j++) free(cardList[j]);
            free(cardList);
            return NULL;
        }
    }
    currentCardListSizeGlobal = CARD_NUMBER;
    cardListGlobal = cardList; // XD

    char cardBuf[MAX_CARD_LEN] = {0};
    size_t cardPos = 0;

    int file = open(CARD_INPUT, O_RDONLY | O_NONBLOCK);
    if (file < 0)
    {
        fprintf(stderr, "Open Card Input Error");
        for (int i = 0; i < currentCardListSize; i++) free(cardList[i]);
        free(cardList);
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
                    pthread_mutex_lock(&mutexState);
                    
                    // Logika dodawania/usuwania karty
                    int ret = findCardInList(cardBuf, (const char**)cardList, currentCardListSize);
                    if (ret == -1)
                    {
                        addCardToList(cardBuf, &cardList, currentCardListSize);
                        // printf("[CARD] Dodano kartę do listy\n");
                    } else {
                        removeCardFromList(cardBuf, ret, &cardList, currentCardListSize);
                        // printf("[CARD] Usunięto kartę z listy\n");
                    }

                    int emptySlots = checkEmptySlots(cardList, currentCardListSize);
                    if (emptySlots == 0 || emptySlots > CARD_NUMBER)
                    {
                        currentCardListSize = resizeBuffer(&cardList, currentCardListSize, emptySlots);
                    }
                    // cardListGlobal = cardList;
                    // currentCardListSizeGlobal = currentCardListSize;

                    // Budowanie payload
                    payload[0] = '\0';
                    // strncat(payload, "{", MAX_PAYLOAD - 1);

                    // int cardCount = 0;
                    // for (int i = 0; i < currentCardListSize; i++){
                    //     if (cardList[i][0] != '\0') {
                    //         char entry[24]; // tymczasowy bufor dla jednej pary // 3473788805 to 9 + 6
                    //         snprintf(entry, sizeof(entry),"\"card%d\":%s,", i, cardList[i]);

                    //         strncat(payload, entry, MAX_PAYLOAD - strlen(payload) - 1);
                    //         cardCount++;
                    //     }
                    // }

                    // // usuń ostatni przecinek, jeśli był
                    // size_t len = strlen(payload);
                    // if (len > 1 && payload[len - 1] == ',')
                    //     payload[len - 1] = '\0';

                    // // zamknij JSON
                    // strncat(payload, "}", MAX_PAYLOAD - strlen(payload) - 1);
                    // fprintf(stderr, "payload = %s", payload);
                    buildPayloud(cardList, currentCardListSize);
                    int currentCardListSizeGlobal = CARD_NUMBER;

                    requestSend = true;
                    
                    if (connectionEstablished && globalWsi)
                    {
                        lws_callback_on_writable(globalWsi);
                        lws_cancel_service(lwsContext);
                    }
                    cardPos = 0;
                    pthread_cond_signal(&payloadCond);
                    pthread_mutex_unlock(&mutexState);

                    // printf("[CARD] Aktualna lista:\n");
                    // for (int i = 0; i < currentCardListSize; i++) {
                    //     if(cardList[i][0] != '\0')
                    //         printf("  [%d] = %s\n", i, cardList[i]);
                    // }
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
        // fprintf(stderr, " [ThredCard] >>> [%d][%s] <<<\n", __LINE__, __func__);
    }

    close(file);
    for (int i = 0; i < currentCardListSize; i++) free(cardList[i]);
    free(cardList);
    return NULL;
}
int main(void)
{
    payload[0] = '\0';
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    FILE *logFile = fopen("/var/log/cardService.log", "a");
    setvbuf(logFile, NULL, _IOLBF, 0);
    stderr = logFile;

    pthread_t tid_ws, tid_card;
    int ret;

    ret = pthread_create(&tid_ws, NULL, wsThread, NULL);
    if (ret != 0)
    {
        fprintf(stderr, "pthread_create wsThread failed: %s\n", strerror(ret));
        return 1;
    }

    sleep(1);

    ret = pthread_create(&tid_card, NULL, cardThread, NULL);
    if (ret != 0)
    {
        fprintf(stderr, "pthread_create cardThread failed: %s\n", strerror(ret));
        return 1;
    }

    pthread_join(tid_card, NULL);
    pthread_join(tid_ws, NULL);

    return 0;
}
