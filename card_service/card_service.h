#include <pthread.h>
#ifndef CARD_SERVICE_H
#define CARD_SERVICE_H

#define MAX_PAYLOAD 1024
#define CARD_NUMBER 24
#define MAX_CARD_LEN 32

typedef struct {
    struct lws *wsi;
    bool connectionEstablished;
    bool requestSend;

    char **cardList;
    int currentCardListSize;
    char payload[MAX_PAYLOAD];

    pthread_mutex_t mutex;
    pthread_cond_t payloadCond;
} AppState;

// deklaracje funkcji, które chcesz testować:
int findCardInList(const char *card, const char** cardList, int cardListLen);
int checkEmptySlots(char** cardList, int cardListLen);
int addCardToList(const char *card, char*** cardList, int cardListLen);
int removeCardFromList(int cardIdx, char ***cardList, int cardListLen);
#endif