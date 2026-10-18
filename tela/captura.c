#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <linux/if_arp.h>

#include "captura.h"

#define TAM_ANEL 8192 // guarda os últimos 8192 quadros da porta do servidor

static Quadro anel[TAM_ANEL];
static unsigned long totalGravados = 0;
static pthread_mutex_t travaAnel = PTHREAD_MUTEX_INITIALIZER;

static int portaServidor = 0;
static int socketCaptura = -1;

// Lê inteiros na ordem de rede (big-endian) byte a byte.
// Fazer assim (e não com um cast para struct) evita problemas de alinhamento.
static unsigned int le16(const unsigned char *p) {return ((unsigned)p[0] << 8) | p[1];}

static unsigned int le32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

// Traduz a área de opções do cabeçalho TCP (tipo, tamanho, valor) para texto
static void lerOpcoes(const unsigned char *o, int tam, char *saida, int cap) {
    int pos = 0, i = 0;
    saida[0] = '\0';
    #define ADD(...) do {if (pos < cap) pos += snprintf(saida + pos, cap - pos, __VA_ARGS__);} while (0)

    while (i < tam) {
        unsigned char tipo = o[i];
        if (tipo == 0) {ADD("EOL "); break;}                 // fim da lista
        if (tipo == 1) {ADD("NOP "); i++; continue;}         // preenchimento de 1 byte
        if (i + 1 >= tam) break;

        int len = o[i + 1];
        if (len < 2 || i + len > tam) break;

        switch (tipo) {
            case 2: if (len == 4) ADD("MSS=%u ", le16(o + i + 2)); break;
            case 3: if (len == 3) {
                        unsigned s = o[i + 2];
                        ADD("WindowScale=%u(x%u) ", s, 1u << (s > 30 ? 30 : s));
                    } break;
            case 4: ADD("SACK-permitido "); break;
            case 5: ADD("SACK "); break;
            case 8: if (len == 10) ADD("Timestamp(val=%u,eco=%u) ", le32(o + i + 2), le32(o + i + 6)); break;
            default: ADD("tipo%u ", tipo);
        }
        i += len;
    }
    #undef ADD
}

// Separa o quadro bruto em camadas. Retorna 1 se é um segmento TCP da porta do servidor.
static int interpretar(const unsigned char *b, int n, Quadro *q) {
    memset(q, 0, sizeof(*q));

    //Camada 2: Ethernet
    if (n < 14) return 0;
    memcpy(q->macDestino, b, 6);
    memcpy(q->macOrigem, b + 6, 6);
    q->etherType = le16(b + 12);
    if (q->etherType != 0x0800) return 0; // só IPv4

    //Camada 3: IPv4
    const unsigned char *ip = b + 14;
    int resto = n - 14;
    if (resto < 20) return 0;

    q->ipVersao = ip[0] >> 4;
    q->ihl = ip[0] & 0x0F;

    if (q->ipVersao != 4 || q->ihl < 5) return 0;
    q->tamIP = (int)q->ihl * 4;

    if (resto < q->tamIP + 20) return 0;

    q->tos = ip[1];
    q->ipTamTotal = le16(ip + 2);
    q->ipId = le16(ip + 4);
    q->ipFlags = le16(ip + 6) >> 13;
    q->ipFragOffset = le16(ip + 6) & 0x1FFF;
    q->ttl = ip[8];
    q->protocolo = ip[9];
    q->ipChecksum = le16(ip + 10);

    inet_ntop(AF_INET, ip + 12, q->ipOrigem, sizeof(q->ipOrigem));
    inet_ntop(AF_INET, ip + 16, q->ipDestino, sizeof(q->ipDestino));

    if (q->protocolo != 6) return 0; // só TCP
    if (q->ipFragOffset != 0) return 0; // fragmento sem cabeçalho TCP

    //Camada 4: TCP
    const unsigned char *tcp = ip + q->tamIP;
    q->portaOrigem = le16(tcp);
    q->portaDestino = le16(tcp + 2);
    if ((int)q->portaOrigem != portaServidor && (int)q->portaDestino != portaServidor) return 0;

    q->seq = le32(tcp + 4);
    q->ack = le32(tcp + 8);
    q->doff = tcp[12] >> 4;
    if (q->doff < 5) return 0;
    q->tamTCP = (int)q->doff * 4;
    if (resto < q->tamIP + q->tamTCP) return 0;

    q->flags = tcp[13];
    q->janela = le16(tcp + 14);
    q->tcpChecksum = le16(tcp + 16);
    q->urgente = le16(tcp + 18);
    lerOpcoes(tcp + 20, q->tamTCP - 20, q->opcoes, sizeof(q->opcoes));

    // Tamanho da carga = tamanho total do pacote IP - cabeçalho IP - cabeçalho TCP.
    // (Não dá para usar o tamanho do quadro: o Ethernet completa com zeros quadros pequenos.)
    // Com segmentation offload o tamanho total pode vir 0: nesse caso usa o que foi capturado.
    int total = (int)q->ipTamTotal;
    if (total == 0 || total > resto) total = resto;
    q->carga = total - q->tamIP - q->tamTCP;
    if (q->carga < 0) q->carga = 0;

    q->tamQuadro = n;
    q->tamCru = 14 + q->tamIP + q->tamTCP;
    memcpy(q->cru, b, q->tamCru < TAM_CRU ? q->tamCru : TAM_CRU);

    // Quem é o cliente? Quem não está na porta do servidor.
    if ((int)q->portaDestino == portaServidor) {
        q->direcao = 0;
        snprintf(q->clienteIP, sizeof(q->clienteIP), "%s", q->ipOrigem);
        q->clientePorta = (int)q->portaOrigem;
    } else {
        q->direcao = 1;
        snprintf(q->clienteIP, sizeof(q->clienteIP), "%s", q->ipDestino);
        q->clientePorta = (int)q->portaDestino;
    }
    return 1;
}

