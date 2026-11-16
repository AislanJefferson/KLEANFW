# Stock no slot — `tools/stockpatch`

Gera, a partir do firmware stock da Quansheng (`K5ENG_v7.00.15_publish.bin`),
uma imagem que roda num slot do multiboot com:

- o **seletor do KLEANFW** (MENU ao ligar) embutido no próprio stock;
- **banco de configuração próprio**: o stock no slot N usa o banco N.

O KLEANFW não muda — 0 bytes no orçamento de 120.832 B. Todo o trabalho é
tooling; o firmware compilado continua exatamente o mesmo.

> [!WARNING]
> O stock é proprietário da Quansheng. Distribua **apenas este tooling** e o
> código do stub — nunca o binário gerado nem a imagem decifrada.

## Como funciona

O stub é um mini-firmware de ~8 KB anexado em `0x08014400`, depois do stock
(que termina em `0x0801405C`). O patcher muda **nove** coisas no stock:

| Onde | O que muda |
|---|---|
| `0x080028E8` | literal do `SystemInit` do `Reset_Handler` passa a apontar para o stub |
| 8 × 4 bytes | hooks nos quatro caminhos de SPI (read / page-program / 2 erases) |

Nenhuma instrução é reescrita. O `Reset_Handler` chama o stub pelo `blx` que já
existia; o stub chama o `SystemInit` original, faz seu trabalho e devolve o
controle, e o stock segue para o `__main` como sempre.

O stub contém:

- **seletor** com banner `STOCK MULTIBOOT`, cinco slots (`M`, `1`–`4`), nome e
  versão de cada um, confirmação e barra de progresso;
- a lógica de slot do próprio KLEANFW (`App/driver/mb_flash_logic.c`), então o
  formato no flash e o caminho de restore são idênticos byte a byte;
- **trampolins de remap**: cada byte de endereço emitido pelo driver SPI passa
  por um trampolim que soma o banco quando o endereço é `< 0x10000`
  (calibração, logo e marcador ficam compartilhados, como no KLEANFW).

O banco vem do marcador (`config_bank`) e só é aceito se a imagem interna
realmente for a daquele slot (`MB_InternalMatchesState`). Sem marcador válido —
stock gravado direto por USB — o banco é 0 e o stock se comporta exatamente
como o original.

## Uso

Requisitos: Docker com a imagem `uvk1-uvk5v3` (a mesma do firmware) e
`python3`. O firmware de entrada é fixado por SHA-256: só a v7.00.15 publicada
pela Quansheng é aceita.

```sh
# 1. compila o stub (stub.bin / stub.elf em build/stockpatch/)
tools/stockpatch/build_stub.sh

# 2. gera a imagem de slot
python3 -I tools/stockpatch/patch_stock.py
# -> build/stockpatch/f4hwn.stock.v7.00.15.bin

# 3. envia a imagem para um slot (UV Studio) e seleciona no MENU ao ligar
```

Outros comandos:

```sh
# confere o caminho de decifrar/reempacotar contra o arquivo original
python3 -I tools/stockpatch/patch_stock.py --rebuild

# desmonta o stock decifrado (build/stockpatch/stock.dis)
tools/stockpatch/disasm.sh
```

## Bancos

| Slot | Banco | Base na SPI |
|---|---|---|
| M (0) | 0 | `0x000000` (região histórica, config do Main do KLEANFW) |
| 1 | 1 | `0x0C0000` |
| 2 | 2 | `0x0D0000` |
| 3 | 3 | `0x0E0000` |
| 4 | 4 | `0x0F0000` |

O banco é o `config_bank` do marcador, então o **SetCfg** do KLEANFW também
vale para o stock. Calibração (`0x10000`), logo de boot (`0x11000`) e a área de
apps continuam compartilhados.

## Usando o seletor

Do stock, ligue segurando **MENU**:

- **UP/DOWN** escolhe o slot; **MENU** pede `Restore …?`; **MENU** de novo
  grava e reinicia; **EXIT** continua o boot no stock.
- Escolher **M** volta para o KLEANFW (o slot 0 é o backup do principal).
- O cursor já abre no slot de onde o rádio subiu.

## Antes de gravar

1. Faça um **dump completo da SPI** (config, calibração e logo) e guarde uma
   imagem **DFU** de recuperação.
2. Tenha o `kleanfw.bin` (ou o `.bin` de release) à mão.
3. Envie a imagem para um slot **que não seja o 0** e mantenha o slot 0 como
   backup do principal.

Se algo der errado, a saída é USB/DFU — o stock não tem seletor próprio, ele só
tem o que este stub injeta.

## Verificação

Estática, depois de `build_stub.sh` e `patch_stock.py`:

- o stub fica em 7.952 B (`.text` 7.032 + copiador RAM 920) e a imagem em
  80.656 B, dentro do limite de 120.832 B por slot;
- `cmake/check_mb_ramfunc.cmake` passa no copiador RAM (nenhuma referência
  externa);
- o diff contra o stock original toca só os 2 bytes do literal de boot, os
  8×4 bytes dos hooks e a cauda (`0xFF…` + stub);
- `--rebuild` reproduz o arquivo original byte a byte.

No rádio, o teste que fecha a fase:

1. Suba o stock a partir do slot 2 e mexa em alguma configuração.
2. Dump antes/depois: as gravações devem aparecer a partir de `0xD0000`
   (banco 2) e `0x000000–0x010000` (Main do KLEANFW) deve ficar idêntico.
3. Ligue segurando MENU no stock: o seletor abre, e escolher **M** volta ao
   KLEANFW.

## Limitações conhecidas

- **Calibração compartilhada**: o stock lê a calibração do KLEANFW em
  `0x10000`. Se bateria ou S-meter saírem errados, é o primeiro suspeito.
- **Só a v7.00.15**: todos os endereços e bytes esperados foram verificados
  nessa versão exata; o patcher aborta em qualquer outra.
- **Relógio**: o stub roda no relógio de reset (HSI 8 MHz) e lê o divisor do
  RCC para os próprios atrasos. Se o stock mudar a ordem de inicialização, os
  atrasos só ficam mais longos, nunca curtos.
- **Regravar o slot com outro firmware** não deixa o banco inconsistente: o
  marcador é reescrito no próprio restore.
