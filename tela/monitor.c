#include "monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "captura.h"

#define MAX_QUADROS 64    // quadros mostrados por conexão
#define ESPERA_FIM_MS 150 // espera o encerramento (FIN/ACK) chegar antes de listar os quadros

static FILE *arquivoLog = NULL;
static int opPolling = 0;
static int opCabecalhos = 0;
static int opHex = 0;
static int capturaAtiva = 0;
static pthread_mutex_t travaMonitor = PTHREAD_MUTEX_INITIALIZER;

void monitorIniciar(const char *caminhoLog, int mostrarPolling, int mostrarCabecalhos,
    int mostrarQuadros, int mostrarHex, int portaServidor) {

    opPolling = mostrarPolling;
    opCabecalhos = mostrarCabecalhos;
    opHex = mostrarHex;

    if (caminhoLog) arquivoLog = fopen(caminhoLog, "a");

    if (mostrarQuadros) {
        if (capturaIniciar(portaServidor) == 0) {
            capturaAtiva = 1;
        } else {
            fprintf(stderr, "[Monitor] Sem captura de quadros: falta permissao (CAP_NET_RAW).\n"
                            "          Roda com sudo ou usa 'make permissao'. Seguindo so com HTTP.\n");
        }
    }
}

void monitorFechar(void) {
    if (arquivoLog) fclose(arquivoLog);
    arquivoLog = NULL;
}

// Copia o texto que vem depois de "marca" até um dos caracteres de "fim"
static int depoisDe(const char *s, const char *marca, char *out, int tam, const char *fim) {
    const char *p = strstr(s, marca);
    if (!p) return 0;
    p += strlen(marca);

    int i = 0;
    while (*p && !strchr(fim, *p) && i < tam - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

// Deduz sistema, modelo, navegador e tipo de aparelho a partir do User-Agent
static void detectarAparelho(const char *ua, char *saida, int tam) {
    if (!ua[0]) {snprintf(saida, tam, "(sem User-Agent)"); return;}

    char so[48] = "SO desconhecido", modelo[48] = "", nav[48] = "navegador desconhecido", v[32];
    const char *tipo = "Computador";

    if (strstr(ua, "Android")) {
        if (depoisDe(ua, "Android ", v, sizeof(v), ";)")) snprintf(so, sizeof(so), "Android %s", v);
        else strcpy(so, "Android");

        const char *p = strstr(ua, "Android ");
        p = p ? strchr(p, ';') : NULL;
        if (p) {
            p++;
            while (*p == ' ') p++;
            int i = 0;
            while (*p && *p != ')' && i < (int)sizeof(modelo) - 1) modelo[i++] = *p++;
            modelo[i] = '\0';
            if (strcmp(modelo, "K") == 0) modelo[0] = '\0';
        }
        tipo = strstr(ua, "Mobile") ? "Celular" : "Tablet";

    } else if (strstr(ua, "iPhone") || strstr(ua, "iPad")) {
        int ipad = strstr(ua, "iPad") != NULL;
        strcpy(modelo, ipad ? "iPad" : "iPhone");

        if (depoisDe(ua, ipad ? "CPU OS " : "CPU iPhone OS ", v, sizeof(v), " )")) {
            for (char *q = v; *q; q++) if (*q == '_') *q = '.';
            snprintf(so, sizeof(so), "iOS %s", v);
        } else strcpy(so, "iOS");
        tipo = ipad ? "Tablet" : "Celular";

    } else if (strstr(ua, "Windows NT ")) {
        depoisDe(ua, "Windows NT ", v, sizeof(v), ";)");
        snprintf(so, sizeof(so), "Windows NT %s", v);

    } else if (strstr(ua, "Mac OS X")) {
        strcpy(so, "macOS");

    } else if (strstr(ua, "CrOS")) {
        strcpy(so, "ChromeOS");

    } else if (strstr(ua, "Linux")) {
        strcpy(so, "Linux");
    }

    static const struct { const char *marca, *nome; } NAVEGADORES[] = {
        {"Edg/", "Edge"}, {"OPR/", "Opera"}, {"Firefox/", "Firefox"}, {"FxiOS/", "Firefox"},
        {"CriOS/", "Chrome"}, {"Chrome/", "Chrome"}, {"Version/", "Safari"}, {"curl/", "curl"},
    };

    for (size_t i = 0; i < sizeof(NAVEGADORES) / sizeof(NAVEGADORES[0]); i++) {
        if (depoisDe(ua, NAVEGADORES[i].marca, v, sizeof(v), ". ;)")) {
            snprintf(nav, sizeof(nav), "%s %s", NAVEGADORES[i].nome, v);
            if (strcmp(NAVEGADORES[i].nome, "curl") == 0) tipo = "Terminal";
            break;
        }
    }

    if (modelo[0]) snprintf(saida, tam, "%s (%s) · %s · %s", so, modelo, nav, tipo);
    else           snprintf(saida, tam, "%s · %s · %s", so, nav, tipo);
}

// Descobre o MAC do aparelho pela tabela ARP do Linux (só funciona na mesma rede local)
static void buscarMAC(const char *ip, char *mac, int tam) {
    snprintf(mac, tam, "-");
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return;

    char linha[256];
    if (!fgets(linha, sizeof(linha), f)) { fclose(f); return; }   // pula o cabeçalho
    while (fgets(linha, sizeof(linha), f)) {
        char ipLinha[64], macLinha[32];
        if (sscanf(linha, "%63s %*s %*s %31s", ipLinha, macLinha) == 2 && strcmp(ipLinha, ip) == 0) {
            snprintf(mac, tam, "%s", macLinha);
            break;
        }
    }
    fclose(f);
}

static void imprimirCorpo(FILE *o, const Requisicao *r) {
    if (r->tamCorpo <= 0) return;
    char tmp[201];
    int n = r->tamCorpo < 200 ? r->tamCorpo : 200;

    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)r->corpo[i];
        tmp[i] = (ch < 0x20) ? ' ' : (char)ch;
    }
    tmp[n] = '\0';
    fprintf(o, "│ Corpo    : %s%s\n", tmp, r->tamCorpo > 200 ? " …" : "");
}

