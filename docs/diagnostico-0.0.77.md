# 0.0.77 — relatos da versão pública 0.1

## Evidências do log recebido

Captura identificada como `SRR Vita 0.01.00`, build `20261101T194404Z`.
O encerramento ocorreu no quadro 53756, após aproximadamente 51 minutos.
O relatório gerado tem 23280 eventos válidos e 183 estados observados.
Há perda parcial de telemetria: 24 eventos descartados e 3774 transbordamentos
na agregação. O tamanho de aproximadamente 7,4 MB do arquivo não demonstra
uma causa de crash.

A sequência final é conclusiva quanto à falha imediata:

1. `realloc(8388608, align 0)` falha, chamado de `libmono.so+0x2ACF94`.
2. O heap informa 172955 KiB usados e 47176 KiB livres. Isso sugere
   fragmentação; o total livre não garante um bloco contíguo de 8 MiB.
3. `libmono.so+0x2B1624` chama `memset` ao expandir um array nativo de
   ponteiros. A desmontagem do APK confirma esse caminho e a ausência de
   tratamento da falha de realocação nesse trecho.
4. O destino é `0x00200000`, derivado do ponteiro nulo mais o deslocamento
   do conteúdo anterior. O log registra Data Abort e `SIGSEGV/SEGV_MAPERR`.
5. Unity encerra o render; o loader registra `game exited with 0`.
   Esse último código não significa que a sessão terminou sem falha.

Não foi um simples fechamento voluntário. A ausência de psp2dump não impede
esse diagnóstico: os registradores e o backtrace estão no próprio log.
Não há evidência suficiente para atribuir toda a pressão de memória a um
vazamento específico.

## Recuperação de alocações nativas

A recuperação nos pools mapeados de vitaGL antes atendia apenas grandes
`memalign` chamados pela Unity. O `realloc` do Mono não era coberto.

Agora os buffers nativos de `libmono.so` e `libunity.so` também podem usar
essa recuperação em `malloc`, `calloc`, `realloc` e `memalign`, quando o
alocador normal falha. Continuam valendo o mínimo de 1 MiB, o limite de
32 registros e o controle separado de propriedade das alocações.

Na migração de um buffer normal, o conteúdo anterior é copiado e liberado
somente depois do sucesso da alocação alternativa. Se todos os pools
falharem, o buffer antigo continua válido e a chamada retorna NULL.
`calloc` mantém a inicialização em zero e rejeita multiplicação com overflow.
A recuperação depende de espaço nos pools; não elimina falta de memória
real nem garante estabilidade ilimitada.

## Efeitos de coleta

O APK bloqueia os efeitos de `StageEffectManager` quando a configuração
salva usa LightMode. Isso afeta o estoque inicial, a reprodução e o retorno
dos objetos ao pool, inclusive os efeitos de argolas e itens de coleta.
Só retirar o retorno antecipado de `PlayEffect` não resolveria: o estoque
continuaria ausente.

O patch verifica o tamanho de 512 bytes e o prefixo original de
`StockStageEffect(bool)`. Substitui apenas a leitura de `get_lightMode`
por `pop; ldc.i4.0`, mantendo os offsets e a pilha. O manager passa a
estocar, reproduzir e reutilizar os efeitos pelo caminho normal do jogo.
A preferência salva, a qualidade global e as condições de recursos/chefe
continuam com seus valores originais.

Isso habilita o pool de efeitos da fase, não apenas uma animação isolada.
Há custo adicional de memória e renderização em LightMode a medir no Vita.
Os guardas deixam o método intacto se o IL recebido for diferente.

## Iluminação, materiais e buddies

A versão pública força um shader difuso sem luz e sem `_EnvTex` nos
personagens. Isso perde a resposta à iluminação e a contribuição metálica
usada também por materiais de argolas e inimigos.

A auditoria das fontes do APK e do cache fornecido identificou ainda:

