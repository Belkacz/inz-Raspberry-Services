#include "common.h"
#include <stdio.h>

volatile bool stopRequested = false;
struct lws_context *lwsContext = NULL;

void handleSignal(int sig)
{
    printf("\n[!] Otrzymano sygnał %d — zatrzymywanie programu...\n", sig);
    stopRequested = true;
    if (lwsContext)
    {
        lws_cancel_service(lwsContext);
    }
}