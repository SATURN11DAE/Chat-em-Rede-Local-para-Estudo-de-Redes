#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>

#include "http.h"

void httpIniciarConexao(Conexao *c, int socket, const struct sockaddr_in *endereco) {
    memset(c, 0, sizeof(*c));

    c->socket = socket;
    inet_ntop(AF_INET, &endereco->sin_addr, c->ip, sizeof(c->ip));
    c->porta = ntohs(endereco->sin_port);

    clock_gettime(CLOCK_MONOTONIC, &c->inicio);
}

static void obterCabecalho(const char *buffer, const char *nome, char *saida, int tam) {
    size_t n = strlen(nome);
    saida[0] = '\0';

    const char *p = strstr(buffer, "\r\n");
    
    while (p) {
        p += 2;
        if (p[0] == '\r' && p[1] == '\n') return;

        if (strncasecmp(p, nome, n) == 0 && p[n] == ':') {
            p += n + 1;
            
            while (*p == ' ') p++;
            int i = 0;
            
            while (*p && *p != '\r' && i < tam - 1) saida[i++] = *p++;
            saida[i] = '\0';
            
            return;
        }
        p = strstr(p, "\r\n");
    }
}

int httpReceber(Conexao *c) {
    char *fimCabecalho;

    while (!(fimCabecalho = strstr(c->buffer, "\r\n\r\n"))) {
        if (c->recebido >= TAM_BUFFER - 1) return -2;
        int n = recv(c->socket, c->buffer + c->recebido, TAM_BUFFER - 1 - c->recebido, 0);

        if (n <= 0) return -1;
        c->recebido += n;
        c->buffer[c->recebido] = '\0';
    }

    Requisicao *r = &c->req;
    if (sscanf(c->buffer, "%7s %127s %15s", r->metodo, r->caminho, r->versao) != 3) return -2;

    char tmp[32];
    obterCabecalho(c->buffer, "Host", r->host, sizeof(r->host));
    obterCabecalho(c->buffer, "User-Agent", r->userAgent, sizeof(r->userAgent));
    obterCabecalho(c->buffer, "Content-Type", r->contentType, sizeof(r->contentType));
    obterCabecalho(c->buffer, "Accept-Language", r->idioma, sizeof(r->idioma));
    obterCabecalho(c->buffer, "Content-Length", tmp, sizeof(tmp));
    r->contentLength = atoi(tmp);

    r->cabecalhosBrutos = c->buffer;
    r->tamCabecalhos = (int)(fimCabecalho - c->buffer);

    char *corpo = fimCabecalho + 4;
    int jaTem = c->recebido - (int)(corpo - c->buffer);

    while (jaTem < r->contentLength) {
        if (c->recebido >= TAM_BUFFER - 1) return -2;
        int n = recv(c->socket, c->buffer + c->recebido, TAM_BUFFER - 1 - c->recebido, 0);

        if (n <= 0) return -1;
        c->recebido += n;
        jaTem += n;
        c->buffer[c->recebido] = '\0';
    }

    r->corpo = corpo;
    r->tamCorpo = jaTem;
    return 0;
}

static const char *textoStatus(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static int enviarTudo(Conexao *c, const char *dados, long tamanho) {
    long total = 0;

    while (total < tamanho) {
        ssize_t n = send(c->socket, dados + total, tamanho - total, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        total += n;
    }

    c->bytesEnviados += total;
    return 0;
}

void httpResponder(Conexao *c, int status, const char *tipoConteudo, const char *corpo, long tamanho) {
    c->status = status;

    char cabecalho[256];
    int n = snprintf(cabecalho, sizeof(cabecalho), "HTTP/1.1 %d %s\r\n", status, textoStatus(status));

    if (tipoConteudo && tamanho > 0)
        n += snprintf(cabecalho + n, sizeof(cabecalho) - n, "Content-Type: %s; charset=UTF-8\r\n",
        tipoConteudo);

    n += snprintf(cabecalho + n, sizeof(cabecalho) - n,
    "Content-Length: %ld\r\nConnection: close\r\n\r\n", tamanho);

    if (enviarTudo(c, cabecalho, n) < 0) return;
    if (tamanho > 0) enviarTudo(c, corpo, tamanho);
}

static long lerArquivo(const char *caminho, char **destino) {
    FILE *f = fopen(caminho, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long tamanho = ftell(f);
    fseek(f, 0, SEEK_SET);

    *destino = malloc(tamanho + 1);
    if (!*destino) { fclose(f); return -1; }

    long lidos = (long)fread(*destino, 1, tamanho, f);
    fclose(f);
    if (lidos != tamanho) { free(*destino); return -1; }

    (*destino)[tamanho] = '\0';
    return tamanho;
}

void httpEnviarArquivo(Conexao *c, const char *caminho, const char *tipoConteudo) {
    char *conteudo = NULL;
    long tamanho = lerArquivo(caminho, &conteudo);

    if (tamanho < 0) {
        const char msg[] = "Arquivo nao encontrado";
        httpResponder(c, 404, "text/plain", msg, sizeof(msg) - 1);
        return;
    }

    httpResponder(c, 200, tipoConteudo, conteudo, tamanho);
    free(conteudo);
}
