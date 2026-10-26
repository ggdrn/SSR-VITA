# SRR Vita

Port do **Sonic Runners Revival** (`srr224.apk`, `com.sonicrunners.revival`) para PS Vita usando um
*so-loader*: as bibliotecas nativas ARM do APK rodam direto no Vita e cada dependência Android
(libc bionic, GLES/EGL, OpenSL ES, libandroid, JNI/Java) é reimplementada sobre o VitaSDK + vitaGL.

> Estado: **0.0.01, ainda não testado em hardware.** O loader compila e gera o VPK; a primeira
> execução no console vai produzir o log que guia as próximas iterações.

## O que fica no Vita

```
ux0:data/srr/
├── srr224.apk          <- único arquivo que você copia (qualquer nome .apk serve)
├── srr_log.txt         <- log de cada execução (criado pelo loader)
├── files/              <- dados do jogo; files/shared_prefs/ guarda os PlayerPrefs (save)
├── cache/
└── sdcard/
```

Não é preciso extrair nada, nem OBB (`settings.xml` do APK tem `useObb=False`: todos os dados estão
dentro do APK). Na inicialização o loader lê o APK, descompacta em memória as 4 `.so` de
`lib/armeabi-v7a/` e as liga ao Vita; o Unity lê `assets/bin/Data` direto do APK, como faz no Android.

Requisitos no console:
- **kubridge.skprx** recente (com `kuKernelMemCommit` e `kuKernelRegisterExceptionHandler`) em `ur0:tai/`, sob `*KERNEL`.
- **libshacccg.suprx** extraído com ShaRKBR33D (compilação de shaders GLSL em tempo real).

## Análise de viabilidade

| Checagem | Resultado | Leitura |
|---|---|---|
| ABI ARM 32-bit | ✅ armeabi-v7a | Roda nativo no Cortex-A9. |
| Engine | Unity **4.7.2f1**, backend **Mono** (JIT) | O JIT do Mono exige memória RWX → `kuKernelMemCommit` RWX (kubridge). |
| Bibliotecas | `libunity` 11 MB, `libmono` 3.9 MB, `libmain`, `libcri_ware_unity` (CRI ADX2) | 733 imports únicos, todos resolvidos (`data/apk_imports.txt`). |
| GLES "3+" (FAIL do checker) | **Falso negativo prático** | O `libunity` só importa símbolos GLES2; o caminho GLES3 é opcional e obtido via `eglGetProcAddress`. O loader recusa contexto ES3 e o Unity cai no renderer GLES2. |
| Áudio | FMOD (Unity, via Java `FMODAudioDevice`) + CRI (OpenSL ES) | Ambos emulados sobre `sceAudioOut`. |
| Vídeo | CRI Sofdec via `MediaCodec` | Sem `.usm` no APK; não implementado. |

**Riscos principais** (em ordem):
1. **GC do Mono (Boehm)** para o mundo com `pthread_kill` + handlers de sinal em outras threads. O Vita
   não entrega sinais entre threads. Hoje isso é só registrado no log; se causar travamento, a saída é
   hookar `GC_stop_world`/`GC_start_world` no `libmono`.
2. **NullReferenceException** do Mono depende de SIGSEGV. Implementado via exception handler do
   kubridge (`signal_bridge.c`), ainda sem validação em hardware.
3. **Rede**: o jogo é online (servidor Revival). `WWW` do Unity 4 é Java (`HttpURLConnection`) e o Mono
   usa sockets BSD. Ambos estão como "indisponível" nesta versão — próximo grande bloco.
4. **Memória**: ~365 MB com `ATTRIBUTE2=12`; heap newlib de 220 MB + vitaGL.

## Mapa de hardware

| Android | Vita | Arquivo |
|---|---|---|
| EGL / GLES2 | vitaGL (GXM) + tradutor GLSL→CG | `src/bridges/gl_bridge.c` |
| Touch (`AInputQueue`/`AMotionEvent`) | `sceTouch` (tela frontal, 1920×1088 → 960×544) | `src/bridges/input_bridge.c` |
| Teclas | `sceCtrl`: ✕/◯ = toque no centro (pular), START/SELECT = BACK, setas/□/△/L/R = gamepad | `src/bridges/input_bridge.c` |
| OpenSL ES (CRI) | `sceAudioOut` (portas MAIN) | `src/bridges/audio_bridge.c` |
| FMOD Java AudioDevice (Unity) | thread + `sceAudioOut` (porta BGM) | `src/bridges/audio_bridge.c` |
| AAssetManager | leitura direta do ZIP do APK | `src/bridges/android_bridge.c`, `src/util/apk.c` |
| Arquivos (`/data/data/...`, `/sdcard`) | `ux0:data/srr/...` | `src/bridges/fs_bridge.c` |
| pthread / semáforos (bionic) | pthread VitaSDK + `SceKernelSema` | `src/bridges/pthread_bridge.c` |
| mmap/mprotect (JIT) | kubridge `MemReserve/Commit` RWX | `src/bridges/mem_bridge.c` |
| Sinais | exception handlers do kubridge | `src/bridges/signal_bridge.c` |
| SharedPreferences (PlayerPrefs) | arquivos em `files/shared_prefs/` | `src/jni/java_impl.c` |
| Idioma (`Locale`) | idioma do sistema do Vita | `src/jni/java_impl.c` |
| CPU/GPU | 444/222/222/166 MHz | `src/main.c` |

## Arquitetura

```
src/main.c            boot: checa kubridge/libshacccg, acha o APK, carrega e liga as .so
src/unity_boot.c      reproduz o lado Java do UnityPlayer (NativeLoader.load, initJni, nativeRender...)
src/loader/so_util.c  loader ELF (texto RX via kubridge em endereço fixo, dados RW logo após)
src/loader/dynlib.c   tabela de imports — GERADA por scripts/gen_dynlib.py
src/bridges/*         compatibilidade bionic/Android sobre o Vita
src/jni/jni.c         JVM falsa e permissiva (toda classe/método "existe"; o não implementado é logado)
src/jni/java_impl.c   APIs Java emuladas
```

Imports sem implementação viram stubs que **logam o nome** no `srr_log.txt` e retornam 0; métodos Java
não emulados aparecem como `jni: unimplemented Classe.metodo(sig)`. O log é a lista de tarefas.

## Build

Pré-requisitos: `$VITASDK` (hard-float; as chamadas soft-float do Android são adaptadas com
`__attribute__((pcs("aapcs")))`) e as pastas locais ao lado deste repo:

```
SRR - VITA/
├── SRR-VITA/      este repositório
├── vitaGL/        usado direto daqui (header + libvitaGL.a)
├── vitasoguide/   guia/checagem de compatibilidade
└── vdpm/          gerenciador de pacotes do VitaSDK
```

```bash
scripts/build.sh
```

Cada execução incrementa o número de build e gera `build/SRR-Vita-v0.0.NN.vpk` (cópia em `releases/`).
No Vita o `param.sfo` só aceita `XX.YY`, então a mesma build aparece como `00.NN`.
`scripts/build.sh --rebuild` recompila sem trocar o número.

Ao mudar os bridges ou a lista de imports:

```bash
python3 scripts/gen_dynlib.py
```

## Licença

Código do loader: MIT. O APK e seus assets pertencem aos seus donos e não são distribuídos aqui.
