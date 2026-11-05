# 0.0.85 — cadência de apresentação e medição da corrida

## Corrida medida na 0.0.84

O log confirma QHD 960×544, Graphics 1 (Default), limite de 30 FPS e Material
Shaders On. Foi copiado antes da análise para
`build/analysis-0.0.85/srr_log_input.txt`; o relatório e o resumo numérico
estão no mesmo diretório (`report/report.md` e `baseline.json`).

No recorte de janelas terminando nos quadros 6212–10058, anterior ao resultado:

- 3995 quadros em 140,321459 segundos: **28,47 FPS observados**;
- 121 quadros acima de 50 ms e 13 acima de 100 ms;
- pior quadro: 269,073 ms;
- médias de blocos de 600 renders: 33,48–34,88 ms. Essas médias excluem parte
  do trabalho fora de nativeRender; por isso o FPS observado pelo relógio é
  menor que o inverso dessas médias.

O recorte é aproximado nas bordas e não inclui o carregamento inicial nem a
sequência de resultado. A imagem voltou e não há a InvalidProgramException
da 0.0.83. A ordenação restaurada na 0.0.84 permanece intacta.

As janelas da corrida não registram GC nem chamadas ao compilador de shaders.
Há JIT residual, sobretudo no começo. Nos picos posteriores aparecem callbacks
de terreno, como `LoadSetTable.MoveNext`, e UIPanel. Exemplos: quadro 7498,
264,84 ms, com 241 ms no callback de carregamento; quadro 9829, 269,02 ms,
com 242,12 ms no callback de carregamento. Os tempos são aninhados.
Não é correto atribuir todos esses picos à preparação de shaders ou à GPU.
Os timers de draw/present medem APIs de CPU, não a execução da GPU.

## Ajuste de cadência

A versão anterior aplicava dois limites de 30: intervalo de apresentação 2
no vitaGL e Application.targetFrameRate=30 na Unity. O log registra esperas
em `libunity.so+0x319BD8`; a inspeção do binário do APK confirma que esse
retorno sucede uma chamada a nanosleep. Isso comprova a presença das esperas,
mas ainda não prova que os dois limitadores causam toda a perda de FPS.

O ajuste mantém o intervalo de apresentação selecionado (2 para 30, 1 para
60) e usa 60 como alvo do escalonador da Unity em ambos os casos. A hipótese
é evitar o segundo orçamento de espera de 33 ms enquanto a fila de
apresentação já limita a saída a 30. Não se remove a sincronização vertical,
nem se alteram resolução, qualidade, materiais, efeitos ou shaders.
A opção de 60 continua com o alvo interno e intervalo anteriores.

O log distingue agora `engine targetFrameRate` de `display cap`. O alvo
interno de 60 não significa que a saída configurada para 30 passou a 60.
A consulta continua usando icalls verificados da Unity, sem alocações
managed, e reaplica o alvo após resets do jogo.

Este ajuste não divide nem elimina o trabalho de carregamento de terreno
que ocupou 200–269 ms. Para não misturar causas, o parser e a criptografia
permanecem como estavam. Também não se amplia novamente a lista JIT:
a versão anterior já compilou todos os 4542 métodos em 9857,63 ms antes
da corrida, e isso não resolveu o custo contínuo observado.

## Medições adicionadas

A cada 600 quadros, `perf: window frames` informa FPS calculado pelo relógio
real, média do tempo de quadro instrumentado, quantidade acima do orçamento
e médias das esperas importadas e APIs de desenho/apresentação/textura/link.
As métricas se sobrepõem e não devem ser somadas como componentes
independentes de CPU/GPU. O FPS por relógio inclui o tempo entre relatórios;
a média de quadro instrumentado tem cobertura menor.

O relatório de picos passa de >100 ms para >50 ms, conservando a amostragem
limitada (primeiros oito e potências de dois por janela de 600), para
identificar também as quedas frequentes menores.

## Validação e próximo teste

Testes passaram para alvo interno e intervalo real de apresentação 30/60,
resets do jogo, APIs ausentes/estrangeiras, menu/configuração, ordenação
restaurada e gate contra o IL rejeitado, preparação JIT, callbacks managed e
relatório de observação. Os harnesses C usam ASan/UBSan. Build VitaSDK sem
avisos, integridade ZIP/eboot e diff verificados.

Pacote: `build/VPK/SRR-Vita-v0.0.85.vpk`.

**Ganho de FPS ainda não medido no Vita. Não há confirmação de 30 fixos.**
Repetir uma corrida com QHD/Default/Shaders On e 30 FPS, comparando o FPS
observado, os quadros acima de 50/100 ms e as médias de esperas com o baseline
preservado. Como o alvo de saída é 30, a meta é sustentá-lo, não excedê-lo.
