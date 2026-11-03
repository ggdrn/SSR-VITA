# SRR Vita 0.0.67 — pausas na corrida longa

## Captura

Log recebido da 0.0.66, build 20261103T142436Z. Original preservado em
`build/diagnostico-0.0.67-baseline/srr_log.txt`; relatório OBS na subpasta
`report/`. SHA-256 do log:
`a5e37139f458645b3ddd57c92149fbf4b7db0ab30abb39f5fbe7d7151bc8d41c`.
O relatório leu 6935 eventos e 64 estados observados.

O usuário relata melhora na tela de amiguinhos e corrida aceitável até
chegar mais longe. A captura continua após as pausas e chega ao envio de
resultados HTTP 200 e novos inputs: não há evidência de congelamento
permanente nessas pausas. Isso não prova ausência de outros problemas.

## Evidências na corrida mais longa

Na fase 51 (cena s_playingstage), há períodos com P50 de 20–36 ms. A janela
terminada no quadro 17049 tem P50 19 ms, P95 110 ms, P99 360 ms e 10
quadros acima de 100 ms. O problema inclui pausas isoladas, não apenas
uma média baixa de FPS. A cena também hospeda início e resultado; suas
estatísticas não são um benchmark exclusivamente de gameplay.

| Quadro | Render | GC | UIPanel individual | Outros dados |
|---|---:|---:|---:|---|
| 12721 | 502,38 ms | 0 | 4,33 ms | I/O e waits medidos zero |
| 14231 | 325,21 ms | 0 | 4,04 ms | I/O e waits medidos zero |
| 14236 | 312,19 ms | 0 | 4,09 ms | I/O e waits medidos zero |
| 16894 | 359,04 ms | 0 | 3,39 ms | I/O e waits medidos zero |
| 16899 | 367,44 ms | 0 | 3,35 ms | I/O e waits medidos zero |
| 17685 | 509,83 ms | 477,77 ms | 6,48 ms | GC mark 382,95 ms, reclaim 80,00 ms |

Nesses quadros não há tempo de JIT reportado, e os tempos de draw,
textura, present e link registrados não explicam as pausas. Timings
aninhados não devem ser somados como custos independentes.

O foco de instrumentação da 0.0.66 na corrida mede apenas UIPanel. Os
3–6 ms reportados como callbacks nesses quadros NÃO representam todo
o trabalho gerenciado. Não é possível identificar o callback responsável
pelas pausas sem GC a partir desse log. Atribuí-las ao terreno, ao spawn
ou a vazamento seria uma hipótese sem confirmação.

## Alteração diagnóstica

A 0.0.67 preserva o foco leve no UIPanel e acrescenta:

- Uma amostra completa dos callbacks a cada 32 quadros também na corrida.
- Após um nativeRender acima de 100 ms durante o foco da corrida, os
  próximos 120 quadros têm cobertura completa. Outra pausa renova o prazo.
- Sair do foco da corrida cancela a janela; menus mantêm a amostragem anterior.

A captura começa no quadro seguinte ao gatilho: não recupera o callback
que já passou. A amostra periódica pode capturá-lo diretamente, e a janela
aumenta a chance de identificar as pausas repetidas próximas, como 16894
seguido de 16899. As chamadas continuam executando os mesmos métodos,
com os mesmos argumentos, resultados e exceções. Os wrappers não passam
a alocar ou registrar mensagens por callback.

Cobertura completa acrescenta consultas de relógio/thread e contabilização;
pode afetar o tempo durante as amostras. Totais de callbacks com cobertura
completa não devem ser comparados diretamente aos totais focados anteriores.
O tempo completo de quadro continua sendo a referência de desempenho.

Não foi alterada a política de GC, a criação de terreno ou a lógica do jogo.
Esta build busca identificar o gargalo restante; não representa uma correção
confirmada das pausas e não tem medição no Vita nesta entrega.

## Validação e pacote

Testes aprovados: managed_trace (ASan/UBSan, argumentos/resultados/exceções,
workers, chamadas aninhadas, amostragem, limiar exato de 100 ms, duração de
120 quadros, renovação, cancelamento e retorno ocioso); work_trace;
buddy_ui; probe_schedule. Build Release VitaSDK sem avisos nesta recompilação.
`git diff --check` passou. ZIP íntegro, oito arquivos e eboot conferido
contra o build atual.

`build/VPK/SRR-Vita-v0.0.67.vpk`, 4592454 bytes.
SHA-256: `1fe2f6d43a3098970dc5496eb6c8f51217fdfcff0015535890da405195dcea98`.

Próxima captura: repetir uma corrida longa com esse pacote e enviar o
`srr_log.txt` após as pausas. O novo log deverá permitir separar melhor
os callbacks do jogo dos picos de GC já identificados.
