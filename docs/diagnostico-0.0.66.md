# SRR Vita 0.0.66 — ordenação da interface

Captura recebida: 0.0.65, build 20261103T034804Z. As otimizações anteriores
estão instaladas no log, incluindo o retorno ocioso das células e cópias de
geometria. Os arquivos já modificados no checkout foram preservados.

## Evidência do log

- Abertura de ChaoSetUIPage no quadro 45217: render 7620,72 ms;
  InitView.MoveNext individual 4125,21 ms, instantiate total 1641,22 ms
  (116 chamadas), GC 1206,66 ms. Tempos inclusivos/aninhados: não somar.
- UIPanel.LateUpdate apresenta picos de 263,56 e 249,40 ms
  durante essa visita à tela. Há picos de UI mesmo sem GC.
- Na primeira visita à tela, as janelas terminadas em 29116 e 29191
  apresentam P50 de 44 e 43 ms, aproximadamente 23 FPS. Outras janelas
  incluem carregamento de ícones, interação e picos muito maiores.
- O log mede LateUpdate inclusivo: ele não isola o custo de Sort, Fill,
  UpdateWidgets ou dos uploads. A participação da ordenação nos picos
  ainda é hipótese a validar no aparelho.

## Alteração

BetterList<T>.Sort original usa bubble sort: a cada troca repete outra
varredura completa. UIPanel.LateUpdate chama esse método ao reconstruir
a lista global de widgets, inclusive durante a chegada de novos ícones.

O método passa a usar gnome sort: compara vizinhos e, ao trocar, recua
até reinserir o item. É estável, não aloca, não altera o tamanho nem a
capacidade das listas e usa os mesmos locais e o mesmo comparador.
Beneficia também outros consumidores de BetterList.Sort. Continua
quadrático no pior caso; não é uma solução completa para o custo da UI.
A sequência e o número de chamadas ao comparador mudam. O contrato
pressupõe um comparador consistente; não preserva efeitos colaterais de
comparadores nem a disposição parcial de uma lista após exceção.

O patch exige MVID, corpo IL completo (133 bytes), stack e APIs esperados.
É aplicado ao IL para JIT futuro, sem sobrescrever código nativo existente.
Criar `ux0:data/srr/original_ui_sort` e reiniciar mantém a ordenação original.
A mensagem `BetterList stable adjacent sort installed` confirma instalação.

## Validação

- `python3 tests/test_ui_sort.py`: ASan/UBSan, MVID/IL/tamanho/stack/API/header,
  marcador de rollback, caches e ausência de reaplicação.
- `python3 tests/test_ui_sort_managed.py`: executa IL original e modificado
  do APK em .NET 10; 1400 casos com valores, referências, nulos, empates,
  ordem inversa, ordem aleatória e capacidade excedente. Verifica os locais
  do método, igualdade dos resultados e identidade/tamanho dos buffers.
- Lista de 1024 itens com último item deslocado para o início:
  1047552 -> 3067 comparações. É um cenário sintético, não uma medição
  de FPS nem uma afirmação de redução equivalente no tempo de quadro.
- Lista invertida: quantidade de comparações limitada ao custo original
  acrescido de no máximo N comparações.
- Regressões aprovadas: Trim nativo/gerenciado, buddy guard/IL, managed trace,
  geometria nativa/wrapper, perf IL, agenda de probes e falha de build.
- Build Release VitaSDK concluída. ZIP íntegro e eboot conferido.

## Pacote

`build/VPK/SRR-Vita-v0.0.66.vpk`, 4592354 bytes.

SHA-256: `56a05614b7027598a5e2c2838b7bcd82bbeedfde96b08eacd44bfe43ef157821`.
SFO APP_VER 00.66, TITLE_ID SRRV00001. Oito arquivos; nenhum APK ou log.

Sem execução no Vita nesta entrega. Comparar a mesma tela/inventário com
marcador presente/ausente: abertura, carregamento dos ícones, tela parada,
rolagem, ordenação, equipar e retornar ao menu. Verificar ordem dos empates,
seleção, placeholders e P50/P95/P99. Ainda é necessário medir o ganho real,
isolar os subcustos do painel e trabalhar na criação inicial das 116 células.
Não há garantia de 30 FPS nesta versão.
