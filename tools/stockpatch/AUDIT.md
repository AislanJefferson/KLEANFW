# Auditoria do stock v7.00.15 + stub de boot

Análise estática de `K5ENG_v7.00.15_publish.bin`, feita com
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
