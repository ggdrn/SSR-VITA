# SRR Vita 0.0.61 — arte fornecida, controles e revisão visual

## Log recebido

Fonte: `build/VPK/srr_log - cópia.txt`, versão **0.0.60**, build **20261030T215043Z**. É uma build intermediária, anterior à entrega final de 0.0.60 (`20261030T220622Z`). Não é uma captura da 0.0.61.

Relatório preservado em `build/diagnostico-0.0.61-baseline/`: 6272 eventos OBS, 64 fases, nenhum JSON inválido. A cobertura é parcial: 15 eventos descartados e 851 estouros da tabela de agregação. Fases classificadas como corrida também podem incluir pausa, resultado e transições; uma média de toda a cena não representa FPS contínuo de gameplay.

- Há janelas de corrida próximas de **34,2 FPS**, com p50 de 30 ms e p95 de 31–32 ms, mas também quedas. Não há evidência de estabilidade constante acima de 30 FPS.
- A captura associa chamadas demoradas a `UIPanel.LateUpdate`, incluindo 126,89, 128,14, 150,05, 151,92 e 152,78 ms. Há chamadas ainda maiores. São tempos inclusivos do callback: não isolam, sozinhos, a subrotina causadora.
- A otimização de geometria **não instalou**: `geometry JIT prologue differs; original append preserved`. A 0.0.61 acrescenta os dois opcodes observados a esse aviso para identificar o prólogo real, mantendo a guarda de compatibilidade.
- A correção da splash também foi rejeitada nessa build intermediária: `original title layout differs; kept APK layout`. O teste atual compara os objetos completos com o APK real e passa.
- O cache registra `hits 567 misses 0 ... errors 0 compiler calls 0`. Não há falha de compilação de shader demonstrada nessa linha; ela não valida o resultado visual nem inventaria todos os materiais carregados.
- Existem dois `free(... ) ignored: misaligned`, de `libunity.so+0xB9358`. A origem e a relação com a corrupção visual não foram demonstradas. Não foram tratados como prova de textura ou shader ausente.

## Arte fornecida pelo usuário

Fonte copiada integralmente para `assets/livearea/fan-background.png`, **1280×720**. O gerador usa essa imagem por padrão e aceita `--background`. Ícone circular e logo com base branca no botão de início foram preservados.

- LiveArea: imagem completa em **840×472**, centralizada em **840×500**.
- Splash do sistema: imagem completa em **960×540**, centralizada em **960×544**, em `sce_sys/pic0.png`.
- Splash do jogo: a mesma arte substitui os bytes RGB24 de todos os mips de `Splash3`. Os widgets passam para **1264×711**, proporção 16:9. O armazenamento original 2048×512 da textura é preservado; o redimensionamento da imagem para esse armazenamento é compensado pelo retângulo final da UI.

O APK no cartão permanece original. Um overlay de leitura atende tanto extração quanto leituras parciais dos cinco splits envolvidos. Cabeçalho original, tamanhos dos splits, comprimento do arquivo e hash do conteúdo empacotado são validados antes de publicar a substituição. O arquivo `app0:assets/game-splash.rgb` contém **4194309 bytes**, FNV-1a **40e53d7f**. A cópia em RAM custa aproximadamente 4 MiB durante a execução; não há processamento de imagem por quadro. O consumo da textura gráfica mantém os tamanhos originais.

## Controles na corrida

| Botão | Ação |
|---|---|
| X | Toque/segurar no centro; comportamento original de salto e habilidades do personagem |
| Quadrado | Tocar o botão de item |
| Triângulo | Tocar o botão de troca de personagem |
| L ou Start | Pausa/voltar através de Android BACK |
| Círculo | Mantém o atalho de toque já existente |
| Select | Mantém o atalho BACK já existente |

A inspeção de `CharacterInput` e `HudCockpit` identificou toque/segurar/soltar, item, troca e pausa. Não foi identificada outra habilidade independente que justifique inventar uma ação para R. As habilidades específicas continuam seguindo os eventos de toque do jogo.

Item e troca acionam a própria UI, preservando disponibilidade, consumo e cooldown. As coordenadas são proporcionais ao HUD paisagem observado: item em (300,480), troca em (675,480), na referência 960×544. Cada atalho tem ponteiro independente: cinco dedos físicos, salto, item e troca podem coexistir. Manter pressionado não repete o clique. O clique conclui ao soltar o botão, seguindo o comportamento de toque da UI.

Esses dois atalhos só ficam ativos em `s_playingstage`, fora do carregamento, com `Time.timeScale > 0`. São liberados ao pausar/sair e não geram um novo toque apenas por entrar na corrida com o botão já pressionado. L/Start/Select compartilham estado BACK, evitando duas alternâncias quando pressionados juntos. A resolução do getter nativo de pausa registra sucesso ou indisponibilidade no log.

## Personagens

A leitura integral das texturas de gameplay introduzida em 0.0.60 permanece. Como o usuário relata corrupção persistente e a build recebida ainda registra `diffuse+envmap compatibility`, a 0.0.61 restaura o par de shaders difusos anterior à 0.0.57: UV principal, `_DifTex`, `_DifCol` e deslocamento Z. Retira o caminho de normais/reflexo suspeito, reduzindo o efeito metálico em materiais que o usam.

O cache usa o código-fonte efetivamente substituído, distinguindo esse par da variante anterior. A seleção foi testada contra os shaders do APK. **Isso é uma tentativa de recuperação por retorno ao caminho anterior; não há confirmação visual no Vita nem prova de causa única.** Não se afirma que os personagens já foram recuperados ou que houve ganho medido de FPS.

## Validação

- Build Release VitaSDK concluída. A compilação ampla ainda mostra avisos anteriores em HTTP, SHA1, watchdog e libc; os arquivos novos não geraram avisos.
- ASan/UBSan: overlay real contra os cinco splits do APK, leituras de 1/43/1024/1048576 bytes, fronteiras entre splits, identidade dos demais bytes, rejeição de cabeçalho/hash divergentes. Todos os mips conferidos contra a arte e orientação vertical de Unity.
- ASan/UBSan: oito ponteiros simultâneos, IDs/índices de DOWN/UP, salto preservado ao usar item, estado mantido sem repetição, liberação em pausa e BACK agregado.
- Passaram os testes de layout da splash com APK real, offsets concorrentes, seleção de shaders/uniforms, geometria e wrapper, callbacks gerenciados, preparação JIT e os 294 bundles (188 intervalos de UI, demais bytes preservados).
- ZIP íntegro, eboot e recursos empacotados idênticos aos arquivos atuais; PNGs indexados de dimensões corretas. Prévia de `pic0.png` inspecionada. `git diff --check` passou.

Entrega: `build/VPK/SRR-Vita-v0.0.61.vpk`, **4588143 bytes**.
Build: **20261030T223730Z**.
SHA-256: **ea915472cea034bf4dc1c505a870fc3bff8136e2b855e267f323831bf58cc7f3**.

Ainda requer teste no aparelho: aparência dos mesmos personagens afetados, posição/hit-test dos atalhos, splash dentro da cena e estabilidade de quadros. A próxima captura deve confirmar os marcadores de arte/controles e, caso a guarda da geometria recuse novamente, trazer os opcodes para análise.
