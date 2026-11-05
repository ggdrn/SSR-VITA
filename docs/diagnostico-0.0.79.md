# 0.0.79 — opção Material Shaders

O print recebido mostra corrupção visível no personagem principal. O log
anexado identifica a versão 0.0.77 e confirma `lit diffuse/envmap compatibility`
ativo. Isso confirma o modo usado na captura, não a causa exata da corrupção.

O menu agora oferece `Material Shaders: On/Off`, com On como padrão em
instalações e arquivos de configuração sem a nova chave. A opção é gravada
como `material_shaders=1` ou `material_shaders=0` em `port_settings.txt` e
aplicada ao iniciar o renderizador.

- On: usa os shaders de iluminação, reflexos e variantes de materiais da
  0.0.77, incluindo UV animado.
- Off: usa o par difuso simples anterior, sem normais, luz ou envmap, nas
  três famílias cobertas. Vidro/falloff continuam excluídos da substituição.

O log registra explicitamente `material shaders on/off` e o caminho escolhido.
As opções de gráficos continuam Low e Default, conforme a 0.0.78.

Validação: teste de menu com ASan/UBSan (padrão On, toque, persistência de Off,
retorno a On, configurações antigas e renderização das sete linhas); teste
com estágios GLES separados do APK/cache nos dois modos; inspeção visual
do menu renderizado; build ARM/VitaSDK sem avisos; integridade ZIP e identidade
do eboot; `git diff --check`.

Pacote: `build/VPK/SRR-Vita-v0.0.79.vpk`.
SHA-256: `3d978aa22e3818ce450fda10a74ee037b63fbe303ecee5142a3881623ed69f10`.

Para comparar a corrupção recebida, abrir SETTINGS, selecionar Material
Shaders Off e Start Game, repetindo a mesma corrida/personagem. A correção
visual no Vita não foi verificada nesta entrega. On continua sendo o padrão
solicitado; escolher Off fica salvo para os próximos inícios.
