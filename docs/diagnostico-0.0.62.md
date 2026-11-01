# SRR Vita 0.0.62 — sessão longa, esgotamento de threads e engasgos de UI

## Captura e limites

Log recebido: `build/VPK/srr_log.txt`, **0.0.61**, build **20261030T223730Z**. Há aproximadamente **34 min 28 s** de tempo observado até os últimos eventos, **56069 quadros**, **13331 eventos OBS** válidos e **138 fases**. A captura termina em crash, sem encerramento normal.

Cópia imutável, hash do log, ELF/eboot da 0.0.61, relatório, tabelas e `spikes.csv` foram preservados em `build/diagnostico-0.0.62-baseline/`. O log original recebido foi mantido. Saúde final: 12 eventos descartados, 876 estouros da agregação, zero descartes do logger. A cobertura é parcial. Fases são observadas periodicamente: `s_playingstage` também contém resultado e interações após a corrida; não se deve interpretar todos os seus quadros como gameplay contínuo.

## Fechamento ao avançar no modo história

O final mostra, nesta ordem:

1. Reconstrução de recursos de menu, incluindo uploads de texturas e links de programas. A última fase observada ainda é `scene=MainMenu loading=0` (138); não há confirmação de entrada na próxima cena de corrida.
2. `opensl: port 256 play state 1`.
3. **`pthread_create: thread table full`**.
4. Data abort, **PC 0x81180AA2**, **LR libcri_ware_unity.so+0x129DFC**, destino inválido **0x000081B0**, seguido por SIGSEGV.

O PC pertence ao executável do port, que estava relocacionado em **+0x64000**, calculado pelo endereço de `main` do log e o ELF correspondente. O endereço ligado **0x8111CAA2** resolve para **`__aeabi_memset4`**: a instrução tenta escrever no destino inválido. A instrução anterior ao LR na CRI chama `__aeabi_memset`, confirmando essa origem. Os arquivos correspondentes foram preservados antes de recompilar.

A tabela do bridge tinha 128 slots. A versão anterior só preenchia `func` ao criar uma thread e limpava esse campo em falha da criação; **join, detach e término normal não liberavam registros**. Logo, o limite contava criações acumuladas ao longo da sessão, mesmo depois de as threads nativas já terem terminado. Isso é um defeito demonstrável de ciclo de vida e explica o esgotamento na sessão longa.

A relação temporal e a biblioteca envolvida são fortes indícios de que a falha de criação participou do fechamento. Não há uma pilha completa que demonstre todo o encadeamento entre o retorno EAGAIN e o endereço inválido; não se afirma que todos os possíveis crashes estejam resolvidos.

A última amostra do coletor ainda informa aproximadamente **80518 KiB livres no heap nativo** e heap gerenciado de **71032 KiB**. Essa amostra antecede o crash, mas não sustenta diagnosticar o fechamento simplesmente como falta geral de RAM.

### Correção aplicada

- A thread usa um cleanup nativo que executa no retorno normal e em `pthread_exit`.
- Threads destacadas liberam o registro ao terminar; threads juntáveis mantêm o registro até um `pthread_join` bem-sucedido.
- `detach` posterior ao término também libera o registro. Falhas de join/detach preservam o registro, e falhas de criação devolvem a reserva.
- Publicação do handle e consultas à tabela passam pelo lock. Uma thread destacada que termine imediatamente não pode apagar o handle antes da cópia para o criador.
- Gerações distinguem registros antigos de handles nativos reutilizados. O join retira o registro exato que aguardou, evitando apagar uma nova thread criada com o mesmo handle durante a conclusão do join.
- A tabela de sinais também retira o registro e semáforo de uma thread encerrada, descontando os bits pendentes. Isso evita um segundo limite acumulativo de 128 entradas.
- Threads criadas pelo bridge usam sua própria tabela para resolver UID, evitando acumular entradas redundantes no mapa auxiliar de `pthread_self`.

O limite permanece **128 registros ocupados**: não foi aumentado para esconder o vazamento. Contadores amostrados em potências de dois registram `created`, `tracked`, `high-water` e `released`. O aviso de tabela cheia agora inclui contagens. `released` também inclui reservas devolvidas por falha de criação.

## Engasgos

