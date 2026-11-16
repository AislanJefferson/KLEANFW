# Auditoria do stock v7.00.15 + stub de boot

Análise estática do stock do UV-K5 V3 (`K5ENG_v7.00.15_publish.bin`, hoje
`f4hwn.k5.stock_singleboot_v7.00.15.bin`; o UV-K1 está no §15), feita com
`tools/stockpatch/disasm.sh` (objdump do Docker `uvk1-uvk5v3`) e com o
decodificador de `tools/stockpatch/patch_stock.py`. Tudo o que está aqui foi
reproduzido neste repositório; os endereços são os da imagem já decifrada.

## 1. Identidade e formato

| Item | Valor |
|---|---|
| SHA-256 do arquivo | `91845f1defa29dd1cad21bea468a05b6e91be9309956a0c34fed2c3756608d2e` |
| Tamanho do arquivo | 71.790 B |
| Bloco de versão | `7.00.15` em `0x2000` (16 B, inserido) |
| Checksum | CRC16-XMODEM sobre os bytes cifrados, little-endian nos 2 últimos bytes (`0x83E3`) |
| Imagem | 71.772 B em `0x08002800`, termina em `0x0801405C` |
| Vetor inicial | SP `0x20002FA0`, reset `0x080028D5` |

`patch_stock.py --rebuild` prova o caminho de ida e volta: decifrar, cortar os
16 B da versão e os 2 B de checksum e reempacotar reproduz o arquivo original
byte a byte.

## 2. Hook de boot (confirmado)

```
080028d4: 4804  ldr r0, [pc, #16]  @ (0x080028e8)   -> SystemInit 0x080111E5
080028d6 4780  blx r0
080028d8 4804  ldr r0, [pc, #16]  @ (0x080028ec)   -> __main 0x080028C1
080028da 4700  bx  r0
```

- `0x080028E8` é um literal de pool usado só por esse `ldr` (2 ocorrências do
  endereço no arquivo: o `ldr` e o próprio literal).
- `0x080111E4` é o `SystemInit` do stock: três instruções que gravam
  `SCB->VTOR = 0x08002800`.
- Trocar o literal não reescreve nenhuma instrução: o stub é chamado pelo
  `blx` existente e o `pop {pc}` devolve o controle ao `Reset_Handler`, que
  segue para `__main`.
- O stub usa a pilha que o stock já montou (SP `0x20002FA0`, crescendo para
  baixo) e não precisa de vector table, VTOR ou interrupções.

## 3. Palavra de RAM livre

Varredura das *cargas de literal* reais (`ldr rX,[pc,#n]` cujo alvo contém um
ponteiro em `0x20000000–0x20003FFF`): 273 endereços distintos, **o maior é
`0x20002FA0`**, que é o próprio SP inicial.

Os três suspeitos do plano original — `0x20003100`, `0x20003480`, `0x20003D24` —
são **falsos positivos**: são pares de instruções lidas como palavra
(`00 31 00 20` = `adds r1,#0; movs r0,#0`, etc.), não ponteiros.

Como o SP inicial é o topo da pilha e ela cresce para baixo, a faixa
`0x20002FA0–0x20003FFF` (4.192 B) não é usada pelo stock.

**Decisão para a fase 4:** `bank_base` em `0x20003FFC` (última palavra da RAM).

**Confirmado pela tabela de regiões do `__main`** (`0x08014008`, lida por
`0x0800319C`): a única cópia é de `0x2F0` B para `0x20000000`, e o único zero-init
é de `0x2CB0` B a partir de `0x200002F0`, terminando em `0x20002FA0`. Nada do
runtime inicializa `0x20003FFC`, então o valor gravado pelo stub chega intacto
ao stock.

## 4. Driver SPI do stock

Quatro rotinas de baixo nível, todas com o endereço em `r4` e os três bytes
emitidos por `lsls r0,r4,#8|#16` + `lsrs` + `uxtb` + `bl SPI_WriteByte`:

