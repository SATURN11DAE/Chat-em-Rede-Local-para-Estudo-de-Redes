# Relatório técnico: CHATinho com monitor de rede

Este documento explica como o projeto funciona, arquivo por arquivo, e depois relaciona cada parte com o que foi visto nas aulas de redes (arquitetura cliente-servidor, camadas, enquadramento e transmissão).

---

## 1. Visão geral

O CHATinho é um chat de rede local em que **um programa em C faz o papel de servidor** e **o navegador (no PC ou no celular) faz o papel de cliente**. Não há aplicativo para instalar no cliente: o servidor entrega uma página HTML e a página conversa com o servidor por HTTP.

O projeto tem quatro partes:

| Parte | Arquivos | Responsabilidade |
|---|---|---|
| Servidor | `servidor.c` | Abre a porta 8080, aceita conexões e decide o que responder a cada rota |
| HTTP | `http/http.c`, `http/http.h` | Lê e interpreta a requisição, monta e envia a resposta |
| Mensagens | `protocolo/protocolo.c`, `protocolo/protocolo.h` | Guarda as mensagens do chat e converte de/para JSON |
| Interface | `index.html`, `style.css` | A tela do chat, que roda no navegador |
| Monitor | `monitor/monitor.c`, `monitor/monitor.h` | Registra cada requisição (o que pediu, quem pediu, de qual aparelho) |
| Captura | `captura/captura.c`, `captura/captura.h` | Captura os quadros da rede e separa em Ethernet, IP e TCP |

Como as peças se ligam:

```
 Navegador (cliente)                     Programa "chat" (servidor)
 ┌──────────────┐    HTTP sobre TCP    ┌────────────────────────────────────┐
 │ index.html   │ ───────────────────► │ servidor.c   (aceita, cria thread) │
 │ style.css    │ ◄─────────────────── │   ├─ http.c       (lê/responde)    │
 └──────────────┘                      │   ├─ protocolo.c  (mensagens)      │
                                       │   └─ monitor.c ◄─ captura.c        │
                                       │                    (thread que     │
                                       │                     escuta a placa)│
                                       └────────────────────────────────────┘
```

---

## 2. O caminho de uma requisição

Quando alguém envia "oi" no chat, acontece isto:

1. O JavaScript da página chama `fetch("/send", { method: "POST", ... })` com `{"usuario":"ana","texto":"oi"}`.
2. O navegador abre uma **conexão TCP** com o servidor (handshake SYN, SYN+ACK, ACK) e envia o texto HTTP.
3. No servidor, `accept()` devolve um socket novo para essa conexão, e o `main` cria **uma thread** para atendê-la.
4. A thread chama `http_receber()`, que lê os bytes até completar cabeçalhos e corpo.
5. `rotear()` vê que é `POST /send` e chama `protocolo_adicionarMensagem()`.
6. `http_responder()` envia `HTTP/1.1 200 OK` com `{"status":"ok"}`.
7. A conexão é fechada (`Connection: close`), e o `monitor_registrar()` imprime o bloco com os detalhes.

Cada requisição usa **uma conexão nova**. O servidor não mantém conexões abertas.

---

## 3. `servidor.c`: sockets, threads e rotas

### 3.1 Preparação do socket

```c
int idServidor = socket(AF_INET, SOCK_STREAM, 0);
setsockopt(idServidor, SOL_SOCKET, SO_REUSEADDR, ...);
bind(idServidor, ...porta 8080, INADDR_ANY...);
listen(idServidor, 10);
```

- `AF_INET` + `SOCK_STREAM` significa **IPv4 e TCP**.
- `SO_REUSEADDR` permite reiniciar o servidor sem esperar o sistema liberar a porta.
- `INADDR_ANY` faz o servidor escutar em **todas as interfaces**, e por isso o celular na mesma rede consegue acessar.
- `listen(..., 10)` define uma fila de até 10 conexões esperando para serem aceitas.

### 3.2 Uma thread por cliente

O laço principal fica em `accept()`. A cada conexão ele aloca uma struct `ArgCliente` (socket + endereço), cria uma thread com `pthread_create` e a destaca com `pthread_detach`, para o sistema limpar a thread sozinho quando ela terminar. Assim, um cliente lento não bloqueia os outros.

