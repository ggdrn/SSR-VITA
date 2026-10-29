# SRR Vita 0.0.44 — Equippable e carregamentos de minutos

Baseline: `build/perf-baselines/srr-0.0.43-equippable.log`. O usuário relata apenas ícones na tela Equippable, sem os fundos, e demora que impediu iniciar a missão.

## Gargalo identificado nas medições

As 88 requests cronometradas têm mediana de 186 ms, máximo de 2035 ms e 77 durações de até 300 ms. Elas não explicam, sozinhas, os maiores bloqueios locais:

| Quadro | nativeRender | Chamadas de textura | Link de shaders | GC registrado |
| --- | ---: | ---: | ---: | ---: |
| 3380 | 141,30 s | 125,90 s | 81 ms | 0 |
| 3416 | 74,25 s | 72,36 s | 43 ms | 0 |

O log mostra repetidas falhas de mipmaps de 11.184.880, 5.592.544 e 1.398.240 bytes e erros de buffers nativos de textura. Os pools de VRAM/RAM/PHY ficam quase sem espaço e o heap normal falha apesar de somar dezenas de MiB livres. Nenhuma mensagem de recuperação de grandes alocações da 0.0.43 aparece nesta execução. A recuperação não encontra um bloco suficiente nos outros pools.

Isso é compatível com atlas grandes de fundo/quadros falhando enquanto ícones menores aparecem. O log não associa cada chamada GLES ao nome do objeto visual, portanto ainda não identifica individualmente cada fundo ausente de Equippable.

## Recuperação do vitaGL sem esperas fixas de um segundo

Os dois caminhos emergenciais de alocação, CPU e GPU, executavam uma limpeza assíncrona seguida de `sceKernelDelayThread(1000000)` por tentativa. Com quatro tentativas por alocação e vários níveis de mipmap repetindo a expansão, essas pausas se acumulam dentro dos uploads medidos.

A biblioteca agora aguarda a conclusão real da limpeza. O semáforo de conclusão começa com quatro permissões, portanto esperar só uma após enfileirar um trabalho não provaria que ele terminou. A recuperação primeiro adquire todas as permissões, aguardando trabalhos anteriores, envia uma nova limpeza, aguarda a permissão devolvida por ela e restaura as quatro permissões normais. Falhas das APIs também restauram o orçamento de permissões correspondente. As sincronizações de GPU já existentes e o limite de ciclos permanecem.

Essa limpeza é a do vitaGL para recursos gráficos; não é o GC gerenciado do Mono. Retirar o atraso fixo reduz espera artificial, mas não cria espaço quando todos os pools estão cheios.

## Orçamento real das texturas

A versão aplica `QualitySettings.masterTextureLimit=1` às texturas elegíveis. A [API do Unity 4.6](https://docs.unity3d.com/ja/460/ScriptReference/QualitySettings-masterTextureLimit.html) descreve esse valor como uso do primeiro mip, com metade de cada dimensão. Uma imagem elegível 2048×2048 passa a usar 1024×1024 no upload; a área cai para um quarto. Isso não significa que a memória total do jogo caia para um quarto: os dados de origem e outros recursos têm custos distintos.

O limite é aplicado pelo próprio motor, sem alterar manualmente UVs, cortar bitmaps ou reescrever os bundles. Imagens e textos podem ficar menos nítidos. Não foi ativado o modo leve nem escolhido outro nível geral de qualidade: a escolha de modo/efeitos do usuário permanece.

O port resolve os icalls nativos originais do getter, setter e SetQualityLevel(int,bool), verifica que pertencem a libunity.so e registra um wrapper para SetQualityLevel. O wrapper chama a implementação original com os mesmos argumentos e restabelece o limite depois. Isso também cobre o overload gerenciado de um argumento quando ele é inlined pelo JIT. A entrada registrada é conferida após a alteração. Um getter nativo sem alocação verifica o limite a cada 30 quadros e corrige resets; limites mais fortes já selecionados são mantidos. A aplicação é adiada e repetida se os icalls ainda não estiverem registrados na inicialização.

O log confirma o valor aplicado com `Unity masterTextureLimit ... -> 1`, ou informa se a API não estiver disponível. Texturas não afetadas pelo limite do motor continuam com seu tamanho original. O caminho de recuperação de buffers da 0.0.43 permanece disponível se houver espaço nos pools.

## Falha adicional no encerramento de WWW

O final do log contém um Data abort original em libunity.so+0x92C4AC, chamado de +0x3CD800. A desmontagem mostra o caminho de callback de cabeçalhos tentando acrescentar texto a uma string cujo ponteiro de dados está nulo. O log não prova sozinho por que essa string ficou inválida.

A classe com.unity3d.player.WWW do próprio APK foi inspecionada com jadx: ela herda de Thread, começa a transferência no construtor e não chama doneCallback após errorCallback. No port, join() era um no-op e o worker sempre chamava doneCallback, inclusive após erro. Esses comportamentos permitiam callbacks posteriores à conclusão/ao descarte e violavam o protocolo de vida útil.

Agora join aguarda todos os callbacks e a limpeza da requisição antes de devolver o controle. O estado tem referências separadas para dono Java, worker e joiners, suporta joins repetidos/concorrrentes e continua atendendo sinais do Mono durante a espera. Descarte reentrante no próprio worker não se auto-bloqueia e impede callbacks posteriores. Erro, aborto e falha na criação da thread produzem um término de erro, sem enviar doneCallback depois. Uma falha ao alocar o estado é notificada sem precisar alocar outra string Java. O helper de criação de threads também apaga a thread do kernel se a tentativa de iniciá-la falhar.

Essa correção trata uma falha concreta do bridge. Ainda é necessário confirmar no aparelho se ela elimina o crash observado, que aconteceu em uma execução já sob forte pressão de memória.

## Verificação

Os 17 testes passaram, com AddressSanitizer/UndefinedBehaviorSanitizer nos testes C. As verificações novas executam o código C real:

- Semáforos: trabalhos de limpeza pendentes, conclusão do novo trabalho, 100 ciclos adicionais, restauração das quatro permissões e erros das APIs.
- Orçamento: espera pela inicialização dos icalls, validação da origem, argumentos de qualidade preservados, chamada original exatamente uma vez, ausência de recursão/alocação no getter, limite mais forte preservado e reaplicação periódica.
- WWW: join bloqueado enquanto o worker ainda pode chamar o nativo, dois joiners no mesmo objeto, oito requisições independentes, sucesso, aborto em cabeçalho/dados, erro HTTP/transporte, descarte reentrante, falha de thread e falha de alocação do estado.

Build ARM/VitaSDK e recompilação da biblioteca vitaGL concluídos. O VPK passou na integridade ZIP e o eboot empacotado coincide com o build. SHA-256: `95589d989eace58b3f2339397fcbe2cff048dee6f904c0568bf59ed64a320be4`.

Pacote: `build/VPK/SRR-Vita-v0.0.44.vpk`. Não há medição nem confirmação visual desta versão no console ainda. Testar menu → Equippable → início de missão e registrar se os fundos reaparecem e quanto cada transição demora. Enviar o novo srr_log.txt. Não é necessário apagar saves ou caches.