| Rotina | Endereço | Chamadores | Bytes de endereço |
|---|---|---|---|
| `Read(buf,addr,len)` cmd `0x03` | `0x0800F958` | 42 | `0x0800F97A`, `0x0800F982` |
| `PageProgram(buf,addr,len)` cmd `0x02` | `0x0800FBB0` | 1 | `0x0800FBD6`, `0x0800FBDE` |
| `SectorErase(addr)` cmd `0x20` | `0x0800F878` | 1 (`WriteBuffer`) | `0x0800F89C`, `0x0800F8A4` |
| `SectorErase(addr)` + invalida cache | `0x0800F8D0` | 3 | `0x0800F8F4`, `0x0800F8FC` |

Não existe um `WriteAddr` compartilhado: cada rotina embute a emissão. Mas as
8 posições acima contêm exatamente as mesmas duas instruções:

```
20 02 00 0e   lsls r0, r4, #8     ; lsrs r0, r0, #24   -> byte 2 (bits 16-23)
20 04 00 0e   lsls r0, r4, #16    ; lsrs r0, r0, #24   -> byte 1 (bits 8-15)
```

Isso permite **8 hooks de 4 bytes**, cada um trocado por um `bl` para um
trampolim que recalcula o mesmo byte a partir de `addr + bank_base`. Vantagens
sobre o desenho original (remaps de `r4`):

- `r4` nunca é modificado, então a comparação de tag do cache em
  `0x0800F914` continua usando o endereço **lógico** — o cache de setor do
  stock permanece coerente sem auditar nenhum ponto extra.
- `r12` não é usado em nenhuma das quatro rotinas, e o byte calculado vai para
  `r0`, que é justamente o destino das instruções deslocadas.
- As instruções deslocadas não leem flags de nada anterior; o próximo uso de
  `r0` é o `bl` que envia o byte.

Regra do trampolim: `addr < 0x10000` (ou seja, `addr >> 16 == 0`) recebe o
offset; `>= 0x10000` (calibração, logo, marcador) fica intocado.

## 5. Caminhos que poderiam furar as quatro rotinas

Os 22 chamadores de `SPI_WriteByte` (`0x0800FF58`) se dividem em: as quatro
rotinas acima e **um** bloco em `0x0800E8E0–E920`, que se revelou um
autoteste/diagnóstico de fábrica (envia `0x5A`, lê 8 bytes, escreve 32 bytes;
não recebe endereço de flash nem toca faixas de configuração). Não é buraco.

Os caminhos DMA (`0x0800FDD0`, `0x0800FE94`) só transferem o **payload**, depois
que os bytes de comando e endereço já saíram por `SPI_WriteByte`.

## 6. Erases e faixas de escrita

Só existem 4 chamadas de erase na imagem inteira:

- `0x0800FAA2` — dentro de `WriteBuffer` (caminho normal de gravação);
- `0x08004832` e `0x08004936` — rotinas de aplicação;
- `0x0800EAD8` — caminho de comando do host, com endereço vindo do texto
  recebido (as rotinas `0x0800EA38/EAC4` citadas no plano).

Com o remap na emissão do endereço, todo erase abaixo de `0x10000` cai no banco
N; um banco vazio (0xFF) faz o stock semear os defaults, que é o desejado.

## 7. Flash interna

Nenhuma ocorrência de `FLASH_KEY1` (`0x45670123`), `FLASH_KEY2` (`0xCDEF89AB`)
ou de `0x40022000` (FLASH_R) na imagem: **o stock não programa a flash
interna**. O stub anexado em `0x08014400` não pode ser apagado pelo stock.

## 8. Riscos registrados

- **Leituras ≥ `0x10000`**: o stock lê calibração e logo das faixas
  compartilhadas do KLEANFW. Se ele espera outro layout de calibração, bateria
  e S-meter podem sair errados, e o logo de boot pode não ser o esperado.
  Estático não dá para provar o layout esperado; medir no rádio (fase 4).
- **Relógio do stub**: no ponto do hook o stock só rodou `SystemInit` (que
  apenas grava o VTOR), então o núcleo ainda está no relógio de reset. O stub
  assume 48 MHz, o que garante atrasos *maiores* que o pedido, nunca menores.
  Confirmar em hardware antes da fase 3 (o `ST7565_Init` da fase 3 depende
  disso para os 120 ms).
