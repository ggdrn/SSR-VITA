# SRR Vita 0.0.47 — orçamento de GC e demora dos menus

Baseline: `build/perf-baselines/srr-0.0.46-intermediate.log` (2199 linhas).

## O que o aparelho confirmou

O cache chegou a 551 hits, zero misses, zero erros e zero chamadas do compilador. Portanto, pré-compilar mais shaders não explica nem corrige os engasgos recorrentes desta execução. As pausas de coleta continuam aproximadamente em 374–397 ms nas janelas finais, com 347–364 ms gastos na marcação. Muitas dessas janelas têm zero solicitações explícitas via mono_gc_collect, confirmando que não basta remover os poucos pedidos manuais conhecidos.

Nos menus/transições há quadros de 8,45, 9,48, 12,10, 14,31 e 15,63 segundos. Por exemplo, o quadro 2453 leva 15.629,05 ms, mas registra 69,56 ms de textura, 16,92 ms de link, zero GC, zero I/O pelas entradas medidas e 124,73 ms de esperas medidas. O restante ainda precisa de atribuição dentro da Unity. Isso não prova ausência de I/O/trabalho em workers ou de etapas não instrumentadas.

As 72 requisições têm mediana de 181,5 ms e máximo de 4042 ms. Algumas contribuem para os carregamentos, mas não explicam sozinhas todos os quadros longos na thread principal. Seus tempos incluem callbacks; somá-los não fornece tempo sequencial porque há concorrência.

O log usa uma compilação intermediária da 0.0.46: contém os marcadores antigos de descarte dos enumeradores, apenas dois grupos de tempo por pico, e nenhum registro de instalação/cobertura dos medidores de criação de assets. Os últimos ajustes da 0.0.46, descritos no documento anterior, continuam no código e estão incluídos integralmente na 0.0.47. A nova versão também identifica cada configuração/build por um timestamp UTC gerado pelo CMake, em vez do horário em que main.c foi compilado pela primeira vez.

## Correção da medição de memória

A política da 0.0.45/46 usava apenas mallinfo.fordblks para decidir se havia espaço para aumentar o orçamento entre coletas. Esse número conta os blocos livres que dlmalloc já administra, mas não o restante da reserva nativa que ainda não passou por sbrk. Isso podia manter a política desativada mesmo com capacidade nativa disponível.

O VitaSDK instalado foi conferido pelo disassembly do executável: _init_vita_heap reserva o bloco completo solicitado (220 MiB); _sbrk_r avança dentro desse bloco; _get_vita_heap_size informa seu tamanho. A nova função native_heap_available consulta o bloco que contém __malloc_sbrk_base e exige que seu tamanho coincida com esse tamanho real do SDK. Valida endereço base, limite, overflow e posição atual de sbrk. Somente então acrescenta a cauda ainda não usada, descontando uma página de margem. Em caso de erro/divergência, mantém o número conservador anterior. Não conta pools gráficos nem RAM livre externa à reserva nativa.

A política do GC continua com os mesmos limites: divisor 2 apenas em gameplay sem carregamento, heap gerenciado de até 64 MiB, pelo menos 12 MiB de folga gerenciada e 48 MiB de espaço nativo; retorno a 3 com pouca folga e fora dessas condições. O novo cálculo pode fazer essa política ativar quando a reserva permitir. A escrita continua limitada a GC_START, com a trava do coletor. O objetivo é reduzir frequência das coletas automáticas; o custo de marcação de cada coleta não foi corrigido por esta alteração.

O log agora separa `native available`, `chunks` e `reserved-tail`, além do divisor desejado/ativo. O teste usa a função C real em conjunto com gc_policy.c: 14 MiB em blocos livres + 100 MiB de reserva ainda não usada permitem solicitar 2; uma identificação inválida do bloco volta ao cálculo conservador e solicita 3. Esses valores são um cenário de teste, não uma medição já feita no Vita.

## Diagnóstico das telas e dos modais

Além dos três medidores de criação/carregamento da 0.0.46, entram wrappers de UnityEngine.WWW::GetTexture e Resources::UnloadUnusedAssets. Medem a conversão da imagem baixada em textura e a entrada de liberação de assets, respectivamente, preservando argumentos e resultados. UnloadUnusedAssets é assíncrono: o tempo da entrada não representa todo o trabalho posterior. Não foram reativados os patches antigos que compartilhavam imagens entre owners da interface.

As cinco entradas só são substituídas se os ponteiros originais estiverem no libunity. O log mostra quantas foram registradas e sua cobertura por janela. Wrappers já compilados podem reter a entrada anterior; zero chamadas não é prova de ausência de trabalho. Tempos de criação/decodificação são inclusivos e podem conter GC/I/O, por isso não devem ser somados como categorias exclusivas.

## Validação e pacote

25 testes passaram. O teste novo cobre a reserva nativa, falhas de consulta, bloco divergente, limites de endereços, overflow, margem, fallback e integração com a política do GC. O teste de timers agora cobre as cinco funções, incluindo bool/this, retorno NULL, encaminhamento único e proteção contra ponteiros externos. Testes C usam AddressSanitizer/UndefinedBehaviorSanitizer.

Build ARM/VitaSDK concluído. Integridade ZIP e igualdade do eboot com o build verificadas. Pacote: `build/VPK/SRR-Vita-v0.0.47.vpk`.

Build ID: `20261029T121930Z`.
SHA-256: `902af0a2f19ea9a55a1e4d68f6178231e318db3c4c2965baf200a7e1e94103f9`.

A melhora e os tempos novos precisam de execução no aparelho. Reproduzir a demora dos modais iniciais, entrar na mesma fase por 2–3 minutos e enviar o srr_log.txt da 0.0.47. Manter saves e cache atuais.
