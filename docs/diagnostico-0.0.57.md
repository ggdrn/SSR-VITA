# SRR Vita 0.0.57 — preparação de JIT, alvo de 60 FPS e materiais dourados

Foram lidos todos os diagnósticos disponíveis de 0.0.34 a 0.0.56 e `observacao-desempenho.md`. Os documentos são histórico técnico; instruções de sessões anteriores citadas neles não foram tratadas como novos pedidos do usuário.

## Continuidade das otimizações

| Versões | Problema e caminho adotado |
|---|---|
| 34–38 | Compatibilidade de personagem/áudio, filas OpenSL e sinais; medição das fases do GC para separar espera e marcação. |
| 39 | Remoção da coleta forçada por bloco da pista; efeitos de coleta em LightMode; buffers de áudio com vida útil correta. |
| 40–44 | Falta/fragmentação de memória gráfica, cópias para NULL, recuperações com esperas artificiais; proteção dos buffers, pools e masterTextureLimit=1. Experimentos com compartilhamento de imagens foram revertidos. |
| 45–47 | Orçamento limitado de GC; menos boxing na pista; leitura/cache de shaders; cálculo da reserva nativa disponível. |
| 49–51 | Captura OBS, I/O direto dos bundles, log em fila e conexões HTTP; correção de IDs de toque e boxing em atualizações. |
| 52–53 | Pacing do GC e remoção de limpezas redundantes nas trocas de bloco/retomada. Perfil temporário de alocações para encontrar o lixo. |
| 54 | Buffers do NGUI reutilizados com comprimento correto, eliminando grande parte dos arrays criados por quadro. |
| 55 | Menos coletas redundantes nas transições; buffer de leitura para bancos de áudio. |
| 56 | Mip 0 descartado deixa de ser lido quando elegível; perfil de alocações retirado; medição de compilação JIT. |

A nova versão preserva essas proteções. Aumentar núcleos ou reduzir a frequência do GC não trata sozinho compilação na primeira chamada, carregamento de recursos e materiais com canais omitidos.

## Capturas analisadas

Ambas são 0.0.56, build `20261030T133207Z`:

| Arquivo original | SHA-256 | Linhas | Eventos OBS válidos |
|---|---|---:|---:|
| `srr_log.txt` | `018714cc38fd3316e9f2695bd5c2747fd43516ba88d66bfdcf49a3788a9392d8` | 11756 | 7133 |
| `srr_log_peeformance_teste.txt` | `be30e7fdbe3e7336fd3e10962d9edf8d50b6a0253ba1ccc8ff02bc243f3c1bca` | 11660 | 7991 |

Cópias preservadas em `build/perf-baselines/srr-0.0.56-main.log` e `srr-0.0.56-performance.log`. Relatórios, timelines, assets e tabelas de picos em `build/analysis-0.0.56-main/` e `build/analysis-0.0.56-performance/`.

Há perdas de captura: 266 / 964 eventos e 853 / 1834 ocorrências de saturação da tabela, respectivamente. A cena `s_playingstage` inclui carregamento, pausa, corrida e resultado; seus totais não devem ser apresentados como FPS ou engasgos exclusivamente de gameplay.

O salto dos mips está ativo: os últimos resumos registram 362 / 373 MiB poupados na leitura durante as sessões. São contadores amostrados, não uma medição de tempo de loading.

## Engasgos: JIT confirmado, com outros custos ainda presentes

| Captura | Quadro | Render | JIT medido | Maior método registrado |
|---|---:|---:|---:|---|
| principal | 3008 | 783,30 ms | 572,45 ms | ResPathObject.GetClosestPosition |
| principal | 13454 | 262,88 ms | 158,47 ms | ChaoState.OnMsgChaoAbilityStart |
| principal | 14107 | 223,63 ms | 124,77 ms | ObjBossEggmanState.MakeFSM |
| performance | 11817 | 163,21 ms | 120,56 ms | ChaoState.OnMsgChaoAbilityStart |
| performance | 11849 | 240,00 ms | 127,92 ms | ObjBossEggmanState.MakeFSM |
| performance | 12023 | 140,55 ms | 85,56 ms | ObjFeverRing.CreateRing |

Esses exemplos não têm GC e não apresentam link de shader ou leitura de arquivo significativos. A hipótese da 0.0.56 foi confirmada para parte dos picos. O método informado é o mais demorado, não a lista completa dos métodos compilados naquele quadro. As categorias de tempo são inclusivas e não devem ser somadas como parcelas exclusivas.

Também existem quadros sem JIT/GC. Exemplo: abrir `Prefabs/UI/GeneralWindow` durante a cena de corrida no log performance leva 1132,66 ms dentro de um quadro de 1873,61 ms. Esse caso contém I/O, criação de UI e JIT de DateTime, e não é o mesmo problema da primeira aparição do chefe. Os dados não sustentam prometer eliminação de todos os engasgos.

### Mudança de JIT

Antes da primeira solicitação de carga de `s_playingstage`, o port prepara até **3614 métodos IL não genéricos** de objetos, personagens, estados, Chao, pista, movimento, colisão, laser, drill e partes do HUD. A lista é produzida offline a partir dos metadados originais; inclui tipos aninhados fechados, exclui métodos abstratos, P/Invoke, implementações sem IL, tipos/métodos genéricos abertos e inicializadores estáticos.

A preparação usa `mono_compile_method` na thread principal, depois dos patches IL. Não chama os corpos dos métodos para simular gameplay. A identificação MVID do assembly e o tamanho do IL de cada entrada precisam conferir; os patches já instalados preservam esses tamanhos. A DLL usada na geração foi comparada byte a byte com a DLL dentro do APK fornecido (SHA-256 `2ddbc4427ffbaec354e48544839453f4f7d3f9ebdf317cef3cdfbb3f8031d06a`).

