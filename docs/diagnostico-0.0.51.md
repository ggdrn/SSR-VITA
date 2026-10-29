# SRR Vita 0.0.51 — modal pós-corrida e alocações por atualização

Log fornecido: `srr_log_game_play_pouco_engasgo.txt`, preservado em `build/perf-baselines/srr-0.0.50-gameplay-modal.log`. São 6319 linhas e 4223 eventos OBS. O relato informa poucos engasgos e melhora leve de navegação, seguidos de um modal de personagem que não aceitava confirmação.

## O que ocorreu no final

O processo não parou de produzir quadros. A fase observada permaneceu em s_playingstage; os toques continuaram chegando à bridge. Login/resultado tiveram respostas no intervalo de 480–483 s, sem requisição pendurada depois disso nos eventos disponíveis. A queda sustentada de desempenho aparece perto de 498 s, depois de duas clonagens próximas de 495,69 s.

Entre 340–470 s, as janelas disponíveis somam 5075 quadros, média medida de 25,73 ms e 13 coletas. Entre 500–644 s, somam 1370 quadros, média de 99,62 ms e 36 coletas. O P50 passa a ficar próximo de 80 ms. As coletas finais levam ~384–392 ms, com ~355–361 ms em marcação. São janelas diferentes da experiência; não uma comparação controlada de FPS.

Isso caracteriza uma interface viva porém sem progresso e com trabalho/garbage aumentando. O log anterior não continha os IDs Android realmente entregues nem a contagem nativa de toques, portanto não prova sozinho a causa exclusiva da falta de confirmação.

## Correções de entrada

A bridge gerava `next_pointer_id++ & 0xff`: até 255, embora o contrato de ponteiros Android use IDs limitados a 0–31. Havia pelo menos 129 DOWNs registrados antes de 490 s (e 174 na sessão). Essa era uma falha concreta de compatibilidade, mesmo que a Unity tolerasse parte desses valores. Agora os IDs são os slots estáveis 0–5, reaproveitados somente depois do UP. O sexto slot continua reservado ao toque virtual de Cross/Circle.

O mapeamento físico também usava `hardware_id % 5`. Dois IDs podiam colidir enquanto um dedo ainda estivesse pressionado. Agora cada ID físico é associado a um slot livre; dedos ausentes são liberados antes de associar os novos. Atualizações de posição dos dedos existentes são agrupadas em um MOVE por amostra, em vez de emitir pacotes intermediários para cada dedo.

A bridge só usa os dados do touch quando sceTouchPeek retorna uma amostra. Antes, retorno zero podia deixar uma estrutura não inicializada ser interpretada como toque. Botões também passam a preservar o estado anterior quando não há amostra válida.

Se a fila de entrada saturar, preserva o evento mais recente em vez de descartar o UP novo; o descarte é contado no log. Isso reduz o risco de deixar um estado pressionado preso após uma interrupção de consumo, embora eventos perdidos continuem sendo uma limitação de uma fila limitada.

Os eventos agora registram DOWN/UP com ID e slot, além da entrega pela AInputQueue. Um contador nativo de Input.touchCount é consultado sem boxing/enumeração a cada 15 quadros, com resumo periódico e aviso se ultrapassar os seis ponteiros da bridge. Isso permitirá separar falha de entrega, toques fantasmas e uma eventual espera lógica da própria interface.

A implementação não força o fechamento do modal nem modifica as condições de recompensa/compra do jogo. A confirmação no aparelho ainda precisa ser verificada.

## Menos alocação nas rotinas por quadro

Uma inspeção dos metadados e IL do Assembly-CSharp original encontrou 22 sequências de boxing evitável em 16 métodos Update/LateUpdate. Todas têm a forma:

```
ldloc enumerador
box System.Collections.Generic.List<T>.Enumerator
callvirt System.IDisposable.Dispose
endfinally
```

O Dispose desse enumerador no mscorlib do APK apenas limpa uma referência dentro da cópia encaixotada. A cópia não escapa; o enumerador original e a lista não são modificados por essa chamada. Remover load/box/call evita criar esse objeto temporário, mantendo endfinally, offsets, demais chamadas e mutações.

A seleção offline exige o tipo exato de enumerador e exclui sequências com destinos de branch ou limites de regiões de exceção no interior. O scanner está em `scripts/scan_boxed_enumerators.cs` e a auditoria dos corpos/spans em `docs/boxed-enumerators-0.0.51.json`. O runtime primeiro confere os oito bytes do Dispose no corlib, depois compara integralmente o corpo original de cada método. Divergências preservam o método. O header gerado é `src/util/hot_loops.h`.

Classes: AssetBundleManager, BackKeyManager, CameraManager, DailyInfoBattle, DelayedMessageManager, HudAlert, ItemSetWindowBuyUI, ObjFeverRing, ObjFriendSignPoint, RankingManager, RankingResultBitWindow, ResourceSceneLoader, RoulettePartsBase, SocialTaskManager, StageMissionManager e TextureAsyncLoadManager.

As edições acontecem antes de entrar nas fases. Métodos já compilados pelo JIT nesse momento mantêm seu código nativo original; o número de corpos alterados no log não demonstra que todos passaram a executar a versão otimizada. Objetos e rotinas que só são compilados depois podem aproveitar a redução. Não foi forçada invalidação de código em execução.

O orçamento de GC e as otimizações de leitura/log/rede da 0.0.50 permanecem. A frequência de GC aumentada no trecho do modal deve ser reavaliada depois da correção de entrada; não foi compensada desligando o coletor ou aumentando mais o orçamento.

## Entrega

A pedido do usuário, não foram executados testes automatizados. Houve leitura do log real, inspeção dos handlers e dos metadados/IL originais, geração dos guards, compilação ARM/VitaSDK e conferência de integridade do VPK e igualdade do eboot com o build.

Pacote: `build/VPK/SRR-Vita-v0.0.51.vpk`.
Build ID: `20261029T194512Z`.
SHA-256: `ef11c3abbab4b449db002662a881150ec9c873c3587dab997be8390d263ebe41`.

São necessárias observações da execução no Vita para confirmar o comportamento do modal e medir a redução de alocações/engasgos. A instalação utiliza apenas o VPK; os dados e assets fornecidos não foram regravados.

Leitura comparável no fluxo: o arquivo MainMenuPages.sharedAssets teve 56.136.144 bytes em 33 chamadas e 4,32 s acumulados na 0.0.50; na captura 0.0.49 havia os mesmos bytes/33 chamadas em 20,73 s. Isso apoia o ganho do caminho de I/O anterior, sem demonstrar que toda a navegação ficou nessa proporção.
