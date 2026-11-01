# SRR Vita 0.0.60 — dados completos dos personagens, UI em bloco e splash proporcional

## Material analisado

Log 0.0.59, build `20261030T195256Z`: `build/VPK/srr_log.txt`, 20855 linhas, SHA-256 `1f98a879ef883de2ad91466c5cc9ebeed1d47f01d3a079fe0726c645fb1115fb`.
Cópia preservada em `build/perf-baselines/srr-0.0.59-main.log`; relatórios em `build/analysis-0.0.59/`, incluindo `spikes.csv` e a inspeção do IL do NGUI.

Imagens conferidas: `dd/2026-10-30-165924.jpg` (LiveArea), `kg/2026-10-30-171213.jpg` e `fe/2026-10-30-171933.jpg` (gameplay). As originais não foram alteradas.

A captura tem 12829 eventos OBS válidos, 126 estados observados e cobertura parcial. A cena `s_playingstage` inclui carregamentos, pausas e resultados; seus totais não representam exclusivamente corrida ativa.

## Pausas de 105–360 ms: o que o medidor encontrou

O medidor da 0.0.59 encontrou `UIPanel.LateUpdate` como o callback mais demorado em **103 dos 155 picos amostrados** no estado observado `s_playingstage loading=0`. Em 92 desses casos não há JIT nem GC registrado. Isso identifica uma origem recorrente; não explica todo o tempo de todos os quadros (alguns dos picos de transição têm pouco tempo dentro dos callbacks).

Exemplos sem JIT/GC:

| Quadro | Render | Maior callback UIPanel.LateUpdate |
|---|---:|---:|
| 6768 | 131,56 ms | 102,46 ms |
| 8567 | 202,03 ms | 153,89 ms |
| 8569 | 134,05 ms | 96,01 ms |
| 8664 | 130,94 ms | 106,82 ms |
| 22276 | 214,85 ms | 158,89 ms |

Também aparecem `<CreateSetData>c__IteratorC3.MoveNext`, `TinyFsmBehavior.Update`, `<LoadSetTable>c__Iterator51.MoveNext` e `HudContinue.Update`. São categorias distintas: gerar/carregar blocos e abrir interfaces também pesa. Tempo de callback é inclusivo e pode conter chamadas nativas; não deve ser somado às outras categorias como parcela independente.

O cache termina com **1673 hits, zero misses, zero erros e zero compilações**. Compilar shaders não explica essas pausas. Os quatro núcleos e clocks 444/222/222/166 MHz continuam ativos.

### Otimização aplicada à UI

A inspeção do IL identificou um trecho substituível sem reduzir a frequência de atualização: `UIGeometry.WriteToBuffers` (293 bytes) percorre os vértices e chama `BetterList.Add` separadamente para posição, UV e cor. No caminho habitual sem normais, os dados já estão transformados e prontos; trata-se de copiar estruturas de 12, 8 e 4 bytes.

A 0.0.60 substitui esse caminho por **três cópias em bloco**, somente quando:

- O corpo inteiro do método corresponde ao original do APK.
- Os campos/tipos das listas foram resolvidos pelos metadados do Mono.
- Os arrays de origem contêm todos os elementos e os destinos têm capacidade livre suficiente.
- As listas não compartilham buffers incompatíveis e os cálculos de tamanho não excedem os limites.

As três listas são conferidas antes de qualquer escrita. Os dados copiados e os tamanhos finais são iguais aos do loop original. Não há alocação no caminho rápido. Crescimento de listas, uso de normais/tangentes e entradas não atendidas continuam no método original, chamado por um trampoline permanente. A instalação exige um prólogo ARM cujas duas instruções sejam seguras para relocação; qualquer divergência mantém o código original. É reservada uma página executável de 4 KiB.

Não foi limitada a frequência de atualização do HUD, removida animação ou alterada a simulação. A linha `ui: NGUI geometry appends use bounded bulk copies ...` confirma instalação; `ui: geometry bulk appends ... original fallbacks ...` informa uso. O ganho sobre o custo total de `UIPanel.LateUpdate` precisa ser medido no aparelho: esse callback também executa outras rotinas.

### Menos custo do próprio diagnóstico

Durante `s_playingstage` fora do carregamento observado, os wrappers passam a cronometrar apenas `UIPanel.LateUpdate`. Os demais callbacks são encaminhados sem consultas de relógio/thread. Nos menus e no carregamento, a captura ampla permanece. O contador `managed callback coverage` durante a corrida passa, portanto, a representar o conjunto focado da UI.

Isso retira consultas repetidas de centenas de callbacks por quadro. O alvo de 60 FPS e as opções gráficas permanecem. **Manter mais de 30 FPS constantemente ainda não está demonstrado**; os testes locais verificam comportamento e compilação, não tempo de CPU/GPU no Vita.

