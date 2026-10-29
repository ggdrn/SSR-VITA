# SRR Vita 0.0.37 — efeitos nas fases e pausas de GC

## Relato confirmado

A última correção do usuário esclarece: todas as músicas tocam; os efeitos das fases não são ouvidos. Essa informação substitui a resposta anterior sobre fase só com efeitos.

## Evidências da 0.0.36

O log `build/VPK/srr_log.txt` registra `BgmPlay(bgm_q_w01, BGM)` e solicitações como `SePlay(act_jump, SE)` e `SePlay(act_ring_collect, SE)`. Os bancos SE/BGM estão carregados e ambos os volumes do SoundManager são 1.0. Esses registros confirmam solicitações e bancos; não comprovam a saída audível de cada efeito.

Foram lidos via FTP apenas bancos de áudio para análise local. A tabela de nomes de `se_runners.acb` contém `act_ring_collect`; a de `BGM_q01.acb` contém `bgm_q_w01`. Nenhum save foi lido ou alterado nesta investigação, nem foram importados dados para o emulador.

Os intervalos de jogo medidos incluem médias de aproximadamente 35–44 ms por frame, com picos recorrentes de aproximadamente 420–440 ms e outros de até 1,9 s. Os contadores correlacionam parte dos frames lentos com coletas de memória. Existem também travadas de carregamento e compilação de shaders. Uma correlação com coleta não identifica sozinha a etapa interna responsável pelo tempo.

## Compatibilidade do canal SE

O IL de `SoundManager.Update` move seu objeto para (-1000, -1000, 0) a cada frame. `SoundManager.Initialize` cria três CriAtomSource, cujo construtor ativa `_use3dPositioning`. O canal compartilhado de efeitos é o índice 2.

A 0.0.37 consulta esse canal a cada 30 frames no thread principal e desativa seu posicionamento 3D pela propriedade original do CRI. O ajuste remove a atenuação por distância de efeitos globais. Os canais BGM mantêm as configurações originais. O log só confirma a mudança depois de reler a propriedade. Isso é uma correção de compatibilidade com uma causa plausível para o silêncio; a recuperação audível ainda depende do teste no console.

## Espera de sinais

`sigsuspend` usava um laço com espera de 100 microssegundos. Agora cada thread usa um semáforo binário para acordar quando um sinal é enfileirado. Os bits pendentes continuam sendo o predicado, preservando sinais mascarados e sinais que chegam antes da espera. Um timeout de 5 ms e um caminho alternativo cobrem falhas de alocação. O protocolo de suspensão e retomada e o coletor original são preservados.

Essa mudança reduz despertares desnecessários de threads suspensas. Não comprova a eliminação das pausas de 420 ms; os contadores existentes continuam permitindo comparar a próxima execução.

## Novos registros

Os snapshots incluem cue, status, tempo de reprodução, volume, posicionamento 3D e seleção de baixa latência de cada canal. O OpenSL mede pico PCM e quantidade de buffers não nulos por janela, em vez de depender apenas do primeiro PCM não nulo.

## Validação

Build ARM/VitaSDK e integridade do VPK passaram. Testes com AddressSanitizer/UndefinedBehaviorSanitizer verificaram ajuste exclusivo do canal SE, ausência de atualização repetida, fonte ausente, chegada de sinal antes da espera, sinais mascarados, falha na alocação do evento, retomada aninhada do GC e fila de áudio concorrente. Os testes existentes de log e shader também passaram.

Pacote: `build/VPK/SRR-Vita-v0.0.37.vpk`. Ainda não instalado ou validado no aparelho nesta etapa. Testar pulo, argolas e checkpoint por aproximadamente um minuto, e depois comparar `ux0:data/srr/srr_log.txt` com o log da 0.0.36. Não é necessário apagar saves ou caches.
