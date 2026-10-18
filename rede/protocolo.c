#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "protocolo.h"

typedef struct {
    char usuario[TAM_USER];
    char texto[TAM_TEXTO];
} Mensagem;

static Mensagem mensagens[MAX_MENSAGENS];
static int totalMensagens = 0;
static pthread_mutex_t travaMensagens = PTHREAD_MUTEX_INITIALIZER;

static void escaparJSON(const char *entrada, char *saida, int tamanhoSaida) {
    int i = 0;
    while (*entrada && i < tamanhoSaida - 2) {
        unsigned char ch = *entrada++;
        switch (ch) {
            case '"':
            case '\\': saida[i++] = '\\'; saida[i++] = ch; break;
            case '\n': saida[i++] = '\\'; saida[i++] = 'n'; break;
            case '\r': saida[i++] = '\\'; saida[i++] = 'r'; break;
            case '\t': saida[i++] = '\\'; saida[i++] = 't'; break;
            default:   if (ch >= 0x20) saida[i++] = ch;
        }
    }
    saida[i] = '\0';
}

static void extrairCampoJSON(const char *json, const char *chave, char *saida, int tamanhoSaida) {
    char busca[64];
    snprintf(busca, sizeof(busca), "\"%s\"", chave);
    saida[0] = '\0';

    const char *pos = strstr(json, busca);
    if (!pos) return;

    pos = strchr(pos + strlen(busca), ':');
    if (!pos) return;

    pos = strchr(pos, '"');
    if (!pos) return;
    pos++;

    int i = 0;
    while (*pos && *pos != '"' && i < tamanhoSaida - 1) {
        char ch = *pos++;
        if (ch == '\\' && *pos) {
            ch = *pos++;
            if (ch == 'n') ch = '\n';
            else if (ch == 't') ch = '\t';
            else if (ch == 'r') ch = '\r';
        }
        saida[i++] = ch;
    }

    if (*pos && *pos != '"' && i > 0) {
        int j = i - 1;
        while (j > 0 && ((unsigned char)saida[j] & 0xC0) == 0x80) j--;
        unsigned char lider = (unsigned char)saida[j];
        int precisa = lider >= 0xF0 ? 4 : lider >= 0xE0 ? 3 : lider >= 0xC0 ? 2 : 1;
        if (j + precisa > i) i = j;
    }
    saida[i] = '\0';
}

int protocoloAdicionarMensagem(const char *corpoJSON) {
    if (!corpoJSON) return 0;

    Mensagem nova;
    extrairCampoJSON(corpoJSON, "usuario", nova.usuario, TAM_USER);
    extrairCampoJSON(corpoJSON, "texto", nova.texto, TAM_TEXTO);
    if (!nova.usuario[0] || !nova.texto[0]) return 0;

    pthread_mutex_lock(&travaMensagens);
    if (totalMensagens == MAX_MENSAGENS) {
        memmove(&mensagens[0], &mensagens[1], sizeof(Mensagem) * (MAX_MENSAGENS - 1));
        totalMensagens--;
    }
    mensagens[totalMensagens++] = nova;
    pthread_mutex_unlock(&travaMensagens);
    return 1;
}

char *protocoloListarJSON(int *tamanho) {
    char escapadoUser[TAM_USER * 2];
    char escapadoTexto[TAM_TEXTO * 2];

    pthread_mutex_lock(&travaMensagens);

    size_t capacidade = (size_t)totalMensagens * (2 * (TAM_USER + TAM_TEXTO) + 40) + 3;
    char *corpo = malloc(capacidade);
    if (!corpo) {
        pthread_mutex_unlock(&travaMensagens);
        *tamanho = 0;
        return NULL;
    }

    int pos = sprintf(corpo, "[");
    for (int i = 0; i < totalMensagens; i++) {
        escaparJSON(mensagens[i].usuario, escapadoUser, sizeof(escapadoUser));
        escaparJSON(mensagens[i].texto, escapadoTexto, sizeof(escapadoTexto));
        pos += sprintf(corpo + pos, "%s{\"usuario\":\"%s\",\"texto\":\"%s\"}",
                       (i > 0) ? "," : "", escapadoUser, escapadoTexto);
    }
    pos += sprintf(corpo + pos, "]");

    pthread_mutex_unlock(&travaMensagens);

    *tamanho = pos;
    return corpo;
}