Dentro de `tratarCliente`, um limite de **5 segundos** (`SO_RCVTIMEO`) evita que uma conexão que não manda nada prenda a thread para sempre.

### 3.3 Roteamento

`rotear()` compara método e caminho:

| Método e caminho | Ação | Resposta |
|---|---|---|
| `GET /` ou `/index.html` | Envia `index.html` | 200 |
| `GET /style.css` | Envia `style.css` | 200 |
| `GET /messages` | Devolve todas as mensagens em JSON | 200 (ou 500 se faltar memória) |
| `POST /send` | Guarda a mensagem | 200, ou 400 se o JSON for inválido |
| qualquer outra rota `GET` | | 404 |
| qualquer outro método | | 405 |

Os arquivos são lidos do **diretório atual**, então o programa precisa ser executado de dentro da pasta do projeto.

---

## 4. `http.c`: lendo e escrevendo HTTP

### 4.1 As structs

- `Requisicao` guarda o que foi extraído: método, caminho, versão, `Host`, `User-Agent`, `Content-Type`, `Accept-Language`, `Content-Length`, além de ponteiros para os cabeçalhos brutos e para o corpo.
- `Conexao` guarda tudo sobre uma conexão: socket, IP e porta do cliente, o buffer de 4096 bytes, a `Requisicao`, o status respondido, quantos bytes foram enviados e o instante de início (relógio `CLOCK_MONOTONIC`, usado para medir a duração).

Os ponteiros `corpo` e `cabecalhosBrutos` apontam **para dentro do buffer**, sem copiar nada.

### 4.2 `http_receber()`: o TCP é um fluxo de bytes

O TCP não preserva "mensagens": um `recv()` pode trazer só metade da requisição. Por isso a leitura é feita em duas fases:

1. **Cabeçalhos:** repete `recv()` até aparecer a sequência `\r\n\r\n`, que marca o fim dos cabeçalhos no HTTP.
2. **Corpo:** lê o `Content-Length` e continua com `recv()` até ter esse número de bytes.

Valores de retorno: `0` ok, `-1` conexão vazia, erro ou timeout, `-2` requisição malformada ou grande demais (o buffer é de 4096 bytes).

`obterCabecalho()` percorre as linhas do buffer procurando `Nome:` sem diferenciar maiúsculas de minúsculas (`strncasecmp`), porque o HTTP não faz essa distinção nos nomes de cabeçalhos.

### 4.3 `http_responder()` e `enviarTudo()`

A resposta é montada assim:

```
HTTP/1.1 200 OK\r\n
Content-Type: application/json; charset=UTF-8\r\n
Content-Length: 15\r\n
Connection: close\r\n
\r\n
{"status":"ok"}
```

- O cabeçalho é enviado em um `send()` e o corpo em outro. Na captura de quadros isso aparece como **dois segmentos TCP** (105 bytes de cabeçalho e depois 15 bytes de corpo).
- `enviarTudo()` repete `send()` até enviar tudo, porque `send()` pode enviar menos do que foi pedido.
- `MSG_NOSIGNAL` evita que o servidor seja derrubado por um sinal `SIGPIPE` se o cliente fechar a conexão no meio do envio.

---

## 5. `protocolo.c`: as mensagens

### 5.1 Armazenamento

As mensagens ficam em um vetor global de **200 posições** (`MAX_MENSAGENS`), cada uma com usuário (até 64 bytes) e texto (até 512 bytes). Quando o vetor enche, `memmove` descarta a mensagem mais antiga. Tudo fica **em memória**: reiniciar o servidor apaga o histórico.

Como várias threads mexem no vetor ao mesmo tempo, um **mutex** (`travaMensagens`) protege tanto a escrita quanto a leitura.

### 5.2 JSON feito à mão

- `extrairCampoJSON()` procura `"chave"`, pula até o `:` e copia o texto entre aspas, tratando `\n`, `\t`, `\r` e `\"`. É um parser simples, suficiente para este formato fixo.
- Quando o texto é truncado, o código recua até uma fronteira de caractere UTF-8, para não cortar um emoji ou acento ao meio (os bytes de continuação começam com os bits `10`).
- `escaparJSON()` faz o caminho inverso ao montar a resposta: escapa aspas, barras e quebras de linha e descarta outros caracteres de controle, para que o JSON não quebre.
- `protocolo_listarJSON()` calcula a capacidade pelo pior caso (todo caractere escapado, 2x) e devolve um buffer alocado com `malloc`. Quem chama precisa dar `free`, como `rotear()` faz.