//Quadros, pacotes e segmentos

static void hexa(FILE *o, const unsigned char *b, int n) {
    for (int i = 0; i < n; i += 16) {
        fprintf(o, "│                ");
        for (int j = i; j < i + 16 && j < n; j++) fprintf(o, "%02x ", b[j]);
        fprintf(o, "\n");
    }
}

static void textoFlagsTCP(unsigned int f, char *out, int tam) {
    static const struct { unsigned int bit; const char *nome; } F[] = {
        {0x80, "CWR"}, {0x40, "ECE"}, {0x20, "URG"}, {0x10, "ACK"},
        {0x08, "PSH"}, {0x04, "RST"}, {0x02, "SYN"}, {0x01, "FIN"},
    };
    int pos = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++)
        if ((f & F[i].bit) && pos < tam)
            pos += snprintf(out + pos, tam - pos, "%s%s", pos ? "," : "", F[i].nome);
    if (!out[0]) snprintf(out, tam, "nenhuma");
}

static const char *nomeProtocoloIP(unsigned int p) {
    return p == 6 ? "TCP" : p == 17 ? "UDP" : p == 1 ? "ICMP" : "?";
}

static void imprimirQuadros(FILE *o, int cor, const Quadro *qs, int n) {
    const char *rst = cor ? "\033[0m" : "";
    const char *neg = cor ? "\033[1m" : "";
    const char *dim = cor ? "\033[2m" : "";
    const char *c2  = cor ? "\033[35m" : "";     // camada 2: magenta
    const char *c3  = cor ? "\033[93m" : "";     // camada 3: amarelo
    const char *c4  = cor ? "\033[36m" : "";     // camada 4: ciano

    fprintf(o, "│\n│ %sQuadros capturados na rede: %d%s\n", neg, n, rst);
    if (n == 0) {
        fprintf(o, "│   (nenhum quadro encontrado para esta conexao)\n");
        return;
    }

    // Wireshark mostra seq/ack "relativos" (contados a partir do SYN). Guarda o ponto de partida.
    unsigned int base[2] = {0, 0};
    int tem[2] = {0, 0};
    for (int i = 0; i < n; i++)
        if (!tem[qs[i].direcao]) { base[qs[i].direcao] = qs[i].seq; tem[qs[i].direcao] = 1; }

    for (int i = 0; i < n; i++) {
        const Quadro *q = &qs[i];
        int d = q->direcao;
        double rel = (q->instante.tv_sec - qs[0].instante.tv_sec) * 1000.0 +
                     (q->instante.tv_nsec - qs[0].instante.tv_nsec) / 1e6;

        char flags[40], seqTxt[64], ackTxt[80], ipFlags[16];
        textoFlagsTCP(q->flags, flags, sizeof(flags));
        snprintf(seqTxt, sizeof(seqTxt), "%u (relativo %u)", q->seq, q->seq - base[d]);

        if (!(q->flags & 0x10))
            snprintf(ackTxt, sizeof(ackTxt), "%u (ignorado: flag ACK desligada)", q->ack);
        else if (tem[1 - d])
            snprintf(ackTxt, sizeof(ackTxt), "%u (relativo %u)", q->ack, q->ack - base[1 - d]);
        else
            snprintf(ackTxt, sizeof(ackTxt), "%u", q->ack);
        snprintf(ipFlags, sizeof(ipFlags), "%s%s%s", (q->ipFlags & 2) ? "DF" : "",
                 ((q->ipFlags & 2) && (q->ipFlags & 1)) ? "," : "", (q->ipFlags & 1) ? "MF" : "");

        if (!ipFlags[0]) snprintf(ipFlags, sizeof(ipFlags), "nenhuma");

        char macO[18], macD[18];
        snprintf(macO, sizeof(macO), "%02x:%02x:%02x:%02x:%02x:%02x", q->macOrigem[0], q->macOrigem[1],
                 q->macOrigem[2], q->macOrigem[3], q->macOrigem[4], q->macOrigem[5]);
        snprintf(macD, sizeof(macD), "%02x:%02x:%02x:%02x:%02x:%02x", q->macDestino[0], q->macDestino[1],
                 q->macDestino[2], q->macDestino[3], q->macDestino[4], q->macDestino[5]);

        int sobra = q->tamQuadro - 14 - q->tamIP - q->tamTCP - q->carga;   // enchimento do Ethernet

        fprintf(o, "│\n│ %s▸ Quadro %d/%d%s  +%.3f ms  %s  %s[%s]%s\n", neg, i + 1, n, rst, rel,
                d == 0 ? "cliente → servidor" : "servidor → cliente", dim, q->interface, rst);
        fprintf(o, "│   %s%d B = 14 (Ethernet) + %d (IP) + %d (TCP) + %d (carga)", dim,
                q->tamQuadro, q->tamIP, q->tamTCP, q->carga);

        if (sobra > 0) fprintf(o, " + %d (enchimento)", sobra);
        fprintf(o, "%s\n", rst);

        // Camada 2
        fprintf(o, "│   %s%-12s%s destino %s · origem %s · tipo 0x%04x (IPv4)\n",
                c2, "L2 QUADRO", rst, macD, macO, q->etherType);
        if (opHex) hexa(o, q->cru, 14);

        // Camada 3
        fprintf(o, "│   %s%-12s%s IPv4 · IHL %u (%d B) · ToS 0x%02x (DSCP %u, ECN %u) · tam. total %u B\n",
                c3, "L3 PACOTE", rst, q->ihl, q->tamIP, q->tos, q->tos >> 2, q->tos & 3, q->ipTamTotal);
        fprintf(o, "│   %-12s ID 0x%04x · flags %s · deslocamento %u · TTL %u · proto %u (%s) · checksum 0x%04x\n",
                "", q->ipId, ipFlags, q->ipFragOffset, q->ttl, q->protocolo,
                nomeProtocoloIP(q->protocolo), q->ipChecksum);
        fprintf(o, "│   %-12s %s → %s\n", "", q->ipOrigem, q->ipDestino);
        if (opHex) hexa(o, q->cru + 14, q->tamIP);

        // Camada 4
        fprintf(o, "│   %s%-12s%s porta %u → %u · flags [%s]\n",
                c4, "L4 SEGMENTO", rst, q->portaOrigem, q->portaDestino, flags);
        fprintf(o, "│   %-12s seq %s · ack %s\n", "", seqTxt, ackTxt);
        fprintf(o, "│   %-12s offset %u (%d B) · janela %u · checksum 0x%04x · urgente %u\n",
                "", q->doff, q->tamTCP, q->janela, q->tcpChecksum, q->urgente);
        if (q->opcoes[0]) fprintf(o, "│   %-12s opções: %s\n", "", q->opcoes);
        if (opHex) hexa(o, q->cru + 14 + q->tamIP, q->tamTCP);

        // Carga
        if (q->carga > 0)
            fprintf(o, "│   %-12s %d B de %s\n", "CARGA", q->carga,
                    d == 0 ? "requisição HTTP" : "resposta HTTP");
    }
}

