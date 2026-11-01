# SRR Vita 0.0.58 — GC na reconstrução de terreno e LiveArea original

## Log analisado e resultado da 0.0.57

Arquivo: `build/VPK/srr_log.txt`, 0.0.57, build `20261030T151647Z`.
SHA-256: `4d9c26a66f3386001436339c2306646fb60fb85a30029a1f13b092c02d1af68f`.
Cópia preservada em `build/perf-baselines/srr-0.0.57-main.log`.
Relatórios de recursos, estados, quadros e picos em `build/analysis-0.0.57/`.

- A preparação de JIT completou **3614/3614 métodos**, sem skips/falhas, em **7734,47 ms**, com **109583 KiB** de reserva nativa disponível ao terminar. Não se repetiu na segunda corrida. Esse custo ocorre na primeira entrada e é uma troca por menos compilação durante o jogo.
- O log confirma CPU/BUS/GPU/XBAR em **444/222/222/166 MHz** e afinidade em quatro núcleos.
- O controle do alvo de 60 FPS foi instalado. Não há linha de mudança de `targetFrameRate`, portanto não há evidência de que o jogo estivesse pedindo 30 nesta sessão.
- Na fase observada 35, existem **75 janelas estáveis** (mais de 100 quadros, sem pico de 100 ms) com **P50 de 17 ms**, compatível com 60 FPS. Há também janelas mais lentas. Isso não é média de toda a corrida nem garantia de quadros sempre abaixo de 16,67 ms.
- Não reapareceram os picos grandes de JIT de `ObjBossEggmanState.MakeFSM` e `ChaoState.OnMsgChaoAbilityStart` nos registros de quadros lentos. Permanecem compilações pequenas de bibliotecas e algumas classes fora da lista.
- Shaders: **645 hits, 2 misses, 2 gravações, zero erros e 2 chamadas do compilador**. As duas fontes de compatibilidade alteradas na 0.0.57 justificam recompilar esse par; não se observa uma falha de compilação.

A captura tem 5974 eventos OBS válidos, com 288 perdas e 848 ocorrências de saturação da tabela. As conclusões respeitam essa cobertura parcial. `s_playingstage` inclui pausa, troca de terreno e resultado: não se deve chamar todos os seus picos de engasgos de corrida ativa.

## Pausas ainda presentes

### Coletas explícitas durante a cena de corrida

| Quadro | Render | GC medido | Pedido explícito registrado |
|---|---:|---:|---:|
| 5793 | 431,75 ms | 399,61 ms | 1 |
| 34661 | 645,89 ms | 613,69 ms | 1 |

No primeiro caso, o quadro anterior cria 37 objetos (14,27 ms dentro das chamadas medidas) e compila parte do envio de resultados/checkpoint. Logo depois aparecem rotinas de terreno. O GC não pode ser explicado como compilação de shader ou leitura de arquivo: essas categorias ficam zeradas nesse quadro. A segunda coleta passa 402,97 ms marcando e 196,61 ms liberando memória.

A auditoria do IL encontrou uma chamada ainda ativa ao final de `TerrainPlacementManager/<CreateTerrain>c__IteratorC8.MoveNext` (token `0x06004c98`, tamanho `0x2db`, offset `0x2c9`). `TerrainPlacementManager.ReCreateTerrain` apaga o terreno anterior e inicia novamente essa coroutine, portanto ela não é exclusiva do carregamento inicial. Depois de devolver os objetos reservados e marcar a construção como concluída, força `GC.Collect()`.

**Atribuição:** o agrupamento observado e o caminho do IL fazem dessa chamada um candidato forte para as pausas. O log não registra a pilha gerenciada do pedido de GC e não prova sozinho que ambos os pedidos vieram desse ponto.

### Alteração aplicada

Somente a instrução de cinco bytes dessa coleta final vira NOP. Todo o corpo original de 731 bytes precisa corresponder à fixture, além do MVID do assembly. Criação/destruição/devolução de terreno, flags de conclusão, retornos e offsets permanecem. A alteração também afeta a chamada final da criação inicial; o GC automático, o pacing, a coleta de `LoadSetTableFirst`, downloads e os descarregamentos de cena continuam disponíveis.

Isso retira um pedido incondicional identificado, sem desligar o coletor. Não garante que a coleta automática jamais coincida com uma troca de terreno. Marcador para a próxima captura:

`stage: CreateTerrain terminal forced GC removed (automatic and scene cleanup preserved)`.

### Compilação residual