---

## 6. Interface: `index.html` e `style.css`

### 6.1 Comportamento (JavaScript)

- **Identificação:** o nome é pedido com `prompt()` e guardado no `localStorage`, então o navegador lembra dele.
- **Polling:** `setInterval(buscarMensagens, 1000)` faz um `GET /messages` por segundo. É o jeito mais simples de "tempo real" com HTTP, ao custo de uma conexão TCP nova a cada segundo (o que explica todo o tráfego repetitivo no monitor).
- **Renderização:** `renderizarMensagens()` só redesenha se a quantidade de mensagens mudou. A mensagem é "minha" se `msg.usuario === meuNome`.
- **Segurança:** o texto entra na página com `textContent`, e não com `innerHTML`. Assim, uma mensagem como `<script>...</script>` aparece como texto e não é executada.
- **Envio:** `enviarMensagem()` faz o `POST /send` e depois força uma atualização imediata.

### 6.2 Visual (CSS)

O tema é rosa e preto. As cores ficam em variáveis no bloco `:root` (`--rosa`, `--rosa-forte`, `--rosa-claro`, `--preto`), então dá para trocar a paleta mudando só ali. As mensagens próprias são balões rosa com texto preto, as dos outros são pretos com contorno rosa. As regras `@media` adaptam o layout para celular (tela cheia) e para telas grandes.

---

## 7. `monitor.c`: o que cada requisição revela

`monitor_registrar()` é chamado depois de cada resposta e imprime um bloco na tela (com cores) e no arquivo `chat-requisicoes.log` (sem cores). Cada bloco mostra:

- **Linha de resumo:** hora, método, caminho, status colorido (verde 2xx, ciano 3xx, amarelo 4xx, vermelho 5xx), duração em ms e bytes enviados.
- **Cliente:** IP, porta e o **MAC**, obtido lendo a tabela ARP do Linux (`/proc/net/arp`). Isso só funciona para aparelhos na mesma rede local, porque o MAC não atravessa roteadores.
- **Aparelho:** `detectarAparelho()` deduz sistema, modelo, navegador e tipo a partir do cabeçalho `User-Agent`.
- **Host, Content-Type, idioma e corpo** (os primeiros 200 bytes).
- Com `--raw`, todos os cabeçalhos HTTP brutos.

O polling de `GET /messages` é escondido por padrão (ver `--polling`), senão o log ficaria dominado por ele. Um mutex evita que blocos de threads diferentes se misturem na saída.

---

## 8. `captura.c`: quadros, pacotes e segmentos

### 8.1 Por que precisa de outro tipo de socket

O socket normal do servidor (`SOCK_STREAM`) entrega só a carga do TCP, porque o sistema operacional já removeu os cabeçalhos das camadas de baixo. Para vê-los, `captura.c` abre um socket de captura:

```c
socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
```

Ele entrega o **quadro inteiro**, como passou pela placa de rede. Isso exige permissão (`CAP_NET_RAW`), que se dá com `make permissao` ou rodando com `sudo`.

### 8.2 Onde ficam as camadas dentro do quadro

Tudo sai do mesmo buffer. O que muda é o deslocamento:

| Camada | Nome da unidade | Onde começa | Tamanho do cabeçalho |
|---|---|---|---|
| 2, Enlace | **Quadro** (Ethernet) | byte 0 | 14 bytes fixos |
| 3, Rede | **Pacote** (IPv4) | byte 14 | `IHL × 4` (mínimo 20) |
| 4, Transporte | **Segmento** (TCP) | byte `14 + IHL×4` | `DOFF × 4` (mínimo 20) |
| 7, Aplicação | Dados (HTTP) | byte `14 + IHL×4 + DOFF×4` | tamanho total do IP menos os cabeçalhos |

O **IHL** e o **DOFF** ("data offset") são contados em palavras de 4 bytes, por isso a multiplicação por 4.

Campos lidos em cada cabeçalho:

- **Ethernet:** MAC de destino (6 bytes), MAC de origem (6 bytes), EtherType (2 bytes, `0x0800` = IPv4).
- **IPv4:** versão, IHL, ToS, tamanho total, identificação, flags (DF/MF), deslocamento de fragmento, TTL, protocolo (6 = TCP), checksum, IP de origem e de destino.
- **TCP:** porta de origem e de destino, número de sequência, número de confirmação (ack), offset, flags (SYN, ACK, PSH, FIN, RST...), janela, checksum, ponteiro urgente e opções (MSS, window scale, SACK, timestamp).

Os inteiros são lidos byte a byte (`le16`, `le32`), porque os cabeçalhos usam a **ordem de rede (big-endian)**, e essa leitura também evita problemas de alinhamento em memória.

### 8.3 Cálculo do tamanho da carga

A carga não pode ser deduzida do tamanho do quadro, porque o Ethernet completa quadros pequenos com zeros (enchimento). O cálculo correto usa o campo do IP:

```
carga = tamanho_total_IP − (IHL × 4) − (DOFF × 4)
```

Exemplo de um quadro real capturado: `230 B = 14 (Ethernet) + 20 (IP) + 32 (TCP) + 164 (carga)`.

### 8.4 A thread de captura e o buffer circular

Uma thread fica em `recvfrom()` o tempo todo. Cada quadro TCP da porta 8080 é interpretado e guardado num **buffer circular de 8192 quadros** (protegido por mutex). Quadros de outras portas e outros protocolos são ignorados. Na interface de loopback, o mesmo quadro pode ser entregue duas vezes, e uma checagem de duplicata descarta a repetição.

### 8.5 Juntando quadros e requisição

Uma conexão é identificada por **IP e porta do cliente**. Quando o monitor vai imprimir uma requisição, ele:

1. espera 150 ms, para os quadros do encerramento (FIN/ACK) chegarem;
2. chama `captura_coletar()`, que devolve os quadros dessa conexão em ordem cronológica, começando pelo último SYN (para não misturar com uma conexão antiga que usou a mesma porta);
3. imprime cada quadro com suas três camadas.

Para isso, `servidor.c` fecha o socket **antes** de chamar o monitor, de modo que o FIN do servidor já tenha saído.

### 8.6 Números de sequência relativos

O TCP começa a numerar bytes a partir de um valor inicial aleatório (ISN), o que gera números de 10 dígitos. O monitor mostra também o valor **relativo**, contado a partir do SYN de cada sentido, como o Wireshark faz. Assim se vê que o `ack` do outro lado avança exatamente o número de bytes recebidos.

### 8.7 O que uma requisição produz na captura

No teste com `POST /send`, a conexão gerou 12 quadros:

| # | Sentido | Flags | Papel |
|---|---|---|---|
| 1 | cliente → servidor | SYN | Abre a conexão (handshake 1/3) |
| 2 | servidor → cliente | SYN, ACK | Handshake 2/3 |
| 3 | cliente → servidor | ACK | Handshake 3/3 |
| 4 | cliente → servidor | PSH, ACK | Requisição HTTP (164 B) |
| 5 | servidor → cliente | ACK | Confirma o recebimento (`ack` relativo 165) |
| 6 | servidor → cliente | PSH, ACK | Cabeçalho da resposta (105 B) |
| 7 | cliente → servidor | ACK | Confirma |
| 8 | servidor → cliente | PSH, ACK | Corpo da resposta (15 B) |
| 9 | cliente → servidor | ACK | Confirma |
| 10 | cliente → servidor | FIN, ACK | Encerramento |
| 11 | servidor → cliente | FIN, ACK | Encerramento |
| 12 | cliente → servidor | ACK | Encerramento concluído |

---

## 9. Relação com as aulas

**Aula 3, tipos de nós e cliente-servidor.** O navegador é o *cliente* (acessa um serviço) e o programa `chat` é o *servidor* (fornece o serviço), exatamente como definido na aula. O celular e o PC estão na mesma **LAN**, ligados por um *switch* (ou pelo Wi-Fi do roteador doméstico). Isso explica por que o MAC do celular aparece no monitor: dentro de uma LAN o quadro carrega o MAC real do dispositivo. Se o acesso viesse de outra rede, o MAC visto seria o do roteador.