## Corrupção das texturas dos personagens

As imagens confirmam corrupção visual em alguns personagens. Na comparação das alterações, a 0.0.59 não modificou as fontes do shader de personagem, que já vinham da 0.0.57. O log não identifica uma falha de compilação nem permite isolar definitivamente a causa visual.

Foi fechado um caminho inseguro para esse caso: desde a 0.0.56, o salto de mip 0 também podia selecionar texturas de gameplay. Ele dependia de o motor nunca reutilizar aqueles bytes enquanto o limite global estivesse em 1; os bytes pulados permaneciam sem inicializar no buffer entregue ao motor.

Na 0.0.60:

- Só **atlas grandes de UI (`ui_`) e fontes `UCGothic_`**, com pelo menos uma dimensão de 512, podem entrar no salto. Todas as demais guardas anteriores permanecem.
- Texturas de personagens, inimigos, cenário e efeitos recebem a leitura completa dos seus mips.
- Os trechos de UI não lidos do cartão são zerados explicitamente, evitando entregar memória indefinida.

A otimização de leitura dos menus continua, com uma seleção mais conservadora. Nos **294 bundles reais**, o teste encontrou **188 intervalos de UI** e confirmou todos os bytes restantes idênticos aos arquivos originais; isso inclui os dados de gameplay agora preservados. O APK e os bundles no armazenamento não foram regravados.

Essa é uma correção preventiva de integridade de dados; **a recuperação das texturas mostradas nas imagens ainda precisa de confirmação visual no Vita**. Não se afirma que a causa tenha sido isolada definitivamente por esse log.

## Fundo e splash: arte inteira, sem corte/deformação

A arte `Splash3` tem proporção original **4:1 (2048×512)**. Preencher um retângulo 840×500 exigia recorte; agora ela é reduzida proporcionalmente e centralizada com faixas pretas:

- LiveArea: arte inteira **840×210**, dentro do canvas **840×500**.
- Splash de abertura do Vita (`sce_sys/pic0.png`): arte inteira **960×240**, dentro de **960×544**. Esse arquivo foi acrescentado ao empacotamento.
- A splash do próprio jogo foi localizada nos objetos UITexture `splash` e `img_blinder` de `mainData`. Os widgets de **1710×721** deformavam a textura; ambos passam para **1264×316** (4:1, cabendo na área virtual da UI do Vita). O tint RGB da splash principal foi normalizado para 1.

A correção da cena é aplicada somente aos bytes entregues ao motor durante leitura/extração do APK. Os dois objetos originais de 96 bytes precisam corresponder integralmente e o tamanho de `mainData` precisa conferir. Se qualquer validação falhar, os dois permanecem originais. Leituras parciais e extração comprimida recebem a mesma substituição. O APK no cartão permanece original.

Marcador: `splash: full Splash3 image fitted at 4:1 in the original title UI (APK unchanged)`.

Ícone circular e logo com relevo branco foram mantidos. O XML da LiveArea está em revisão 4. As prévias do fundo e `pic0.png` foram inspecionadas; a apresentação final da splash dentro da cena ainda requer o aparelho.

## Validação e entrega

- Compilação Release ARM/VitaSDK concluída, sem avisos nos novos arquivos.
- ASan/UBSan: cópia em bloco comparada byte a byte com appends escalares em 500 conjuntos; guardas de capacidade/origem/alias/overflow sem escrita parcial; wrapper real com contagem, fallback, normais, geometria vazia e filtro de prólogo ARM.
- ASan/UBSan: overlay da splash validado contra o `mainData` real, leituras de 1/7/43/1024/65536 bytes, identidade das saídas e rejeição integral de objeto divergente.
- ASan/UBSan: leitura dos 294 bundles, seleção exclusiva de UI, leitura completa com limite 0 e zeros somente nos intervalos pulados.
- Testes de encaminhamento/foco dos callbacks, offsets do APK com concorrência, preparação de JIT e shader/uniforms passaram.
- VPK/ZIP íntegro, eboot idêntico ao build, imagens/XML empacotados iguais aos arquivos atuais, paletas e dimensões corretas. Verificados os retângulos completos 840×210 e 960×240. `git diff --check` passou.

Pacote: `build/VPK/SRR-Vita-v0.0.60.vpk` (**1379419 bytes**).
Build: `20261030T220622Z`.
SHA-256: `1e64b5a280f1fd0ad32593c4f65099abcf0060d571741ded3262d400e4c98ae0`.

Sem execução desta build no Vita nesta entrega. O próximo teste deve repetir os mesmos personagens/fases das fotos, verificar as duas splashes e a imagem inteira da LiveArea, e comparar quadros de corrida com o contador `geometry bulk appends` crescendo. A recuperação visual e a estabilidade acima de 30 FPS precisam dessa medição.