- **SysTick**: o stub usa o contador com `TICKINT` desligado e não instala
  handler; o stock reprograma o seu próprio tick no `__main`.

## 9. Fase 2 — stub mínimo

`tools/stockpatch/stub/` compila com `tools/stockpatch/build_stub.sh`
(imagem `uvk1-uvk5v3`, `-nostdlib -nostartfiles`, linker script próprio):

| Seção | Endereço | Tamanho |
|---|---|---|
| `.text` | `0x08014400` | 516 B |
| `.bss` | `0x20000000` | 4 B |
| `.data` | — | 0 (asserção do linker) |

Fluxo: `stub_entry` → `SystemInit` original (`0x080111E5`) → `stub_boot` →
retorno. `stub_boot` configura só GPIOB (linhas PB15:12 e PTT PB10 com
pull-up, colunas PB6:3 em nível alto), arma o SysTick polled e faz **uma**
amostra do teclado. Só se MENU estiver pressionado ele espera 20 ms e repete a
amostra (sem PTT), como `BOOT_GetMode()` — no boot comum o custo é o de uma
varredura de teclado (~400 µs), não 40 ms.

`patch_stock.py` produz `build/stockpatch/f4hwn.stock.v7.00.15.bin` (imagem
crua, 73.220 B) e verifica:

- SHA-256, CRC16, tamanho da imagem, SP e reset vector do stock;
- o literal `0x080028E8` contém `0x080111E5` antes do patch;
- a entrada do ELF do stub é `0x08014401` e o stub cabe nos 48.128 B;
- o diff contra o original toca **só** os bytes `0xE8–0xE9` do arquivo (parte
  da palavra do literal) e a cauda é `0xFF…` + stub;
- a imagem final cabe no limite de slot (`0x1D800`).

## 10. Fase 3 — seletor

`stub_sel.c` mostra o seletor com o mesmo layout do KLEANFW:

- página 0: banner `STOCK MULTIBOOT` (cápsula inversa, fonte 3x5);
- página 1: linha pontilhada;
- páginas 2–6: slots 0–4, índice `M`/`1`–`4` na fonte 6x7, nome (ou o erro do
  header) e versão em 3x5 à direita; o slot selecionado ganha a cápsula larga;
- página 7: hints `MENU SELECT` / `EXIT QUIT` e, depois, a barra de progresso
  do restore.

Teclas: UP/DOWN navega (circular), MENU pede confirmação
(`Restore Main?`/`Restore slot N?`, `MENU CONFIRM` / `EXIT BACK`), MENU de novo
chama `MB_SetActiveSlot` + `MB_RestoreSlot` (o reset está dentro do copiador) e
EXIT devolve o controle ao stock. O cursor abre no slot do marcador.

O LCD é próprio (`stub_ui.c`): SPI1, sequência de init idêntica a
`ST7565_Init()` com contraste 31 e sem inversão, framebuffer de 8 páginas e as
duas fontes do firmware (`gFontSmallPacked`, `gFont3x5`, de `App/font.c`), cujos
demais arrays o `--gc-sections` descarta.

O relógio passou a ser **lido** em vez de suposto: `stub_delay_init()` decodifica
`RCC->CFGR`/`ICSCR`/`CR`. No ponto do hook a CPU está no relógio de reset
(HSI 8 MHz), então os 120 ms do LCD e os 10 µs do teclado são reais; se algum
dia o stock configurar o relógio antes, o código cai em 48 MHz e os atrasos só
ficam mais longos.

O copiador RAM do restore vai para `0x20000000` (4 KiB) e passa pelo mesmo
`cmake/check_mb_ramfunc.cmake` do firmware: 920 B, 85 branches, nenhuma
referência externa.

| Seção | Endereço | Tamanho |
|---|---|---|
| `.text` | `0x08014400` | 7.032 B |
| `.mb_ramfunc` | `0x20000000` (LMA `0x08015F78`) | 920 B |
| `.bss` | `0x20001000` | 1.032 B |

Imagem final: 80.656 B (limite do slot 120.832 B).

A tela de restore segue o KLEANFW: título na linha 1, `DO NOT POWER OFF` na 3,
`Writing / Verify` na 5 e a barra vazia (contorno + hachura) na linha 6. O
contorno tem de ser desenhado no mesmo buffer que o copiador usa como barra
(`stub_fb[7]`), senão a barra é escrita por cima do texto daquela linha.

