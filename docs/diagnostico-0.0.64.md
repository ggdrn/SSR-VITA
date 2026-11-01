# SRR Vita 0.0.64 — abertura, rede e menus

## Captura

Log recebido da **0.0.63**, build **20261031T031201Z**: aproximadamente **20 min 25 s** observados, 8791 eventos OBS válidos e 82 fases. Cópia, SHA-256 e relatório preservados em `build/diagnostico-0.0.64-baseline/`. Saúde final: 12 eventos descartados, 880 estouros da agregação e zero descartes do logger; cobertura parcial. Não há registro de encerramento normal no final.

O usuário relata melhora dos engasgos nas corridas. O log confirma os appends rápidos de UI ativos, chegando a 999054 com oito fallbacks. Isso não mede isoladamente o ganho de `Trim` nem garante estabilidade em todas as fases.

## A demora da abertura não é toda conexão

| Operação observada | Duração |
|---|---:|
| Download de net_bg.png (1,26 MB) | 1,182 s |
| Primeiro Login/login | 0,828 s |
| Download de ablist.txt (78 KB) | 0,644 s |
| Quadro 990, processamento após ablist | **76,267 s** |
| Segundo Login/login | 0,827 s |
| Login/getVariousParameter | 0,154 s |
| Player/getPlayerState | 0,222 s |

Os tempos de rede incluem conexão/TLS, resposta e callbacks, não apenas tempo no servidor. Há outras requisições sequenciais durante o login, que somam espera, mas o maior bloqueio individual identificado é local.

No quadro 990:

- `<ExecuteRequest>c__IteratorC.MoveNext`: 76,246 s inclusivos.
- I/O já instrumentado: 5,303 s, 2068 chamadas.
- JIT: 0,629 s; GC observado: 0,319 s; esperas instrumentadas: 0,014 s.
- Consultas a arquivos `UnityCache/Shared/.../__info` se estendem de aproximadamente 51,25 até 126,02 s de tempo da sessão.

`AssetBundleLoader.ParseAssetBundleList` percorre as entradas e chama `Caching.IsVersionCached` para cada URL/versão. O parser CSV termina antes dessa passagem. Não há evidência para responsabilizar o servidor pelos 76 segundos, nem para atribuir todo o restante à CPU: `stat`, `access` e enumeração de diretórios ainda não estavam cronometrados pelo contador de I/O.

Não foram puladas verificações de versão, removidos arquivos do cache nem tratados assets ausentes como presentes. A observação do tempo completo da cena de título também inclui permanência/interação do usuário; não é um cronômetro puro de loading.

## Menus

Além da rede, a captura mostra carregamento de bundles, criação de objetos, JIT e reconstrução de UI nas transições. Há requisições de leaderboard chegando a 2,60 s; isso não demonstra que todo esse tempo seja processamento remoto. Há picos de `SetupObject.MoveNext` em torno de 261 ms e de `UIPanel.LateUpdate` em torno de 132 ms. Esses custos são distintos do tráfego de rede e exigem comparação das mesmas telas.

## Mudanças

### POSTs pequenos em uma escrita

O cliente enviava cabeçalho e corpo com duas chamadas separadas, inclusive em TLS. A 0.0.64 junta os bytes quando o corpo tem até **8192 bytes**, mantendo métodos, headers e conteúdo. Corpos maiores seguem o caminho separado anterior. A escrita efetiva ainda pode ser parcial e é completada por `conn_write`.

Sockets novos recebem `TCP_NODELAY`. Isso evita a retenção deliberada de pequenos pacotes pelo algoritmo de Nagle, que pode interagir com delayed ACK em Wi-Fi. A opção é uma tentativa de configuração pelo sistema; o teste em host não mede seu efeito na rede do Vita. A combinação dos bytes pequenos é verificada pelo teste.

Certificados, autenticação, ordem dos pedidos, endpoints, timeout e política existente de keep-alive/retry permanecem. Não há cache de respostas de conta, compras, recompensas ou inventário. **Não foi medido ganho de latência desta build no Vita.**

### Medição do transporte

Cada resposta completa passa a registrar:

`http: transport host=... reused=... dns=... tcp=... tls=... request-status=... response=... ms`

DNS/TCP/TLS são reportados como zero na reutilização da conexão. `request-status` começa no envio e termina na leitura da linha de status; `response` inclui headers, body e callbacks. Portanto não são medidas puras do tempo do servidor. Falhas anteriores à resposta continuam nos logs de erro já existentes. O novo registro usa apenas o host, sem acrescentar parâmetros/credenciais ao diagnóstico.

### Medição do cache e metadados

- `cache-lookup`: tempo da chamada original de `Caching.IsVersionCached`, com argumentos e booleano de retorno preservados. A instalação só aceita função resolvida dentro de libunity e verifica o registro no Mono.
- `filesystem-meta`: `stat`, `access`, `opendir` e `readdir` do bridge. Resultado e errno são preservados. A atribuição ocorre somente na thread principal, através de `work_trace`; não há um evento OBS extra por entrada de diretório.

Os dois contadores aparecem nos relatórios de quadros lentos, junto de file-io, JIT e callbacks. Seus tempos podem ser aninhados; não devem ser somados como categorias exclusivas. Isso permitirá separar a varredura de cache do restante do processamento de ablist.

**O bloqueio de 76 segundos ainda não está resolvido nesta versão.** A melhoria executável é o envio de POSTs pequenos; a instrumentação adicional é necessária para escolher com segurança a próxima alteração de cache/menus.

## Validação

- ASan/UBSan: serializador HTTP real; corpos binários de 0, 1, 64, 8192 e 8193 bytes, escrita única dos pequenos, caminho dos grandes, headers personalizados, Content-Length correto, porta alternativa e falha de envio sem continuar com o corpo.
- ASan/UBSan: timer de cache preserva URL, versão, booleanos e chamada única; instalação única e recusa de API ausente/função de outro módulo. Nome do icall conferido no libunity do APK.
- ASan/UBSan: resultados, saída e errno de stat/access preservados, inclusive com alteração de errno pelo diagnóstico e atalhos de dispositivo aleatório.
- Passaram os testes de atribuição de trabalho à thread principal e encaminhamento dos oito timers de assets. O recorte antigo deste último incluía indevidamente o warmup acrescentado em versões anteriores; foi delimitado para testar os timers, mantendo as verificações.
- Release VitaSDK compilada. Permanecem avisos de truncamento anteriores em HTTP, watchdog e libc; o build não falhou.
- VPK/ZIP íntegro; eboot e todos os recursos visuais conferidos contra os arquivos atuais. `git diff --check` passou.

Pacote: **`build/VPK/SRR-Vita-v0.0.64.vpk`**, **4589665 bytes**.
Build: **20261101T170546Z**.
SHA-256: **e2548b2adeac7f24c292028bbcd8e903c69e1aa389e4c3f6a53790e00dce739e**.

Sem execução da 0.0.64 no Vita nesta entrega. Para o próximo diagnóstico basta capturar uma abertura completa, conexão e navegação pelas telas lentas. Os marcadores de transporte e o quadro de ablist com `cache-lookup`/`filesystem-meta` são os dados mais importantes para avançar; não é necessária outra sessão longa de corrida para esse objetivo.
