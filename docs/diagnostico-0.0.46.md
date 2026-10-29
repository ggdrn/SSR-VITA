# SRR Vita 0.0.46 — alocações da pista, leitura do APK e cache de shaders

Baseline preservado: `build/perf-baselines/srr-0.0.45-gameplay.log`.

## Evidência e limites

Na 0.0.45, a corrida mantém coletas com cerca de 374–390 ms, principalmente na marcação. Há picos sem GC e um quadro com 280,04 ms acumulados em link de shaders. Nos demais picos, link costuma ser zero. O orçamento adaptativo continua em divisor 3: heap gerenciado chega a 79.720 KiB e a folga nativa fica aproximadamente em 13–14 MiB. Essas condições não satisfazem as guardas da 0.0.45. Aumentar o orçamento nessa situação não foi considerado uma correção segura; os limites permanecem.

Esta versão reduz trabalho evitável e acrescenta atribuição dos picos. O build e os testes locais não demonstram redução das pausas no aparelho.

## Menos lixo gerenciado nas verificações da pista

`ObjectSpawnManager.Update` chama `CheckRangeIn` e `CheckRangeOut`. Seus finallys fazem boxing de um `List<SpawnableInfo>.Enumerator` exclusivamente para chamar Dispose na cópia. No mscorlib do APK, Dispose apenas limpa a referência à lista nessa cópia. Remover a sequência load/box/call evita as duas alocações habituais por Update e a alocação dos finallys secundários quando executados. Os endfinally e os limites das regiões de exceção continuam nos mesmos endereços.

Os dois métodos também contêm `new List<SpawnableInfo>(); pop`, sem atribuir nem usar a lista construída. O construtor sem argumentos apenas inicializa o novo objeto. Essas alocações descartadas foram removidas. A alteração não corrige ou modifica a lógica original de inclusão/remoção de objetos: todas as chamadas de criar/destruir e todas as mutações das listas continuam iguais.

Cada método é comparado integralmente com os bytes originais antes da alteração. Em um APK divergente, o método é preservado. Fixtures registram SHA-256 do Assembly-CSharp e os métodos completos; o teste também confere as implementações de Dispose/construtor no mscorlib original.

## Menos leituras de cabeçalho no APK

`apk_read_stored` chamava `apk_data_offset`, que relia o cabeçalho ZIP local de 30 bytes em cada leitura. O endereço dos dados agora é memorizado em cada entrada do índice, depois de validar o cabeçalho, com publicação atômica de 32 bits. São aproximadamente 2,5 KiB adicionais para as 628 entradas. O conteúdo dos assets continua lido normalmente. Falhas não são memorizadas; concorrência pode duplicar a primeira leitura, mas não altera seu resultado. O teste faz quatro mil leituras concorrentes e verifica bytes, limites e ausência de releitura do cabeçalho após preencher o cache.

## Cache de shaders

O cache existente continua no mesmo diretório e formato do vitaGL, sob `ux0:data/srr/shader_cache_v35`. A inicialização agora garante as subpastas v/f mesmo quando a pasta da versão já existe. Isso repara uma árvore parcialmente criada, sem exigir limpeza manual.

A gravação passa por arquivo temporário e rename depois de conferir escrita e fechamento. A leitura confere tamanho, metadados variáveis, identificação GXP e tamanho declarado do programa antes de desserializar. Arquivos comprovadamente truncados/incompatíveis são descartados individualmente e recompilados; falhas transitórias de alocação ou leitura preservam o arquivo existente. O caminho de cache permanece compatível com shaders válidos já salvos.

Os contadores cumulativos `gl: shader cache hits ... misses ... saved ... errors ... compiler calls ...` distinguem reutilização efetiva, ausências e chamadas reais do compilador. O patch reproduzível está em `patches/vitagl-shader-cache.patch`, incluindo a proteção de falha de compilação que já estava aplicada anteriormente nesse arquivo.

Não foi adicionado um warmup global de todos os shaders: ainda não há lista medida dos shaders ausentes e a folga nativa do baseline é pequena. Os contadores novos permitem decidir qual preparação antecipar sem carregar indiscriminadamente mais recursos.

## Atribuição dos próximos picos

- `perf: explicit GC requests` e `gc: explicit requests in window`: leem a estatística existente de solicitações via `mono_gc_collect`. O leitor verifica a função ARM completa, o endereço exportado, seu slot calculado pela instrução PC-relative e a faixa mapeada da estrutura de estatísticas. Não altera o coletor, o código dessa função ou seus callbacks. Chamadas que contornam essa entrada não são classificadas como automáticas por subtração.
- `perf: main file-io`: tempo, chamadas, bytes e maior chamada de fopen/open/fread/read e leituras físicas do leitor do APK.
- `perf: main wait`: tempo observado nas esperas instrumentadas de bridges e endereço do chamador da maior espera.
- `perf: main instantiate/resources-load`: wrappers das entradas nativas de clonar/instanciar e Resources.Load, preservando argumentos, retorno e uma única execução da função original. Os ponteiros originais precisam pertencer ao libunity. Registro e contagem de chamadas ficam no log; contagem zero não comprova ausência desse trabalho, porque wrappers de icall já compilados podem ter retido a entrada anterior. LoadAsync, descompressão e trabalhos internos da Unity não ficam integralmente cobertos por essas três entradas.

Os registros são restritos à thread do render, sem alocações gerenciadas ou enumeração da cena. Trabalho em workers não é somado como se bloqueasse diretamente o quadro. As categorias são inclusivas: uma criação pode conter leitura, espera e GC; seus tempos não devem ser somados para calcular CPU residual. Os detalhes dos picos continuam limitados pela amostragem já existente, e a cobertura de criação é informada a cada janela de 600 quadros.

## Validação e próxima execução

24 testes passaram, incluindo seis novos: descarte/boxing da pista, offsets do APK, atribuição por thread, contador explícito, cache e encaminhamento das chamadas nativas de assets. Testes C usam AddressSanitizer/UndefinedBehaviorSanitizer. Build ARM/VitaSDK e integridade ZIP concluídos; o eboot do pacote coincide com o build. O símbolo de diagnóstico do cache está ligado na aplicação.

Pacote: `build/VPK/SRR-Vita-v0.0.46.vpk`.
SHA-256: `ace71acc3fee58e9ad0e9f71e6a42edd18d1da28b71b21078c1c4d7ea684f1db`.

Testar a mesma fase por dois a três minutos, voltar ao menu e entrar novamente. Manter os saves e o cache atuais. O próximo srr_log.txt permitirá comparar frequência/duração das pausas, solicitações explícitas, I/O/esperas/criação e reutilização do cache. O efeito real de desempenho e a compatibilidade visual/sonora ainda precisam dessa execução no Vita.
