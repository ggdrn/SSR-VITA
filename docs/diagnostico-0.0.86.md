# 0.0.86 — CBC nativo na leitura dos dados

## Sessão 0.0.85

O log confirma QHD 960×544, Graphics 1 (Default), saída limitada a 30 FPS e
Material Shaders On. A amostra foi preservada em
`build/analysis-0.0.86/srr_log_input.txt`, com relatório em `report/report.md`
e recortes numéricos em `baseline.json`.

Há várias entradas em pista e retornos ao menu. Para evitar misturar telas
paradas/resultados com corrida, os recortes longos usam apenas janelas
completas de 600 quadros, depois de 300 quadros da última carga observada e
até 100 quadros antes de postGameResults. São aproximações pelas transições
do log, não uma identificação de cada estado de gameplay:

| Recorte | Janelas completas | FPS aproximado |
|---|---:|---:|
| Corrida 4 (quadros 7246–12685) | 7 | 29,47 |
| Corrida 5 (15826–18572) | 3 | 29,93 |
| Corrida 6 (23791–28682) | 6 | 29,12 |

O cálculo usa média harmônica ponderada dos FPS arredondados dos blocos.
Não é uma comparação controlada com a sessão anterior: pistas e ações podem
diferir. As esperas importadas nos blocos estáveis são próximas de zero e
há janelas próximas do teto de apresentação. Permanecem picos de terreno,
por exemplo 277,31 ms no quadro 18089 e 276,29 ms no quadro 25905, com cerca
de 259 ms em `LoadSetTable.MoveNext`. UIPanel também permanece caro em vários
quadros. Tempos de callbacks são inclusivos e não são medidas de uso da GPU.

O contador de blocos Rijndael nativos chegou a 102711 na sessão. A inspeção
do APK mostra que CreateSetData chama AESCrypt.Decrypt, que usa um transform
e CryptoStream para ler os dados criptografados antes de interpretar o XML.
A preparação de shader/JIT e o ajuste de cadência não removem esse trabalho.

## Alteração

A primitiva Rijndael-256 de cada bloco já estava em C. A etapa CBC ainda
executava em Mono duas chamadas BlockCopy, o despacho ECB e um laço de XOR
byte a byte para cada bloco de 32 bytes. A nova especialização executa esses
passos em C e compartilha as mesmas tabelas da primitiva já instalada.

O CBC original salva o ciphertext no buffer temp2, descriptografa, aplica
XOR com o IV temp e copia temp2 para temp. O caminho novo preserva os dois
buffers completos, os bytes fora dos primeiros 32 e a possibilidade de
entrada/saída usarem o mesmo array. Não mantém cache nem cria alocações por
bloco. Expansão de chave, CryptoStream, último bloco, padding, decodificação
de texto, XML e publicação das camadas continuam no código original.

A instalação exige a mesma mscorlib do APK (MVID), os corpos originais
completos de CBC e ECB, a relação de herança e os campos resolvidos pela API
Mono. Também exige a primitiva nativa já instalada e um prólogo ARM
compatível para o trampoline permanente. Não altera o IL de CBC/ECB.

Em execução, só aceita a classe exata RijndaelTransform em descriptografia,
bloco de 32 bytes, Nb=8, Nr=14, arrays de bytes/chaves com classes e limites
válidos, e buffers de estado distintos dos dados. Casos diferentes usam o
CBC original, inclusive outros cifradores, criptografia, objetos derivados,
arrays incompletos, transform descartado e aliases incomuns. Falhas de
validação não escrevem nos buffers. O arquivo
`ux0:data/srr/original_rijndael_cbc` desativa essa especialização no próximo
lançamento para comparação.

O log inicial informa a instalação; a cada 600 quadros,
`stage: native CBC blocks ... managed fallbacks ...` mostra se o caminho foi
usado. O contador anterior de blocos cobre o hook antigo: blocos processados
pelo CBC novo têm seu próprio contador e não são contados duas vezes.

QHD, Default, shaders, efeitos, limite de saída e a cadência da 0.0.85 foram
preservados. A ordenação restaurada na 0.0.84 continua intacta.

## Validação

- 4096 blocos CBC encadeados comparados byte a byte com execução do CBC e
  Decrypt256 originais da mscorlib do APK: saída, IV, scratch, caudas e
  entrada/saída no mesmo buffer.
- 4096 comparações da primitiva de bloco com o IL original.
- Instalador CBC e dispatcher testados com APIs/layouts/MVID/IL divergentes,
  falhas de alocação/compilação, modo/tamanho/rodadas/classes diferentes,
  trampoline e encaminhamento ao original.
- Helper de arrays testado com classes/limites/aliases inválidos e ausência
  da primitiva, sem mutações parciais. Harnesses C com ASan/UBSan.
- Testes de cadência, gate do Sort legado e parser XML gerenciado passaram.
- APIs necessárias verificadas nos exports do libmono do APK. Build VitaSDK
  sem avisos; ZIP, identidade do eboot e diff verificados.

Pacote: `build/VPK/SRR-Vita-v0.0.86.vpk`.

**Ainda não há medição de ganho no Vita nem confirmação de 30 FPS fixos.**
Comparar uma corrida na mesma configuração, uso efetivo do CBC nativo e
picos de LoadSetTable. Esta alteração não elimina todo o custo da interface
ou do carregamento de pista.