## 11. Fase 4 — banco de configuração próprio

Oito hooks de 4 bytes, um por emissão de byte de endereço. Cada site tem
exatamente as duas instruções auditadas e é trocado por um `bl` para um dos
dois trampolins do stub (`stub_hooks.S`), que recalculam o mesmo byte a partir
de `endereço + bank_base`:

| Rotina | Byte 2 (bits 16–23) | Byte 1 (bits 8–15) |
|---|---|---|
| `SectorErase` `0x0800F878` | `0x0800F89C` | `0x0800F8A4` |
| `SectorErase`+cache `0x0800F8D0` | `0x0800F8F4` | `0x0800F8FC` |
| `Read` `0x0800F958` | `0x0800F97A` | `0x0800F982` |
| `PageProgram` `0x0800FBB0` | `0x0800FBD6` | `0x0800FBDE` |

Regra: `addr < 0x10000` recebe o offset do banco; `>= 0x10000` (calibração,
logo, marcador) fica intacto. `r4` nunca é alterado, então a comparação de tag
do cache de setor continua lógica e o cache permanece coerente. O byte 0 do
endereço não muda (os bancos são alinhados a 64 KiB), então não tem hook.

ARMv6-M não tem formas de 32 bits para `ldr`/`adds` com registradores altos:
os trampolins usam `r1` salvo com `push`/`pop`, e o valor de `bank_base` vem de
`0x20003FFC` (palavra no topo da RAM, acima do SP inicial `0x20002FA0`).

`stub_bank_apply()` só aceita o banco do marcador quando o marcador é válido
**e** `MB_InternalMatchesState()` confirma que a imagem interna é a daquele
slot; caso contrário grava 0. Com `bank_base = 0` os trampolins são
transparentes (`addr + 0`), então um stock gravado por USB continua exatamente
como o original, usando o banco 0.

Verificação no rádio: depois de rodar o stock a partir do slot 2, um dump da
SPI deve mostrar as gravações dele em `0xD0000` (banco 2) e
`0x000000–0x010000` (banco 0, Main do KLEANFW) intacto.

## 12. O que fica para a próxima fase

1. `README.md` de uso, `MANUAL.md` §20 e `CHANGELOG.md` (fase 5).
2. Teste no rádio desta fase: dump de `0x000000–0x010000` antes e depois de
   rodar o stock no slot 2, e confirmar as gravações em `0xD0000`.
3. A calibração compartilhada em `0x10000` segue como risco aberto: se bateria
   e S-meter saírem errados no stock, as leituras ≥ `0x10000` passam a ter um
   remap próprio.

## 13. Stock congelando na tela de VFO (slot, banco N)

Sintoma relatado: restaurado de um slot de 1 a 4, o stock mostra a tensão da
bateria e congela na tela de VFO, sem luz e sem teclas. O mesmo arquivo, sem
patch e gravado por USB, funciona no mesmo rádio.

### Confirmado no rádio

- A mesma imagem, com só o estado de hardware restaurado, travou igual. Isso
  descarta o estado deixado pelo stub como causa única.
- Com o banco N apagado pelo UV Studio, a mesma imagem copiou o banco 0 e
  **subiu normalmente**. A causa é o conteúdo do banco: config de outro firmware
  que estava ali (o `0xA000` não estava apagado, então a versão anterior do seed
  nem rodava).

### Provado na análise estática

- O boot rodou no banco N: veja a tabela de regiões no §3.
- O stock **não inicializa uma config apagada**. O factory reset (`0x080047A4`)
  só é chamado pelo menu (`0x0800A8FC`, um `case` da tabela de itens). O loader
  `0x080098B0` limita os campos que lê de `0x4000`, `0x5000` e `0x6000`.
- O próprio stock **nunca grava `0xA000` nem `0xD000`**. Todos os
  `WriteBuffer` com endereço fixo ou calculado foram decodificados:
  - canais em `ch*16`;
  - VFO em `ch*32 − 0x8F0` (`0x1010+`);
  - nomes em `0xE000 + ch*16`;
  - atributos em `0x2000 + ch`;
  - settings em `0x3000–0x9000`, `0x7020 + n*16` e `0xB000`.

  Só o caminho de programação pelo host (CPS) recebe endereço livre. O stock
  também não lê `0xC000` nem `0xD000`.
