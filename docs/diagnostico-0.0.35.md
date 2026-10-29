# 0.0.35 — personagem, áudio, engasgos e CPU3

## O que o log da 0.0.34 confirma

- O menu não registrou novas `FormatException` e o guard da data foi instalado.
- A fase `s_playingstage` carrega. Sonic tem 2386 vértices, 52 ossos e renderer
  ativo/visível. O shader `Custom/ykChrLine_dme1` é marcado como suportado.
  Isso não comprova que os pixels do personagem foram desenhados corretamente.
- OpenSL recebe PCM audível no título e no menu. Na transição para a fase cria
  outra porta, mas o log não mostra `play state 3` para ela. O FMOD permanece
  com rate/block -1; o jogo usa CRI e não tem AudioSources Unity nas amostras.
- Depois do carregamento, os renders ficam próximos de 35 ms, com picos de
  aproximadamente 450–473 ms e 2 s. Há também pausas maiores durante os loads.
  A contagem de links de shader permanece em 198 durante esses picos; compilação
  de shaders não explica todas as pausas.
- A tentativa de usar CPU3 retornou `0x80028025`; a rede caiu para `0x70000`.

## Alterações

### Renderização

A implementação lê o shader do APK sem alterar o APK. O material de personagem
usa dois passes: `Custom/ykOutLine/OUTLINE` e `Custom/ykChr_dme1/ENVMAP`.

O pass de envmap recebe uma alternativa de compatibilidade: preserva posição,
depth offset, UV e textura `_DifTex` com a cor `_DifCol`, mas usa iluminação
simples, sem o reflexo e iluminação por vértice originais. O contorno original
permanece. Trata-se de uma alternativa para validar o problema do material;
ainda requer confirmação visual no Vita. A seleção foi conferida contra os
shaders reais do APK e não seleciona o pass de contorno.

Para comparar com o caminho original, criar o arquivo vazio
`ux0:data/srr/original_character_shader` e reiniciar o jogo. Removê-lo restaura
compatibilidade. O log informa o modo e cada shader substituído.

Também foi acrescentado fallback de lookup de `uniform[0]` para `uniform`,
quando o vitaGL não encontra o nome com colchetes. As localizações pequenas por
programa continuam a evitar tabelas gigantes no Unity.

O compilador usa otimização FAST com fastmath desativado, em vez de UNSAFE.
O cache passa a `ux0:data/srr/shader_cache_v35` para não reutilizar binários
compilados com as opções antigas; a primeira execução recompila shaders.

### Áudio

- Fila de buffers dimensionada pelo `numBuffers` solicitado, em vez de 16 fixos.
- Proteção da fila durante enqueue, consulta, clear e consumo. O clear aguarda
  o término da leitura do buffer atual; não deixa a contagem negativa nem
  devolve memória que o consumidor ainda está lendo.
- Clear reinicia índices e estado de resampling; GetPosition passa a avançar.
- Falhas de porta, semáforo, thread e alocação passam a ser verificadas.
- A thread do player termina sem se autoexcluir, para ser aguardada e excluída
  pelo destrutor que possui o objeto.
- O log registra primeiro enqueue, registro de callback, tamanho da fila,
  contagem produzida/consumida e o primeiro erro nativo guardado pelo CRI.

Essas correções resolvem falhas concretas do bridge. O log anterior não prova
qual delas causa o silêncio da fase; não há confirmação de áudio corrigido
em hardware.

### Engasgos

A entrega de sinais agora retira um sinal por vez. Antes, um lote contendo
suspend/resume podia retirar a retomada da fila antes de o handler de suspensão
entrar na espera aninhada. SIGPWR de suspensão precede SIGXCPU de retomada,
e o mesmo handler não reentra enquanto estiver ativo. O caso tem teste local.

O intervalo das esperas cooperativas dos bridges de pthread cai de 20 para
5 ms, para reduzir a demora de resposta ao GC. Há um custo de mais despertares
nas threads ociosas, que precisa ser avaliado no console.

O dump completo de centenas de GameObjects/renderers foi removido. Permanecem
as amostras focadas em personagens, texturas, câmeras, áudio e tempo por frame.
A consulta inicial de Mono foi movida para depois do primeiro render, evitando
consultar o registro de imagens antes de sua inicialização.

### Quarto núcleo

O loader tenta `0xF0000` para main e workers de jogo/áudio. Se o kernel recusar,
mantém os três núcleos de usuário (`0x70000`) para workers. A linha `cpu:`
registra resultado e máscara. A rede já possui tentativa e fallback próprios.

O quarto núcleo depende de permissão no console, como a disponibilizada por
[CapUnlocker](https://github.com/GrapheneCt/CapUnlocker). Não há garantia de ganho
sem medir: ele aumenta a capacidade de executar trabalho paralelo, mas não
corrige sozinho espera de GC, shader ou fila de áudio.

## Validação

Build Release e VPK gerados. Testes de host usam trechos das funções reais,
com I/O e mutexes do Vita simulados, AddressSanitizer e UndefinedBehaviorSanitizer:

- `python3 tests/test_signals.py`: retomada aninhada do GC, sinais mascarados e
  ausência de reentrada do mesmo handler.
- `python3 tests/test_audio_queue.py`: capacidade de 64 buffers, overflow,
  PCM inválido, clear concorrente com consumo e resampling.
- `python3 tests/test_gl_uniforms.py`: alias `[0]`, localizações por programa,
  seleção dos shaders do APK e modo original. Esse teste precisa do APK local.

Esses testes não executam o driver GXM nem o compilador de shaders do Vita.
O teste de áudio também não valida a saída física ou o estado interno do CRI.

## Próximo teste no aparelho

Instalar `build/VPK/SRR-Vita-v0.0.35.vpk`. Verificar personagem, música e efeitos
na fase; testar uma segunda corrida e retorno ao menu. Comparar também uma
segunda execução após o cache de shaders ser criado. Copiar
`ux0:data/srr/srr_log.txt` antes de iniciar novamente.
