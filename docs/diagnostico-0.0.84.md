# 0.0.84 — regressão de tela cinza da 0.0.83

## Causa confirmada pelo log

O log informa 0.0.83 em QHD 960×544, Graphics 1, 30 FPS e Material Shaders On.
A interface falha repetidamente com:

```
InvalidProgramException: Invalid IL code in BetterList`1:Sort
(System.Comparison`1<T>): IL_0006: dup
  at UIPanel.LateUpdate ()
```

Nos quadros 1200 e 1800, há zero draw calls, zero erros GL e nenhum programa
com link malsucedido. A exceção da ordenação interrompe a atualização dos
painéis antes do desenho. Isso explica a tela cinza com áudio e entrada
funcionando. Uma cópia do log foi preservada em
`build/analysis-0.0.84/srr_log_83_gray.txt`.

## Correção

A substituição de Sort é novamente idêntica, byte por byte, à implementação
adjacente usada antes da 0.0.83. Remove-se a inserção por deslocamento que o
Mono embarcado rejeitou. O gerador também deixa de produzir essa variante.
A mensagem de inicialização identifica a ordenação restaurada.

O IL rejeitado mantinha um índice na pilha de avaliação ao atravessar os
laços. Isso é uma hipótese para a incompatibilidade com o JIT antigo, não
uma reprodução completa do compilador Mono. Os testes anteriores rodavam
em .NET moderno e passaram; eram insuficientes para comprovar compatibilidade
com o Mono embarcado na Unity 4.7.

Um novo teste conservador exige pilha vazia nas arestas de retorno dos laços
de Sort e identidade com os bytes da implementação anterior. Ele aceita o
corpo original e o restaurado, e rejeita explicitamente o IL da 0.0.83,
preservado apenas como fixture de regressão. Não emula o JIT do Vita.

O atalho de propriedade dos painéis e a preparação JIT da experiência e dos
resultados da 0.0.83 permanecem. QHD, qualidade, shaders, efeitos e limite de
FPS não foram alterados. Não é necessário apagar saves nem cache de shaders:
o defeito está no IL alterado pelo executável.

## Validação

Passaram os testes do gate conservador, instalação/guards/rollback de Sort,
1400 casos gerenciados contra o Sort original, identidade com a variante
anterior, UpdateWidgets (guards e 30.000 visitas gerenciadas) e preparação
JIT. Build VitaSDK, integridade ZIP, identidade do eboot empacotado e
`git diff --check` verificados.

Pacote: `build/VPK/SRR-Vita-v0.0.84.vpk`.
A volta da imagem e os tempos de quadro ainda precisam ser conferidos no
Vita; não há medição de 30 FPS fixos nesta entrega.
