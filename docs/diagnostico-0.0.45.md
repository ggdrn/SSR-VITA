# SRR Vita 0.0.45 — pausas residuais na corrida

Baseline: `build/perf-baselines/srr-0.0.44-gameplay.log`. O usuário informou melhora leve na navegação e poucos engasgos restantes nas fases.

## Evidência

O novo log não contém falhas de alocação de texturas nem o crash de WWW das execuções anteriores. O limite de texturas está confirmado: `Unity masterTextureLimit 0 -> 1`. Isso não confirma sozinho que todos os fundos de Equippable reapareceram; o relato atual não especificou esse resultado visual.

Durante a corrida, os quadros 8501, 8764 e 8982 duram 439,06, 453,73 e 443,24 ms, respectivamente, cada um com uma coleta de GC. Na janela terminando no quadro 9000, a pior coleta leva 418,93 ms: 385,01 ms de marcação, 3,71 ms de suspensão, 23,10 ms de liberação e 0,72 ms de retomada. As chamadas gráficas medidas nesses picos somam poucos milissegundos, sem upload ou link significativo.

Há outros picos sem GC, como 245,89 ms no quadro 7617, e agrupamentos maiores que incluem criação/carregamento de objetos. Esses picos não são resolvidos apenas por ajustar o coletor. O log também não permite identificar a origem de cada coleta como automática ou explícita. A mudança abaixo atua no orçamento das automáticas; chamadas explícitas do jogo continuam executando normalmente.

## Ajuste limitado do orçamento de GC

Foi identificado no libmono.so do APK o divisor usado no cálculo do mínimo de bytes alocados entre coletas. O valor original é 3. O contrato do [Boehm GC](https://github.com/bdwgc/bdwgc/blob/master/include/gc/gc.h) permite trocar frequência de coleta por espaço: diminuir esse divisor aumenta o orçamento de alocações, ao custo de mais memória. O valor 1 não foi adotado; a própria [documentação do projeto](https://github.com/bdwgc/bdwgc) alerta para crescimento forte do heap nesse caso.

A 0.0.45 permite divisor 2 apenas com folga de memória na cena s_playingstage, fora de carregamento. Para ativar, exige pelo menos 48 MiB livres no heap nativo, 12 MiB livres no heap gerenciado e heap gerenciado total de até 64 MiB. Com o ajuste ativo, volta a solicitar 3 abaixo de 32 MiB livres nativos, 8 MiB livres gerenciados, ou ao exceder 64 MiB de heap gerenciado, entrar em carregamento ou sair da cena. A diferença entre limites de entrada/saída evita alternância frequente. A consulta é feita a cada 30 quadros, sem percorrer os objetos da cena.

Essa política pretende reduzir a frequência das pausas, não o custo de marcação de cada coleta. Em condições equivalentes, 3→2 aumenta em 50% a parcela de orçamento governada pelo divisor; não é promessa de aumento de FPS ou de redução fixa na quantidade de coletas. Também influencia heurísticas de expansão de heap do coletor. Não desativa GC, não torna o coletor incremental, não ignora chamadas explícitas nem remove novas coletas das transições.

## Guardas do binário e sincronização

A API de configuração do Boehm não está exportada nesse libmono. Antes de usar o campo, o port verifica os 276 bytes da função nativa correspondente, o endereço exportado de mono_gc_collect, a faixa do módulo, o slot GOT já relocado e o valor inicial 3. Qualquer divergência mantém a política original. A função de cálculo e o slot foram conferidos no ELF local do APK; não há busca por um valor 3 arbitrário na memória.

O coletor exige sincronização ao modificar o divisor. Por isso o fluxo principal publica somente uma solicitação atômica. A escrita no campo acontece exclusivamente no evento GC_START do notifier já instalado, dentro do bloqueio de alocação do Boehm. O callback não aloca, não registra mensagens e não chama métodos gerenciados. O notifier original do Mono continua chamado exatamente uma vez. Se outro componente alterar o campo para um valor diferente do último aplicado, o port deixa de ajustá-lo e registra a situação pelos contadores fora do callback.

O log mostra `gc: adaptive collection budget ready`, alterações `gc: requested divisor ...` e, a cada 600 quadros, os valores desejado/ativo e a memória nativa livre. Se o ajuste nunca ativar por falta de folga, os registros indicarão esse limite. Uma solicitação de mudança entra em vigor no início da próxima coleta, não durante uma coleta já em andamento.

## Validação

Os 18 testes passaram. O teste novo usa o ELF real do APK e a função C real de validação do layout, rejeita função/slot/valor/exportação divergentes, verifica ausência de escrita fora de GC_START, limites e histerese, carregamento/saída de cena, leitura sem underflow, cem mil solicitações concorrentes e respeito a mudanças externas. Os testes C usam AddressSanitizer/UndefinedBehaviorSanitizer.

Build ARM/VitaSDK concluído. VPK passou na integridade ZIP e seu eboot coincide com o build. SHA-256: `3a6cc4400cdecbdc9805411b18f186da90b1a6c79f3548932a0b72059662c6e8`.

Pacote: `build/VPK/SRR-Vita-v0.0.45.vpk`. O efeito no Vita ainda precisa ser medido. Testar a mesma fase por dois a três minutos e enviar srr_log.txt para comparar a frequência de coletas, seu custo e os picos sem GC. Não é necessário apagar saves ou caches.
