#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../card_service_fn.h"

// Struktura pomocnicza do testów
typedef struct {
    char **cardList;
    int cardListSize;
    char payload[MAX_PAYLOAD];
} TestState;

// Funkcja pomocnicza do budowania payload
void buildPayloadTest(TestState *state)
{
    AppState cardState;
    cardState.cardList = state->cardList;
    cardState.currentCardListSize = state->cardListSize;
    
    buildPayloud(&cardState);
    
    strcpy(state->payload, cardState.payload);
}

// ============ SETUP/TEARDOWN ============

static int setup(void **state)
{
    TestState *testState = malloc(sizeof(TestState));
    assert_non_null(testState);
    
    testState->cardListSize = CARD_NUMBER;
    testState->cardList = calloc(testState->cardListSize, sizeof(char*));
    assert_non_null(testState->cardList);
    
    for (int i = 0; i < testState->cardListSize; i++)
    {
        testState->cardList[i] = calloc(MAX_CARD_LEN, sizeof(char));
        assert_non_null(testState->cardList[i]);
    }
    
    *state = testState;
    return 0;
}

static int teardown(void **state)
{
    TestState *testState = *state;
    
    for (int i = 0; i < testState->cardListSize; i++)
    {
        free(testState->cardList[i]);
    }
    free(testState->cardList);
    free(testState);
    
    return 0;
}

// ============ TESTY ============