- O factory reset percorre `0x1000–0xF000` e pula `0x8000`, `0xA000–0xDFFF` e
  `0xF000` (exclusões em `0x080047D6–0x0800482E`).
- Nos bancos 1–4, o KLEANFW usa `0x0000–0xA170` e `0xB000–0xCFFF` (os setores
  privados do GOGUFW remapeados em `py25q16.c`). Logo, `0xD000` está livre nos
  dois layouts.
- O SPI2 deixado ligado pelo stub não muda a configuração. Com SPE=1, o
  `LL_SPI_Init` do stock (`0x08010A44`) retorna sem escrever, mas o struct que
  ele monta (`0x0800FD90–0x0800FDAE`) é igual ao que o stub programou.

### Não provado

Qual campo da config alheia leva ao travamento. Os consumidores dessas
leituras não foram seguidos; a correção não depende disso.

### Correção

1. `stub_hw.c`: na entrada de `stub_boot()`, um snapshot de:
   - `IOPENR` e `APBENR1/2`;
   - GPIOA/B/F: MODER, OTYPER, OSPEEDR, PUPDR, ODR e AFR;
   - SPI1 e SPI2: CR1 e CR2;
   - SysTick.

   Na saída, SPI1 e SPI2 passam por reset no RCC (limpa SR e FIFO). Depois
   voltam os GPIO (ODR antes do MODER), o SysTick e os clocks. Isso não era a
   causa, mas o stock passa a começar do mesmo estado que teria sem o stub.
2. `stub_bank_seed()` (`stub_mb.c`), só com banco ≠ 0:
   - sem a marca `KLEANFW STOCK B3` em `banco + 0xD000`, e com o `0xA000` do
     banco 0 gravado, o banco vira uma cópia do banco 0 (`0x0000–0xFFFF`, exceto
     `0xD000`), e a marca é a última escrita;
   - o setor `0x4000` fica apagado em vez de copiado. No stock ele é o bloco de
     settings: o loader `0x080098B0` lê 16 B e limita todos os campos, e 0xFF
     dá o default. No KLEANFW ele guarda os nomes dos canais em registros de
     16 B preenchidos com zero (`SETTINGS_SaveChannelName`). Copiado, o byte
     `0x400D` (tempo de luz, `[0x200025C0+29]`) lia 0, e `0x08008D70` só liga
     PF8 com tempo ≠ 0: a luz da tela nunca acendia (relato no rádio com a
     marca `B1`). A marca passou a `B2`, então um banco semeado pela versão
     anterior é refeito uma vez; os setores iguais ao Main são pulados;
   - setor de destino igual à origem é pulado; diferente e não apagado, é
     apagado antes. Num banco vazio são 0 erases;
   - em erro de SPI, para antes da marca, e o próximo boot retoma.

   Teste no host com flash emulada (primitivas reais do stub, SPI simulado):

   | Cenário | Resultado |
   |---|---|
   | banco vazio | cópia idêntica, 0 erases |
   | segundo boot | nada gravado |
   | dados que o stock gravou depois | preservados |
   | banco alheio | 14 erases, uma vez, cópia idêntica e `0x4000` apagado |
   | banco marcado `B1` igual ao Main | 2 erases (`0x4000` e `0xD000`) e 1 program |
   | settings que o stock grava em `0x4000` depois | preservados |
   | banco 0 apagado | nada |
   | corte de energia em cada uma das 87 operações, a partir de um banco alheio | nenhuma marca com cópia incompleta; o boot seguinte completa; um terceiro boot não grava nada |

### Voz

Confirmado no rádio: o stock no slot não fala, e o firmware de fábrica falava.
O pacote de voz de fábrica foi sobrescrito pelo multiboot (setores do marcador
em `0x100000`/`0x101000`) e não há dump dele. Sem o pacote, não há correção
possível dentro da imagem.

