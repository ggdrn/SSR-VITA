# SRR Vita 0.0.63 — revisão da 0.0.62 e cópia dos arrays de UI

## Captura recebida

`build/VPK/srr_log.txt`: **0.0.62**, build **20261031T012137Z**. Aproximadamente **12 min 54 s** observados, último quadro 19999, 6216 eventos OBS válidos, 52 fases. Cópia e hash preservados em `build/diagnostico-0.0.63-baseline/`, com relatório e tabelas.

A captura não tem encerramento normal nem registro de erro fatal no final: termina em criação de objetos e link de programa na fase 52. Isso não demonstra que a sessão inteira terminou sem problemas. Os registros finais de saúde apontam 12 eventos descartados, 875 estouros da agregação e zero descartes do logger; os totais são parciais.

## O que a 0.0.62 confirmou no aparelho

- A otimização de geometria instalou: `NGUI geometry appends use bounded bulk copies`. O contador chegou a **634431 appends rápidos**, com **7 fallbacks originais**. A recusa do prólogo ARM foi resolvida nesta captura.
- A correção de threads está reciclando registros: `created 32 tracked 11/128 high-water 14 released 21`. Não há `thread table full` nesta captura. A amostra confirma reutilização; esta sessão não ultrapassou 128 criações nem substitui o teste longo de transições.
- Arte e gate dos botões registram instalação bem-sucedida.
- Persistem alguns `free(...): misaligned` de `libunity.so+0xB9358`. Não há evidência suficiente para relacioná-los a um novo crash ou ao principal gargalo de UI. Não foi alterada a política de liberação com base somente nesses registros.

## Desempenho restante

Nas fases longas observadas como corrida, existem janelas de cinco segundos próximas de 55–58 FPS, outras de 33–43 FPS e quedas a aproximadamente 23–29 FPS. Os valores foram calculados por quadros/tempo completo da janela, incluindo trabalho fora de `nativeRender`. Cena de corrida também contém início, resultado e espera do usuário; esses números não constituem um benchmark exclusivamente de gameplay.

Ainda há picos do callback `UIPanel.LateUpdate` de **120,08, 130,24, 135,73, 138,28, 141,82, 146,51 e 147,41 ms**, mesmo com os appends rápidos ativos. Portanto a cópia em bloco não elimina todo o custo desse callback. Seus tempos são inclusivos: atualização de widgets, preenchimento, conversão dos buffers e upload de malhas podem ocorrer dentro dele.

Há também esperas e criação de objetos nos menus/transições, com quadros acima de dois segundos; não devem ser confundidos com os picos durante a corrida. Algumas janelas de aproximadamente 60 FPS no menu/resultados têm somente 28 draws e não servem para declarar 60 FPS em todas as fases.

## Alteração da 0.0.63

A inspeção do IL mostrou outra cópia gerenciada no caminho de UI:

`UIPanel.Fill` → `UIDrawCall.Set` → `BetterList<T>.ToArray` → `BetterList<T>.Trim`.

`Trim` cria um array do tamanho exato quando o buffer tem capacidade maior que a quantidade usada, e copia os elementos com um laço de `ldelem/stelem`. Os arrays resultantes alimentam vertices, normais, tangentes, UVs e cores da malha. Os appends otimizados da 0.0.62 não substituem essa etapa.

O laço passa a chamar **`System.Array.Copy(source, 0, destination, 0, size)`**, usando a referência já existente no APK. Alocação, tamanho exato, identidade do array quando já tem o tamanho correto, campo `size`, limpeza para tamanho zero e atribuição final permanecem como no método original. O runtime conserva o tratamento de tipos e as barreiras de escrita do GC para referências. Não há cópia nativa de referências gerenciadas.

Guardas antes de editar:

- MVID exato da Assembly-CSharp suportada.
- Método `0x060001EC`, corpo inteiro de 105 bytes idêntico ao original.
- `maxstack >= 5`; o APK original declara 6.
- Apenas o intervalo **0x2B–0x54** muda. O restante das instruções, saltos, locais e tamanho do método permanecem.

A edição ocorre antes da preparação de geometria/JIT no mesmo ponto das demais alterações de IL. Métodos já compilados podem manter o código anterior. O marcador é `ui: BetterList Trim uses runtime Array.Copy ... future JIT`.

**Não foi medido o tempo isolado de Trim no Vita nem o ganho desta troca.** É uma redução de custo no caminho identificado, sem alterar a quantidade de elementos ou reduzir qualidade gráfica. Não se afirma que os picos de 147 ms desapareceram. A próxima captura deve comparar o mesmo trecho de corrida; se continuarem, o próximo diagnóstico deve separar UpdateWidgets, Fill e o upload de Mesh para evitar atribuir todo o callback à mesma cópia.

## Validação

- ASan/UBSan do patch real: rejeição de MVID, tamanho, IL, stack insuficiente, header/API ausentes; alteração restrita aos 42 bytes previstos e nenhuma segunda edição do corpo já alterado.
- Execução do **IL original e do IL modificado do próprio APK** em runtime .NET, carregados em contextos separados: 90 casos em cada versão, com int, long, string, object e estrutura de três floats. Conferidos comprimento, conteúdo, preservação da identidade quando não há redimensionamento, tamanho zero, repetição e referências após GC.
- O teste gerenciado também verifica MVID, assinatura do token de `Array.Copy`, maxstack, ausência de EH e identidade de todos os bytes fora do intervalo alterado. A cópia de teste da DLL é temporária; o APK do usuário não foi regravado.
- Passaram os testes de retirada do GC terminal do terreno, wrapper de geometria e 1649 threads. A compilação Release VitaSDK concluiu sem avisos nesta recompilação.
- ZIP íntegro; eboot, imagens, XML e payload da splash empacotados iguais aos arquivos atuais. `git diff --check` passou.

Entrega: **`build/VPK/SRR-Vita-v0.0.63.vpk`**, **4588586 bytes**.
Build: **20261031T031201Z**.
SHA-256: **73dafb563c624e266bb5928d495a50ad6edc9e140e60914200f25cd1d0a1e86f**.

Sem execução da 0.0.63 no Vita nesta entrega. As melhorias já demonstradas no aparelho são as da 0.0.62 descritas acima; o ganho da nova cópia precisa da próxima medição.
