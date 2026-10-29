# SRR Vita 0.0.52 — engasgos da corrida = coletas do GC

Log analisado: `srr_log.txt` da 0.0.51 (9079 linhas, ~730 s, duas corridas), preservado em `build/perf-baselines/srr-0.0.51-full.log`; relatório em `build/analysis-0.0.51/`. Os outros dois logs enviados são da 0.0.49 e 0.0.50 e já tinham sido analisados.

## O que causa os engasgos na corrida

Na corrida longa (fase 48, ~486–605 s) as janelas sem GC ficam com P50 de 17–25 ms e pior quadro de 50–140 ms. Toda janela com pior quadro de ~400–440 ms tem exatamente uma coleta de lixo. As coletas automáticas aconteceram a cada ~20 s (497,8 / 519,1 / 539,5 / 559,2 / 582,9 / 602,2 s), cada uma com ~360 ms só de marcação e ~400 ms no total. Há também um grupo de 3 coletas em 563–566 s com `UnloadUnusedAssets` e 2 pedidos explícitos do próprio jogo.

O Boehm GC 7.1 do libmono é "stop-the-world" e não incremental: a pausa é proporcional ao conjunto vivo (~20–25 MB na corrida), não ao lixo. Raízes estáticas somam ~2,2 MB (bss do libunity 1,6 MB); não são a parte principal. Não há como encurtar uma coleta daqui; dá para fazer menos coletas.

Nos menus o mesmo mecanismo pesa mais: coletas de 640–1020 ms a cada 7–13 s (fases 41–43), o que contribui para a sensação de lentidão ao navegar.

## Mudança 1 — ritmo das coletas (pacing)

O gatilho automático do Boehm 7.1 é `min_words_allocd()` (libmono+0x2bcb30): coleta quando o que foi alocado desde a última coleta passa de `(heap − livre_grande + livre_grande/4 + raízes) / 4 / divisor` palavras. O corpo inteiro da função e os slots de GOT que ela lê são conferidos byte a byte; a função é substituída **antes dos construtores do libmono**, então nenhuma thread do coletor pode estar dentro dela durante a troca. A substituta calcula a fórmula original exatamente e devolve o maior valor entre ela e um orçamento.

O orçamento é calculado a cada ~0,5 s e fixado uma vez por ciclo de coleta: blocos livres já existentes no heap gerenciado + no máximo 24 MiB de heap novo, mantendo 64 MiB de memória nativa livre para a Unity; teto de 64 MiB. Ele é retirado imediatamente se a memória nativa livre cair abaixo de 48 MiB ou o heap passar de 96 MiB (o gancho também ignora o orçamento acima de 96 MiB de heap). Com o orçamento retirado, a próxima alocação de bloco coleta como antes. Coletas explícitas (GC.Collect, UnloadUnusedAssets) não mudam.

Pelos números do log (heap 63 MB, ~20–30 MB em uso, 90–105 MB nativos livres na corrida), o intervalo entre coletas automáticas deve passar de ~20 s para algo perto de 1 minuto: espera-se 1–2 engasgos numa corrida de 2 minutos em vez de ~6, e menos coletas de ~1 s nos menus. Isso ainda precisa ser medido no aparelho.

O log registra cada orçamento fixado: `gc: pacing budget ... (collector default ...) heap ... large-free ... native free ... cycle N`.

## Mudança 2 — de onde vem o lixo (diagnóstico)

Zerar os engasgos exige reduzir a alocação durante a corrida (~1 MB/s estimado). Foi instalado um perfil de alocações pelo próprio profiler do Mono (`mono_profiler_install_allocation`, evento 0x80 só para este perfil). A cada ~600 quadros o log mostra o total da janela e as 10 classes que mais alocaram:

```
alloc: window objects N bytes K KiB untracked-classes U
alloc:   Namespace.Classe objects N bytes K KiB
```

Limitações: o Mono só reporta alocações de código compilado pelo JIT depois da instalação (no quadro ~300). O código da corrida é compilado na primeira corrida, então fica coberto; métodos de menu já compilados antes não aparecem. Métodos compilados com o perfil ativo usam o alocador genérico do Mono em vez do alocador inline, o que custa um pouco de CPU por alocação. É uma medida temporária para a próxima versão atacar as classes certas; depois disso o perfil deve ser removido.

## Mudança 3 — menos trabalho do diagnóstico nos menus

O instantâneo completo da cena (enumeração de todos os objetos) rodava a cada 3600 quadros nos menus. Agora roda só em trocas de cena e uma vez no quadro 900.

## Entrega

Compilação ARM/VitaSDK sem avisos; código gerado da função substituta conferido contra o disassembly do original. Nenhum teste no aparelho foi feito.

Pacote: `build/VPK/SRR-Vita-v0.0.52.vpk`.
SHA-256: `0213b1adb93d96c93f60e9ce2a5742df98b705e0a93894bd24331aee90a87a30`.

Para a próxima rodada: jogar duas corridas normais e enviar o `srr_log.txt`. As linhas `gc: pacing` e `alloc:` são as que importam.
