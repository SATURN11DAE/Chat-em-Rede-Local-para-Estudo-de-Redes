#ifndef HTTP_H
#define HTTP_H

#include <arpa/inet.h>
#include <time.h>

#define TAM_BUFFER 4096

typedef struct {
    char metodo[8];
    char caminho[128];
    char versao[16];
    char host[128];
    char userAgent[256];
    char contentType[64];
    char idioma[64];
    int contentLength;

    const char *cabecalhosBrutos;
    int tamCabecalhos;
    const char *corpo;
    int tamCorpo;
} Requisicao;

typedef struct {
    int  socket;
    char ip[INET_ADDRSTRLEN];
    int  porta;

    char buffer[TAM_BUFFER];
    int  recebido;
    Requisicao req;

    int  status;
    long bytesEnviados;
    struct timespec inicio;
} Conexao;

void httpIniciarConexao(Conexao *c, int socket, const struct sockaddr_in *endereco);
int  httpReceber(Conexao *c);
void httpResponder(Conexao *c, int status, const char *tipoConteudo, const char *corpo, long tamanho);
void httpEnviarArquivo(Conexao *c, const char *caminho, const char *tipoConteudo);

#endif