**Aula 6, transmissão de dados.** As ideias de bits como sinais, intervalo de sinalização, baud, Nyquist e Shannon pertencem à **camada física**, e este projeto não consegue observá-la: a placa de rede converte os sinais em bits e entrega o quadro já pronto ao sistema. O que o programa vê começa no cabeçalho Ethernet. O que a aula chama de enquadramento e detecção de erros aparece aqui nos campos de checksum do IP e do TCP.

**Camadas do modelo e o que o código mostra:**

| Camada | Unidade | Onde aparece no projeto |
|---|---|---|
| 1, Física | Bits e sinais | Não observável (fica na placa de rede) |
| 2, Enlace | Quadro | `captura.c` (MAC de origem e destino) |
| 3, Rede | Pacote | `captura.c` (IPs, TTL, protocolo) |
| 4, Transporte | Segmento | `captura.c` (portas, seq/ack, flags) e `socket()`/`accept()` em `servidor.c` |
| 7, Aplicação | Dados | `http.c` (HTTP) e `protocolo.c` (JSON) |

---

## 10. Como executar

```
make                  # compila (gera o binário "chat")
make permissao        # dá permissão de captura ao binário (rodar após cada compilação)
./chat                # servidor + monitor com quadros
```

Opções:

| Opção | Efeito |
|---|---|
| `--polling` | Mostra também os `GET /messages` repetitivos |
| `--raw` | Mostra os cabeçalhos HTTP brutos |
| `--hex` | Mostra os bytes brutos (em hexadecimal) de cada cabeçalho de quadro, pacote e segmento |
| `--sem-quadros` | Desliga a captura e mostra só o HTTP |

Acesse `http://<IP-do-PC>:8080` no navegador. Para ver MACs reais, use o IP da rede (e não `localhost`), de preferência pelo celular.

Para conferir a captura, rode o Wireshark com o filtro `tcp.port == 8080` e compare os campos.

---

## 11. Limitações e pontos de atenção

- **Mensagens novas podem não aparecer depois de 200.** O vetor guarda no máximo 200 mensagens, e o `index.html` só redesenha quando a *quantidade* muda. Com o vetor cheio, a quantidade fica em 200 mesmo com mensagens novas chegando, então a tela de quem não enviou deixa de atualizar. Uma correção é comparar o conteúdo da última mensagem (ou um contador que só cresce) em vez do tamanho da lista.
- **Sem autenticação.** O nome é só um texto que o navegador envia. Qualquer pessoa na rede pode usar qualquer nome, e o servidor aceita conexões de qualquer aparelho da LAN.
- **Sem persistência.** O histórico existe só na memória.
- **Requisições limitadas a 4096 bytes** (cabeçalhos + corpo). Acima disso o servidor responde 400.
- **Um limite de 500 caracteres no campo, mas 512 bytes no servidor.** Textos com muitos acentos ou emojis podem passar de 512 bytes e são cortados (sem quebrar o caractere UTF-8).
- **Uma conexão TCP por requisição.** É simples, mas gera muito handshake e encerramento com o polling de 1 s. Conexões persistentes (`keep-alive`) ou WebSocket reduziriam isso.
- **Captura:** só IPv4/TCP, sem VLAN. O preâmbulo e o FCS do Ethernet não aparecem, porque a placa os remove. Os checksums de pacotes que saem do próprio computador podem parecer errados por causa do *checksum offload* da placa. Em `localhost` os MACs vêm zerados.
- **Permissão elevada.** A captura exige `CAP_NET_RAW`. Prefira `make permissao` a rodar tudo como root.

---

## 12. Ideias para continuar estudando

1. Compare o handshake capturado com o diagrama de três vias visto em aula e confira, quadro a quadro, como o `ack` relativo avança.
2. Acesse pelo celular e compare o MAC de origem no quadro com o MAC mostrado pelo próprio aparelho.
3. Mude `Connection: close` para `keep-alive` e observe quantos quadros de handshake e encerramento deixam de aparecer.
4. Mande uma mensagem longa e veja como a resposta é dividida em segmentos.
5. Corrija o problema das 200 mensagens no `index.html` e teste enchendo o histórico.
