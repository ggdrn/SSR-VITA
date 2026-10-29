# SRR Vita 0.0.43 — menus lentos e elementos ausentes

Baseline preservada em `build/perf-baselines/srr-0.0.42-menus.log`. O usuário relatou demora desde o título ao menu, modais de avisos demorados, elementos do cabeçalho/fundos ausentes e retorno lento após a fase.

## O que o log permite concluir

Há quadros de 63,87 s, 99,18 s, 42,34 s e 131,19 s dentro de nativeRender. No intervalo do quadro 7215 (131,19 s), a request anterior já havia terminado e não há outra request WWW registrada até o quadro terminar. Portanto, a rede não explica esse maior atraso. A carga registra falha de uma alocação nativa de 16 MiB, apesar de o heap somar aproximadamente 82,56 MiB livres. No mesmo quadro, as chamadas de upload de texturas somam 4,16 s e os links 0,23 s; não há coleta de GC registrada. O tempo restante fica dentro do Unity, sem detalhamento suficiente para atribuir cada segundo a uma rotina específica.

Antes disso aparecem falhas de mipmaps de 11,18 e 5,59 MB, falhas de buffers nativos de textura e `No Texture memory available to upload`. Essas falhas são compatíveis com os elementos gráficos ausentes. Não é possível provar pelo log que todos os elementos relatados tenham a mesma causa, nem atribuir a regressão visual exclusivamente ao ajuste IL de imagens da 0.0.42.

As requests presentes retornam em geral 200/304, mas a versão anterior não media suas durações. Há também 404 para `pages_net_bg.png`; esse mesmo 404 está na baseline da 0.0.40. Esse fundo remoto específico continua indisponível no endereço registrado; não foi substituído por outra arte.

## Correções e investigação da regressão

- A chamada que aplicava as alterações de criação de imagens da 0.0.42 foi desativada. Os carregadores do menu voltam ao IL original do APK, sem alterar arquivos persistentes. O log de início confirma `original menu image loaders restored`. Os testes e o código experimental foram mantidos, mas o executável não aplica esses patches.
- Quando memalign do Unity falhar para um bloco de pelo menos 1 MiB, o port pode tentar espaço ainda livre nos pools mapeados do vitaGL, na ordem PHYCONT, RAM e VRAM. Não se reserva mais memória física nem se aumenta o heap. O caminho normal de alocação continua usando newlib. Essa recuperação trata a fragmentação quando há espaço útil em outro pool; não resolve esgotamento de todos os pools.
- As alocações recuperadas são registradas em uma tabela limitada a 32 blocos, com base original, ponteiro alinhado, tamanho e alinhamento. Free devolve ao pool correto. Realloc preserva o bloco em caso de erro, copia seus dados ao crescer e pode voltar ao heap normal. Sem blocos recuperados ativos, um contador atômico evita percorrer a tabela ou adquirir seu mutex em cada free. O mutex da tabela não engloba espera ou purga de GPU. Os pools podem ter características de cache diferentes do heap normal, portanto esse caminho é uma recuperação de OOM e não uma promessa de execução mais rápida para todo acesso de CPU.
- Para texturas cujo filtro já solicita mipmaps, vitaGL tenta reservar a capacidade da cadeia junto com a imagem base, sem purgas nem esperas nessa tentativa antecipada. Se não couber, usa o caminho da imagem base anterior. Ao acrescentar mipmaps, reutiliza a capacidade existente quando suficiente, evitando realocação e a necessidade temporária de manter a imagem base e uma cadeia nova simultaneamente. Resolução, formato e conteúdo das texturas permanecem.
- Cada chamada HTTP agora registra seu tempo total e o resultado, incluindo erro. Esse tempo inclui conexão/TLS, espera, redirecionamentos, leitura e callbacks do corpo; não é uma medição isolada da latência do servidor. Os valores permitirão separar melhor o custo das requests do custo local nos próximos logs. A política de respostas/cache não foi modificada.

As proteções contra cópia para NULL, as coletas originais das transições e a redução das mensagens de depuração de efeitos continuam presentes.

## Validação

Os 14 testes passaram, incluindo AddressSanitizer/UndefinedBehaviorSanitizer nos testes C. O teste novo usa os trechos reais do alocador do port para simular heap fragmentado e pools distintos: verifica alinhamento até 4096 bytes, origem Unity, propriedade correta no free, crescimento e cópia, falha de realloc preservando o original, realloc zero, limite da tabela, overflow e oito usuários concorrentes. O teste de texturas verifica a capacidade de uma cadeia 2048×2048/16 bits, reutilização sem realocação, fallback para a imagem base e comportamento de erro. Também há uma asserção de que o patch de imagens não é aplicado na inicialização.

Build ARM/VitaSDK concluído. VPK passou na integridade ZIP e seu eboot coincide com o build. SHA-256: `34fbe4bb0b68759d6effc8f806be4717a475f4b27067647395b84ff1ee063527`.

Pacote: `build/VPK/SRR-Vita-v0.0.43.vpk`. Ainda não há validação visual ou medição desta versão no console. Testar título → menu → modais → seleção de itens → fase → retorno ao menu. O novo log deve mostrar os tempos `http: elapsed`, eventuais recuperações `mem: recovered fragmented Unity allocation` e eventuais falhas restantes. Não é necessário apagar saves ou caches.
