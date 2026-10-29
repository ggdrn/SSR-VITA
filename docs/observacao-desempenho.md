# Observação de desempenho — SRR Vita 0.0.49

A captura detalhada é automática, desde main até a saída. Continua no arquivo `ux0:data/srr/srr_log.txt`, junto aos registros existentes.

## Correção da captura no aparelho

O fluxo completo enviado pelo usuário com a 0.0.48 confirmou `OBS writer unavailable -2147319773`, isto é, `0x80028023 / SCE_KERNEL_ERROR_ILLEGAL_PRIORITY`. A prioridade 0x10000120 impediu a criação da thread e nenhum evento OBS foi gravado. A 0.0.49 usa a prioridade já utilizada pelas threads de rede e, se recusada, tenta a prioridade do watchdog. O início bem-sucedido registra `OBS writer ready`.

O log real foi preservado em `build/perf-baselines/srr-0.0.48-full-flow.log`. Ele já contém tempos dos oito medidores nativos e mostra chamadas de carregamento de recursos de até 2169,73 ms, criação de objeto de até 1678,78 ms e conversão de imagem WWW de até 1287,38 ms. Há quadros de até 19,39 s ainda sem atribuição completa. No quadro 628, as 2017 chamadas de I/O somam 2836,15 ms; o maior chamador, libunity+0x3d5b84, é uma chamada a fopen no binário. Portanto, não se deve interpretar essa contagem como 2017 leituras de dados ou resolver isso apenas aumentando um buffer de leitura.

## O que é registrado

- Marcos de abertura do processo, requisitos, índice APK, bibliotecas, inicialização gráfica e Unity.
- Pedidos de troca de cena e estados observados de cena/loading a cada 15 quadros. Essa observação não enumera a cena. Busca o nome apenas quando o índice da cena muda.
- Toques, com coordenadas, para relacionar as ações do usuário aos pedidos e recursos seguintes. O instante de um toque não prova o instante em que um modal terminou de desenhar.
- Abertura de arquivos pelo nome e leituras pelo nome associado ao handle. O mapa é limitado, respeita reutilização/fechamento de handles e usa um identificador quando não consegue identificar o arquivo com segurança.
- Leitura/extração de entradas do APK pelo nome, incluindo o tempo de descompressão na extração.
- Resources.Load, AssetBundle.Load/LoadAsync, clonagem/criação de objetos, WWW.GetTexture e UnloadUnusedAssets, preservando argumentos e retornos.
- Links de shader por programa e IDs dos shaders; eventos de hit/miss/gravação/erro/compilador do cache por programa. O tempo de link em modo postponed inclui a compilação que ocorrer nele.
- Uploads por ID de textura, dimensões e mip. Bytes são registrados onde o tamanho é conhecido; zero nos uploads não comprimidos significa tamanho não calculado, não ausência de dados.
- Cada amostra de GC disponível no ring, com seu início real e tempos de suspensão, marcação, liberação e retomada.
- Resumos por janela de aproximadamente cinco segundos: número de quadros, P50/P95/P99 em histogramas de 1 ms, pior quadro, total de render, quadros >50/>100 ms e coletas. 2001 nos percentis significa 2001 ms ou mais; o máximo mantém seu valor exato.

Os registros `OBS {...}` são JSON em uma linha, com tempo relativo ao início da captura em microssegundos, quadro/fase ao concluir a operação, thread, nome, chave, identificador, bytes e status. Os IDs de objetos são opacos e têm o bit alto removido para não manter ponteiros gerenciados reais nas tabelas estáticas da telemetria. Chaves também ficam fora da faixa de endereços do heap.

## Custo e cobertura

Produtores usam uma tentativa de trava curta, sem espera e sem gravação em disco. Uma thread consome a fila. São 512 eventos e 1024 entradas de agregação, com nomes de até 127 caracteres. O primeiro evento de cada recurso/fase aparece individualmente; repetições de pelo menos 2 ms também aparecem. Operações menores permanecem nos totais cumulativos por recurso. Registros de fases anteriores já emitidos podem ser reutilizados.

`OBS health` informa perdas, saturação da tabela, tamanho da fila e tempo acumulado/máximo gasto na escrita. Esses valores permitem reconhecer uma captura parcial e observar o custo do próprio diagnóstico. A instrumentação tem custo e pode influenciar os tempos; a fila limitada evita crescimento ilimitado e esperas dos produtores.

Os tempos são de parede e inclusivos. Uma criação pode conter GC, I/O, imagem ou outros recursos; chamadas em workers podem se sobrepor. Não some categorias como se fossem parcelas exclusivas de CPU ou GPU. LoadAsync/UnloadUnusedAssets medem a chamada de entrada, não todo seu processamento futuro. Subassets internos que não atravessam essas APIs não recebem nomes individuais. WWW é identificado pelo objeto e uploads pelo ID gráfico; correlação temporal não prova que sejam o mesmo asset. Modais são relacionados por toques, recursos e cenas; não foi adicionado um detector de primeiro pixel visível de cada modal.

Eventos podem ser perdidos se a fila saturar, o coletor de GC sobrescrever suas 16 amostras entre quadros, ou o aplicativo for encerrado antes de a thread gravar o final. Divisores desejado/ativo e memória continuam nos registros `gc:`.

## Relatório automático

Depois de copiar o log:

```sh
python3 scripts/report_observation.py build/VPK/srr_log.txt --out-dir build/observacao
```

Saídas: `report.md`, `assets.csv`, `timeline.csv`, `phases.csv` e `summary.json`. O relatório evita somar duas vezes snapshots cumulativos da mesma entrada. Quando só há spans disponíveis, indica cobertura parcial. Duração de estado inclui interação/permanência do usuário e não deve ser chamada de loading puro.

Para uma sessão normal: abrir o jogo, passar pelos avisos iniciais, navegar pelos menus/Equippable, entrar em uma fase, jogar, voltar ao menu e entrar novamente. Não é necessário limpar saves ou cache.

## Validação desta entrega

A pedido do usuário, novos testes automatizados foram interrompidos. A entrega final é conferida por compilação ARM/VitaSDK e integridade do VPK. O funcionamento da prioridade corrigida e a captura completa precisam da próxima execução no aparelho.
