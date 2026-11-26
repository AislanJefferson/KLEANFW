# Stock no slot — `tools/stockpatch`

Gera, a partir do firmware stock da Quansheng do **UV-K1** (7.03.05) ou do
**UV-K5 V3** (7.00.15), uma imagem que roda num slot do multiboot com:

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

Antes de devolver o controle, o stub ainda:

- **toma posse do banco**: se o banco N ainda não tem a marca do stock
  (`KLEANFW STOCK B3` em `banco + 0xD000`), o stub copia o banco 0 (config do
  Main) por cima dele e grava a marca por último. Os setores `0x4000`
  (settings), `0x8000` (códigos DTMF), `0x9000` (scan list) e `0xA000` (chave
  AES) ficam apagados em vez de copiados: no KLEANFW eles guardam outra coisa
  (nomes, atributos, VFOs, settings), e apagados dão exatamente os defaults do
  stock. Veja o `AUDIT.md` §14. O stock trava na tela de VFO
  com a config de outro firmware (confirmado no rádio), não inicializa sozinho
  uma config apagada (o factory reset dele só sai pelo menu), e o banco 0 é o
  único conteúdo com que ele comprovadamente sobe. A config antiga daquele banco
  se perde. A cópia leva alguns segundos, uma única vez, com a tela ainda
  apagada: **não desligue**. Num banco vazio ela não apaga nenhum setor; num
  banco com dados, apaga só os setores que diferem do Main. Uma queda de energia
  no meio refaz a cópia no boot seguinte, pulando o que já foi copiado;
- **devolve o hardware como encontrou**: GPIOA/B/F (ODR incluso), SPI1/SPI2
  (reset pelo RCC), SysTick e os enables de clock voltam ao estado de entrada,
  então o stock começa do mesmo estado que teria sem o stub.

## Uso

Requisitos: Docker com a imagem `uvk1-uvk5v3` (a mesma do firmware) e
`python3`. O firmware de entrada é fixado por SHA-256, um por modelo:

| Modelo | `-m` | Arquivo de entrada (raiz do repo) | Versão no bloco | Imagem gerada |
|---|---|---|---|---|
| UV-K1 | `k1` | `K1ENG_v7.03.05_publish.bin` | **7.03.05** | `build/stockpatch/f4hwn.k1.stock.v7.03.05.bin` |
| UV-K5 V3 | `k5v3` (ou `k5`) | `K5ENG_v7.00.15_publish.bin` | 7.00.15 | `build/stockpatch/f4hwn.k5v3.stock.v7.00.15.bin` |

Use a imagem do modelo do rádio: o script recusa um arquivo do outro modelo e
diz qual é o certo.

```sh
# UV-K1
tools/stockpatch/make_multiboot.sh K1ENG_v7.03.05_publish.bin -m k1

# UV-K5 V3, com saída escolhida
tools/stockpatch/make_multiboot.sh K5ENG_v7.00.15_publish.bin -m k5v3 -o stock-k5v3.bin
```

O script compila o stub, confere o arquivo contra o modelo, gera a imagem e
mostra o resumo. Depois:

1. envie a imagem para um slot de 1 a 4 (UV Studio);
2. selecione o slot no MENU ao ligar.

Na primeira subida, o banco do slot é preparado em alguns segundos: não
desligue o rádio.

Comandos de baixo nível (o `make_multiboot.sh` usa os dois primeiros):

```sh
tools/stockpatch/build_stub.sh                                   # stub.bin / stub.elf
python3 -I tools/stockpatch/patch_stock.py <firmware> -m k1      # imagem de slot
python3 -I tools/stockpatch/patch_stock.py <firmware> --rebuild  # ida e volta byte a byte
python3 -I tools/stockpatch/patch_stock.py <firmware> --dump-raw <saida>  # stock sem patch, como imagem de slot
python3 -I tools/stockpatch/patch_stock.py <firmware> --no-hooks # diagnóstico: stub sem remap de banco
tools/stockpatch/disasm.sh <firmware> -m k1                      # desmonta o stock decifrado
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

- o stub fica em 9.328 B (`.text` 8.408 + copiador RAM 920) e a imagem em
  82.032 B, dentro do limite de 120.832 B por slot;
- `cmake/check_mb_ramfunc.cmake` passa no copiador RAM (nenhuma referência
  externa);
- o diff contra o stock original toca só os 2 bytes do literal de boot, os
  8×4 bytes dos hooks e a cauda (`0xFF…` + stub);
- `--rebuild` reproduz o arquivo original byte a byte.

No rádio, o teste que fecha a fase:

1. Suba o stock a partir do slot 2. No primeiro boot há uma pausa de alguns
   segundos com a tela apagada (a cópia do banco 0); nos seguintes, não.
2. Confirme que a luz acende e as teclas respondem; mexa em alguma
   configuração, desligue e ligue: ela persiste.
3. Dump antes/depois: as gravações devem aparecer a partir de `0xD0000`
   (banco 2) e `0x000000–0x010000` (Main do KLEANFW) deve ficar idêntico.
4. Ligue segurando MENU no stock: o seletor abre, e escolher **M** volta ao
   KLEANFW.
5. Se o stock ainda travar, grave `…orig-slot.bin` no mesmo slot: se ele também
   travar, o problema está no caminho de restore do slot, não nesta imagem.

## Limitações conhecidas

- **Calibração compartilhada**: o stock lê a calibração do KLEANFW em
  `0x10000`. Se bateria ou S-meter saírem errados, é o primeiro suspeito.
- **Só os dois arquivos da tabela de Uso**: todos os endereços e bytes
  esperados foram verificados nesses arquivos exatos (o K1 é o mesmo código do
  K5 V3 nos pontos do patch, deslocado; veja o `AUDIT.md` §15). O patcher aborta
  em qualquer outro.
- **Relógio**: o stub roda no relógio de reset (HSI 8 MHz) e lê o divisor do
  RCC para os próprios atrasos. Se o stock mudar a ordem de inicialização, os
  atrasos só ficam mais longos, nunca curtos.
- **Regravar o slot com outro firmware** não deixa o banco inconsistente: o
  marcador é reescrito no próprio restore.
- **Posse do banco**: qualquer banco sem a marca do stock é sobrescrito com a
  cópia do Main na primeira subida, inclusive um banco de outro slot escolhido
  pelo **SetCfg**. Só o banco 0 nunca é tocado. Se a CPS do stock gravar algo em
  `0xD000` do banco, a marca some e o próximo boot copia o Main de novo.
- **Sem voz falada**: o pacote de voz de fábrica fica na SPI em
  `0x100000–~0x1CA000` (índice CHI em `0x100000`, ENG em `0x100800`, áudio em
  `0x101000+`). No KLEANFW essa faixa é dos marcadores do multiboot
  (`0x100000`/`0x101000`, regravados a cada troca de slot) e dos apps overlay
  (`0x102000–0x162000`), então o pacote some na primeira troca de slot. O stock
  rejeita o índice pelo próprio teste de limites e fica mudo; não trava. Para
  ter voz seria preciso um dump do pacote (de um rádio com o firmware de
  fábrica), uma área livre que o comporte e um remap dessas leituras nos
  trampolins.
