#ifndef AI_OPENAI_H
#define AI_OPENAI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t len;
} ai_string;

typedef struct {
    char id[256];
    ai_string raw_http;
    ai_string body;
} ai_openai_response;

typedef enum {
    AI_OPENAI_OK = 0,
    AI_OPENAI_ERR_ARG = -1,
    AI_OPENAI_ERR_ENV = -2,
    AI_OPENAI_ERR_DNS = -3,
    AI_OPENAI_ERR_SOCKET = -4,
    AI_OPENAI_ERR_CONNECT = -5,
    AI_OPENAI_ERR_TLS = -6,
    AI_OPENAI_ERR_WRITE = -7,
    AI_OPENAI_ERR_READ = -8,
    AI_OPENAI_ERR_PARSE = -9,
    AI_OPENAI_ERR_ALLOC = -10,
    AI_OPENAI_ERR_HTTP = -11
} ai_openai_status;

ai_openai_status ai_openai_create_response(
    const char *json_body,
    ai_openai_response *out
);

ai_openai_status ai_openai_get_response(
    const char *response_id,
    ai_openai_response *out
);

void ai_openai_response_free(ai_openai_response *resp);

const char *ai_openai_strerror(ai_openai_status status);

#ifdef __cplusplus
}
#endif

#endif /* AI_OPENAI_H */
