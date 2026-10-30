# SRR Vita 0.0.55 — transições, menus e leitura de arquivos

Log analisado: `srr_log.txt` da 0.0.54 (~1100 s, duas corridas, muita navegação em menus), preservado em `build/perf-baselines/srr-0.0.54.log`; relatório em `build/analysis-0.0.54/`.

## Resultado da 0.0.54

- Patch aplicado (`ui: UIDrawCall.Set passes NGUI buffers ...`, `ui: NGUI mesh uploads use the shared buffers ...`); ~224 mil envios de malha usaram os buffers.
- `Vector3[]/Vector2[]/Color32[]` saíram do topo das alocações. Corrida: ~1,5 MB por janela de 10 s (antes ~12 MB). Na primeira corrida houve **uma** coleta automática em ~110 s.

## Onde a navegação perde tempo

### 1. Coletas explícitas redundantes nas transições
O jogo chama `GC.Collect(); Resources.UnloadUnusedAssets(); GC.Collect();` em `GameModeStage.StateInit/StateFadeIn/StatePause/StateQuickModeTimeUp/StateEnd`, `RegisterAllResource`, `GameModeTitle.StateLoadLevel`, `MainMenu.PrepareForSceneMove`, `FirstLaunchUserSetting`, além de `GC.Collect()` sozinho em `MainMenu.Start` e no início/fim do giro da roleta. A operação de `UnloadUnusedAssets` já faz uma coleta completa. Entrada na corrida (895–925 s): 3 coletas (1,8 s) na saída do menu + 5 coletas (~3 s) no início da fase. Nos menus cada coleta custa 1,4–1,9 s.

### 2. `se_runners.acb` lido a 3 MB/s
Em toda troca de cena o banco de efeitos do CRI (4,1 MB) é lido em 4 blocos de 1 MiB, ~350 ms cada (1,4 s). Os bundles (`.sharedAssets`) são lidos a ~12,5 MB/s porque já tinham leitura direta; os demais arquivos abertos com `fopen` usavam o buffer padrão do newlib (1 KiB), ou seja ~1000 leituras no cartão por MiB. O streaming da música (`BGM_menu_v2_streamfiles.awb`) ficava abaixo de 1 MB/s.

### 3. Leitura dos bundles de janela e do `MainMenuPages`
Cada janela lê 13–17 MB (atlas de textura RGBA) e o `MainMenuPages` 53 MB, a ~12,5 MB/s — limite do cartão com leitura direta. Isso é ~4,3 s em toda volta ao menu e ~1,1–1,3 s por janela. A carga é síncrona (`sync=1`): a thread principal espera a leitura e envia cada textura assim que ela chega.

### 4. Conjunto vivo crescendo nos menus
O heap em uso após coletas sobe de ~34 MB para ~83 MB ao navegar (fases 28–62) e só cai ao entrar na corrida. A marcação do GC passou para 1,4–1,8 s. Cada janela desserializa fontes NGUI inteiras (`BMGlyph` + `List<int>` de kerning: 6–17 MB por carga, ~48 bytes por glifo, ~7900 glifos por fonte), o que indica cópias da mesma fonte em cada bundle.

## Mudanças

1. **IL:** 18 chamadas `GC.Collect()` em 12 métodos viram NOP (chamada de 5 bytes, sem efeito de pilha; tamanho do método e bytes conferidos). Todas as chamadas `UnloadUnusedAssets` continuam, assim como as coletas de tela de carregamento (`LoadSetTableFirst`, `CreateTerrain`, downloads) e o GC automático. Log: `stage: removed N redundant GC.Collect calls in M methods`.
2. **Leitura:** arquivos somente-leitura com 256 KiB ou mais abertos por `fopen` (exceto `.sharedAssets`, que continuam diretos) recebem buffer de 64 KiB. Log: `io: 64 KiB read buffer streams ...`.

Esperado: ~1,2 s a menos por troca de cena (ACB), ~2–3 s a menos na entrada da corrida, ~1,2–3,6 s a menos ao voltar ao menu e na roleta, música do menu sem engasgos de leitura. A leitura do `MainMenuPages` e das janelas continua no limite do cartão.

## Próximo alvo

Fontes duplicadas nos bundles de janela (desserialização + memória viva). Precisa de um censo (`UIFont`/`BMFont` vivos por nome) antes de deduplicar.

## Entrega

Compilação ARM/VitaSDK sem avisos novos. Sem teste no aparelho.

Pacote: `build/VPK/SRR-Vita-v0.0.55.vpk`.
SHA-256: `7a256bfd3a576d24b5e31110d5c8acfa46c7b2376f8e2bdaa7d9d6f3a3890535`.
