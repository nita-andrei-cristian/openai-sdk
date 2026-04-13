#include "openai.h"

#if defined(AI_MODE) && (AI_MODE == OPENAI)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#define AI_OPENAI_HOST "api.openai.com"
#define AI_OPENAI_PORT "443"
#define AI_OPENAI_RAW_CAP (256 * 1024)

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
} ai_tls_conn;

static void ai_tls_close(ai_tls_conn *c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) {
        SSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static const char *ai_get_api_key(void) {
    return getenv("OPENAI_API_KEY");
}

static ai_openai_status ai_tls_connect(ai_tls_conn *conn) {
    if (!conn) return AI_OPENAI_ERR_ARG;

    conn->fd = -1;
    conn->ctx = NULL;
    conn->ssl = NULL;

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(AI_OPENAI_HOST, AI_OPENAI_PORT, &hints, &res);
    if (gai != 0) {
        return AI_OPENAI_ERR_DNS;
    }

    int fd = -1;
    struct addrinfo *it = res;
    for (; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    if (fd < 0) {
        freeaddrinfo(res);
        return AI_OPENAI_ERR_CONNECT;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        freeaddrinfo(res);
        close(fd);
        return AI_OPENAI_ERR_TLS;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ctx);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        freeaddrinfo(res);
        SSL_CTX_free(ctx);
        close(fd);
        return AI_OPENAI_ERR_TLS;
    }

    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, AI_OPENAI_HOST);

    if (SSL_connect(ssl) <= 0) {
        freeaddrinfo(res);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return AI_OPENAI_ERR_TLS;
    }

    freeaddrinfo(res);
    conn->fd = fd;
    conn->ctx = ctx;
    conn->ssl = ssl;
    return AI_OPENAI_OK;
}

static ai_openai_status ai_ssl_write_all(SSL *ssl, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, buf + sent, (int)(len - sent));
        if (n <= 0) {
            return AI_OPENAI_ERR_WRITE;
        }
        sent += (size_t)n;
    }
    return AI_OPENAI_OK;
}

static ai_openai_status ai_read_all(SSL *ssl, ai_string *out) {
    if (!ssl || !out) return AI_OPENAI_ERR_ARG;

    char *buf = malloc(AI_OPENAI_RAW_CAP);
    if (!buf) return AI_OPENAI_ERR_ALLOC;

    size_t total = 0;
    for (;;) {
        char chunk[8192];
        int n = SSL_read(ssl, chunk, sizeof(chunk));
        if (n > 0) {
            if (total + (size_t)n + 1 > AI_OPENAI_RAW_CAP) {
                free(buf);
                return AI_OPENAI_ERR_READ;
            }
            memcpy(buf + total, chunk, (size_t)n);
            total += (size_t)n;
            continue;
        }
        break;
    }

    buf[total] = '\0';
    out->data = buf;
    out->len = total;
    return AI_OPENAI_OK;
}

