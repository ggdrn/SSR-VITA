# 0.0.76 — texto no Low e shader Compatible fixo

- Low continua em Unity Fastest/Light (nível 0). O limite global de texturas
  passa de masterTextureLimit=2 para 1: metade da largura/altura original,
  em vez de um quarto. Isso preserva mais detalhes nos atlas de texto.
- O ajuste também afeta as outras texturas sujeitas ao limite da Unity;
  o consumo de memória pode subir em comparação ao Low anterior. Resolução,
  FPS e demais escolhas do usuário são preservados.
- Character Shader sai do menu. O bridge usa sempre Compatible, ignorando
  a chave antiga character_shader_original e o arquivo original_character_shader.
- Build VitaSDK, teste de menu/configurações/eventos com sanitizers, teste de
  seleção de shaders e integridade do VPK passaram. Legibilidade e desempenho
  ainda precisam de validação no Vita.
