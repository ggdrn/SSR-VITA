# SRR Vita 0.0.70 — parsing dos trechos em lotes

## Captura de referência

Log da 0.0.69, build 20261103T174735Z, preservado em
`build/diagnostico-0.0.70-baseline/srr_log.txt`; relatório OBS em `report/`.
16966 eventos e 160 estados observados. SHA-256 do log:
`eec7019e9e4a305d2ed836823618397296f7beb516a6486bb50426e71291525f`.

A rotina Rijndael nativa está ativa nesta captura. Ainda há picos de
carregamento: quadro 38573 com LoadSetTable.MoveNext individual 308,28 ms;
quadros posteriores têm CreateSetData em aproximadamente 90–123 ms.
No quadro 41198 há render de 932,20 ms e GC de 580,51 ms aninhado ao
carregamento. Os tempos inclusivos não devem ser somados.

## Alteração

O ReadLayer original é uma coroutine, mas chama ReadObjects para processar
uma camada inteira antes de ceder o controle. O parser faz leituras de XML,
criação de parâmetros, resolução de tipos, vetores e parâmetros personalizados
dentro dessa chamada longa.

A 0.0.70 mantém esse mesmo parser de objetos e divide o trabalho:

- ReadObjects processa um objeto e retorna. Apenas os 16 bytes do loop
  terminal são retirados; o corpo que interpreta cada objeto permanece igual.
- ReadLayer chama o parser até 16 vezes por MoveNext. Havendo mais objetos,
  retorna true com Current nulo e continua no próximo avanço da coroutine.
- A camada só entra em StageSpawnableParameterContainer.AddData quando todos
  os seus objetos foram processados. A ordem, IDs, parâmetros e publicação
  final são preservados; nenhuma camada parcialmente montada é publicada.
- ReadLayer mantém seu corpo de 243 bytes, a assinatura de locais original
  (uint32 e bool) e os campos existentes. Nenhuma nova classe, assembly,
  cache, relógio ou alocação por lote foi introduzida pelo patch.

O salto original de ReadObjects volta ao offset zero, reiniciando seus locais
por objeto. Essa propriedade foi confirmada executando o IL original e é
preservada pelas chamadas individuais. Current é sempre nulo no ciclo de vida
suportado: o construtor não atribui valor e todos os yields originais são nulos.
O novo estado de retomada usa os campos do mesmo enumerador.

A divisão reduz o tamanho do trabalho feito em uma etapa; não é um orçamento
de milissegundos, nem elimina o custo total do XML. Unity pode avançar outras
coroutines no mesmo quadro. O número de quadros até concluir uma carga pode
aumentar. A margem de carregamento antecipado em corrida rápida ainda precisa
ser validada no Vita. Não houve alteração na física, velocidade do jogador,
GC, dados do APK ou lógica de spawn.

## Guardas e comparação

O instalador exige o MVID suportado e os corpos completos exatos de ambos os
métodos: ReadObjects (681 bytes) e ReadLayer.MoveNext (243 bytes). Não escreve
em nenhum deles se uma das verificações falhar. A instalação ocorre junto às
otimizações iniciais, antes do warmup/JIT da corrida. Não sobrescreve código
nativo já compilado nem altera o arquivo APK.

O teste de metadados confirma que ReadObjects tem exatamente um ponto de
referência IL no assembly: essa coroutine ReadLayer. A edição pareada não
trunca outro chamador do jogo.

Mensagem esperada:
`stage: XML layer parsing yields every 16 objects (complete layers published only; future JIT)`.

Criar `ux0:data/srr/original_stage_parse` e reiniciar mantém os dois métodos
originais. Remover o marcador e reiniciar habilita novamente a divisão.

## Validação

- `python3 tests/test_stage_parse.py`: ASan/UBSan, MVID, APIs, módulos, métodos,
  headers, tamanhos, stack e IL incompatíveis; marcador; preflight dos dois
  métodos, ausência de edição parcial, intervalos exatos, flush e idempotência.
- `python3 tests/test_stage_parse_managed.py`: 203 casos comparando execução
  do IL original extraído do APK com o IL modificado. As instruções originais
  são reemitidas com seus branches e tokens; XmlReader e reflexão são reais.
  Os tipos Unity e containers de jogo usam fixtures. ReadVector3 e
  ReadUserParameters também executam o IL original com tokens relocados.
- Conferidos: ordem e conteúdo dos objetos, atributos omitidos/defaults,
  campos e tipos personalizados, vetores, duas culturas, exceções, limites
  de 1/15/16/17/31/32/33/128/513 objetos, coleta entre yields, publicação
  somente da camada completa e Dispose durante um lote incompleto.
- Verificado limite de 16 chamadas por MoveNext. O gerador reproduz o header
  de produção byte a byte a partir dos corpos originais.
- Regressões aprovadas: terrain_gc, spawn_il, stage_il, race_warmup,
  probe_schedule, build_failure, Rijndael nativo/wrapper, managed_trace e
  wrapper de geometria. O harness antigo de stage_il teve seu intervalo de
  extração restringido à função testada e aceita métodos fora da fixture
  como ausentes; não houve mudança adicional nessas otimizações de produção.
- Build Release VitaSDK concluída sem avisos nesta recompilação.
  `git diff --check` passou. ZIP íntegro, eboot conferido e APP_VER 00.70.

## Pacote e teste no aparelho

`build/VPK/SRR-Vita-v0.0.70.vpk`, 4597930 bytes, oito arquivos.
SHA-256: `1964a8aab3103623a746e3c9fd09294b0f3c56a1796c4a2e6034009c3be68146`.
O pacote não inclui APK, bibliotecas do jogo, fixtures ou logs.

Sem medição desta build no Vita nesta entrega. Repetir a corrida longa,
confirmar a mensagem de instalação e comparar os picos de
LoadSetTable/CreateSetData/ReadLayer e P50/P95/P99 de quadro. Verificar se
os trechos, objetos e colisões continuam disponíveis a tempo na velocidade
alta. Os picos de GC e o custo contínuo dos demais sistemas não são resolvidos
por esta alteração; não há garantia de 30 FPS constantes.
