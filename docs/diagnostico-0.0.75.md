# 0.0.75 — abertura das configurações antes do jogo

O teste no Vita da 0.0.74 abriu o jogo ao tocar SETTINGS, sem mostrar o menu.
O teste anterior no Mac validava a tela, mas não o evento real da LiveArea.

- O alvo agora é `psla:-settings`, com `content-rev=8`, seguindo o formato
  `psla:-config` usado pelo GTA SA Vita. A alteração ainda precisa de teste no Vita;
  não há log do dispositivo que confirme a causa da falha anterior.
- A leitura aceita eventos LiveArea de tipo `0x05`. O log registra inicialização
  do AppUtil, retorno da recepção, tipo, resultado do parser e argumento recebido.
- O menu continua antes da checagem de requisitos e do carregamento do APK/Unity.
  Somente Start Game libera o carregamento após entrar no menu. Se a alocação
  da tela falhar, o app sai em vez de iniciar o jogo silenciosamente.
- Os testes no Mac agora cobrem também pedido LiveArea simulado, evento inválido,
  argumento de reinício, SELECT e a posição do menu antes do carregamento.

Instalar o VPK por cima, fechar o jogo e testar SETTINGS com o jogo fechado.
Se ainda iniciar direto, coletar `ux0:data/srr/srr_log.txt` dessa tentativa.

Referência: https://github.com/TheOfficialFloW/gtasa_vita/blob/master/sce_sys/livearea/contents/template.xml
Leitura do evento: https://github.com/TheOfficialFloW/gtasa_vita/blob/master/loader/main.c
