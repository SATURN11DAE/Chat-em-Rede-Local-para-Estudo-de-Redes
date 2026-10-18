#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#define MAX_MENSAGENS 200
#define TAM_USER 64
#define TAM_TEXTO 512

int protocoloAdicionarMensagem(const char *corpoJSON);
char *protocoloListarJSON(int *tamanho);

#endif