// Na interface de loopback o mesmo quadro pode ser entregue duas vezes (saída + entrada).
// Descarta o quadro se for idêntico ao anterior e chegou com menos de 1 ms de diferença.
static int repetido(const Quadro *a, const Quadro *b) {
    if (a->tamQuadro == 0) return 0;
    double ms = (b->instante.tv_sec - a->instante.tv_sec) * 1000.0 +
                (b->instante.tv_nsec - a->instante.tv_nsec) / 1e6;
    return ms < 1.0 && a->seq == b->seq && a->ack == b->ack && a->flags == b->flags &&
           a->ipId == b->ipId && a->janela == b->janela && a->carga == b->carga &&
           a->portaOrigem == b->portaOrigem && a->portaDestino == b->portaDestino;
}

static void *laco(void *arg) {
    (void)arg;
    static unsigned char buf[65536];
    Quadro ultimo;
    memset(&ultimo, 0, sizeof(ultimo));

    while (1) {
        struct sockaddr_ll de;
        socklen_t tam = sizeof(de);
        ssize_t n = recvfrom(socketCaptura, buf, sizeof(buf), 0, (struct sockaddr *)&de, &tam);
        if (n < 0) { if (errno == EINTR) continue; break; }

        // Só interfaces que usam cabeçalho Ethernet de 14 bytes (Ethernet, Wi-Fi, loopback)
        if (de.sll_hatype != ARPHRD_ETHER && de.sll_hatype != ARPHRD_LOOPBACK) continue;

        Quadro q;
        if (!interpretar(buf, (int)n, &q)) continue;
        clock_gettime(CLOCK_REALTIME, &q.instante);
        if (!if_indextoname(de.sll_ifindex, q.interface)) snprintf(q.interface, sizeof(q.interface), "?");

        if (repetido(&ultimo, &q)) continue;
        ultimo = q;

        pthread_mutex_lock(&travaAnel);
        anel[totalGravados % TAM_ANEL] = q;
        totalGravados++;
        pthread_mutex_unlock(&travaAnel);
    }
    return NULL;
}

int capturaIniciar(int porta) {
    portaServidor = porta;
    socketCaptura = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)); // recebe o quadro inteiro
    if (socketCaptura < 0) return -1;

    pthread_t thread;
    if (pthread_create(&thread, NULL, laco, NULL) != 0) {
        close(socketCaptura);
        socketCaptura = -1;
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

int capturaColetar(const char *ipCliente, int portaCliente, Quadro *saida, int max) {
    int achados = 0;

    pthread_mutex_lock(&travaAnel);
    unsigned long inicio = totalGravados > TAM_ANEL ? totalGravados - TAM_ANEL : 0;
    for (unsigned long i = inicio; i < totalGravados; i++) {
        const Quadro *q = &anel[i % TAM_ANEL];
        if (q->clientePorta != portaCliente || strcmp(q->clienteIP, ipCliente) != 0) continue;

        // SYN sem ACK vindo do cliente = começo de uma conexão nova: descarta a anterior
        if (q->direcao == 0 && (q->flags & 0x12) == 0x02) achados = 0;
        if (achados < max) saida[achados++] = *q;
    }
    pthread_mutex_unlock(&travaAnel);

    return achados;
}
