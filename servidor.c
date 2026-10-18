#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "rede/http.h"
#include "rede/protocolo.h"
#include "tela/monitor.h"

#define PORTA 8080
#define ARQUIVO_LOG "chat-requisicoes.log"

typedef struct {
    int socket;
    struct sockaddr_in endereco;
} ArgCliente;

// Decide o que fazer com cada rota
static void rotear(Conexao *c) {
    const Requisicao *r = &c->req;
    static const char ok[] = "{\"status\":\"ok\"}";
    static const char erro[] = "{\"status\":\"erro\"}";

    if (strcmp(r->metodo, "GET") == 0) {
        if (strcmp(r->caminho, "/") == 0 || strcmp(r->caminho, "/index.html") == 0) {
            httpEnviarArquivo(c, "index.html", "text/html");

        } else if (strcmp(r->caminho, "/style.css") == 0) {
            httpEnviarArquivo(c, "style.css", "text/css");
        } else if (strcmp(r->caminho, "/messages") == 0) {
            int tamanho;
            char *json = protocoloListarJSON(&tamanho);
            if (json) {
                httpResponder(c, 200, "application/json", json, tamanho);
                free(json);
            } else {
                httpResponder(c, 500, NULL, NULL, 0);
            }
        } else {
            httpResponder(c, 404, NULL, NULL, 0);
        }

    } else if (strcmp(r->metodo, "POST") == 0 && strcmp(r->caminho, "/send") == 0) {
        if (protocoloAdicionarMensagem(r->corpo))
            httpResponder(c, 200, "application/json", ok, sizeof(ok) - 1);
        else
            httpResponder(c, 400, "application/json", erro, sizeof(erro) - 1);

    } else {
        httpResponder(c, 405, NULL, NULL, 0);
    }
}

// Atende um cliente. Roda em uma thread.
static void *tratarCliente(void *arg) {
    ArgCliente *a = arg;
    Conexao c;
    httpIniciarConexao(&c, a->socket, &a->endereco);
    free(a);

    struct timeval limite = {5, 0};      // não deixa a thread presa se o cliente não mandar nada
    setsockopt(c.socket, SOL_SOCKET, SO_RCVTIMEO, &limite, sizeof(limite));

    int resultado = httpReceber(&c);
    if (resultado == 0) {
        rotear(&c);
    } else if (resultado == -2) {
        const char msg[] = "Requisicao invalida";
        httpResponder(&c, 400, "text/plain", msg, sizeof(msg) - 1);
    } else {
        close(c.socket);                 // conexão vazia/timeout: nada a registrar
        return NULL;
    }

    close(c.socket);                     // fecha antes: o FIN do servidor sai já e o monitor vê o encerramento
    monitorRegistrar(&c);
    return NULL;
}

int main(int argc, char *argv[]) {
    int mostrarPolling = 0, mostrarCabecalhos = 0, mostrarQuadros = 1, mostrarHex = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--polling") == 0) mostrarPolling = 1;
        else if (strcmp(argv[i], "--raw") == 0) mostrarCabecalhos = 1;
        else if (strcmp(argv[i], "--hex") == 0) mostrarHex = 1;
        else if (strcmp(argv[i], "--sem-quadros") == 0) mostrarQuadros = 0;
        else { fprintf(stderr, "Uso: %s [--polling] [--raw] [--hex] [--sem-quadros]\n", argv[0]); return 1; }
    }
    
    monitorIniciar(ARQUIVO_LOG, mostrarPolling, mostrarCabecalhos, mostrarQuadros, mostrarHex, PORTA);

    int idServidor = socket(AF_INET, SOCK_STREAM, 0);
    if (idServidor < 0) { perror("Erro ao criar o socket"); exit(EXIT_FAILURE); }

    int op = 1;
    setsockopt(idServidor, SOL_SOCKET, SO_REUSEADDR, &op, sizeof(op));

    struct sockaddr_in enderecoServidor = {0};
    enderecoServidor.sin_family = AF_INET;
    enderecoServidor.sin_addr.s_addr = INADDR_ANY;
    enderecoServidor.sin_port = htons(PORTA);

    if (bind(idServidor, (struct sockaddr *)&enderecoServidor, sizeof(enderecoServidor)) < 0) {
        perror("Erro no bind"); close(idServidor); exit(EXIT_FAILURE);
    }
    if (listen(idServidor, 10) < 0) {
        perror("Erro ao escutar"); close(idServidor); exit(EXIT_FAILURE);
    }

    printf("[Chat Local] Servidor rodando na porta %d\n", PORTA);
    printf("Acesse http://<IP-do-PC>:%d pelo navegador do PC ou do celular na mesma rede.\n", PORTA);
    printf("Monitor: log em %s%s%s\n\n", ARQUIVO_LOG,
           mostrarPolling ? " | mostrando polling" : "", mostrarCabecalhos ? " | cabecalhos brutos" : "");

    while (1) {
        ArgCliente *a = malloc(sizeof(ArgCliente));
        if (!a) continue;

        socklen_t tamanho = sizeof(a->endereco);
        a->socket = accept(idServidor, (struct sockaddr *)&a->endereco, &tamanho);
        if (a->socket < 0) { perror("Erro ao aceitar conexao"); free(a); continue; }

        pthread_t thread;
        if (pthread_create(&thread, NULL, tratarCliente, a) != 0) {
            close(a->socket); free(a); continue;
        }
        pthread_detach(thread);
    }

    close(idServidor);
    monitorFechar();
    return 0;
}