Foram extraídos **409 registros de picos**, dos quais 129 estavam em fases observadas como `s_playingstage loading=0`. Em 24 desses registros, o callback mais longo foi `UIPanel.LateUpdate` com pelo menos 100 ms. Esses números representam a captura disponível e a classificação de cena, não uma contagem exaustiva de engasgos de gameplay.

Exemplos de quadros:

| Quadro | Render | UIPanel.LateUpdate | GC | JIT | I/O principal |
|---:|---:|---:|---:|---:|---:|
| 24271 | 213,75 ms | 161,87 ms | 0 | 0 | 0 |
| 6594 | 209,28 ms | 153,82 ms | 0 | 8,18 ms | 0 |
| 35613 | 199,82 ms | 153,15 ms | 0 | 0 | 0 |

Os tempos do callback são inclusivos e podem conter trabalho aninhado. Ainda assim, os exemplos isolam um alvo concreto de CPU na interface. Não é correto atribuir todos esses picos ao GC ou à compilação de shaders.

A otimização de geometria continuava desligada em toda a captura: `geometry bulk appends 0 original fallbacks 0`. O aviso novo da 0.0.61 finalmente identifica o prólogo recusado:

```
E1A0C00D  mov ip, sp
E92D59F0  push {r4-r8, fp, ip, lr}
```

`mov ip, sp` é independente de PC e pode ser copiado para o trampoline. A 0.0.62 aceita **essa instrução exata**, além das anteriores. O push observado já passa pelo filtro conservador. O método ainda precisa corresponder integralmente ao IL esperado; normais, crescimento de capacidade, buffers divergentes e demais condições especiais seguem pelo método original.

Isso remove o bloqueio encontrado na instalação da cópia em bloco para os buffers de geometria da UI. **Instalação e ganho efetivo precisam ser confirmados no próximo log do Vita**, com a mensagem de instalação e o contador `geometry bulk appends` crescendo. Não se declara redução medida dos 162 ms nesta entrega.

Há janelas posteriores com aproximadamente **42–48 FPS**, mas também janelas próximas de **25–29 FPS** e picos. Os registros de JIT de até 100–223 ms no estado da corrida também incluem métodos de ranking/resultado; antecipar toda essa compilação sem separar gameplay de resultado aumentaria memória e loading sem garantia de ajudar os engasgos durante a partida. A prioridade desta versão foi a falha demonstrada de threads e a otimização de UI que estava inativa.

## Validação e entrega

- ASan/UBSan: **1649 threads** usando o código real do bridge, incluindo retorno normal, `pthread_exit`, criação destacada, detach antes/depois do término, seis criadores concorrentes, falha de criação/join/detach e preservação do resultado de retorno. O teste reproduziu uma corrida de reutilização de handle durante o desenvolvimento, corrigida com gerações e retirada do registro exato.
- O mesmo teste mantém 128 workers ocupados, confirma EAGAIN para o 129º e depois recupera todos os registros.
- ASan/UBSan: **2560 registros de sinais** em ciclos de enchimento/retirada, contagem de pendências, liberação de semáforos e retirada repetida segura.
- Passaram os testes de entrega aninhada de sinais de GC, espera por sinal, cópia de geometria, wrapper/fallback e filtro ARM com os dois opcodes reais, além do encaminhamento de callbacks e preservação do notifier do GC.
- O teste antigo do notifier extraía também o bloco JIT acrescentado em versões anteriores e não compilava isoladamente. Sua delimitação foi corrigida para testar somente o notifier, preservando suas verificações.
- Release VitaSDK compilada. Não houve avisos nos arquivos alterados de threads/sinais/geometria; permanecem avisos anteriores em HTTP, SHA1, watchdog e libc.
- VPK/ZIP íntegro; eboot e recursos de arte empacotados conferidos contra os arquivos atuais. `git diff --check` passou. Arte e controles da 0.0.61 permanecem.

Pacote: **`build/VPK/SRR-Vita-v0.0.62.vpk`**, **4588707 bytes**.
Build: **20261031T012137Z**.
SHA-256: **b2e2fb3269a96f453d8d835e00e02b0c1213d7bcbf01a06a2e749f34facd23e7**.

Sem execução desta build no Vita nesta entrega. O teste decisivo é repetir várias transições de corrida no modo história: criações acumuladas podem ultrapassar 128, mas `tracked` deve acompanhar apenas registros ainda ocupados. O novo log também deve confirmar a instalação da cópia de geometria e permitir comparar os picos de UI nas mesmas fases.
