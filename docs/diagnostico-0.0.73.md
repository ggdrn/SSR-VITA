# 0.0.73 — botão SETTINGS da LiveArea com link psla:

## Problema

Na 0.0.72, tocar no botão SETTINGS da LiveArea não fazia nada. O alvo era `psgm:play?titleid=SRRV00001&param=settings`, que o sistema não trata como um link do próprio app.

## Correção

- O alvo passou a ser `psla:settings` (`content-rev` 7). `psla:<arg>` é o esquema da LiveArea que abre o próprio app, ou volta para ele se já estiver aberto, e entrega `<arg>` como evento do AppUtil.
- No boot, o loader lê o evento com `sceAppUtilReceiveAppEvent` + `sceAppUtilAppEventParseLiveArea` e abre a tela de configurações se o argumento for `settings`. O log mostra `port: LiveArea event 0x... "settings"`.
- **Jogo já aberto:** a cada 30 quadros o loader verifica o mesmo evento. Se SETTINGS foi tocado, o jogo reinicia direto na tela de configurações (`sceAppMgrLoadExec("app0:eboot.bin", {"settings"})`), já que elas só valem no boot. O log mostra `port: restarting into the settings screen`.
- O parâmetro de `sceAppMgrGetAppParam` é registrado no log, se houver, para diagnóstico. Segurar SELECT ao abrir continua funcionando.

## A conferir no Vita

Reinstalar o VPK para a LiveArea atualizar e então:

- Com o jogo fechado, tocar em SETTINGS deve abrir a tela azul de configurações.
- Com o jogo aberto, tocar em SETTINGS deve reiniciar o jogo nessa tela.
- Se ainda não funcionar, as linhas `port:` do `srr_log.txt` mostram o que chegou.
