# 0.0.81 — UV separada em malhas com posição e normal intercaladas

O log recebido identifica 0.0.80 e Material Shaders On. Os oito programas
iluminados registrados expõem `_glesVertex` na localização 0, `_glesNormal`
na 2 e `_glesMultiTexCoord0` na 3. O personagem continua visualmente
corrompido segundo o usuário: a correção da enumeração na 0.0.80 não resolveu
o defeito observado.

## Falha concreta encontrada

Nas três rotas de desenho de `vitaGL/source/custom_shaders.c`, a seleção do
caminho packed verificava o intervalo apenas do segundo atributo. Em modo
não STRICT, atributos de cliente posteriores não tinham seus intervalos
conferidos; o uso do mesmo VBO também não garante que os arrays estejam
intercalados nem que tenham o mesmo stride.

Exemplo reproduzido no teste:

- posição no endereço 0x1000, stride 24;
- normal no endereço 0x100C, stride 24;
- UV no endereço 0x8000, stride 8; todos sem VBO.

O código anterior aceita posição e normal como packed e estende a decisão
à UV. Copia somente o bloco da posição e depois monta a UV com um offset
para fora desse bloco. Isso pode gerar coordenadas inválidas e amostragem
aparentemente aleatória da textura. Adicionar normais ao shader muda qual
atributo ocupa a segunda posição e pode expor esse erro; no caminho difuso
simples, posição e UV separadas já podiam ser classificadas como unpacked.

Esse é um layout válido para dados de malha animada: os dados deformados
de posição/normal podem coexistir com coordenadas UV estáticas separadas.
O log atual confirma os canais ativos, mas não registra seus endereços e
strides de desenho. Portanto a falha foi reproduzida em host, e sua relação
com a captura permanece uma hipótese a validar no Vita.

## Correção

O novo predicado verifica todos os atributos ativos: mesmo buffer, mesmo
stride e offset dentro do primeiro vértice, sem somar endereços com risco
de overflow. Layouts mistos usam os streams separados que já existem no
vitaGL. Layouts realmente intercalados continuam usando o caminho packed.
As três rotas de desenho foram atualizadas.

As primeiras oito rejeições de layouts mistos registram
`gl: split vertex streams: ... using separate streams`, com índices,
buffers e strides. O patch está em `patches/vitagl-vertex-streams.patch`.
A biblioteca local foi recompilada e o port foi religado contra ela.

Material Shaders permanece On, com iluminação, reflexos e variantes ativos.
As equações dos shaders e a configuração padrão não foram trocadas.
Não é necessário limpar o cache de shaders para a nova decisão de streams.

## Validação

- Teste C da função de produção com ASan/UBSan: posição/normal intercaladas
  e UV separada, arrays distintos com o mesmo stride, regiões separadas do
  mesmo VBO, VBOs diferentes, layouts intercalados válidos, canais desativados,
  bindings 0/2/3, limites de endereços e caminho simples de dois atributos.
- Testes de atributos ativos, seleção de shader/uniforms do APK/cache nos
  dois modos e menu/configurações passaram com sanitizers.
- vitaGL e port compilados sem avisos; patch reversível verificado;
  `git diff --check`, integridade ZIP e identidade do eboot passaram.

Pacote: `build/VPK/SRR-Vita-v0.0.81.vpk`.
SHA-256: `7b8664b32f958d7a30e9ffa51498a7f5b7540ed0972eaf95dbbfdcf07b035de7`.

Validação pendente: mesma corrida/personagem com Material Shaders On e novo
log. Não houve execução ou confirmação visual desta build no aparelho.
