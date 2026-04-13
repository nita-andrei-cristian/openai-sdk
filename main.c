#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

char *find_body(char *http_response) {
    char *body = strstr(http_response, "\r\n\r\n");
    if (!body) {
        return NULL;
    }
    return body + 4;
}

int extract_response_id(const char *json, char *out, size_t out_size) {
    const char *p = strstr(json, "\"id\"");
    if (!p) return 0;

    p = strchr(p, ':');
    if (!p) return 0;
    p++;

    while (*p == ' ' || *p == '\t') p++;
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

struct addrinfo* openai_ip(){
	int status;


	struct addrinfo hints, *res = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;       // IPv4
	hints.ai_socktype = SOCK_STREAM; // TCP

	status = getaddrinfo("api.openai.com", "443", &hints, &res);
	if (status != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
	}
	assert(status == 0);

	return res;
}

_Bool connectSSL(SSL **ssl, SSL_CTX **ctx, int* client_fd){
	struct addrinfo* ip = openai_ip();

	*client_fd = socket(ip->ai_family, ip->ai_socktype, ip->ai_protocol);
	if (*client_fd < 0){
		fprintf(stderr, "Failed to create socket\n");
		freeaddrinfo(ip);
		return 0;
	}
	if (connect(*client_fd, ip->ai_addr, ip->ai_addrlen) != 0){
		freeaddrinfo(ip);
		close(*client_fd);
		return 0;
	}

	freeaddrinfo(ip);

	*ctx = SSL_CTX_new(TLS_client_method());
	if (!*ctx) {
		ERR_print_errors_fp(stderr);
		close(*client_fd);
		return 0;
	}

	SSL_CTX_set_verify(*ctx, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_default_verify_paths(*ctx);

	*ssl = SSL_new(*ctx);
	if (!*ssl) {
		ERR_print_errors_fp(stderr);
		SSL_CTX_free(*ctx);
		close(*client_fd);
		return 0;
	}

	SSL_set_fd(*ssl, *client_fd);
	SSL_set_tlsext_host_name(*ssl, "api.openai.com");

	if (SSL_connect(*ssl) <= 0) {
		ERR_print_errors_fp(stderr);
		SSL_CTX_free(*ctx);
		SSL_free(*ssl);
		close(*client_fd);
		return 0;
	}

	return 1;
}

int read_http_response(SSL *ssl, char *out, size_t out_size) {
    char chunk[16384];
    size_t total = 0;
    int bytes;

    while ((bytes = SSL_read(ssl, chunk, sizeof(chunk))) > 0) {
        if (total + (size_t)bytes >= out_size) {
            fprintf(stderr, "response too large\n");
            return -1;
        }

        memcpy(out + total, chunk, (size_t)bytes);
        total += (size_t)bytes;
    }

    out[total] = '\0';
    return (int)total;
}

void POST(char* PostBuffer, char* response_id){
	SSL* ssl = NULL;
	SSL_CTX *ctx = NULL;
	int client_fd = -1;

	assert(connectSSL(&ssl, &ctx, &client_fd));

	assert(SSL_write(ssl, PostBuffer, strlen(PostBuffer)) > 0);

	char out[4096];
	assert(read_http_response(ssl, out, sizeof(out)) > 0);

	char *body = find_body(out);
	assert(body);

	printf("POST body:\n%s\n", body);
	assert(extract_response_id(out, response_id, 256));

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(client_fd);	
}

void GET(char* GetBuffer){
	SSL* ssl;
	SSL_CTX *ctx;
	int client_fd;

	assert(connectSSL(&ssl, &ctx, &client_fd));

	assert(SSL_write(ssl, GetBuffer, strlen(GetBuffer)) > 0);

	char out[4096];
	assert(read_http_response(ssl, out, sizeof(out)) > 0);

	printf("Body:\n%s\n", out);

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(client_fd);	
}

int main(int argc, char const* argv[])
{

	const char *api_key = getenv("OPENAI_API_KEY");
	if (!api_key) {
		fprintf(stderr, "OPENAI_API_KEY is not set\n");
		return 1;
	}

	char PostBuffer[1204];
	char GetBuffer[1204];

	const char* json = "{ \"model\": \"gpt-5\", \"input\": \"Hello\" }";

	snprintf(PostBuffer,
			1204,
			"POST /v1/responses HTTP/1.1\r\n"
			"Host: api.openai.com\r\n"
			"Authorization: Bearer %s\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n"
			"\r\n"
			"%s",
			api_key,
			strlen(json),
			json
	       );

	char response_id[256];

	POST(PostBuffer, response_id);

	snprintf(GetBuffer,
			1204,
			"GET /v1/responses/%s HTTP/1.1\r\n"
			"Host: api.openai.com\r\n"
			"Authorization: Bearer %s\r\n"
			"Connection: close\r\n"
			"\r\n",
			response_id,
			api_key
	       );

	GET(GetBuffer);

	return 0;
}
