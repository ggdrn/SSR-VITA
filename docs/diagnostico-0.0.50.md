# SRR Vita 0.0.50 — atuação sobre o fluxo completo da 0.0.49

Baseline preservado em `build/perf-baselines/srr-0.0.49-full.log`. Foram analisadas as 37.472 linhas, com 30.468 eventos OBS válidos, 118 estados de cena/loading e aproximadamente 2.125,93 s de sessão. O relatório completo, tabela de assets e linha do tempo estão em `build/analysis-0.0.49/`. A pasta `../data do jogo` contém 1.386 arquivos, aproximadamente 954,4 MiB. Seus arquivos foram usados para conferir tamanhos e nomes completos, sem modificar o conteúdo fornecido.

## Fluxo observado

| Marco | Tempo relativo | Quadro |
|---|---:|---:|
| Título observado | 43,27 s | 16 |
| MainMenu observado | 100,43 s | 991 |
| Primeira entrada em s_playingstage | 433,85 s | 6556 |
| Retorno ao MainMenu | 545,69 s | 9196 |
| Segunda entrada em s_playingstage | 1116,95 s | 19216 |
| Retorno ao MainMenu | 1548,69 s | 35056 |
| Retorno ao título | 2040,09 s | 55411 |
| MainMenu novamente | 2080,29 s | 56206 |

Esses intervalos incluem interação/permanência do usuário. Pedidos e estados intermediários estão no relatório; não representam um único loading contínuo. A captura perdeu 1466 eventos e registrou 2047 ocorrências de saturação da tabela de agregação, portanto a cobertura não é absoluta.

## 1. Assets grandes e caminho de leitura

`BuildPlayer-MainMenuPages.sharedAssets` tem 56.136.144 bytes (53,5 MiB). Na fase de observação 6, seus registros somam exatamente esse volume em 33 chamadas, com 20,73 s acumulados. O mesmo arquivo reaparece em outros retornos aos menus. Isso indica uma passagem completa a cada carga, não necessariamente repetição de cada bloco dentro da mesma passagem.

Outros casos: `BuildPlayer-RouletteTopUI.sharedAssets` tem 45.253.584 bytes (43,2 MiB), com uma leitura individual de 12,25 s no log; `BuildPlayer-WorldRankingWindowUI.sharedAssets` tem 22.213.032 bytes, com leitura de até 9,35 s. Os nomes dos eventos são truncados; a pasta fornecida permitiu resolver os nomes e tamanhos reais. Os cruzamentos constam de `real_files.csv`.

O disassembly de `_fread_r` no executável vinculado mostra dois caminhos: o fluxo buffered faz refills/copias; o fluxo unbuffered entrega temporariamente o buffer do chamador ao backend de leitura para uma chamada grande. A 0.0.50 configura `_IONBF` ao abrir para leitura, sem `+`, exclusivamente os arquivos `.sharedAssets` em `UnityCache/Shared`. Isso seleciona o caminho de bulk read do próprio newlib antes de qualquer leitura. FILE, seek, EOF e fechamento continuam gerenciados pelo stdio. A Unity pode continuar aplicando suas próprias configurações posteriores de setvbuf.

Não há preload de dezenas de megabytes nem cache novo de assets em RAM. A alteração atua na transferência dos bytes dos arquivos grandes, sem reserializar texturas ou mudar sua qualidade. O marcador `io: direct sharedAssets streams ...` confirma as aberturas configuradas. O ganho de throughput ainda precisa ser medido no aparelho.

## 2. Diagnóstico perturbando o jogo

A captura da 0.0.49 registra cerca de 282,24 s acumulados em chamadas de emissão de log e máximo de 811,74 ms. São tempos de parede, incluindo esperas/desagendamento, não tempo exclusivo de CPU ou disco. Contudo, o código mantinha `log_lock` durante sceClibPrintf/sceIoWrite; outras threads podiam esperar pela gravação sob essa mesma trava.

A 0.0.50 usa uma fila de bytes de 128 KiB. Depois que o consumidor inicia, render, áudio e rede formatam e enfileiram suas linhas; não escrevem no armazenamento. O consumidor copia até 16 KiB sob uma trava curta, libera a trava dos produtores e grava lotes fora dela. A trava de I/O é usada pelo consumidor e pelo encerramento, não pelo caminho normal de produtores em modo buffered. A saída de console síncrona fica limitada ao fallback inicial; os logs do jogo continuam no mesmo srr_log.txt.

