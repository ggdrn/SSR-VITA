# SRR Vita 0.0.40 — fechamento com gráficos completos

Os dois logs enviados são da 0.0.39 e terminam com SIGSEGV no endereço zero. O relatório do Unity mostra destino r0=0, origem r1=8f1ceb40 (8edbeb40 no outro log) e tamanho r2=007fffc0, compatível com uma cópia de aproximadamente 8 MiB para um destino nulo. Os registradores também incluem dimensões 2048×2048. Isso sugere falha de alocação de textura, mas o contexto original não identifica com segurança o chamador. O PC e00099b8 está fora das bibliotecas Android carregadas. A falha posterior em PC=0 ocorre durante o encadeamento do próprio relatório e não identifica a origem.

As cópias dos logs estão em `build/perf-baselines/srr-0.0.39-srr_log*.txt`.

## Mudanças

No vitaGL realmente vinculado ao port (`../vitaGL/libvitaGL.a`), foram identificadas duas cópias sem verificação do resultado da alocação:

- Atualização de uma textura usada recentemente: se a cópia para um buffer novo não puder ser alocada, agora chama glFinish antes de atualizar o armazenamento original. Não libera esse buffer e não copia para NULL. Essa recuperação pode produzir uma pausa, mas mantém a textura válida.
- Expansão para mipmaps: se tanto realloc quanto a alocação alternativa falharem, preserva o buffer e descritor originais e informa GL_OUT_OF_MEMORY. A atualização posterior rejeita níveis que não chegaram a ser alocados, evitando escrita além do buffer original.

Não se afirma que esses sejam os únicos caminhos possíveis da falha observada. Os novos registros permitem distinguir essas falhas de outras causas: até 96 uploads grandes recebem marcadores begin/end e totais de memória livre de VRAM/RAM/PHY. Esses totais não medem o maior bloco contíguo disponível. Um callback opcional do vitaGL registra as recuperações no srr_log, mesmo com o printf de depuração da biblioteca desativado. O contexto da exceção é registrado antes de chamar o relatório do Unity, preservando PC/LR originais.

O patch reproduzível da biblioteca está em `patches/vitagl-texture-oom.patch`. Recompilar vitaGL com `make HAVE_SHARK_LOG=1 HAVE_SHADER_CACHE=1` e depois o port. A biblioteca local foi recompilada e vinculada nesta versão.

## Validação e teste no aparelho

Os 11 testes de host passaram. O teste novo extrai os trechos reais da biblioteca e simula sucesso e falha de alocação: verifica preservação do buffer, espera da GPU antes da atualização em lugar, ausência de cópia/liberação no erro de mipmap e funcionamento do caminho normal. Os testes C usam AddressSanitizer/UndefinedBehaviorSanitizer. Build ARM/VitaSDK concluído e pacote conferido.

Instalar `build/VPK/SRR-Vita-v0.0.40.vpk`, manter o modo leve desligado e testar abrir configurações e iniciar a mesma fase. A correção do fechamento ainda depende desse teste no console. Caso feche, enviar o novo srr_log.txt: os últimos marcadores de textura e o primeiro contexto nativo são os dados importantes. Não é necessário apagar save ou cache.
