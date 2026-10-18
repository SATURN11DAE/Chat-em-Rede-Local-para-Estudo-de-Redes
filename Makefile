CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra -O2 -pthread -I.
SRC = servidor.c rede/http.c rede/protocolo.c tela/monitor.c tela/captura.c
HDR = rede/http.h rede/protocolo.h tela/monitor.h tela/captura.h

chat: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o chat

permissao: chat
	sudo setcap cap_net_raw+ep ./chat

clean:
	rm -f chat chat-requisicoes.log
