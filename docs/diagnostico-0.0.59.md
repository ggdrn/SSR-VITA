# SRR Vita 0.0.59 — logo original com base branca, ícone circular e callbacks gerenciados

## Entrada e resultado observado da 0.0.58

Log `build/VPK/srr_log.txt`: 0.0.58, build `20261030T172248Z`, 5350 linhas, SHA-256 `d2e9c3e1b853666454fe646f5f5b1cf9b803e6ff2862680f89b3271c26ccde59`.
Cópia preservada em `build/perf-baselines/srr-0.0.58-main.log`.
Relatório em `build/analysis-0.0.58/` (3761 eventos OBS válidos). A captura perdeu 130 eventos e teve 822 ocorrências de saturação da tabela; a cobertura é parcial.

Imagem de referência: `build/VPK/je/2026-10-30-113923.jpg`. O arquivo original não foi alterado. O print mostra o título a 60 FPS; isso não mede a taxa da corrida.

- A preparação de JIT terminou **3666/3666**, sem falhas, em **8349,18 ms**, com **109389 KiB** nativos disponíveis. Esse custo da primeira entrada está separado do gameplay.
- O patch `CreateTerrain terminal forced GC removed` foi confirmado.
- CPU/BUS/GPU/XBAR: **444/222/222/166 MHz**, afinidade de quatro núcleos.
- Cache de shaders: **337 hits, zero misses, zero erros e zero compilações**. Nenhuma mudança de shaders foi necessária nesta entrega.
- Há uma coleta de **386,24 ms**, no quadro 4236, a 233,18 s: apenas 1,21 s após começar o estado observado `s_playingstage loading=0`. Ela está no limite final do carregamento, quando ainda se concluem recursos da pista. As janelas posteriores não registram GC. O log não confirma que essa corrida tenha atravessado todas as trocas de terreno que causavam pausas na sessão anterior.
- A cena de corrida observada cobre aproximadamente **232–304 s**. Nas janelas após a entrada, o P50 varia de **18 a 29 ms**; não são 60 FPS constantes. Há poucas janelas, uma corrida e trechos de transição: não constitui comparação controlada com a captura anterior.

## Onde ainda melhorar

### JIT de estados do chefe

Quadro **6037**: render **134,24 ms**, JIT **32,43 ms**, 37 métodos, maior método `BossStateAppearMapBase..ctor` (4,18 ms). Criação medida de objetos: 5,87 ms. Sem GC, leitura ou link de shaders relevantes.

A janela seguinte registra 85 métodos compilados em 156,61 ms acumulados, maior `BossStateDamageBase.Step` (10,47 ms). Os estados comuns de chefe não entravam no prefixo `Obj` da preparação anterior.

**Mudança:** incluir as classes com prefixo `BossState` e `TenseParameter` na mesma preparação, depois dos patches IL. A lista passa de **3666 para 3845 métodos** (+179), mantendo identificação do assembly, exclusão de genéricos abertos/inicializadores estáticos e margem de memória. A primeira entrada poderá demorar um pouco mais; o ganho e custo reais precisam do próximo teste no Vita.

### Pausas sem atribuição suficiente

Os quadros **6606 / 6611** levam **309,15 / 359,63 ms**. Não têm JIT ou GC registrado, nem leitura, criação ou link relevantes; as chamadas gráficas medidas ficam abaixo de 1 ms. Os quadros próximos chegam a **105–129 ms** com o mesmo padrão.

Isso localiza o tempo dentro de `nativeRender`, mas não demonstra que seja exclusivamente código gerenciado ou execução da GPU. Chamadas gráficas medem tempo de CPU no bridge e não toda a execução gráfica. Não foi desativada lógica do jogo com base nessa lacuna.

**Novo medidor limitado:** as duas importações da Unity de `mono_runtime_invoke` e `mono_runtime_invoke_array` passam por wrappers que encaminham os mesmos argumentos uma única vez e preservam retorno/ponteiro de exceção. São medidas apenas chamadas na thread principal; reentradas não são somadas duas vezes. O medidor não altera código gerado pelo JIT, não ativa eventos de alocação/entrada-saída do profiler e não enumera objetos.

Nos quadros lentos, a linha `perf: main managed-callbacks ... longest ... Classe.Método` informa o tempo inclusivo e o callback mais demorado. A cada 600 quadros, `perf: managed callback coverage N outer calls` mostra se há cobertura efetiva. O código executado diretamente por thunks ou por outros caminhos não fica coberto; contagem zero não demonstra ausência de trabalho gerenciado.

