#ifndef MONITOR_H
#define MONITOR_H

#include "rede/http.h"

void monitorIniciar(const char *arquivoLog, int mostrarPolling, int mostrarCabecalhos,
                    int mostrarQuadros, int mostrarHex, int portaServidor);
void monitorRegistrar(const Conexao *c);
void monitorFechar(void);

#endif
