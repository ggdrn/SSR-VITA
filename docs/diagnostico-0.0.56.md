# SRR Vita 0.0.56 — bundles de menu sem o mip 0 e tempo de JIT na corrida

Log analisado: `srr_log.txt` da 0.0.55 (~445 s, duas corridas, seleção de corrida duas vezes), preservado em `build/perf-baselines/srr-0.0.55.log`; relatório em `build/analysis-0.0.55/`.

## Resultado da 0.0.55

- `stage: removed 17 redundant GC.Collect calls in 12 methods` (todos os 12 métodos conferiram).
- `se_runners.acb`: 4 blocos de 1 MiB em ~90 ms cada (antes ~350 ms) — ~0,37 s por troca de cena em vez de ~1,4 s.
- Corrida longa (fase 32, ~90 s jogáveis): **uma** coleta automática (397 ms). Taxa estável em 30 fps (P50 34 ms; o modo de gráficos limitados trava em 30 fps).

## Primeira entrada na seleção de corrida (~10 s)

A seleção de corrida abre quatro bundles de janela: `PlayerSetWindowUI`, `ChaoSetWindowUI`, `DeckViewWindow` e `ItemSet_3_UI`. Cada um lê 25–33 MB do cartão a ~13 MB/s (≈3,2 / 1,8 / 1,8 / 3,3 s) e depois passa ~1,2 s desserializando antes de compilar os shaders. Ao voltar de uma corrida a cena descarrega tudo e a seleção paga os ~10 s de novo (fase 27). Depois disso, o resto dos menus fica rápido porque os bundles continuam carregados.

Inspeção dos bundles em cache (`data do jogo/files/UnityCache`, UnityPy + parser próprio):

- Cada bundle de janela carrega cópias próprias dos atlas compartilhados (`ui_mainmenu_Atlas`, `ui_general_window_Atlas`, `ui_cmn_btn_bg_Atlas`, `ui_roulette_2_Atlas`...) e da fonte `UCGothic_26_mix` (textura 2048² RGBA4444, ~11 MB, e o objeto `UIFont` de 1,78 MB, presente em 33 bundles).
- Todas essas texturas têm mipmaps. Com `masterTextureLimit = 1`, a Unity envia para a GPU só a partir do mip 1 (o log confirma: 2048² → `1024x1024`, 1024² → `512x512`, 512² → `256x256`). O mip 0 — 3/4 dos bytes — é lido do cartão e descartado.

Nos 294 `.sharedAssets` em cache: 860 MiB, dos quais 228 MiB são mip 0 descartável. `PlayerSetWindowUI`: 27,6 de 38,7 MiB; `MainMenuPages`: 35,6 de 53,5 MiB.

## Engasgos restantes na corrida

Na corrida longa, os quadros lentos fora da coleta (120–250 ms, ex.: quadros 4618, 4791, 4805, 5084) não têm GC, leitura de arquivo, link de shader nem `Instantiate` relevante (< 6 ms). O tempo é de CPU gerenciada; a hipótese principal é a compilação JIT de métodos na primeira chamada (inimigos, itens e eventos novos). O perfil de alocações também custava CPU em toda alocação e forçava o alocador genérico do Mono em todo código compilado depois dele.

## Mudanças

1. **Leitura dos bundles sem o mip 0** (`src/util/bundle_mips.c`). Ao abrir um `.sharedAssets` do UnityCache, o cabeçalho do arquivo serializado (formato 9, sem type tree) e o cabeçalho de cada `Texture2D` ≥ 64 KiB são lidos uma vez (resultado guardado por caminho/tamanho/data). Só entram texturas com mipmap, não legíveis (`isReadable = 0`), uma imagem 2D, formato sem compressão conhecido, e cujo tamanho declarado bate exatamente com a cadeia completa de mips. Quando uma leitura da Unity cobre o mip 0 de uma delas e o `masterTextureLimit` atual é ≥ 1, esse trecho não é lido do cartão (o restante da leitura é idêntico; o retorno é o mesmo). Durante `SetQualityLevel` o salto fica desligado. Teste no Mac com os 294 bundles reais, lendo em blocos de tamanhos variados: todos os bytes fora do mip 0 iguais à leitura completa.
   - Log: `io: <bundle> top mips N, X of Y KiB not read while masterTextureLimit >= 1` e `io: top-mip reads skipped N, M MiB not read from the card`.
2. **Perfil de alocações removido.** Alocações voltam ao caminho rápido do Mono.
3. **Tempo de JIT** (só o evento de compilação do profiler, sem mudar o código gerado). Em todo quadro lento: `perf: main jit X ms methods N longest Y ms Classe.Método`; a cada 600 quadros: `jit: window main N methods X ms longest ...`.

Esperado: ~70% menos bytes lidos nos bundles de menu — seleção de corrida de ~10 s para algo perto de 5 s (a desserialização da fonte, ~1,2 s por janela, continua), volta ao menu (`MainMenuPages`) de ~4,3 s para ~1,5 s. Precisa ser medido no aparelho; conferir também se nenhuma textura de menu aparece corrompida.

## Próximo alvo

- Objeto `UIFont` da UCGothic (1,78 MB, ~7900 glifos) desserializado de novo em cada bundle de janela (~1,2 s cada e memória viva).
- Engasgos da corrida, conforme o que as linhas `perf: main jit` mostrarem.

## Entrega

Compilação ARM/VitaSDK sem avisos novos (os dois avisos de `libc_bridge.c` são antigos). Parser e leitura com salto testados no Mac contra os 294 bundles em cache. Sem teste no aparelho.

Pacote: `build/VPK/SRR-Vita-v0.0.56.vpk`.
SHA-256: `fd87bf7bb8ba4db0c06f8bc261923a6a2aeb6f4e071590ec081cfd3960d2754b`.

Na próxima captura procurar: `io: ... top mips`, `io: top-mip reads skipped`, `jit: main-thread compile timing installed`, as linhas `perf: main jit` nos quadros lentos da corrida e o tempo da seleção de corrida (fases com `PlayerSetWindowUI`/`ItemSet_3_UI`).