- `ykCmn_dme1` não tem `_InnerZOffset`, portanto ficava fora da substituição
  de vértices enquanto seu fragmento era substituído.
- `ykChr_dmue1` tem UV animado, perdido na substituição anterior.
- Vidro e falloff compartilham `_DifTex`, `_EnvTex` e `_DifCol`, mas têm
  outra equação. A regra antiga podia substituir apenas seu fragmento.

O novo par preserva a equação difusa/envmap do jogo, luz e ambiente,
máscara de reflexão, normal, UV, offset Z e a variante com UV animado.
Mantém a distinção entre Lambert assinado do material comum e
half-Lambert dos personagens. Usa normalização protegida contra vetores
zero; quando luz e ambiente estão ambos zerados, aplica luz neutra para
não tornar o modelo inteiro preto. Esse fallback é uma adaptação do port,
não uma reprodução exata da iluminação móvel nessa condição.

A seleção agora abrange as três variantes verificadas e exclui vidro e
falloff. O cache usa a própria fonte como chave, então os programas alterados
serão recompilados no primeiro uso; não precisa apagar todo o cache.

São correções de caminhos concretos de renderização. O relato dos buddies
pretos e a fidelidade visual de Eggman/personagens ainda precisam de
comparação em hardware; o log sozinho não confirma qual material apareceu
preto na tela do usuário. Não repetir como garantida a antiga tentativa
0.0.57: esta implementação preserva os coeficientes de luz e as variantes,
em vez de apenas somar um reflexo neutro ao shader difuso.

## Validação e pacote

Oito testes de host passaram com AddressSanitizer/UndefinedBehaviorSanitizer:

- Recuperação de alocação: reprodução de crescimento de 2 para 8 MiB com
  falha do alocador normal, preservação dos bytes, falha de todos os pools,
  alinhamento, zeros, overflow, propriedade e oito usuários concorrentes.
- Patch de efeitos: identidade do APK, execução do prefixo IL original e
  alterado, recursos ausentes, flag de LightMode e guardas contra divergência.
- Shaders/uniforms: estágios GLES separados do APK e do cache disponível,
  variantes comum/animada, exclusão de vidro/falloff e aliases de uniforms.
- IL da fase, IL de desempenho, menu/configuração, cache de shader e
  orçamento de texturas. O harness deste último recebeu o stub da API
  `bundle_mips_set_limit` que faltava; a implementação de produção não mudou.

Build ARM/VitaSDK concluído sem avisos. Integridade ZIP do VPK e igualdade
do `eboot.bin` empacotado com o compilado verificadas. `git diff --check` passou.
Os testes de seleção de shader não executam o compilador Cg nem a GPU do Vita.

Pacote: `build/VPK/SRR-Vita-v0.0.77.vpk`.
Build: `20261104T105106Z`.
SHA-256: `5d5341b2e6dba095923dcb78f76d9b2c4828994d71a91d8482c60c3a6a97dc6f`.

## Validação pendente no Vita

1. Conferir Eggman, personagens adicionais, buddies, argolas normais e
   douradas em menus e corrida, incluindo giro do modelo e reflexos.
2. Coletar argolas e gemas em LightMode, incluindo fase de chefe. Confirmar
   partículas e reutilização dos efeitos durante uma corrida prolongada.
3. Alternar menus, seleção de buddies, roleta e várias corridas por pelo
   menos uma hora, incluindo retorno à fase de chefe próxima ao crash.
4. Comparar FPS e memória com efeitos habilitados; conferir compilação/link
   no primeiro uso e reutilização do cache na segunda execução.
5. No novo log procurar `effects: stage pickup pool enabled`,
   `lit diffuse/envmap compatibility` e eventuais recuperações
   `mem: recovered fragmented engine allocation`. Investigar qualquer nova
   falha de alocação ou Data Abort em vez de aceitar apenas o código de saída.

Não houve validação visual ou sessão longa no aparelho nesta entrega.
