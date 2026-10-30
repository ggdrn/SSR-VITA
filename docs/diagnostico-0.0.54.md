# SRR Vita 0.0.54 — lixo da UI (NGUI) na corrida e na tela de resultado

Logs analisados (0.0.53): `srr_log_Full_graficos.txt` (gráficos sem limitação, ~1310 s, três corridas) e `srr_log.txt` (gráficos limitados, ~780 s, duas corridas), preservados em `build/perf-baselines/srr-0.0.53-fullgfx.log` e `srr-0.0.53-limgfx.log`; relatórios em `build/analysis-0.0.53-full/` e `build/analysis-0.0.53-lim/`.

## O que a 0.0.53 resolveu

- Os patches carregaram (`stage: GameModeStage.StateChangeLevel no longer forces ...` e `HudCockpit.OnClickEndPause ...`). Nenhuma trava de ~2,8 s na corrida; dentro da corrida só restou uma coleta automática de ~430 ms a cada ~32 s (ex.: 484,9 / 515,7 / 547,9 / 580,3 s).
- O perfil de alocações funcionou: 75 janelas (gráficos completos) e 30 (limitados).

## Gráficos completos × limitados

Com gráficos completos a corrida fica em 60 fps (P50 17 ms, P95 18 ms nas janelas sem GC). Com gráficos limitados a corrida fica travada em 30 fps (P50 33–34 ms em todas as janelas), com o mesmo número de draw calls — a opção do jogo limita a taxa de quadros, não alivia o Vita. Recomendação: usar gráficos completos.

## De onde vem o lixo

Em todas as telas, as três classes que mais alocam são `Vector3[]`, `Vector2[]` e `Color32[]` na proporção exata 12:8:4 bytes — posições, UVs e cores de vértices de malhas da UI:

| Momento | Alocação por janela de 600 quadros | Arrays por quadro | GC automático |
|---|---|---|---|
| Menu parado | ~4,5 MB | ~1 de cada | a cada ~24 s (770 ms cada) |
| Corrida | ~12,3 MB (≈1,2 MB/s) | 5 de cada | a cada ~32 s (430 ms) |
| Tela de resultado | ~165 MB (≈16 MB/s) | 6 de cada, ~23 KB cada | a cada ~6,6 s (390 ms) |

Origem (IL do Assembly-CSharp): `UIDrawCall.Set` do NGUI 2.x envia a malha com `BetterList.ToArray()`. Esse método faz `Trim()`, que cria um array do tamanho exato e o põe no lugar do buffer da lista. As listas (`UIPanel.mVerts/mNorms/mUvs/mCols`) são estáticas e compartilhadas por todos os draw calls; como cada draw call tem um tamanho diferente, todo rebuild de UI (o HUD da corrida reconstrói a cada quadro, a tela de resultado reconstrói textos grandes a cada quadro) cria arrays novos e ainda força a lista a crescer de novo no próximo painel.

## Mudança — malhas da UI sem cópia por rebuild

1. **IL de `UIDrawCall.Set`**: aplicado no momento em que o Assembly-CSharp é carregado (gancho `mono_install_assembly_load_hook`, antes de qualquer método ser compilado). O corpo inteiro (0x2bf bytes) é comparado; só então quatro instruções `callvirt BetterList<T>::ToArray` viram `ldfld BetterList<T>::buffer` (tokens já existentes no assembly; mesmo tamanho e mesmo efeito na pilha). Tangentes (só com `generateNormals`) continuam com `ToArray`.
2. **Setters de malha da Unity** (`Mesh::set_vertices/set_normals/set_uv/set_colors32`): o registro de internal calls do libunity passa por um intermediário (import religado e `dlsym`) que troca só esses quatro por wrappers. Quando o array recebido é o buffer atual de uma das listas do `UIPanel`, o wrapper mostra à Unity o comprimento igual ao número de elementos preenchidos durante a chamada e restaura em seguida. A malha recebe exatamente o mesmo número de vértices e os mesmos dados de antes; qualquer outra chamada passa direto.

Se qualquer conferência falhar (IL diferente, setters não registrados), nada é alterado e o log diz `... left original`.

Efeito esperado: a alocação na corrida cai de ~12 MB para ~0,3 MB por janela, o que deve levar as coletas automáticas da corrida de uma a cada ~32 s para nenhuma (ou quase nenhuma) numa corrida normal; a tela de resultado deixa de coletar a cada ~6,6 s; o menu parado aloca bem menos. Precisa ser medido no aparelho.

## Ainda em aberto

- Entrada na corrida e saída: grupos de 3–5 coletas de ~370 ms (limpezas explícitas do próprio jogo nas transições).
- Menus: coletas de 0,7–1,1 s (conjunto vivo maior, ~40 MB). Carregamento de fontes (`BMGlyph` + `List`, ~10 MB por entrada no menu) e leitura do `MainMenuPages` (~4,3 s) continuam.
- `CameraFade.OnGUI` (IMGUI) cria 2 `GUILayoutGroup` por quadro — pouco lixo (~130 KB/10 s).

## Entrega

Compilação ARM/VitaSDK sem avisos nos arquivos alterados (os dois avisos de `watchdog.c` são antigos). Sem teste no aparelho.

Pacote: `build/VPK/SRR-Vita-v0.0.54.vpk`.
SHA-256: `063ac288568700a0cabdfa9bd34b30bc452d9d0eeb84c64c532c2c5af0348282`.

Na próxima captura procurar: `ui: UIDrawCall.Set passes NGUI buffers ...`, `ui: NGUI mesh uploads use the shared buffers ...`, o contador `(NGUI buffer uploads total N)` nas linhas `alloc: window` (deve crescer) e a ausência de `Vector3[]/Vector2[]/Color32[]` no topo das janelas `alloc:`. Conferir também se a UI aparece normal (HUD, textos, resultado).