A fila mantém linhas inteiras e conta descartes se não couberem. `log_dropped` e `storage_ms` aparecem em OBS health. `writer_us` passa a refletir o tempo de enfileiramento/formatação do consumidor, enquanto storage_ms contabiliza a escrita em lote. O encerramento/fatal solicita a drenagem. Em encerramento abrupto, as últimas linhas ainda podem faltar.

Também foram reduzidos os registros: repetições individuais exigem pelo menos 50 ms; resumos de assets priorizam >=50 ms acumulados, >=50 ms de máximo ou pelo menos 128 chamadas. Mips pequenos deixam de gerar eventos individuais; uploads de mip zero e uploads lentos continuam. Os contadores de gráficos por quadro ainda abrangem todas as chamadas. Hits e gravações bem-sucedidas de shader deixam de emitir marcadores individuais, mantendo os contadores cumulativos; misses, erros, compilações e tempos de link permanecem.

## 3. Rede: cinco ocorrências perto do timeout

A mediana das 137 requisições individuais capturadas foi de 167,03 ms. Cinco chegaram a 30,63–30,89 s: tutorial, resultados da fase, MileageMapData005_01, ticker e net_bg.png. Esse padrão é compatível com o timeout de I/O de 30 s seguido de tentativa em conexão nova. O log não identifica sozinho cada etapa de socket; não é prova de que todos os cinco casos foram causados por keep-alive obsoleto.

O pool anterior não expirava conexões ociosas. Agora aposenta conexões após cinco segundos sem uso, antes de enviar um novo pedido. A busca ainda aproveita outra conexão recente do mesmo host, se houver. Fechamento e liberação ficam fora da trava do pool; slots expirados também podem ser substituídos na devolução. Handshakes continuam reaproveitados em rajadas curtas de assets. O timeout global, a validação TLS e as regras de repetição de requests não foram ampliados.

## 4. GC durante a corrida

Entre as amostras capturadas e atribuídas a s_playingstage, houve 68 coletas, mediana de 483,1 ms e máximo de 499,3 ms. Nos menus, 120 amostras tiveram mediana de 772,6 ms e máximo de 1590,9 ms. A atribuição de fase é observada e pode atrasar em transições. A marcação domina o custo. Requests HTTP, links e tempos de chamadas aninhadas não devem ser somados como parcelas exclusivas.

O heap alcançou 87.916 KiB (~85,9 MiB); o limite de 64 MiB impedia qualquer ativação do orçamento adaptativo, apesar da reserva nativa disponível. Na 0.0.50, o teto passa a 96 MiB, exigindo folga gerenciada de pelo menos max(12 MiB, heap/4) para ativar, e voltando ao padrão abaixo de max(8 MiB, heap/8). Continuam os limites nativos de 48 MiB para ativar e 32 MiB para desativar, a condição de gameplay sem carregamento, a escrita apenas sob GC_START e o respeito a mudanças externas.

Isso pretende espaçar coletas automáticas sem desativá-las. Não reduz diretamente o custo de marcação de cada coleta e não garante ausência de engasgos. O log seguinte mostrará divisor desejado/ativo, frequência, duração e folga resultante.

## 5. Shaders e outras operações

O cache chegou a 729 hits, zero misses e zero compilações. Compilar mais shaders não explica as pausas recorrentes desta sessão. Casos adicionais medidos: Resources.Load de GeneralWindow até 3,20 s, fonte UCGothic até 2,55 s, criação inicial até 1,57 s e WWW.GetTexture até 2,35 s. Essas operações podem conter outras etapas e coletas; a mudança de leitura e de logging atua em caminhos compartilhados, sem remover recursos das interfaces.

## Entrega

Por solicitação do usuário, não foram executados testes automatizados. Houve análise do log real, inspeção dos arquivos fornecidos, revisão dos caminhos de código, compilação ARM/VitaSDK e conferência da integridade do VPK/igualdade do eboot.

Pacote: `build/VPK/SRR-Vita-v0.0.50.vpk`.
Build: `20261029T173412Z`.
SHA-256: `def40a1296902e66a972fa44277b543d0200df2561855a6753728607665a6c62`.

A instalação envolve o VPK; os arquivos da pasta de dados não foram alterados por esta entrega. A comparação de performance no Vita ainda está pendente.