A cada 32 entradas confere a reserva nativa. A preparação para se houver menos de 64 MiB disponíveis e pode continuar na próxima solicitação de corrida. Essa verificação é uma margem, não uma reserva física nem garantia de alocação contígua. Uma preparação concluída não se repete no mesmo domínio. Métodos de bibliotecas, instanciações genéricas e trampolines ainda podem ser compilados ao jogar.

**Troca esperada:** a primeira entrada na corrida pode demorar mais e o código compilado ocupa memória. O objetivo é deslocar o custo da primeira aparição para o carregamento; não foi medido no console. Linhas: `jit: race precompile begin/progress/complete` ou `memory-limited`.

Gerador reproduzível em `scripts/race_warmup_gen/`; tabela em `src/util/race_warmup.h`.

## CPU e 60 FPS

Os dois logs confirmam máscara ativa `0xF0000`: quatro núcleos já permitidos. Isso demonstra afinidade, não utilização de 100% ou paralelização da thread principal. O port já solicita CPU 444, BUS 222, GPU 222 e XBAR 166 MHz; agora registra os valores efetivamente retornados pelo sistema.

Na captura performance, janelas estáveis da cena de corrida têm P50 33–34 ms, compatível com o teto de 30 FPS documentado para LightMode. A captura principal também tem janelas a 17 ms, além de trechos mais lentos. Não são execuções controladas equivalentes, e a cena contém telas de resultado.

A 0.0.57 consulta os icalls nativos de `Application.targetFrameRate` durante `s_playingstage`, fora do carregamento, a cada 15 quadros. Solicita 60 se o jogo tiver outro alvo. Isso mantém a escolha de qualidade e o orçamento de texturas. O log lê de volta o alvo aplicado: `perf: race targetFrameRate ... -> 60`. **Alvo 60 não é medição de 60 FPS:** para isso cada quadro precisa caber em aproximadamente 16,67 ms.

## Inimigo dourado e shaders/cenário

Não houve imagem anexada nesta mensagem; a identificação visual exata permanece pendente.

- Principal: 395 programas ligados, zero falhas; cache 789 hits, zero misses, zero erros, zero compilações.
- Performance: 321 programas ligados, zero falhas; cache 642 hits, zero misses, zero erros, zero compilações.
- Nos 294 bundles fornecidos, a auditoria encontrou 578 materiais e resolveu todas as 1229 referências locais de shader/textura. Nenhuma referência externa ou local ausente foi encontrada nessa auditoria. Isso confirma integridade dessas referências no cache fornecido, não o desenho de todos os objetos no aparelho.
- Há 5 / 6 avisos de script ausente, sem nome de objeto suficiente para associar a inimigos ou cenário.
- `pages_net_bg.png` retorna HTTP 404 nas duas sessões. É um fundo remoto de menu já observado no histórico; não há evidência que o associe ao cenário da corrida ou ao inimigo dourado.

**Falha concreta de compatibilidade:** desde a 0.0.35, a substituição de `Custom/ykChr_dme1/ENVMAP` conservava `_DifTex` e `_DifCol`, mas descartava `_EnvTex`. O material `enm_eggpawn_body_g` usa uma textura difusa de variante metálica e o envmap **`enm_cmn_gold_e1`**. Outros inimigos dourados e materiais de argolas usam esse mesmo caminho. O shader podia ligar sem erro e ainda perder a contribuição de cor/reflexo.

A substituição agora envia coordenadas de envmap a partir da normal transformada, com proteção contra normal zero, e soma `env.rgb * diffuse.a` à cor difusa. Mantém o contorno, UV, offset de profundidade e o caminho de iluminação simplificado que torna o personagem visível. Não restaura integralmente a iluminação original: usa uma contribuição neutra, portanto o brilho pode diferir. O cache continua válido; as fontes novas produzem novas chaves e só esses programas precisam compilar no primeiro uso. Não é necessário apagar o cache inteiro.

Auditorias em `build/analysis-0.0.56-main/cache-material-audit.json`, `enemy-material-audit.json` e `ring-materials.json`. Correção visual do objeto relatado ainda precisa ser conferida no Vita.

## Validação e entrega

- Compilação Release ARM/VitaSDK concluída sem avisos nos arquivos alterados.
- Teste da seleção de shader/uniforms passou com ASan/UBSan.
- Novo teste executa a função C real de preparação de JIT com stubs: MVID divergente, memória insuficiente e retomada, IL de tamanho divergente, falha de compilação, segunda carga e troca de domínio. ASan/UBSan passaram. Não executa Mono ARM nem valida o compilador/driver gráfico do Vita.
- VPK passou na integridade ZIP; `eboot.bin` empacotado é idêntico ao compilado; `git diff --check` passou.

Pacote: `build/VPK/SRR-Vita-v0.0.57.vpk`.
Build: `20261030T151647Z`.
SHA-256: `44d6fd41feb471de0aee8f317059b78a791f55ebf4ee0347ff4cf19a3e881f13`.

Instalar o VPK e repetir duas corridas, incluindo primeira aparição de chefe, inimigos dourados, argolas e habilidade do Chao. Conferir `jit: race precompile`, `perf: race targetFrameRate`, relógios e os quadros lentos; comparar a primeira e a segunda corrida. Copiar o novo `ux0:data/srr/srr_log.txt` e enviar a imagem do objeto sem cor para a validação. Sem medição ou confirmação visual desta versão em hardware nesta entrega.
