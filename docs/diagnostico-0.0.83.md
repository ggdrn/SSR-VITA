# 0.0.83 — CPU da interface e preparação do resultado

**Versão com regressão confirmada no Vita:** a ordenação introduzida aqui
causa `InvalidProgramException` e tela cinza. Usar a 0.0.84; ver
[diagnóstico da correção](diagnostico-0.0.84.md). Os resultados abaixo são
históricos de host e não comprovam compatibilidade com o Mono do jogo.

## Evidência e objetivo

Manter QHD 960×544, Graphics Default e Material Shaders On. Esta entrega não
reduz qualidade, resolução, shaders ou efeitos. O alvo de FPS continua sendo
a configuração existente do usuário; selecionar 30 FPS no menu.

Na primeira leitura do log fornecido (QHD/30), o quadro 10900 levou 1190,72 ms,
com 231,80 ms de JIT e `GameResultScores.Setup` como método de compilação mais
caro. Os tempos são aninhados, não somáveis. `UIPanel.LateUpdate` apareceu
repetidamente entre os callbacks mais caros. A janela terminando no quadro
11054 teve 26 quadros, mediana de 166 ms e todos acima de 100 ms, durante a
sequência de resultado. Durante a corrida, janelas de 600 renders tiveram
médias de aproximadamente 33–37 ms e picos; 35 ms equivalem a cerca de 28,6 FPS.

O arquivo `build/VPK/srr_log.txt` foi substituído durante a investigação por
um log da 0.0.80 iniciando em HD/60 FPS. Esses números vieram da leitura
anterior; o novo arquivo não serve de comparação QHD/30. A identificação
exata da janela da experiência exige nova captura correlacionada ao teste.

## Alterações

`BetterList<T>.Sort` usa inserção estável por deslocamentos. A implementação
anterior usava trocas adjacentes (gnome sort), repetindo comparações e
escrevendo duas posições por troca. A nova versão guarda o elemento,
desloca predecessores e o insere uma vez. Preserva empates, buffer,
capacidade, tamanho, assinatura e locais. Não aloca arrays. O pior caso
continua quadrático.

Para 1024 elementos invertidos, o teste de host contou 1.046.529 chamadas ao
comparador na substituição anterior e 523.776 na nova. Isso mede operações,
não FPS no Vita. O comparador de widgets considera painel e profundidade.

`UIPanel.UpdateWidgets`, após o teste original de `enabled`, verifica se o
campo do painel aponta para a própria instância que está atualizando. Nesse
caso evita o getter e a igualdade nativa redundantes. Referências diferentes
seguem a sequência original, incluindo criação tardia de painel e tratamento
de wrappers destruídos/alternativos. Geometria, culling, callbacks e draw
calls continuam sendo atualizados. Não há cache entre quadros nem salto de
animação.

Os dois patches exigem MVID, corpo IL original completo, tamanho e maxstack
compatíveis. Divergências mantêm o original. Os arquivos
`ux0:data/srr/original_ui_sort` e `ux0:data/srr/original_ui_panel` permitem
comparação isolada com os métodos originais no próximo lançamento.
O gerador das substituições está em `scripts/gen_ui_hotpaths.py`.

A preparação JIT passa de 3845 para 4542 métodos, incluindo `GameResult*`,
`HudResult*`, `GlowUp*`, interpolação da experiência, continuação, mensagens e
criação de conjuntos usados durante a corrida. Continua no carregamento da
primeira corrida, sem invocar métodos, criar objetos ou conceder recompensas.
Preserva MVID, tamanho IL, exclusão de inicializadores estáticos e reserva
de 64 MiB de memória nativa. O carregamento pode ficar maior e mais código
nativo permanece residente. Não elimina carregamento de assets ou trabalho
de interface.

## Validação e limites

Passaram 11 testes: instalação/rollback/guards de Sort e UpdateWidgets,
1400 casos de ordenação gerenciada, comparação com a implementação anterior,
30.000 visitas de widgets no IL original e modificado (donos nulos,
destruídos, wrappers alternativos e reparenting durante callback), preparação
JIT, cópias de geometria, Trim, despacho de callbacks, menu e efeitos de
coleta. Os harnesses C executaram com ASan/UBSan.

Build VitaSDK concluído sem avisos; integridade ZIP e identidade do eboot
empacotado conferidas. Ainda não há medição desta versão em hardware:
**30 FPS fixos e a melhora da barra de experiência não estão confirmados**.

Pacote: `build/VPK/SRR-Vita-v0.0.83.vpk`.

No Vita, manter QHD/Default/Shaders On, selecionar 30 FPS e repetir
pré-corrida, corrida completa, resultado, experiência e retorno ao menu.
Comparar também o tempo de carregamento. Copiar o log antes de outra sessão
para preservar a configuração e a amostra.
