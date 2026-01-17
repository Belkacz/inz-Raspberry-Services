#ifndef COMMON_H
#define COMMON_H

#include <signal.h>
#include <libwebsockets.h>
#include <stdbool.h>

#define BASE_LWS_TIMEOUT 90

extern volatile bool stopRequested;
extern struct lws_context *lwsContext;

void handleSignal(int sig);

#endif