static char *ai_find_body(char *http_response) {
    char *p = strstr(http_response, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

static int ai_http_status_code(const char *http_response) {
    if (!http_response) return 0;
    int code = 0;
    if (sscanf(http_response, "HTTP/%*s %d", &code) == 1) {
        return code;
    }
    return 0;
}

static int ai_extract_json_string_field(
    const char *json,
    const char *field,
    char *out,
    size_t out_size
) {
    if (!json || !field || !out || out_size == 0) return 0;

    char needle[128];
    int written = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (written <= 0 || (size_t)written >= sizeof(needle)) return 0;

    const char *p = strstr(json, needle);
    if (!p) return 0;

    p = strchr(p, ':');
    if (!p) return 0;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }

    if (*p != '"') return 0;
    p++;

    const char *end = strchr(p, '"');
    if (!end) return 0;

    size_t len = (size_t)(end - p);
    if (len + 1 > out_size) return 0;

    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static ai_openai_status ai_dup_string(const char *src, ai_string *dst) {
    if (!src || !dst) return AI_OPENAI_ERR_ARG;
    size_t len = strlen(src);
    char *copy = malloc(len + 1);
    if (!copy) return AI_OPENAI_ERR_ALLOC;
    memcpy(copy, src, len + 1);
    dst->data = copy;
    dst->len = len;
    return AI_OPENAI_OK;
}

static ai_openai_status ai_openai_request(
    const char *request,
    ai_openai_response *out,
    int expect_id
) {
    if (!request || !out) return AI_OPENAI_ERR_ARG;
    memset(out, 0, sizeof(*out));

    ai_tls_conn conn;
    ai_openai_status st = ai_tls_connect(&conn);
    if (st != AI_OPENAI_OK) {
        return st;
    }

    st = ai_ssl_write_all(conn.ssl, request, strlen(request));
    if (st != AI_OPENAI_OK) {
        ai_tls_close(&conn);
        return st;
    }

    ai_string raw = {0};
    st = ai_read_all(conn.ssl, &raw);
    ai_tls_close(&conn);
    if (st != AI_OPENAI_OK) {
        return st;
    }

    int code = ai_http_status_code(raw.data);
    if (code < 200 || code >= 300) {
        out->raw_http = raw;
        char *body = ai_find_body(out->raw_http.data);
        if (body) {
            ai_dup_string(body, &out->body);
        }
        return AI_OPENAI_ERR_HTTP;
    }

    char *body = ai_find_body(raw.data);
    if (!body) {
        free(raw.data);
        return AI_OPENAI_ERR_PARSE;
    }

    out->raw_http = raw;
    st = ai_dup_string(body, &out->body);
    if (st != AI_OPENAI_OK) {
        ai_openai_response_free(out);
        return st;
    }

    if (expect_id) {
        if (!ai_extract_json_string_field(out->body.data, "id", out->id, sizeof(out->id))) {
            ai_openai_response_free(out);
            return AI_OPENAI_ERR_PARSE;
        }
    }

    return AI_OPENAI_OK;
}

ai_openai_status ai_openai_create_response(
    const char *json_body,
    ai_openai_response *out
) {
    if (!json_body || !out) return AI_OPENAI_ERR_ARG;

    const char *api_key = ai_get_api_key();
    if (!api_key || !*api_key) {
        return AI_OPENAI_ERR_ENV;
    }

    size_t body_len = strlen(json_body);
    size_t cap = body_len + strlen(api_key) + 1024;
    char *req = malloc(cap);
    if (!req) return AI_OPENAI_ERR_ALLOC;

    int n = snprintf(
        req, cap,
        "POST /v1/responses HTTP/1.1\r\n"
        "Host: " AI_OPENAI_HOST "\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        api_key,
        body_len,
        json_body
    );

    if (n < 0 || (size_t)n >= cap) {
        free(req);
        return AI_OPENAI_ERR_ALLOC;
    }

    ai_openai_status st = ai_openai_request(req, out, 1);
    free(req);
    return st;
}

ai_openai_status ai_openai_get_response(
    const char *response_id,
    ai_openai_response *out
) {
    if (!response_id || !*response_id || !out) return AI_OPENAI_ERR_ARG;

    const char *api_key = ai_get_api_key();
    if (!api_key || !*api_key) {
        return AI_OPENAI_ERR_ENV;
    }

    size_t cap = strlen(response_id) + strlen(api_key) + 1024;
    char *req = malloc(cap);
    if (!req) return AI_OPENAI_ERR_ALLOC;

    int n = snprintf(
        req, cap,
        "GET /v1/responses/%s HTTP/1.1\r\n"
        "Host: " AI_OPENAI_HOST "\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        response_id,
        api_key
    );

    if (n < 0 || (size_t)n >= cap) {
        free(req);
        return AI_OPENAI_ERR_ALLOC;
    }

    ai_openai_status st = ai_openai_request(req, out, 1);
    free(req);
    return st;
}

void ai_openai_response_free(ai_openai_response *resp) {
    if (!resp) return;
    free(resp->raw_http.data);
    free(resp->body.data);
    resp->raw_http.data = NULL;
    resp->raw_http.len = 0;
    resp->body.data = NULL;
    resp->body.len = 0;
    resp->id[0] = '\0';
}

const char *ai_openai_strerror(ai_openai_status status) {
    switch (status) {
        case AI_OPENAI_OK: return "ok";
        case AI_OPENAI_ERR_ARG: return "invalid argument";
        case AI_OPENAI_ERR_ENV: return "OPENAI_API_KEY not set";
        case AI_OPENAI_ERR_DNS: return "DNS resolution failed";
        case AI_OPENAI_ERR_SOCKET: return "socket error";
        case AI_OPENAI_ERR_CONNECT: return "connect error";
        case AI_OPENAI_ERR_TLS: return "TLS error";
        case AI_OPENAI_ERR_WRITE: return "write error";
        case AI_OPENAI_ERR_READ: return "read error";
        case AI_OPENAI_ERR_PARSE: return "parse error";
        case AI_OPENAI_ERR_ALLOC: return "allocation error";
        case AI_OPENAI_ERR_HTTP: return "non-2xx HTTP response";
        default: return "unknown error";
    }
}

#endif /* AI_MODE == OPENAI */