Os wrappers consultam relógio/thread em cada entrada medida e têm custo próprio. Nenhuma alocação ou gravação de log ocorre dentro deles. As categorias continuam inclusivas: um callback pode conter criação, GC ou I/O, e seus tempos não devem ser somados como parcelas independentes. O custo dessa instrumentação ainda precisa ser avaliado no Vita.

O final do log registra também JIT de `RankingDataContainer.CheckRankingList` em um quadro de 239,33 ms. A captura termina nessa transição, sem demonstrar um ciclo completo de resultado e retorno ao menu.

## Artes solicitadas

### Ícone redondo

O extrator agora usa **`res/mipmap-xxxhdpi-v4/ic_launcher_round.png`**, a variante circular original do APK. Saída `sce_sys/icon0.png`: **128×128**, PNG indexado de 8 bits, com exterior transparente.

A validação encontrou um detalhe na conversão para paleta: a média de cores do quantizador podia transformar alfa 0 em alfa 1. Foi reservada uma entrada de paleta com transparência exata e todos os pixels originalmente transparentes são remapeados a ela. Os quatro cantos foram conferidos também no arquivo dentro do VPK.

### Fundo

O fundo da LiveArea passa a usar a textura original **`Splash3`** do APK, no lugar do sprite usado na 0.0.58. Ela é uma faixa 2048×512; para preencher **840×500** sem deformar os personagens, o gerador mantém a proporção e faz recorte central. A textura completa é preservada em `assets/livearea/splash-background.png`.

### Logo Start igual à composição do título

A composição anterior só empilhava as três palavras, omitindo elementos que aparecem no print. A nova composição reproduz seis camadas da cena **`assets/bin/Data/mainData`**, hierarquia de título 337:

1. Base branca: `ui_title_img_sonic_base_white`.
2. Estrelas: `ui_title_img_star`.
3. Base azul: `ui_title_img_base_blue`.
4. Palavra Sonic: `ui_title_img_sonic`.
5. Palavra Runners: `ui_title_img_runners`.
6. Palavra Revival: `revival`.

O gerador lê os recortes e paddings do UIAtlas e os tamanhos dos widgets, posição, escala, cor e profundidade da cena. Isso recupera a **base/relevo branco, o contorno azul e as estrelas**, além de acertar as proporções e sobreposições das palavras. Camadas de flash com alfa negativo não entram na imagem estática.

A composição sem redução está em `assets/livearea/title-logo.png`; o Start usa essa composição ajustada a **280×158**. As fontes continuam sendo os assets do APK, sem recorte da interface/HUD do JPG e sem geração de arte. A proveniência e as seis camadas estão em `assets/livearea/sources.json`.

`content-rev` da LiveArea foi elevado para 3. Prévia de composição em `build/livearea-assets/livearea-preview.png`; a posição do gate, sua borda e o botão são controlados pelo sistema do Vita.

## Validação e pacote

- Build Release ARM/VitaSDK concluído. Permanecem avisos de truncamento antigos em `watchdog.c` e `libc_bridge.c`, recompilados por dependerem do header de medição; nenhum aviso nos novos wrappers.
- Teste com ASan/UBSan dos wrappers reais: argumentos, retornos e ponteiro de exceção preservados, encaminhamento em workers, aninhamento na thread principal, worker durante chamada principal e APIs ausentes.
- Testes da preparação real de JIT e dos contadores por quadro passaram com ASan/UBSan.
- ZIP/VPK íntegro, eboot idêntico ao compilado, imagens empacotadas iguais aos arquivos atuais, dimensões/paleta de 8 bits, cantos transparentes do ícone/logo e XML rev 3 conferidos.
- Logo, ícone e prévia inspecionados visualmente. `git diff --check` passou.

Pacote: `build/VPK/SRR-Vita-v0.0.59.vpk` (**1328297 bytes**).
Build: `20261030T195256Z`.
SHA-256: `9cad7358498c8a12d52726065fd1371ed2effa7e6170924730a1023e8a6443c5`.

Sem teste desta build no Vita nesta entrega. Na próxima sessão, conferir as artes da LiveArea, jogar até uma aparição do chefe e troca de terreno, e enviar o log com `managed callback coverage` e `managed-callbacks` nos picos. Não há confirmação de eliminação das pausas de 105–360 ms nesta etapa.
