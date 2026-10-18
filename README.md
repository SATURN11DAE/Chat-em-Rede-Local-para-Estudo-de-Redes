# CHATinho

Chat de rede local escrito em C, com um monitor de rede embutido que mostra o que acontece em cada camada (Ethernet, IP e TCP) a cada requisição.

O programa em C é o servidor, o navegador (no PC ou celular) é o cliente. Não há nada para instalar no cliente, o servidor entrega uma página HTML e ela conversa com ele por HTTP.

Projeto feito para estudar redes de computadores: arquitetura cliente-servidor, sockets, threads, camadas do modelo de rede e como um quadro Ethernet carrega um pacote IP, que carrega um segmento TCP, que carrega uma requisição HTTP.

---

## Funcionalidades

- Servidor HTTP escrito do zero com sockets POSIX (sem bibliotecas externas)
- Uma thread por cliente, com timeout de 5 s para conexões paradas
- Chat em "tempo real" por polling (1 req/s), com histórico das últimas 200 mensagens
- Interface responsiva (celular e desktop)
- Monitor que registra cada requisição: método, rota, status, tempo, IP, MAC, aparelho e navegador (detectados pelo `User-Agent`)
- Captura de quadros com `AF_PACKET`: mostra cabeçalhos Ethernet / IPv4 / TCP campo a campo, com flags, seq/ack relativos (como no Wireshark) e opções TCP
- Modo hexadecimal para ver os bytes brutos de cada cabeçalho
- Log salvo em `chat-requisicoes.log`

---

## Estrutura

```
.
├── servidor.c            # main: sockets, threads e rotas
├── index.html            # interface do chat (roda no navegador)
├── style.css             # tema
├── Makefile
├── relatorio.md          # relatório técnico detalhado
├── rede/
│   ├── http.c / http.h           # lê requisições e monta respostas HTTP
│   └── protocolo.c / protocolo.h # guarda as mensagens e converte de/para JSON
└── tela/
    ├── monitor.c / monitor.h     # log de requisições e impressão dos quadros
    └── captura.c / captura.h     # captura e interpretação de Ethernet/IP/TCP
```

---

## Requisitos

- Linux (usa `AF_PACKET`, `/proc/net/arp` e `setcap`)
- `gcc` e `make`
- Permissão `CAP_NET_RAW` para a captura de quadros (opcional: sem ela o servidor roda só com o monitor HTTP)

No Debian/Ubuntu:

```bash
sudo apt install build-essential libcap2-bin
```

---

## Como executar

```bash
make                # compila e gera o binário "chat"
make permissao      # dá permissão de captura ao binário (repita após cada compilação)
./chat
```

> Execute de dentro da pasta do projeto: o servidor lê `index.html` e `style.css` do diretório atual.

Depois abra no navegador:

```
http://<IP-do-seu-PC>:8080
```

Pelo celular, use o IP do PC na rede local (veja com `ip a`). Para ver MACs reais no monitor, acesse pelo IP da rede e não por `localhost`.

Sem `make permissao`, você também pode rodar com `sudo ./chat`.

### Opções

| Opção | Efeito |
|---|---|
| `--polling` | Mostra também os `GET /messages` repetitivos (ocultos por padrão) |
| `--raw` | Mostra os cabeçalhos HTTP brutos |
| `--hex` | Mostra os bytes brutos de cada cabeçalho de quadro, pacote e segmento |
| `--sem-quadros` | Desliga a captura e mostra só o HTTP |

Para limpar o binário e o log: `make clean`.

---

## Rotas

| Método e caminho | Ação | Resposta |
|---|---|---|
| `GET /` ou `/index.html` | Envia a página | 200 |
| `GET /style.css` | Envia o CSS | 200 |
| `GET /messages` | Lista as mensagens em JSON | 200 |
| `POST /send` | Guarda uma mensagem (`{"usuario":"...","texto":"..."}`) | 200 ou 400 |
| outra rota `GET` | | 404 |
| outro método | | 405 |

---

## Exemplo de saída do monitor

```
┌─ 14:32:07.481  POST /send  →  200  (0.4 ms, 120 B enviados)
│ Cliente  : 192.168.0.12:51234   MAC aa:bb:cc:dd:ee:ff
│ Aparelho : Android 14 (Pixel 7) · Chrome 120.0 · Celular
│
│ Quadros capturados na rede: 12
│ ▸ Quadro 4/12  +1.250 ms  cliente → servidor  [wlan0]
│   230 B = 14 (Ethernet) + 20 (IP) + 32 (TCP) + 164 (carga)
│   L2 QUADRO   destino ... · origem ... · tipo 0x0800 (IPv4)
│   L3 PACOTE   IPv4 · IHL 5 (20 B) · TTL 64 · proto 6 (TCP)
│   L4 SEGMENTO porta 51234 → 8080 · flags [PSH,ACK]
│   CARGA       164 B de requisição HTTP
└──
```

*(Exemplo ilustrativo; os valores variam em cada execução.)*

Uma única requisição gera cerca de 12 quadros: handshake de 3 vias, requisição, ACKs, resposta (cabeçalho e corpo em segmentos separados) e encerramento. A tabela completa está no [relatório](relatorio.md).

---

## Limitações conhecidas

- Depois de 200 mensagens, a tela de quem não enviou pode parar de atualizar (o `index.html` só redesenha quando a *quantidade* muda).
- Sem autenticação: qualquer pessoa na rede pode usar qualquer nome.
- Histórico só em memória (reiniciar o servidor apaga tudo).
- Requisições limitadas a 4096 bytes.
- Captura só de IPv4/TCP, sem VLAN. Em `localhost` os MACs vêm zerados, e os checksums de pacotes locais podem parecer errados por causa do *checksum offload* da placa.

Mais detalhes na seção 11 do [relatório](relatorio.md).

---

## Documentação

O [relatório técnico](relatorio.md) explica o projeto arquivo por arquivo e relaciona cada parte com os conceitos vistos em aula (cliente-servidor, camadas, enquadramento e transmissão).

---

## Aviso de uso

A captura de pacotes serve para fins educacionais. Use apenas em redes e aparelhos seus ou com autorização de quem os administra.# CHATinho
