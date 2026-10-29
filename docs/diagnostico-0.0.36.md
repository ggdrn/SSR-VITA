# SRR Vita 0.0.36 — áudio e pausas nas fases

Relato no aparelho com a 0.0.35: som das fases/argolas ausente e engasgos. O player OpenSL ativo e o primeiro PCM não nulo não provam que os efeitos ou a música correta foram reproduzidos.

## Mudança no áudio

O consumidor OpenSL mantinha `bq_lock` durante `sceAudioOutOutput`, uma chamada que espera o relógio de áudio. Nesse intervalo, o produtor CRI, GetState e Clear também esperavam a mesma fila. Agora o consumidor copia o PCM para memória própria e libera a fila antes do processamento e da saída. A área de cópia é reutilizada, crescendo apenas quando necessário. A geração da fila impede que a conclusão de um buffer antigo remova um buffer novo após Clear.

A mudança elimina esse bloqueio específico. Seu efeito sobre os engasgos e o silêncio das fases ainda precisa de validação no console.

## Diagnósticos limitados

- O logger opcional `SoundManager.OutputPlayLog` registra as primeiras 24 solicitações por cena e depois somente contagens que são potências de dois. Os métodos de reprodução permanecem os originais.
- Os snapshots registram volume BGM/SE e se cada banco registrado pelo jogo tem um objeto ACB carregado no CRI. Isso distingue registro do nome e carregamento efetivo.
- A cada 600 frames, contadores nativos de Mono registram coletas, frames acima de 50 ms com e sem coleta, heap e memória usada. Nenhuma coleta é forçada. A coincidência no mesmo intervalo não comprova causalidade.

## Verificação local

Build ARM/VitaSDK concluído; VPK íntegro e eboot do pacote idêntico ao compilado. Testes de log, sinais, fila OpenSL e seleção de shader passaram. O teste de áudio usa AddressSanitizer/UndefinedBehaviorSanitizer e verifica também que a espera do hardware ocorre sem o mutex da fila e que Clear durante a saída preserva a nova fila.

## Teste no Vita

Instalar `build/VPK/SRR-Vita-v0.0.36.vpk`, entrar em uma fase e jogar cerca de um minuto, coletando argolas. Conferir música, efeito das argolas e pausas. Depois recolher `ux0:data/srr/srr_log.txt`. Não é necessário apagar saves ou caches. A conexão FTP com o aparelho não respondeu durante esta preparação; o pacote foi gerado localmente, sem instalação remota.
