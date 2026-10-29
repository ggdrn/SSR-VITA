# SRR Vita 0.0.41 — engasgos com gráficos completos

Baseline: `build/perf-baselines/srr-0.0.40.log`, cópia do novo log do usuário. O usuário confirmou que o modo completo funciona. O log não apresenta SIGSEGV, mas ainda há erros recuperados de memória de textura.

## Evidência

Durante a corrida, os intervalos terminando nos quadros 15600, 16200, 16800 e 17400 têm médias de 34,21 a 41,80 ms por nativeRender. Há picos de aproximadamente 415–436 ms com GC: a marcação ocupa cerca de 350–355 ms, enquanto a suspensão leva cerca de 1–5 ms. Há também picos sem GC, como 242,58 e 145,51 ms, e um quadro de 1876,79 ms com uma coleta, cujo custo não é explicado integralmente pela coleta típica.

Não se devem misturar esses dados com os carregamentos: antes da corrida há falta de memória, tentativas repetidas de mipmaps de 5.592.544 bytes, falha de alocação de aproximadamente 11 MiB no heap nativo apesar de cerca de 40 MiB livres no total, e um quadro de 100.804 ms. Os totais livres não garantem um bloco contíguo suficiente.

O pedido anterior de usar quatro núcleos já está efetivo: o log confirma máscara ativa 0xF0000. Os quatro núcleos não eliminam as pausas de coleta.

## Mudanças concretas

1. `SoundManager.SePlay` criava um array de cinco strings e uma string concatenada para chamar `OutputPlayLog`, mesmo quando a amostragem nativa descartava o registro. O IL desses 48 bytes foi substituído por NOPs, com correspondência exigida de todo o método original. Verificação da instância, chamada real de reprodução, argumentos e retorno permanecem. Isso elimina duas alocações gerenciadas de depuração por efeito. As mensagens individuais de SePlay deixam de aparecer; isso não significa ausência de áudio.
2. `GameModeStage.StatePause` e `StateChangeLevel` executavam `GC.Collect`, `Resources.UnloadUnusedAssets`, descarte do resultado e outro `GC.Collect`. Foi removida apenas a primeira chamada de cinco bytes, protegida por tamanho do método e sequência completa de 16 bytes. O descarregamento, a coleta posterior e as demais coletas automáticas permanecem. A alteração trata pausas/transições; não comprova eliminação dos GCs periódicos da corrida.
3. Se a expansão de uma textura para mipmaps falhar, as tentativas seguintes para a mesma textura e o mesmo buffer no mesmo quadro não repetem a recuperação emergencial demorada do alocador. Uma nova imagem base ou um quadro posterior pode tentar novamente. A textura original continua preservada e o erro continua sendo informado. Esse limite não resolve a falta de memória em si.
4. Os picos agora incluem tempos nativos separados das chamadas de desenho, upload de texturas, apresentação e link de programas. São tempos de CPU nessas chamadas, incluindo esperas que elas efetuarem; não são medições diretas da execução da GPU. Outras chamadas GLES e trabalho interno do Unity não entram nesses quatro grupos. Os contadores são consumidos e zerados por quadro, sem percurso de objetos da cena.

A biblioteca local vitaGL foi recompilada e vinculada. O patch atualizado está em `patches/vitagl-texture-oom.patch`. Os ajustes IL são feitos somente na memória, antes do JIT, sem modificar o APK ou save.

## Validação

Os 12 testes passaram, com AddressSanitizer/UndefinedBehaviorSanitizer nos testes C. O teste novo compara o SHA-256 da DLL do APK com a fixture, executa a função real de patch, verifica os bytes vizinhos e a rejeição de tokens divergentes. Um interpretador do IL de SePlay confirma argumentos/retorno idênticos e duas alocações antes contra zero depois, incluindo retorno sem instância. O teste de texturas confirma ausência de nova tentativa no mesmo quadro e recuperação permitida no quadro seguinte.

Build ARM/VitaSDK concluído; VPK e eboot conferidos. Avisos de compilação existentes em arquivos não alterados permanecem. SHA-256 do VPK: `7d436729246af39311befc37ac100f85e0e2590fe78bd52e87e48f25b1e57a05`.

Instalar `build/VPK/SRR-Vita-v0.0.41.vpk` e repetir a mesma fase por dois a três minutos com o modo leve desligado, incluindo uma pausa/retomada. Enviar o novo `srr_log.txt` para medir o efeito e analisar as linhas `perf: graphics`. Não há resultado de desempenho desta versão no aparelho ainda.
