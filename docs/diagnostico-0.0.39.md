# SRR Vita 0.0.39 — áudio, coleta de itens e GC por bloco

Relato da 0.0.38: música com chiado/abafada, efeitos de argolas e itens ausentes, engasgos durante a corrida. Baseline preservada em `build/perf-baselines/srr-0.0.38.log`.

## Engasgos: evidência e alteração

O novo medidor mostrou um GC de 379,88 ms, com 348,92 ms em marcação, 4,80 ms para suspender threads, 18,86 ms em liberação e 0,74 ms na retomada. Outro intervalo marcou 348,95 ms. No carregamento, a marcação chegou a 1486,30 ms. Portanto, continuar reduzindo apenas a espera por sinais não trata o principal custo observado.

O IL original de `ObjectSpawnManager.ActivateBlock` termina com `GC.Collect()` após restaurar posição/rotação. Isso força uma coleta completa a cada ativação de bloco. A 0.0.39 substitui somente essa chamada de cinco bytes por NOPs, antes da compilação JIT do método. Coletas automáticas por alocação e as demais coletas explícitas, incluindo transições, permanecem originais.

O patch só aplica se o tamanho do método e sua sequência final corresponderem ao APK analisado. Não modifica o APK no armazenamento, nem desativa o coletor. A confirmação de aplicação aparece como `stage: removed per-block forced GC`.

Isso remove um gatilho desnecessário e identificado, mas não elimina o custo de futuras coletas automáticas ou outros trabalhos de carregamento. É necessário comparar a execução no console.

## Argolas e itens

A argola comum usa `ObjRing.SetPlayRingSE` -> `ObjUtil.LightPlaySE("obj_ring", "SE")`. Esse caminho é diferente do efeito `act_ring_collect` encontrado anteriormente no HUD. `LightPlaySE` retorna sem tocar quando `ObjUtil.IsLightMode()` é verdadeiro. Assim, mudar o canal para 2D não corrige esse bloqueio.

A 0.0.39 mantém o modo leve dos gráficos e remove apenas a verificação que silencia `LightPlaySE`, preservando argumentos, escolha da cue e retorno original. O mesmo caminho atende efeitos de coleta que usam essa função, inclusive variantes de eventos. A tabela do banco `se_runners.acb` contém `obj_ring`. A aplicação exige correspondência com todos os bytes do método original e registra `sound: enabled pickup cues in LightMode`.

Os cabeçalhos e o IL são acessados pelas [APIs de metadados do Mono](https://github.com/mono/mono/blob/main/mono/metadata/metadata.h), com liberação dos cabeçalhos após os ajustes. A pilha de avaliação do IL permanece balanceada.

## Chiado e som abafado

O consumidor OpenSL usava um único buffer de saída e o reescrevia ao preparar o próximo bloco. Agora alterna dois buffers alinhados e drena a saída antes de liberar sua memória. Isso preserva o bloco anterior enquanto ele pode estar em reprodução. A abordagem acompanha o [backend Vita do SDL](https://github.com/libsdl-org/SDL/blob/SDL2/src/audio/vita/SDL_vitaaudio.c), que também alterna buffers e usa a porta BGM para frequências abaixo de 48 kHz.

Foi adicionado um teste que simula o dispositivo retendo o ponteiro PCM. Ele falha quando se força a reutilização de um só buffer e passa com a alternância. Esse teste demonstra a falha de vida útil que pode produzir corrupção audível; a qualidade ouvida ainda deve ser validada no aparelho.

A saída tenta usar a frequência nativa do CRI (44,1 kHz no log), via porta BGM com ALC desativado. Nesse caminho, cada amostra é copiada exatamente, evitando a interpolação manual para 48 kHz. Se a porta BGM não estiver disponível, há fallback para MAIN/48 kHz, identificado no log. O caminho de fallback também usa dois buffers.

Os registros nativos periódicos incluem pico, proporção de amostras perto do limite e frequência de saída. O contador `clipped` usa limiar absoluto 32760: ele indica proximidade da saturação, não comprova sozinho distorção. Serve para separar possível saturação já presente no PCM do jogo de problemas na entrega ao dispositivo.

## Validação

Dez testes passaram, incluindo execução do pequeno fluxo IL de LightPlaySE em ambos os modos, equilíbrio da pilha, rejeição de tokens diferentes, preservação dos bytes vizinhos ao GC, amostras exatas a 44,1 kHz em mono/estéreo, vida útil dos buffers, Clear concorrente, sinais e medidores de GC. AddressSanitizer/UndefinedBehaviorSanitizer foram usados nos testes nativos. A simulação da reutilização antiga foi detectada pelo teste de vida útil.

Build ARM/VitaSDK concluído. Pacote: `build/VPK/SRR-Vita-v0.0.39.vpk`. Ainda não houve teste desta versão no Vita. Instalar e testar música, argolas e itens por dois minutos na mesma fase. Depois enviar `ux0:data/srr/srr_log.txt`. Não é necessário apagar saves ou caches.
