# SRR Vita 0.0.65 — tela de amiguinhos

## Captura de referência

Análise do log recebido da **0.0.64**, build **20261101T170546Z**.

- Quadro 58496: render **7163,76 ms**, callbacks gerenciados **7103,12 ms**
  inclusivos e maior chamada individual `InitView.MoveNext` **3002,11 ms**.
  As 116 chamadas de instantiate somaram 425,68 ms; JIT 105,91 ms; sem GC
  observado nesse quadro. Esses tempos podem ser aninhados.
- Na tela estabilizada, a janela de 600 renders encerrada no quadro 60000 teve
  média **42,34 ms** e máximo 47,40 ms. Janelas OBS repetem P50 de 43 ms,
  aproximadamente **23 FPS**.
- `ui_chao_set_cell.Update` chegou a **207,93 ms** no quadro 58866.
  `UIPanel.LateUpdate` chegou a **340,36 ms** no quadro 60572, sem GC observado.
- Quadros lentos nessa tela reportam aproximadamente 1100–1200 callbacks externos
  medidos. Não foi isolado o custo da própria instrumentação.

## Mudanças desta build

### Retorno ocioso das células

O bridge Unity → Mono reconhece o método original `ui_chao_set_cell.Update`
quando a imagem, o IL completo e o layout dos campos são os suportados pelo APK.
Para a classe exata, com imagem carregada, estado de exibição concluído e os dois
atrasos sem trabalho pendente, o bridge retorna diretamente o resultado void.
O Update original foi verificado: nesse estado ele percorre apenas leituras e
branches até `ret`, sem chamadas nem escritas.

A checagem aceita somente o MVID esperado, os **968 bytes exatos** do método,
campos boolean/float com offsets dentro da instância e a classe exata do objeto.
Um atraso positivo, carregamento incompleto, mudança de estado ou classe diferente
encaminha a chamada original. Argumentos, resultado e exceções das chamadas
encaminhadas permanecem. O retorno ocioso limpa o ponteiro de exceção, como uma
invocação bem-sucedida de um método void. Nenhum código JIT é sobrescrito.

Contadores cumulativos a cada 600 quadros:

```text
ui: buddy idle skips N active forwards N
```

### Instrumentação dos menus

Em `MainMenu` fora do loading observado, a medição é focada em
`UIPanel.LateUpdate`, `ui_chao_set_cell.Update`, `UICamera.Update` e na coroutine
original `ChaoSetUI.InitView.MoveNext`. A cada 32 quadros há uma amostra completa
dos demais callbacks. Todos continuam sendo executados; somente as consultas de
relógio/thread dos callbacks fora do foco são reduzidas.

A medição de quadro e os timers de assets, I/O, GC e JIT continuam ativos.
Carregamentos e outras cenas conservam a cobertura ampla anterior; a corrida
conserva o foco existente no UIPanel. Se os métodos necessários não forem
resolvidos, a cobertura completa dos menus é mantida.

O total e o maior callback reportados em quadros focados cobrem apenas os métodos
medidos, portanto não devem ser comparados com os totais de cobertura ampla como
se fossem uma redução equivalente do tempo de CPU do jogo. O tempo real de quadro
permanece a referência para medir ganho.

### Comparação com o caminho anterior

Criar o arquivo vazio **`ux0:data/srr/original_buddy_ui`** e reiniciar desativa as
novas duas otimizações. Remover o arquivo e reiniciar as habilita novamente.
A mensagem de instalação e os contadores indicam qual caminho foi usado.

## Validação

- ASan/UBSan: instalação única, recusa de MVID/IL/tipo/offset incompatíveis,
  APIs ausentes, marcador de rollback e encaminhamento dos estados ativos.
- ASan/UBSan: ambos os wrappers Mono, argumentos/resultados/exceções, ausência de
  relógios/chamadas Mono no retorno ocioso, foco e amostragem a cada 32 quadros,
  cobertura completa com configuração ausente e preservação do foco da corrida.
- IL do APK original comparado byte a byte com a guarda. Interpretação dos caminhos
  de **150 estados ociosos**, incluindo NaNs, infinitos, lock de carga e IDs,
  confirma chegada a `ret` sem chamada, escrita ou alocação.
- Regressões: atribuição de trabalho por thread, agenda dos probes, geometria NGUI,
  wrapper de geometria e guardas de Trim.

Comandos dos novos testes:

```sh
python3 tests/test_buddy_ui.py
python3 tests/test_buddy_il.py
python3 tests/test_managed_trace.py
```

O teste de IL requer .NET 10 e o APK local em `../srr224.apk`; a variável
`SRR_TEST_APK` permite indicar outro caminho. Ele lê metadados, sem executar ou
modificar os assemblies do jogo.

## Próxima medição no Vita

Sem medição de FPS no aparelho nesta entrega. Comparar o mesmo inventário com e
sem `original_buddy_ui`: primeira abertura, reabertura, tela parada, rolagem,
ordenação, equipar/remover e retorno ao menu. Conferir ícones, placeholders e
seleção; observar os contadores e P50/P95/P99 de quadro.

A montagem inicial da grade conserva a sequência de criação original. Os 7,16 s
não estão resolvidos por esta build. Reutilizar células e criar a lista em lotes
exige uma alteração posterior no ciclo de vida da UI e validação de inventário,
ordenação e rolagem. Também permanece a necessidade de separar UpdateWidgets,
Fill e upload de Mesh para escolher a próxima otimização do UIPanel.

## Artefato gerado

- Build Release VitaSDK concluída; versão de teste **0.0.65** (modo de release pública desativado).
- VPK: **`build/VPK/SRR-Vita-v0.0.65.vpk`**.
- Tamanho: **4591852 bytes**.
- SHA-256: **`f3b68fb7239f5e5a3103ffe9c58d69ded87e44dc71133bf1ece36279b9c0d800`**.
- SFO: APP_VER **00.65**, TITLE_ID **SRRV00001**.
- ZIP íntegro; eboot e os recursos visuais conferidos contra os arquivos atuais.
- O pacote contém apenas os oito arquivos do port; nenhum APK, biblioteca original ou log foi incluído.
- As APIs Mono usadas pelo caminho rápido estão exportadas no libmono original do APK.
- Compilação dos módulos novos sem avisos; o build conserva avisos anteriores de truncamento em outros módulos.
