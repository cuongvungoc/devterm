#pragma once
#include "sessions.h"
typedef struct LoginMachine LoginMachine;
typedef enum { LOGIN_WAIT, LOGIN_SEND_USER, LOGIN_SEND_PASSWORD, LOGIN_READY, LOGIN_FAILED } LoginAction;
LoginMachine *login_machine_new(const SessionProfile *profile);
LoginAction login_machine_feed(LoginMachine *machine, const char *bytes, gsize length);
void login_machine_free(LoginMachine *machine);
