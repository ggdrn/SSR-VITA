# 0.0.80 — atributos dos materiais com shaders ligados

## Captura recebida

O log identifica 0.0.79, Material Shaders On, e o caminho
`lit diffuse/envmap compatibility`. A Unity aplicou nível gráfico 1.
Há 150 programas ligados, zero falhas de link, `glGetError=0` nas amostras
e 299 hits de cache. Esses indicadores não validam a imagem produzida.
O usuário confirmou que o objetivo é corrigir o personagem com On.

## Erro encontrado na base

Em `vitaGL/source/custom_shaders.c`, `glGetActiveAttrib` procurava um
atributo usando `resourceIndex / 4 == index`. O índice da consulta GLES é
a posição na enumeração dos atributos ativos, não um endereço de registro
da GPU. Esse cálculo só funciona quando os registros coincidem com essa
sequência. Metadados intercalados, registros esparsos ou atributos eliminados
pela otimização podem fazer a procura falhar. Nesse caso o código antigo
continuava usando o último parâmetro visitado, inclusive um uniform.

Isso permite reportar nome/tipo incorretos de normal ou coordenada UV à
Unity. É um defeito de reflexão reproduzido em teste, e uma hipótese para
a corrupção observada. O log antigo não contém os registros GXM nem as
respostas dessas consultas; portanto, não comprova que esse defeito foi
a causa da imagem recebida.

A correção enumera somente os parâmetros de categoria ATTRIBUTE, sem
inferir o índice a partir dos registros. Também valida índices e buffers
negativos e respeita consultas com buffer de comprimento zero.
O patch reproduzível está em `patches/vitagl-active-attrib.patch`.
O checkout local do vitaGL foi recompilado, e o port foi religado contra
a biblioteca nova; a mudança não está apenas no arquivo de patch.

## Comportamento e diagnóstico

Material Shaders continua On por padrão. Iluminação, reflexos e variantes
permanecem no caminho ligado, com as mesmas equações da 0.0.79.
Nenhum material foi automaticamente redirecionado ao modo Off.

Após ligar um material com iluminação, o port registra os atributos
refletidos: programa, índice, nome, localização, tipo e tamanho. No novo
log, procurar `gl: lit material program ... attribute ...`, conferindo
`_glesVertex`, `_glesNormal` e `_glesMultiTexCoord0`.
Essa informação ajuda a conferir no aparelho o contrato que o teste de
host verifica com metadados simulados.
Não é necessário apagar o cache de shaders: o problema corrigido está na
consulta em tempo de execução, não no conteúdo compilado dos programas.

## Verificação

- O teste da função C antiga reproduz a resposta incorreta de atributos.
- A função corrigida passa com ASan/UBSan: posição/normal/UV, uniforms
  intercalados, registros esparsos, buffers zero/truncados e índices inválidos.
- Passaram os testes de seleção de shaders do APK/cache nos modos On e Off
  e de menu/configuração/persistência, também com sanitizers.
- vitaGL e port compilados com VitaSDK; VPK íntegro e eboot empacotado igual
  ao compilado; aplicação reversa do patch validada; `git diff --check` passou.

Pacote: `build/VPK/SRR-Vita-v0.0.80.vpk`.
SHA-256: `5f1552b39d2cc10cd10648d055944c4abbfaba747bedd500af6b54db4e885faf`.

A validação visual no Vita continua pendente. Repetir a mesma corrida com
o mesmo personagem e Material Shaders On, preservando o novo log. Esta
entrega corrige o defeito reproduzido de atributos; não declara a imagem
recuperada antes dessa verificação.
