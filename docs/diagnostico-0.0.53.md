# SRR Vita 0.0.53 — trava de troca de nível e perfil de alocações

Log analisado: `srr_log.txt` da 0.0.52 (13.696 linhas, ~1230 s, cinco corridas), preservado em `build/perf-baselines/srr-0.0.52-full.log`; relatório em `build/analysis-0.0.52/`.

## Resultado do ritmo de GC (0.0.52)

O gancho foi instalado (`patch: libmono min_words_allocd -> paced budget`) e o orçamento ficou em 52–64 MiB nas corridas. As coletas automáticas na corrida passaram de uma a cada ~20 s (0.0.51) para uma a cada 32–47 s. Cada uma ainda leva ~400 ms (marcação ~355 ms). A memória nativa livre ficou em 62–103 MiB; não houve falta.

## Maior trava restante: troca de nível no meio da corrida

A cada ~70 s de corrida (ex.: 268, 339, 591 s) aparece uma trava de **~2,8 s**: 3 coletas seguidas (2 pedidas explicitamente) e `UnloadUnusedAssets`, chamado por código gerenciado. A varredura do IL do Assembly-CSharp localizou a origem em `GameModeStage.StateChangeLevel`, que executa `GC.Collect(); Resources.UnloadUnusedAssets(); GC.Collect();` em toda troca de nível. A mesma sequência existe em `HudCockpit.OnClickEndPause` (ao sair da pausa).

A memória nativa livre ficou praticamente igual antes/depois das trocas de nível (92,0 → 92,7 → 92,3 MiB): o descarregamento não libera nada relevante nesse ponto, só custa tempo.

**Mudança:** as três chamadas e o `pop` nesses dois métodos foram trocados por NOP (16 bytes cada). Conferências: tamanho do método, os 16 bytes exatos, instruções vizinhas (StateChangeLevel) ou o corpo inteiro (OnClickEndPause); análise offline confirmou que não há destino de desvio nem limite de bloco de exceção dentro do trecho (o único destino é o próprio início, 0xc6). A mesma limpeza continua no início/fim da fase (StateInit, StateFadeIn, StateEnd), ao entrar na pausa (StatePause) e nas trocas de cena; o GC automático continua normal.

## Perfil de alocações não registrava nada

Nenhuma linha `alloc: window` saiu na 0.0.52. O Mono testa uma flag estática (`profile_allocs`, libmono+0x3ae5ac) antes de chamar o profiler em todos os caminhos de alocação; ela é zerada no boot quando nenhum profiler pede alocações, e instalar depois não a religa. A 0.0.53 confere o literal que endereça a flag (0x1fe438 = 0x001b01a0 e as instruções que o usam) e a liga. Na próxima captura devem aparecer as classes que mais alocam — em especial na tela de resultado, onde houve coletas automáticas a cada ~4 s mesmo com orçamento de 64 MiB (~16 MB/s alocados) e na corrida (~1,5–2 MB/s).

## Entrega

Compilação ARM/VitaSDK sem avisos. Sem teste no aparelho.

Pacote: `build/VPK/SRR-Vita-v0.0.53.vpk`.
SHA-256: `42e269e235b114406ecf53ebd5d38e0006a44342b7da1be8952d83658afedf2a`.

Na próxima captura procurar: `stage: GameModeStage.StateChangeLevel no longer forces ...` (patch aplicado), ausência das travas de ~2,8 s, e as linhas `alloc:`.
