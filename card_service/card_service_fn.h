#ifndef CARD_SERVICE_FN_H
#define CARD_SERVICE_FN_H

#include <stdbool.h>
#include <stddef.h>
#include "card_service.h"

#define MAX_PAYLOAD 1024
#define CARD_NUMBER 24
#define MAX_CARD_LEN 32

// Deklaracje funkcji do testowania
int findCardInList(const char *card, const char** cardList, int cardListLen);
int checkEmptySlots(char** cardList, int cardListLen);
int addCardToList(const char *card, char*** cardList, int cardListLen);
int removeCardFromList(int cardIdx, char ***cardList, int cardListLen);

void buildPayloud(AppState *state);

#endif