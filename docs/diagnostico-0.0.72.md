# 0.0.72 — configurações fora do jogo (botão Settings na LiveArea)

## Motivo

Na 0.0.71 o menu L + R + START mudava resolução e qualidade com o Unity rodando, o que causava muitos glitches. Agora as configurações só são editadas antes do jogo carregar e só são aplicadas no boot.

## Como abrir

- **Botão SETTINGS na LiveArea**, logo abaixo do portão (Start). O link é `psgm:play?titleid=SRRV00001&param=settings`; o loader lê esse parâmetro com `sceAppMgrGetAppParam`. O `content-rev` do template subiu para 6.
- **Segurar SELECT** enquanto o jogo abre (alternativa).

Se o jogo já estiver aberto em segundo plano, a LiveArea só volta para ele: feche o jogo antes de tocar em SETTINGS.

## A tela

Desenhada pela CPU num framebuffer próprio de 960x544 (2,25 MiB de CDRAM), antes de carregar as bibliotecas do APK, o Unity e o vitaGL. Texto em inglês. ↑/↓ escolhem, ←/→ (ou X) mudam, O sai. Cada mudança é salva em `ux0:data/srr/port_settings.txt`.

| Opção | Valores |
|---|---|
| Resolution | SD 640x368 · HD 720x408 · QHD 960x544 |
| Graphics | Low · Medium · High (níveis 0/1/2 do Unity do APK; texturas 1/4, 1/2, 1/2) |
| Frame Rate | 30 · 60 |
| FPS Counter | Off · On |
| Character Shader | Compatible · Original |
| Start Game | salva e segue o boot normal |
| Exit | salva e volta para a LiveArea |

Ao sair, a tela é apagada, o display é desligado (`sceDisplaySetFrameBuf(NULL)`) e a memória é liberada antes do vitaGL criar os buffers dele.

## Aplicação no boot

- **Resolução:** o vitaGL nasce direto no tamanho escolhido (não há mais `vglSwapResolution` nem resize do Unity em jogo).
- **Gráficos:** o wrapper de `SetQualityLevel` troca o nível pedido pelo jogo na tela de título (0 com Light Mode, 1 normal) pelo nível escolhido, de modo que não há troca dupla. O nível inicial do Unity é corrigido no primeiro tick do guard de texturas.
- **Frame rate:** `targetFrameRate` em todas as cenas e swap interval 2 em 30 fps.
- **FPS Counter:** o callback de display do vitaGL só é registrado quando ele está ligado.

O menu dentro do jogo e a opção Effects (Light Mode do jogo, que depende do Mono) foram removidos; o Light Mode continua na janela de opções do próprio jogo.

## A conferir no Vita

- O botão aparece abaixo do portão e abre a tela (é preciso reinstalar o VPK para a LiveArea atualizar).
- Start Game segue para o jogo sem tela corrompida; Exit volta para a LiveArea.
- SD/HD: imagem esticada para a tela inteira e toque nas posições certas.
