# 0.0.82 — preparação do cache antes de iniciar o jogo

## Ordem de inicialização

A etapa ocorre depois de `gl_bridge_init()` e antes de `unity_run()`.
É anterior à inicialização da Unity, não uma pausa introduzida ao entrar
na corrida. O menu de configurações ainda aparece primeiro quando solicitado;
Exit continua encerrando antes de carregar o jogo.

A tela usa o framebuffer do vitaGL com desenho de texto pela CPU. Exibe:

- `Scanning shader cache...`
- `Preparing cached shaders...`, itens processados, percentual, barra,
  quantidade preparada e quantidade ignorada.
- `Shader preparation complete` ao terminar.
- `No cached shaders yet.` e `The cache will be created while you play.`
  quando não existem arquivos de cache.

Os números refletem o trabalho executado, incluindo arquivos rejeitados ou
não preparados por falta de orçamento; não há contagem fictícia de compilação.
Não existe uma espera artificial para alongar a tela.

## O que é antecipado

O port consulta o diretório efetivo do cache no vitaGL, incluindo a versão
do formato. Percorre os diretórios de vértices/fragmentos e shards de hash,
seleciona arquivos `.gxp` com nomes válidos e prioriza os mais recentes.

Os programas já compilados são lidos, validados, desserializados e registrados
no shader patcher GXM antes da Unity. Quando o jogo solicita o mesmo shader,
o registro e os metadados preparados são transferidos para o objeto GL.
Esse primeiro uso não repete a leitura do arquivo nem o registro do programa.
A identidade GL, a fonte original, flags e contagem de referências do shader
criado pela Unity são preservadas. Os metadados de matriz continuam apontando
para o mesmo programa alocado, cuja propriedade passa ao shader do jogo.

Não são criados handles GL artificiais que poderiam colidir com os da Unity.
A ligação dos pares, os layouts de vértices, as variantes de pipeline e as
texturas ainda dependem do estado e dos materiais criados pelo jogo. Esta
etapa não elimina esses custos, GC ou JIT.

Sem cache existente, a etapa não inventa nem compila variantes desconhecidas.
O jogo segue seu carregamento normal e grava os shaders para próximas
execuções. Arquivos inválidos e programas que não couberem no orçamento
usam o caminho normal quando necessários.

## Memória e falhas

- Até 4096 arquivos na fila, com crescimento por blocos e lista liberada
  antes de iniciar a Unity. O total omitido é informado no log.
- Orçamento de 8 MiB para programas preparados ainda não consumidos,
  incluindo os registros e metadados de matriz. A leitura usa um buffer
  temporário adicional de até 1 MiB; a fila e o overhead interno do GXM
  não fazem parte desse orçamento.
- Cada transferência libera seu registro da fila de programas preparados.
  Os programas restantes são liberados no encerramento normal.
- Falhas de alocação, validação GXP, índice de metadados ou registro GXM
  deixam o caminho de carregamento/compilação normal disponível. A fonte
  GLSL não é liberada por uma tentativa malsucedida de desserialização.
- A preparação roda na thread principal com o contexto gráfico disponível.
  Progresso e watchdog são atualizados durante o processamento.

O patch da biblioteca está em `patches/vitagl-shader-preload.patch`, incluindo
as declarações públicas necessárias em `vitaGL.h`. A biblioteca local foi
recompilada e religada ao port. As correções da 0.0.81 para streams separados
continuam presentes e Material Shaders continua On por padrão.

## Logs e comparação no Vita

O resumo inicial registra:
`gl: boot shader preparation files=... prepared=... invalid=... limited=... omitted=... elapsed=... ms (before Unity)`.

Durante o jogo, `gl: boot-prepared shader programs reused N` informa quantos
programas preparados foram efetivamente consumidos. Os contadores de hits
agora incluem as consultas da preparação e os usos posteriores do cache.
O span `boot.shader-preparation` separa o trabalho antecipado do carregamento
de programas durante a sessão.

Medir o tempo de boot e os tempos de link/quadros lentos da mesma primeira
corrida nas duas versões. Ganho de FPS e redução dos engasgos não foram
confirmados em hardware nesta entrega. O tempo antecipado aumenta o boot;
não é correto somá-lo como ganho total de tempo sem comparar as sessões.

## Validação e pacote

Oito testes de host passaram com ASan/UBSan: loader de cache, transferência
de programas preparados, desserialização e falhas, enumeração/progresso/ordem
de boot, menu/tela, shaders/uniforms, streams de vértices e atributos ativos.
A tela em inglês foi renderizada e inspecionada visualmente. Os testes usam
stubs para GXM e não substituem validação na GPU do Vita.

Build VitaSDK do vitaGL e do port concluído sem avisos. Integridade ZIP,
identidade do eboot empacotado, reversibilidade do patch e `git diff --check`
verificadas.

Pacote: `build/VPK/SRR-Vita-v0.0.82.vpk`.
SHA-256: `f0d5154462a8ac3a4f4dd7aa80cb2757ea571a38ddaed9417ecdc616ba0ef8f2`.