`BossTypeUtil.GetBossCharaType` aparece num quadro de 140,04 ms com 40,95 ms totais de JIT. `TerrainPlacementManager` e `TenseEffectTable` também ainda compilam durante o fluxo. As três classes foram acrescentadas à preparação: **3666 métodos**, mais 52 que a 0.0.57. O limite de memória e as demais guardas permanecem. O tempo adicional no Vita ainda não foi medido.

### Outros próximos alvos

- Picos de **100–300 ms** sem JIT, GC, upload, leitura ou espera suficientes para atribuir o restante. Exemplo: quadro 5803, 301,24 ms, com 0,87 ms de espera e demais categorias medidas zeradas. Não há base para afirmar que são custo de GPU, GC ou uma classe gerenciada específica. É necessário detalhar o trabalho interno do Unity antes de aplicar outra substituição.
- Menus: `MainMenuPages` continua com leituras de cerca de **1,54–1,56 s** após o salto dos mips; a fonte `UCGothic_26_mix` tem uma carga medida de **1,56 s**. Esses custos são distintos dos engasgos de inimigos durante gameplay.
- Há avisos `free(...): misaligned` em `libunity+0xb9358` nas transições. O bridge bloqueia essas liberações inválidas. O chamador foi conferido no disassembly e faz uma conversão a partir do header do alocador da Unity. A origem do ponteiro incorreto ainda não foi atribuída; aceitar o ponteiro ou arredondá-lo para tentar liberar poderia corromper o heap. Nenhuma mudança especulativa nesse alocador foi aplicada.

## Bolha/LiveArea com os assets do APK

As imagens vieram do APK local, sem geração de arte ou downloads:

- Ícone: `res/mipmap-xxxhdpi-v4/ic_launcher.png`, reduzido para **128×128**.
- Fundo: sprite **`ui_cmn_bg_sky4`**, retângulo `(0,1648,710,400)` do **`ui_title_Atlas`**, usado pelos dados da tela de título em `mainData`. Adaptado para **840×500**, preservando proporção com um pequeno recorte central.
- Imagem de abertura da LiveArea: composição dos sprites **`ui_title_img_sonic`**, **`ui_title_img_runners`** e **`revival`**, com suas cores e transparências originais, dentro de **280×158**. O sistema desenha o botão de iniciar da LiveArea sobre essa área.

O atlas está em `assets/bin/Data/sharedassets0.assets.split*`. O extrator reúne as partes em ordem numérica, localiza o `Texture2D` pelo nome e lê os retângulos dos registros NGUI do UIAtlas. Não usa recortes estimados visualmente. Arquivos de origem extraídos e proveniência (SHA-256 do APK, entradas e retângulos) ficam em `assets/livearea/`.

Gerador reproduzível: `python3 scripts/make_livearea.py ../srr224.apk` (Pillow e UnityPy). As imagens para o Vita são PNGs indexados de 8 bits; a transparência do ícone/logo é mantida. `content-rev` foi atualizado para 2. O CMake agora trata imagens e XML como dependências do VPK, para que uma edição de arte também provoque novo empacotamento.

Prévia de composição das artes: `build/livearea-assets/livearea-preview.png`. A posição/borda/botão final são controlados pelo sistema do Vita e precisam de conferência no aparelho.

## Verificação e entrega

- Compilação Release ARM/VitaSDK concluída sem novos avisos.
- Teste com ASan/UBSan executa a função C real do patch de terreno: altera somente os cinco bytes da chamada; rejeita MVID, tamanho ou corpo divergente, header/API ausentes; não aplica novamente sobre um corpo alterado.
- Teste da preparação de JIT com ASan/UBSan passou com a lista atualizada.
- Integridade ZIP do VPK, igualdade do eboot e igualdade de todas as imagens/XML empacotados com os arquivos atuais verificadas.
- Dimensões, paleta de 8 bits e XML de LiveArea conferidos; prévia inspecionada visualmente; `git diff --check` passou.

Pacote: `build/VPK/SRR-Vita-v0.0.58.vpk` (1404951 bytes).
Build: `20261030T172248Z`.
SHA-256: `3d17386d54c9cdde74ba801ef21e3f1f6a8ab2a96811d271a5fbc09e97df84ea`.

Sem teste da 0.0.58 no Vita nesta entrega. Na próxima execução, conferir a bolha/LiveArea e repetir uma corrida longa com trocas de terreno, incluindo uma segunda entrada; comparar a presença do marcador do patch, os pedidos explícitos de GC e os picos. A eliminação dos dois picos atribuídos por hipótese ao terreno ainda precisa dessa medição.