O aviso de voz (`0x08008F64`, chamado no boot em `0x08012030`) roda quando o
byte `0x7010` do banco é ≠ 0. Num banco apagado, o clamp põe 2 (inglês). Ele lê
um índice de 8 B em `0x100000+8n` (chinês) ou `0x100800+8n` (inglês) e os dados
em `0x101000+offset`, ou seja, nos marcadores A/B e nos overlays do KLEANFW.
Hoje o índice é rejeitado pelos limites do próprio stock (offset ≤ `0xB0000`,
len ≤ `0x19000`), porque o registro do marcador fica no início do setor e
`0x100800` está apagado. Essa faixa continua sem remap.

O `App/audio.c` do KLEANFW (herdado do F4HWN, com `ENABLE_VOICE` desligado no
Fusion) usa o mesmo formato numa base deslocada de `+0x4C000`: índices em
`0x14C000`/`0x14C800` e dados em `0x14D000+`. Com um pacote em mãos, um remap
linear das leituras `≥ 0x100000` do stock para uma área livre resolveria. O
pacote pode chegar a ~0xC9000 B (offset ≤ `0xB0000` + len ≤ `0x19000`), mais
que os 496 KiB da reserva livre `0x162000–0x1DE000`; isso teria de ser medido no
pacote real.

## 14. Defaults de menu no slot vs. stock singleboot

### Referência: o que é default no stock

- O RESET do menu (`0x080047A4`) chama `0x0800A638`, que grava em RAM o valor
  padrão de cada campo. Campo a campo, o fallback do loader `0x080098B0` para
  byte inválido ou `0xFF` é **o mesmo valor** do RESET. Exemplos:

  | Campo | Default |
  |---|---|
  | `0x4000`: SQL | 4 |
  | TOT | 2 |
  | VOX | 0, nível 5 |
  | MIC | 2 |
  | BAT-SAVE | 4 |
  | luz | 5 |
  | `0x5000`: canais de tela | 205/0/205/205/0/205/207/207 → RAM 205,205,205,205,0,0,207,207 |

  Os campos de `0x7000` também batem. Um setor de settings apagado dá,
  portanto, os defaults do stock.
- O RESET apaga `0x1000–0x7000`, `0x9000` e `0xE000`. Depois grava:
  - VFOs 200–206: 55,025 / 118,025 / 145,025 / 245,025 / 360,025 / 435,025 /
    490,025 MHz (`0x080132E4`);
  - 5 canais `CH-001…005`: 434,775 / 435,025 / 436,975 / 145,025 / 145,525
    (`0x08012DD4`);
  - os canais de tela.

  Ele preserva `0x0000`, `0x8000`, `0xA000–0xDFFF` e `0xF000`.
