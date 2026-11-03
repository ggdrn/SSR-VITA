# SRR Vita 0.0.68 — decodificação nativa de dados da corrida

## Captura de referência

Log da 0.0.67, build 20261103T160849Z, preservado em
`build/diagnostico-0.0.68-baseline/srr_log.txt`. Relatório OBS na subpasta
`report/`: 7848 eventos, 52 estados observados. SHA-256 do log:
`7b923b22b8672d28fe72932f5cbc95dcf8251842ffeba0a1f70eb28010830102`.

A captura ampliada identificou mais trabalho dentro dos callbacks do jogo:

- Quadro 21826: render 327,16 ms, TinyFsmBehavior.Update individual 307,85 ms.
- Quadro 21831: render 369,94 ms, LoadSetTable.MoveNext individual 347,35 ms.
- Quadros 21827–21830: CreateSetData.MoveNext individual 90,90, 89,69,
  95,47 e 79,70 ms, respectivamente.
- Quadro 16356: render 692,47 ms, GC 468,93 ms. Esse pico não é explicado
  somente pelo carregamento dos trechos.
- Quadros 10338, 15539 e 23706: HudContinue.Update 1017,45, 782,77 e
  777,15 ms. A cena de corrida também contém a transição para resultados;
  não tratar esses picos como custo contínuo da corrida.
- Quadro 11466: TinyFsmBehavior.Update 2619,73 ms, instantiate total
  529,10 ms / 898 chamadas, durante preparação (loading observado).

Os tempos são inclusivos/aninhados e não devem ser somados como se fossem
operações independentes. O tipo TinyFsmBehavior é compartilhado por vários
comportamentos; seu nome não identifica sozinho qual estado consumiu CPU.

## Alvo e limites da otimização

O IL original de SpawnableParser.CreateSetData.MoveNext chama
AESCrypt.Decrypt para decodificar o texto de cada grupo de trechos antes
de iniciar XmlReader e suas coroutines por camada. AESCrypt usa o
RijndaelManaged original, com bloco de 256 bits. A rotina de bloco
RijndaelTransform.Decrypt256 contém 6490 bytes de IL, com muitos acessos
verificados a arrays e tabelas.

A 0.0.68 substitui somente esse cálculo de bloco por C nativo, com os
mesmos deslocamentos de linhas, tabelas inversas e 120 palavras da chave
expandida do próprio Mono. A expansão de chave, CBC, padding, conversão
de texto e tratamento de erros continuam no caminho original. Não foi
adicionado cache de textos ou de chaves, nem modificado o arquivo APK.

O custo individual de Decrypt256 não foi isolado no log recebido: sua
participação no carregamento vem da inspeção do IL. A redução do custo
de decodificação é o objetivo desta alteração; quanto isso reduz os
347 ms do carregador precisa ser medido no Vita. Parsing XML, criação de
objetos, tela de continuar e GC ainda podem gerar pausas.

## Instalação e fallback

A instalação exige o MVID suportado de mscorlib e comparação dos 6490
bytes exatos de Decrypt256. Copia as tabelas originais após inicializar a
classe; verifica os cinco arrays, comprimentos, classes e deslocamento
do payload. Confere a entrada ARM alinhada e aceita apenas duas instruções
de prólogo independentes do PC para o trampoline permanente de fallback.

Cada chamada exige a classe exata do objeto, byte[] de entrada/saída
com pelo menos 32 bytes e uint[] com pelo menos 120 palavras. Casos fora
desses requisitos continuam na rotina original. Buffers maiores mantêm
inalterados todos os bytes após os primeiros 32. Operação in-place funciona.
Os contadores usam operações atômicas e não há relógio/log/alocação por bloco.

Mensagem esperada:
`stage: Rijndael256 native blocks installed (original keys/CBC/padding/text preserved)`.
A cada 600 quadros: `stage: Rijndael256 native blocks N original fallbacks N`.
Se a guarda de prólogo ou qualquer outra falhar, o log indica que preservou
a implementação original. Criar `ux0:data/srr/original_rijndael256` e reiniciar
desativa a otimização para comparação.

## Validação

- `python3 tests/test_rijndael256.py`: executa o IL real de Decrypt256 extraído
  do mscorlib do APK em DynamicMethod, relocando apenas os tokens das cinco
  tabelas estáticas para campos de teste. Locais e instruções são originais.
  4096 entradas, chaves expandidas e tabelas aleatórias; saída C idêntica
  byte a byte, incluindo in-place, com ASan/UBSan e limites de escrita.
- `python3 tests/test_rijndael256_wrapper.py`: ASan/UBSan, instalação,
  MVID/IL/API/header/tamanho/tabelas/layout/prólogo incompatíveis, marcador,
  idempotência, tipos/comprimentos/nulos e encaminhamento ao fallback.
- As 15 APIs Mono necessárias foram conferidas na tabela de símbolos
  exportados do libmono original do APK.
- Regressões aprovadas: terrain_gc, spawn_il, managed_trace, race_warmup,
  geometria/wrapper, ordenação da UI nativa/gerenciada, native_heap e gc_trace.
- `test_gc_policy.py` falhou em uma expectativa já incompatível com o código
  existente: exige divisor 2 com heap 55 MiB e 12 MiB livres; a política
  existente exige ao menos 55/4 = 13,75 MiB para entrar nesse estado.
  Esses dois arquivos não foram alterados nesta tarefa. Não foi ajustada
  a política de memória para satisfazer esse teste.
- Build Release VitaSDK concluída sem avisos nesta recompilação.
  `git diff --check` passou; ZIP íntegro e eboot conferido contra o build.

## Artefato e próximo teste

`build/VPK/SRR-Vita-v0.0.68.vpk`, 4596806 bytes, oito arquivos.
SHA-256: `cb1c59bb329c8e5e667c6e6d44a6c646915578672622184fb5e6374388cf5429`.

Sem execução desta versão no Vita nesta entrega. Repetir a mesma corrida
longa, observar instalação e aumento dos contadores de blocos nativos,
e comparar os picos LoadSetTable/CreateSetData e os tempos completos de
quadro. Uma comparação com marcador presente/ausente separa o efeito desta
alteração de diferenças de inventário, trecho ou cache da sessão.
