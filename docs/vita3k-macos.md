# SRR no Vita3K — instalação limpa no Mac

Instalação realizada em 27/10/2026 com Vita3K 0.2.1 4236-17a8b340 (arm64).

## Abrir

Executar `build/vita3k-srr-clean/Abrir SRR no Vita3K.command`, ou:

```sh
./scripts/run-vita3k.sh
```

O atalho usa um perfil separado do perfil habitual do Vita3K. Abrir apenas o
Vita3K em /Applications pode mostrar a biblioteca do perfil habitual.

## Conteúdo e origem

- VPK 0.0.35 instalado do zero pelo instalador de arquivos do Vita3K.
- `srr224.apk` copiado do arquivo local do projeto.
- `ur0/tai/kubridge.skprx` e `ur0/data/libshacccg.suprx` copiados por FTP do
  PS Vita autorizado pelo usuário. O config taiHEN contém somente kubridge.
- Usuário local novo, chamado SRR.
- Nenhum save ou cache foi importado do console. As pastas de cache e os
  arquivos de preferência que aparecerem são criados pela execução local.
- Não foram alterados arquivos no console.

A pasta `build/vita3k-import/reference` contém referências do diagnóstico da
preparação; não é usada pelo emulador. CapUnlocker está somente nessa pasta,
não foi habilitado no emulador. Na execução testada o emulador recusou a máscara
CPU3; o loader utilizou seu fallback de três núcleos de usuário.

## Configuração

Perfil: `build/vita3k-srr-clean/config.yml`.
VitaFS: `build/vita3k-srr-clean/fs`.
Vulkan/MoltenVK, resolução 1x, memória com mapping desabilitado, áudio SDL em
100%, NGS habilitado, CPU pool de 4 e compilação assíncrona de pipelines.
O launcher desativa os logs detalhados de shader/uniform por opção CLI.

## Validação realizada

- Instalador confirmou `SRRV00001` / versão 0.35.
- Eboot instalado comparado byte a byte com o VPK; APK local também comparado.
- taiHEN carregou o kubridge copiado.
- Loader carregou as quatro bibliotecas ARM e registrou os exception handlers.
- Unity atingiu a cena `s_title`, apresentou frames e obteve resposta HTTP 200
  do servidor de assets. O CRI produziu PCM não zero.
- No título, depois do carregamento inicial, a medição de 600 renders ficou
  em 16,63 ms de média e 19,48 ms de máximo. Isso não mede desempenho da fase.

A validação foi feita pelos logs. Não houve controle visual automatizado nem
validação de corrida, personagem ou saída de som física nesse teste. A automação
de janelas do macOS não estava autorizada; o usuário abriu o emulador manualmente
uma vez, após o que a execução por CLI funcionou.

## Logs

- Jogo: `build/vita3k-srr-clean/fs/ux0/data/srr/srr_log.txt`.
- Cópia do log de boot: `build/vita3k-srr-clean/bootstrap-vita3k.log`.
- Log global do Vita3K: `~/Library/Application Support/Vita3K/Vita3K/vita3k.log`.

O último é compartilhado pelo emulador e pode ser sobrescrito por outra janela.
