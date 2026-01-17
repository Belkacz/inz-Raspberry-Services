#include "card_service_fn.h"
#include <string.h>
#include <stdio.h>

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
        if(cardList[i][0] == '\0')
            emptyCount++;
    }
    return emptyCount;
}

int addCardToList(const char *card, char*** cardList, int cardListLen)
{
    for (int i = 0; i < cardListLen; i++)
    {
        if ((*cardList)[i][0] == '\0')
        {
            size_t len = strlen(card);
            memcpy((*cardList)[i], card, len);
            (*cardList)[i][len] = '\0';
            return 1;
        }
    }
    return -1;
}

int removeCardFromList(int cardIdx, char ***cardList, int cardListLen)
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

void buildPayloud(AppState *state)
{
    state->payload[0] = '\0';
    strncat(state->payload, "{", MAX_PAYLOAD - 1);

    int cardCount = 0;
    for (int i = 0; i < state->currentCardListSize; i++)
    {
        if (state->cardList[i][0] != '\0')  
        {
            char tmpCardStr[64];
            snprintf(tmpCardStr, sizeof(tmpCardStr),"\"card%d\":%s,", i, state->cardList[i]);
            strncat(state->payload, tmpCardStr, MAX_PAYLOAD - strlen(state->payload) - 1);
            cardCount++;
        }
    }
    snprintf(state->payload + strlen(state->payload),  
         MAX_PAYLOAD - strlen(state->payload), 
         "\"cardCounter\":%d}", cardCount);
}