- O layout que o KLEANFW herdou do stock K1 está documentado em
  `App/driver/eeprom_compat.c`: settings em `0xA000+` ("ex 0x004000", "ex
  0x005000"…), nomes em `0x4000` (no stock, `0xE000`) e atributos em `0x8000`
  (no stock, `0x2000`).

### Setor por setor, com a cópia do Main

| Setor | No stock | No Main do KLEANFW | Na imagem |
|---|---|---|---|
| `0x0000` | canais 0–199 | freq. dos canais, mesmo formato | copiado. Fica invisível porque os atributos em `0x2000` estão em 0xFF |
| `0x1000+` | VFOs 200–206 | apagado | fallback do stock. Só o RESET grava a tabela |
| `0x2000`, `0x3000`, `0x5000–0x7000`, `0xB000`, `0xE000`, `0xF000` | atributos, FM, tela, voz, PONMSG, menu oculto, nomes | apagados | defaults do stock |
| `0x4000` | SQL, TOT, VOX, MIC, luz… | nomes dos canais com zeros (luz = 0) | **apagado** |
| `0x8000` | ANI-ID, UPCODE, DWCODE + 2 códigos DTMF (validados por `0x08004DF0`: `0-9`, `A-D`, `*`, `#`) | atributos (2 B/canal) | **apagado**: `123`, `ABCD9`, `9DCBA`, `12345`, `54321` |
| `0x9000` | scan list; canais prioritários em `[2,3,5,6]` **sem clamp** | VFOs | **apagado** (0xFF = nenhum, como no RESET) |
| `0xA000` | chave AES (16 B → `0x20002858`; uma palavra ≠ FFFFFFFF liga `[0x2000011C]`) | settings | **apagado** (sem chave, como na fábrica) |

### Fora do alcance da imagem

- **Voz:** o clamp dá 2 (ENG). O RESET preserva o valor anterior.
- **VFOs e canais de exemplo:** ficam no fallback do stock até um **RESET → ALL**
  no menu, que grava as tabelas acima.

Não verificado: que a flag de chave AES faça a CPS do stock exigir
autenticação. Isso é o comportamento da linhagem K5, mas o caminho de `uart`
do v7.00.15 não foi seguido.

## 15. UV-K1 (stock 7.03.05)

O arquivo `f4hwn.k1.stock_singleboot_v7.00.15.bin` é outro build. Até o §14,
a análise valeu para o do K5 V3, que também foi o arquivo testado no rádio do
usuário, um K1.

| Item | K5 V3 | K1 |
|---|---|---|
| SHA-256 | `91845f1d…608d2e` | `26d9e46b…756b6b` |
| Arquivo / imagem | 71.790 / 71.772 B | 71.350 / 71.332 B |
| Bloco de versão | `7.00.15` | `7.03.05` |
| Fim da imagem | `0x0801405C` | `0x08013EA4` |
| SP inicial = fim do ZI | `0x20002FA0` | `0x200031A0` |
| Tabela de regiões do `__main` | `0x08014008` | `0x08013E50` |
| `SystemInit` (literal em `0x080028E8`) | `0x080111E5` | `0x08011009` |
| `SectorErase` | `0x0800F878` | `0x0800F69C` |
| `SectorErase` + cache | `0x0800F8D0` | `0x0800F6F4` |
| `Read` | `0x0800F958` | `0x0800F77C` |
| `PageProgram` | `0x0800FBB0` | `0x0800F9D4` |

Provado na análise estática:

- **O hook de boot é o mesmo.** O `Reset_Handler` (`0x080028D4`: `ldr`/`blx`)
  e o literal ficam no mesmo endereço, e o `SystemInit` do K1 tem o mesmo corpo
  (grava `VTOR = 0x08002800`). O literal tem uma só ocorrência.
- **O driver SPI é o mesmo código, 0x1DC abaixo.** Os 8 sites têm os bytes
  auditados (`20 02 00 0e` / `20 04 00 0e`), com o endereço em `r4` e `lr`
  empilhado no prólogo.
- **O mapa de flash é idêntico.** As 42 chamadas de leitura e as 14 de gravação
  do driver resolvem para os mesmos endereços e tamanhos nos dois builds
  (`0x2000/207`, `0x3000/40`, `0x5000/8`, `0x6000/8`, `0x7000/144`,
  `0x8000/56`, `0x9000/8`, `0xA000/16`, `0xB000/8`, calibração `0x100C0…`,
  logo `0x11000…`). Os literais de endereço da SPI ≥ `0x10000` também são os
  mesmos, inclusive os da voz (`0x100000`/`0x100800`/`0x101000`). Por isso o
  banco, o seed, os setores apagados, a marca em `0xD000` e a limitação da voz
  (§13–14) valem sem mudança para o K1.
- O ZI do K1 termina em `0x200031A0`, abaixo de `BANK_WORD` (`0x20003FFC`), e
  nada referencia essa palavra. O stub em `0x08014400` cabe depois do fim da
  imagem.
- Nenhum dos dois builds grava a flash interna.

Implementação:

- `patch_stock.py` tem uma tabela `VARIANTS` indexada pelo SHA-256, com os sites
  gerados por `hook_sites(-0x1DC)` para o K1.
- Uma verificação nova lê a tabela de regiões e confere que o fim do ZI é igual
  ao SP e fica abaixo de `BANK_WORD`.
- O stub deixou de ter o `SystemInit` fixo: `stub_entry` chama pela palavra
  `stub_stock_system_init` (placeholder `0xFFFFFFFF`), que o patcher preenche
  com o valor do literal que substitui e depois confere.
- Com isso, a imagem do K5 V3 ficou byte a byte igual à anterior.
