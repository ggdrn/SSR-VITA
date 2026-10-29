# Diagnóstico e build 0.0.34

Base: `build/VPK/srr_log.txt`, execução da versão 0.0.33. Não houve teste
em hardware da 0.0.34 neste ambiente.

| Área | Evidência | Implementado em 0.0.34 | Ainda precisa verificar |
|---|---|---|---|
| Menu | `FormatException` repetida em `SaveData.SystemData.IsFacebookWindowOrg` | Guard no método JIT: mantém comparação com DateTime.Now, tenta leitura invariante e depois cultura atual; data inválida desabilita somente o aviso opcional de Facebook, sem alterar save | Linha `menu: installed Facebook date guard` e retorno ao menu após corrida |
| Áudio CRI | OpenSL cria players estéreo 44100 Hz; `getAssets` Java não implementado | Implementado handle AssetManager; log de estado de reprodução, primeiro PCM não zero, 512 buffers silenciosos e erro de saída | Ouvir música/efeitos e conferir `opensl:` no aparelho; ainda não comprovado que getAssets era a causa |
| Áudio Unity/FMOD | `fmodInitJni -> 1`, mixer retorna rate/block -1 | Resolução das três funções pela tabela RegisterNatives, coerente com Unity | Pode ser mixer não utilizado pelo jogo; não há confirmação de defeito nele |
| Personagem | Fase s_playingstage carregada, duas câmeras, 499 objetos; dump antigo só cobria título | Dump nas mudanças de cena e lista separada de SkinnedMeshRenderer com malha, ossos, visibilidade e shader | Identificar se modelo não existe, está oculto ou falha no desenho; não há correção gráfica confirmada |
| Engasgos | 200 links de shader, 5075 ms acumulados, quatro links acima de 100 ms; exceções repetidas gravadas em disco | Coalescência de mensagens consecutivas idênticas, correção do loop do menu; estatísticas de 600 renders (média, máximo, quantidade acima de 50 ms) | Medir no Vita, inclusive numa segunda execução para observar cache de shaders |

## Limites da medição

`perf:` mede nativeRender, incluindo eventuais esperas de apresentação. Não é
uma medição isolada de CPU ou GPU. Os dumps gerenciados ficam fora desse intervalo
e também podem causar uma pausa; ocorrem nas transições e em amostras espaçadas.
O nome da cena é consultado a cada 300 frames. Não foram alterados resolução,
qualidade, clocks ou lógica de gameplay sem evidência de causa.

O guard da data depende dos exports de Mono e do método original da versão 2.2.4.
A identificação de namespace, classe, getters e comparação foi conferida no IL
local da Assembly-CSharp.dll. Compilar não valida o hook JIT no console.

## Validação local

- Build Release e empacotamento VPK concluídos.
- `python3 tests/test_log.py`: teste do código real do logger com I/O simulado,
  AddressSanitizer e UndefinedBehaviorSanitizer: mensagem vazia, 10 mil duplicadas,
  resumo e truncamento.
- `git diff --check`.

## Teste no Vita

Instalar `build/VPK/SRR-Vita-v0.0.34.vpk`, mantendo o APK e save existentes.
Entrar numa fase, verificar personagem, música e efeitos, concluir/abandonar a
corrida e retornar ao menu. Copiar `ux0:data/srr/srr_log.txt` antes da próxima
execução, pois o log é substituído ao iniciar.