// Test 1: Znajdowanie karty na liście
static void test_find_card_in_list(void **state)
{
    TestState *testState = *state;
    
    strcpy(testState->cardList[0], "1234567890");
    strcpy(testState->cardList[1], "9876543210");
    strcpy(testState->cardList[2], "5555555555");
    
    int idx = findCardInList("1234567890", (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, 0);
    
    idx = findCardInList("9876543210", (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, 1);
    
    idx = findCardInList("5555555555", (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, 2);
    
    idx = findCardInList("0000000000", (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, -1);
}

// Test 2: Sprawdzanie pustych slotów
static void test_check_empty_slots(void **state)
{
    TestState *testState = *state;
    
    int empty = checkEmptySlots(testState->cardList, testState->cardListSize);
    assert_int_equal(empty, CARD_NUMBER);
    
    strcpy(testState->cardList[0], "1111111111");
    strcpy(testState->cardList[1], "2222222222");
    strcpy(testState->cardList[2], "3333333333");
    
    empty = checkEmptySlots(testState->cardList, testState->cardListSize);
    assert_int_equal(empty, CARD_NUMBER - 3);
    
    for (int i = 0; i < testState->cardListSize; i++)
    {
        sprintf(testState->cardList[i], "card%d", i);
    }
    
    empty = checkEmptySlots(testState->cardList, testState->cardListSize);
    assert_int_equal(empty, 0);
}

// Test 3: Dodawanie karty do listy
static void test_add_card_to_list(void **state)
{
    TestState *testState = *state;
    
    int result = addCardToList("1234567890", &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 1);
    assert_string_equal(testState->cardList[0], "1234567890");
    
    result = addCardToList("9876543210", &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 1);
    assert_string_equal(testState->cardList[1], "9876543210");
}

// Test 4: Dodawanie karty do pełnej listy
static void test_add_card_to_full_list(void **state)
{
    TestState *testState = *state;
    
    for (int i = 0; i < testState->cardListSize; i++)
    {
        char card[MAX_CARD_LEN];
        sprintf(card, "card%d", i);
        int result = addCardToList(card, &testState->cardList, testState->cardListSize);
        assert_int_equal(result, 1);
    }
    
    int result = addCardToList("overflow", &testState->cardList, testState->cardListSize);
    assert_int_equal(result, -1);
}

// Test 5: Usuwanie karty z listy
static void test_remove_card_from_list(void **state)
{
    TestState *testState = *state;
    
    strcpy(testState->cardList[0], "1111111111");
    strcpy(testState->cardList[1], "2222222222");
    strcpy(testState->cardList[2], "3333333333");
    strcpy(testState->cardList[3], "4444444444");
    
    int result = removeCardFromList(1, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 0);
    
    assert_string_equal(testState->cardList[0], "1111111111");
    assert_string_equal(testState->cardList[1], "3333333333");
    assert_string_equal(testState->cardList[2], "4444444444");
    assert_string_equal(testState->cardList[3], "");
    
    int idx = findCardInList("2222222222", (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, -1);
}

// Test 6: Usuwanie pierwszej i ostatniej karty
static void test_remove_first_and_last_card(void **state)
{
    TestState *testState = *state;
    
    strcpy(testState->cardList[0], "first");
    strcpy(testState->cardList[1], "middle");
    strcpy(testState->cardList[2], "last");
    
    int result = removeCardFromList(0, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 0);
    assert_string_equal(testState->cardList[0], "middle");
    assert_string_equal(testState->cardList[1], "last");
    assert_string_equal(testState->cardList[2], "");
    
    result = removeCardFromList(1, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 0);
    assert_string_equal(testState->cardList[0], "middle");
    assert_string_equal(testState->cardList[1], "");
}

// Test 7: Usuwanie z nieprawidłowym indeksem
static void test_remove_card_invalid_index(void **state)
{
    TestState *testState = *state;
    
    strcpy(testState->cardList[0], "validcard");
    
    int result = removeCardFromList(-1, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, -1);
    
    result = removeCardFromList(testState->cardListSize, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, -1);
    
    result = removeCardFromList(testState->cardListSize + 10, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, -1);
}

// Test 8: Budowanie payload - pusta lista
static void test_build_payload_empty(void **state)
{
    TestState *testState = *state;
    
    buildPayloadTest(testState);
    
    assert_string_equal(testState->payload, "{\"cardCounter\":0}");
}

// Test 9: Budowanie payload - jedna karta
static void test_build_payload_single_card(void **state)
{
    TestState *testState = *state;
    
    strcpy(testState->cardList[0], "1234567890");
    
    buildPayloadTest(testState);
    
    assert_non_null(strstr(testState->payload, "\"card0\":1234567890"));
    assert_non_null(strstr(testState->payload, "\"cardCounter\":1"));
}

// Test 10: Budowanie payload - wiele kart
static void test_build_payload_multiple_cards(void **state)
{
    TestState *testState = *state;
    
    strcpy(testState->cardList[0], "1111111111");
    strcpy(testState->cardList[1], "2222222222");
    strcpy(testState->cardList[2], "3333333333");
    
    buildPayloadTest(testState);
    
    assert_non_null(strstr(testState->payload, "\"card0\":1111111111"));
    assert_non_null(strstr(testState->payload, "\"card1\":2222222222"));
    assert_non_null(strstr(testState->payload, "\"card2\":3333333333"));
    assert_non_null(strstr(testState->payload, "\"cardCounter\":3"));
}

// Test 11: Cykl życia karty
static void test_card_lifecycle(void **state)
{
    TestState *testState = *state;
    const char *testCard = "9999999999";
    
    int idx = findCardInList(testCard, (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, -1);
    
    int result = addCardToList(testCard, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 1);
    
    idx = findCardInList(testCard, (const char**)testState->cardList, testState->cardListSize);
    assert_int_not_equal(idx, -1);
    
    buildPayloadTest(testState);
    assert_non_null(strstr(testState->payload, testCard));
    
    result = removeCardFromList(idx, &testState->cardList, testState->cardListSize);
    assert_int_equal(result, 0);
    
    idx = findCardInList(testCard, (const char**)testState->cardList, testState->cardListSize);
    assert_int_equal(idx, -1);
}

// ============ MAIN ============

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_find_card_in_list, setup, teardown),
        cmocka_unit_test_setup_teardown(test_check_empty_slots, setup, teardown),
        cmocka_unit_test_setup_teardown(test_add_card_to_list, setup, teardown),
        cmocka_unit_test_setup_teardown(test_add_card_to_full_list, setup, teardown),
        cmocka_unit_test_setup_teardown(test_remove_card_from_list, setup, teardown),
        cmocka_unit_test_setup_teardown(test_remove_first_and_last_card, setup, teardown),
        cmocka_unit_test_setup_teardown(test_remove_card_invalid_index, setup, teardown),
        cmocka_unit_test_setup_teardown(test_build_payload_empty, setup, teardown),
        cmocka_unit_test_setup_teardown(test_build_payload_single_card, setup, teardown),
        cmocka_unit_test_setup_teardown(test_build_payload_multiple_cards, setup, teardown),
        cmocka_unit_test_setup_teardown(test_card_lifecycle, setup, teardown),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}