static void imprimir(FILE *o, const Conexao *c, int cor, const char *hora, double ms,
    const Quadro *qs, int nq) {

    const Requisicao *r = &c->req;
    const char *rst = cor ? "\033[0m" : "";
    const char *neg = cor ? "\033[1m" : "";
    const char *dim = cor ? "\033[2m" : "";
    const char *corStatus = !cor ? "" : c->status < 300 ? "\033[32m" : c->status < 400 ? "\033[36m"
                                      : c->status < 500 ? "\033[33m" : "\033[31m";
    char aparelho[192], mac[32];
    detectarAparelho(r->userAgent, aparelho, sizeof(aparelho));
    buscarMAC(c->ip, mac, sizeof(mac));

    fprintf(o, "%s┌─ %s  %s%s %s%s  →  %s%d%s  %s(%.1f ms, %ld B enviados)%s\n",
            dim, hora, neg, r->metodo[0] ? r->metodo : "?", r->caminho[0] ? r->caminho : "?", rst,
            corStatus, c->status, rst, dim, ms, c->bytesEnviados, rst);

    fprintf(o, "│ Cliente  : %s:%d   MAC %s\n", c->ip, c->porta, mac);
    fprintf(o, "│ Aparelho : %s\n", aparelho);

    if (r->host[0])        fprintf(o, "│ Host     : %s\n", r->host);
    if (r->contentType[0]) fprintf(o, "│ Tipo     : %s (%d bytes)\n", r->contentType, r->contentLength);
    if (r->idioma[0])      fprintf(o, "│ Idioma   : %s\n", r->idioma);
    imprimirCorpo(o, r);

    if (opCabecalhos && r->cabecalhosBrutos) {
        fprintf(o, "│ Cabeçalhos brutos:\n");
        const char *p = r->cabecalhosBrutos, *fim = p + r->tamCabecalhos;

        while (p < fim) {
            const char *eol = strstr(p, "\r\n");
            if (!eol || eol > fim) eol = fim;
            fprintf(o, "│   %.*s\n", (int)(eol - p), p);
            p = eol + 2;
        }
    }

    if (capturaAtiva) imprimirQuadros(o, cor, qs, nq);
    fprintf(o, "%s└──%s\n\n", dim, rst);
    fflush(o);
}

