#include <stdio.h>
#include "openai.h"

int main(void) {
    const char *json =
        "{"
        "\"model\":\"gpt-5\","
        "\"input\":\"Hello from a toy C library\""
        "}";

    ai_openai_response created = {0};
    ai_openai_status st = ai_openai_create_response(json, &created);
    if (st != AI_OPENAI_OK) {
        fprintf(stderr, "create failed: %s\n", ai_openai_strerror(st));
        if (created.body.data) {
            fprintf(stderr, "body: %s\n", created.body.data);
        }
        ai_openai_response_free(&created);
        return 1;
    }

    printf("created id: %s\n", created.id);
    printf("create body:\n%s\n\n", created.body.data);

    ai_openai_response fetched = {0};
    st = ai_openai_get_response(created.id, &fetched);
    if (st != AI_OPENAI_OK) {
        fprintf(stderr, "get failed: %s\n", ai_openai_strerror(st));
        if (fetched.body.data) {
            fprintf(stderr, "body: %s\n", fetched.body.data);
        }
        ai_openai_response_free(&created);
        ai_openai_response_free(&fetched);
        return 1;
    }

    printf("fetched id: %s\n", fetched.id);
    printf("get body:\n%s\n", fetched.body.data);

    ai_openai_response_free(&created);
    ai_openai_response_free(&fetched);
    return 0;
}
