# SRR Vita 0.0.69 — correção da instalação do decodificador

## Captura

Log recebido da 0.0.68, build 20261103T172057Z. Referência preservada em
`build/diagnostico-0.0.69-baseline/srr_log.txt`, relatório OBS em `report/`.
7299 eventos, 49 estados. SHA-256 do log:
`bece22c2412203b27de55f5da908d6285b053e95784d6508fc48a30593e04941`.

O usuário esclareceu que a queda fica pior quando a corrida fica rápida.
Na fase longa 44 há janelas sem GC com P50 de 34–38 ms (aproximadamente
26–29 FPS típicos), além dos picos. Não é apenas uma pausa isolada.
A cena também contém preparação e resultado; não confundir suas janelas
com um benchmark exclusivamente de gameplay.

## Falha encontrada na 0.0.68

O log registra `Rijndael256 IL differs; original decrypt preserved` e todos
os contadores de blocos nativos permanecem zero. A otimização não instalou.

O instalador procurava `Mono.Security.Cryptography.RijndaelTransform`,
mas o tipo do mscorlib desse APK é
`System.Security.Cryptography.RijndaelTransform`. A consulta retornava
nulo; a mensagem genérica de IL diferente encobria essa causa. Foi um erro
da implementação da 0.0.68, não uma diferença no APK do usuário.

Os testes de cálculo da rotina eram corretos, mas o mock de instalação
repetia o namespace errado, e por isso não detectou a falha de integração.

## Alteração e testes

- Corrigido o namespace na busca de classe.
- O teste gerenciado agora lê a string usada no código de produção e a
  compara diretamente com o namespace do tipo dono de Decrypt256 nos
  metadados reais do APK. Verifica também o nome e a assinatura de instância
  `void(byte[], byte[], uint[])`.
- Falhas de classe ausente, método ausente e IL diferente agora possuem
  mensagens distintas. O mock foi corrigido e cobre classe/método ausentes.
- O cálculo de blocos e todas as guardas restantes da 0.0.68 foram mantidos.

Passaram `test_rijndael256.py` (4096 blocos contra execução do IL original,
in-place e limites, ASan/UBSan), `test_rijndael256_wrapper.py`,
`test_managed_trace.py` e `test_race_warmup.py`. Build Release VitaSDK
concluída sem avisos nesta recompilação. ZIP íntegro, eboot conferido e
SFO APP_VER 00.69. `git diff --check` passou.

## Gargalos ainda observados

- Quadro 22363: LoadSetTable.MoveNext 253,30 ms, sem GC.
- Quadro 24801: LoadSetTable.MoveNext 354,48 ms, sem GC.
- CreateSetData apresenta vários callbacks individuais de 79–120 ms.
- Quadro 23258: GC 531,36 ms em render de 566,10 ms.
- Quadro 24796: render de 879,91 ms, LoadSetTable inclusivo 853,64 ms,
  com GC de 527,68 ms aninhado. Não somar esses valores.
- Há picos de UIPanel de 50–80 ms e mais; não são explicados somente pela
  decodificação de trechos.

A 0.0.69 corrige a instalação da otimização destinada às pausas de
carregamento, mas não demonstra resolver o tempo contínuo de 34–38 ms
por quadro. Não há medição desta build no Vita nesta entrega. Não houve
alteração na física, velocidade do jogo, qualidade gráfica ou política de GC.

## Pacote

`build/VPK/SRR-Vita-v0.0.69.vpk`, 4596856 bytes, oito arquivos.
SHA-256: `94e60c3dd43fcb62c24553d7c2b143a59ee8618e17b8dbc19f34b1ca11f27bbf`.

No próximo teste, conferir primeiro a mensagem
`Rijndael256 native blocks installed` e o aumento do contador de blocos
nativos. Comparar a mesma corrida longa com a 0.0.68; o efeito da rotina
nativa só poderá ser avaliado após confirmar sua instalação no aparelho.
O marcador `ux0:data/srr/original_rijndael256` continua disponível para
comparação com o caminho original após reiniciar.
