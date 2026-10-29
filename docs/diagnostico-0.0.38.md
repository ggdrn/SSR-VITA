# SRR Vita 0.0.38 — picos durante a corrida

O usuário relata melhora geral na 0.0.37, mas ainda percebe picos nas fases. A análise usa o novo `build/VPK/srr_log.txt`, versão 0.0.37, SHA256 `248b61a458b3087cbcc18e7a95ff30ecf1605f88c65c5375a3a3a28c336c8e30`. Uma cópia foi preservada em `build/perf-baselines/srr-0.0.37.log` para comparação futura.

## O que o log permite concluir

| Frame de término do intervalo | Média de 600 renders | Pico | Coletas | Frames >50 ms com / sem coleta |
|---|---:|---:|---:|---:|
| 3000 | 35,56 ms | 427,69 ms | 2 | 2 / 19 |
| 3600 | 35,96 ms | 428,76 ms | 3 | 3 / 9 |
| 4200 | 43,16 ms | 1852,34 ms | 7 | 6 / 21 |
| 4800 | 36,82 ms | 426,41 ms | 5 | 5 / 8 |
| 5400 | 35,79 ms | 420,61 ms | 3 | 3 / 6 |
| 6000 | 47,12 ms | 712,78 ms | 14 | 14 / 25 |

Há pausas recorrentes perto de 420–430 ms e uma associação com GC. Os agregados não permitem atribuir precisamente cada pico individual ao coletor. O intervalo com o pico de 1,85 s contém também uma coleta completa de diagnóstico da cena. O número de programas GL permanece em 154 até o frame 4800; a única nova ligação posterior acrescenta cerca de 7 ms. Não há ligações acima de 100 ms nesta execução. Portanto, compilação de shader não explica os grandes picos nesses intervalos.

O log mostra o canal SE em 2D, com status de reprodução ativo para efeitos da fase. O usuário não pediu novos ajustes de áudio nesta etapa; a compatibilidade da 0.0.37 foi mantida.

## Ação aplicada

A coleta pesada de cena não se repete durante uma mesma `s_playingstage`. Ela executava enumeração de GameObjects/skins e dezenas de chamadas gerenciadas e escritas no log durante a corrida. Um snapshot na entrada da cena continua disponível. As verificações leves de nome de cena e a compatibilidade de áudio permanecem. Isso retira uma fonte evitável de trabalho periódico; não comprova que ela causou sozinha o pico de 1,85 s.

O tempo medido passou a abranger também entrada, compatibilidade de áudio e diagnóstico, antes da saída do próprio medidor. O log distingue tempo dentro e fora de nativeRender. Antes, uma pausa fora de render podia aparecer ao jogador sem entrar na estatística de render.

## Medição do GC

A versão mede início/fim da coleta e as etapas stop-world, mark, reclaim e restart-world. A numeração das etapas segue a [interface do Boehm](https://github.com/ivmai/bdwgc/blob/master/include/gc/gc.h) e os eventos inicial/final foram confrontados com o handler do libmono original.

O callback original `GC_notify_event` é preservado e chamado uma vez por evento. O coletor só escreve números em um anel prealocado com publicação atômica; não há alocações, logging, chamadas gerenciadas ou aquisição de mutex no callback. O leitor é limitado e não espera o escritor. Essas restrições evitam bloquear uma thread que o próprio GC precisa suspender. Até 16 coletas entre frames são recuperadas; amostras mais antigas podem ser descartadas se esse limite for ultrapassado.

O log agrega o pior GC a cada 600 frames e amostra picos individuais acima de 100 ms. Isso permitirá decidir se os 420 ms são espera por threads, marcação/liberação ou retomada. O coletor não foi desativado, nem sua frequência alterada sem essa evidência. Os tempos exatos ainda precisam ser coletados no Vita.

## Validação e teste

Nove testes locais passaram: fila de áudio, cadeia do notifier Mono, fases/ring do GC com concorrência, shader/uniforms, logger, agendamento dos snapshots, espera de sinais, retomada aninhada e canal SE. Os testes usam AddressSanitizer/UndefinedBehaviorSanitizer onde aplicável. Build ARM/VitaSDK concluído; VPK íntegro e eboot empacotado idêntico ao compilado.

Pacote: `build/VPK/SRR-Vita-v0.0.38.vpk`. Instalar e jogar pelo menos dois minutos na mesma fase, incluindo checkpoint e chefe se possível. Recolher `ux0:data/srr/srr_log.txt`. Procurar `perf: installed allocation-free GC phase timings`, `perf: spike` e `perf: full frame max`. Ainda não houve validação no console desta versão. Saves e caches não foram alterados.
