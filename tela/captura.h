#ifndef CAPTURA_H
#define CAPTURA_H

#include <time.h>

#define TAM_CRU 134 // 14 (Ethernet) + 60 (IP, máximo) + 60 (TCP, máximo)

// Um quadro Ethernet capturado, já separado em camadas.
// Quadro (L2) contém um Pacote IP (L3), que contém um Segmento TCP (L4), que contém a carga (HTTP).
typedef struct {
    struct timespec instante;      // CLOCK_REALTIME, quando o quadro foi capturado
    char interface[16];            // eth0, wlan0, lo...
    int direcao;                   // 0 = cliente -> servidor, 1 = servidor -> cliente
    char clienteIP[16];            // identifica a conexão: IP + porta do cliente
    int clientePorta;
    int tamQuadro;                 // bytes capturados (sem preâmbulo e sem FCS)

    //Camada 2: cabeçalho Ethernet (14 bytes)
    unsigned char macDestino[6], macOrigem[6];
    unsigned int  etherType;

    //Camada 3: cabeçalho IPv4 (20 a 60 bytes)
    unsigned int  ipVersao, ihl, tos, ipTamTotal, ipId;
    unsigned int  ipFlags, ipFragOffset, ttl, protocolo, ipChecksum;
    char ipOrigem[16], ipDestino[16];
    int tamIP;

    //Camada 4: cabeçalho TCP (20 a 60 bytes)
    unsigned int portaOrigem, portaDestino, seq, ack;
    unsigned int doff, flags, janela, tcpChecksum, urgente;
    int tamTCP;
    char opcoes[128];     // opções TCP já em texto (MSS, WS, SACK, Timestamp...)
    int carga;            // bytes de dados dentro do segmento (HTTP)

    // Bytes originais dos três cabeçalhos, para o modo --hex
    unsigned char cru[TAM_CRU];
    int tamCru;
} Quadro;

// Abre o socket de captura e inicia a thread que grava os quadros da porta indicada.
// Retorna 0 se ok, -1 se não houver permissão (precisa de root ou CAP_NET_RAW).
int capturaIniciar(int portaServidor);

// Copia para saida os quadros da conexão (IP + porta do cliente), em ordem cronológica,
// começando pelo último SYN (para não misturar com uma conexão antiga que usou a mesma porta).
// Retorna quantos quadros foram copiados (no máximo "max").
int capturaColetar(const char *ipCliente, int portaCliente, Quadro *saida, int max);

#endif
