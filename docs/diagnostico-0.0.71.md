# 0.0.71 — menu de configurações do port (L + R + START)

## O que entrou

Menu do port aberto com **L + R + START** (fecha com O, com o mesmo combo ou em *Close*). Texto da interface em inglês. Setas ↑/↓ escolhem, ←/→ (ou X) mudam o valor; cada mudança vale na hora e é salva em `ux0:data/srr/port_settings.txt` (arquivo criado pelo loader, como o log).

| Opção | Valores | Como funciona |
|---|---|---|
| Resolution | SD 640x368 · HD 720x408 · QHD 960x544 (padrão) | `vglSwapResolution` troca os buffers de tela; o escalonador do Vita estica para a tela. No quadro seguinte o Unity recebe `nativeRecreateGfxState` + `nativeResize` com o novo tamanho. O toque é mapeado para o tamanho atual. O vitaGL sempre nasce em 960x544, para o depth buffer servir a todos os tamanhos. |
| Graphics | Low · Medium (padrão) · High | Níveis de qualidade do próprio Unity do APK: 0 *Fastest* (o Light Mode do jogo), 1 *Fast* (o Normal do jogo), 2 *Simple* (filtro anisotrópico, LOD bias 0.7). Texturas: Low = `masterTextureLimit` 2 (1/4), Medium/High = 1 (1/2, o teto de memória que o port já usava). Aplicado pelo icall nativo `SetQualityLevel` e reaplicado a cada 30 quadros, porque o jogo escolhe 0/1 na tela de título. |
| Frame Rate | 30 · 60 (padrão) | `Application.targetFrameRate` em todas as cenas (antes só a corrida era forçada a 60) + intervalo de swap do vitaGL 2 em 30 fps, para ritmo uniforme. |
| Effects | Full · Light | É o `SystemData.lightMode` do jogo (mesma opção da janela de desempenho): menos efeitos e física a 30 Hz. Salvo no save do jogo; vale a partir da próxima corrida. Aparece "Not loaded yet" até o save carregar. |
| FPS Counter | Off (padrão) · On | Quadros apresentados por segundo, no canto. |
| Character Shader | Compatible (padrão) · Original | Antes só pelo arquivo `original_character_shader` (que continua definindo o padrão). Exige reiniciar. |

## Como o menu é desenhado

Pela CPU, direto em cada buffer de tela, no callback de display do vitaGL (`vglSetDisplayCallback`), depois que a GPU termina o quadro. O estado GL do Unity não é tocado. Com o menu aberto o jogo continua rodando sem receber controles; dedos e teclas pressionados são soltos ao abrir, e os controles só voltam depois que todos os botões forem soltos.

## Encontrado no jogo e não incluído

- `SystemData.highTexture`: troca o servidor de assets para as texturas HD; o jogo volta ao título e baixa tudo de novo.
- `ShaderQuality` (LOD de shader por nível): componente de exemplo do NGUI; não há sinal de uso nas cenas.

## A conferir no Vita

- Troca de resolução em jogo (realocação dos buffers de tela e recriação do estado gráfico do Unity).
- Efeito real de *High* e de *Low* nas texturas já carregadas (podem só valer após trocar de cena).
- Ao apertar L antes de R + START, o L já envia BACK (pausa/voltar) para o jogo.
