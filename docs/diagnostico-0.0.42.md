# SRR Vita 0.0.42 — tela cinza ao jogar

O log da 0.0.41 termina em falta de memória, sem uma nova cena jogável confirmada. Há 21 falhas nativas de memalign, incluindo solicitações de aproximadamente 5,3 e 10,7 MiB para texturas, além de 4550 mensagens em que até a formatação da exceção falha com OutOfMemoryException. A primeira falha grande ocorre ainda no menu, antes das exceções em repetição. O total livre do heap não garante um bloco contíguo suficiente.

Baseline preservada em `build/perf-baselines/srr-0.0.41-gray.log`. O log identifica esgotamento de memória nesta execução; ele não demonstra isoladamente qual alteração da 0.0.41 foi responsável pelo agravamento em relação à execução anterior.

## Correção do consumo de imagens

O IL do APK mostra acessos repetidos a WWW.texture para um mesmo download. Essa propriedade cria uma nova Texture2D a cada acesso, comportamento descrito na [documentação do Unity](https://docs.unity.com/en-us/engine/6000.5/script-reference/unityengine/www/texture). A DLL UnityEngine do APK também apresenta um getter que chama GetTexture a cada acesso, sem cache no código gerenciado.

No `HudLoadingCharaInfo/<LoadWWW>c__Iterator37.MoveNext`, o código acessava a propriedade para conferir nulidade, novamente para atribuir a imagem ao registro e uma terceira vez para adicionar ao dicionário. A correção atribui o resultado uma única vez ao registro e reutiliza esse resultado no dicionário. Os caminhos que recebem null e o Dispose do WWW permanecem.

No `InformationImageManager/ImageData/<DoFetchImages>c__Iterator3B.MoveNext`, a propriedade era acessada para conferir nulidade e novamente para atribuir ImageData.Image. Agora a atribuição ocorre uma única vez antes da verificação, que lê a mesma imagem armazenada. O setter original foi inspecionado: apenas atribui o campo, sem callback ou outro efeito. Quando a criação retorna null, o campo recebe null antes de seguir a saída original; no fluxo anterior a atribuição era ignorada. As chamadas posteriores de callbacks e gravação da imagem permanecem originais.

Os dois caminhos continuam criando texturas legíveis, pois InformationImageManager usa EncodeToPNG ao gravar o arquivo. Não foi aplicado textureNonReadable, redução de resolução, mudança do modo leve nem alteração global do getter WWW.texture. Outros carregadores do APK ainda podem acessar a propriedade mais de uma vez; esta correção se limita aos dois métodos descritos.

Os ajustes são feitos no IL em memória antes do JIT, com verificação do tamanho do método e de todos os trechos substituídos. As classes internas são localizadas pela API de tipos aninhados do Mono. Se houver divergência, o método permanece original e há aviso no log. Os dois trechos do HUD são verificados antes de qualquer escrita, evitando alteração parcial.

## Retornos ao comportamento anterior

Foram restauradas as chamadas de GC anteriores ao descarregamento em StatePause e StateChangeLevel. A otimização que removia essas chamadas não está ativa nesta versão. Também foi retirado o bloqueio de novas tentativas de mipmaps dentro do mesmo quadro, voltando à recuperação da 0.0.40. As proteções contra copiar para NULL permanecem. A remoção das alocações de depuração de SePlay continua ativa, preservando argumentos e retorno da reprodução.

## Validação

Os 13 testes passaram. O teste novo executa a função C real de patch, confere SHA-256 da DLL contra o APK, verifica resolução das classes aninhadas, rejeição de tokens/tamanhos diferentes e ausência de alterações parciais. Um interpretador dos trechos IL simula a criação de uma textura nova a cada acesso: confirma três para uma e duas para uma criações nos casos de sucesso, identidade compartilhada entre registro e dicionário, resultado null, destinos dos saltos, pilha balanceada e preservação de Dispose. Os testes C usam AddressSanitizer/UndefinedBehaviorSanitizer.

Build ARM/VitaSDK concluído. O arquivo eboot do VPK coincide com o build e o ZIP passou na verificação de integridade. SHA-256: `56fd7bd57539ba8df1b685ca4e42f1ca4fcee1530f4801359609acbb6ffbac15`.

Pacote: `build/VPK/SRR-Vita-v0.0.42.vpk`. Testar abrir o menu e clicar em jogar com a mesma configuração gráfica. Ainda não houve teste desta versão no Vita; a abertura da fase precisa ser confirmada. Os marcadores `mem: ... image loader now creates one texture per download` confirmam a aplicação no aparelho. Não é necessário apagar saves ou caches.