void monitorRegistrar(const Conexao *c) {
    // O navegador consulta /messages a cada 1s: some com isso do log, a menos que peçam
    if (!opPolling && strcmp(c->req.metodo, "GET") == 0 &&
        strcmp(c->req.caminho, "/messages") == 0 && c->status == 200) return;

    struct timespec fim, agora;
    clock_gettime(CLOCK_MONOTONIC, &fim);
    clock_gettime(CLOCK_REALTIME, &agora);
    double ms = (fim.tv_sec - c->inicio.tv_sec) * 1000.0 + (fim.tv_nsec - c->inicio.tv_nsec) / 1e6;

    struct tm t;
    localtime_r(&agora.tv_sec, &t);
    char hora[32];
    size_t n = strftime(hora, sizeof(hora), "%H:%M:%S", &t);
    snprintf(hora + n, sizeof(hora) - n, ".%03ld", agora.tv_nsec / 1000000);

    Quadro *quadros = NULL;
    int nq = 0;
    if (capturaAtiva) {
        struct timespec espera = {0, ESPERA_FIM_MS * 1000000L};
        nanosleep(&espera, NULL);
        quadros = malloc(sizeof(Quadro) * MAX_QUADROS);
        
        if (quadros) nq = capturaColetar(c->ip, c->porta, quadros, MAX_QUADROS);
    }

    pthread_mutex_lock(&travaMonitor);
    imprimir(stdout, c, 1, hora, ms, quadros, nq);

    if (arquivoLog) imprimir(arquivoLog, c, 0, hora, ms, quadros, nq);
    pthread_mutex_unlock(&travaMonitor);
    free(quadros);
}
