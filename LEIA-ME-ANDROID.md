# DKC1Recomp: port Android (rascunho sem teste)

Este pacote só adiciona e troca alguns arquivos. Ele foi escrito sem conseguir compilar nem rodar no celular, então espere erros no primeiro build.

## O que tem aqui

- `CMakeLists.txt`: **substitui** o do repositório. A única mudança é um bloco `if(ANDROID)`, logo depois do bloco do iOS.
- `cmake/android.cmake`: monta o SDL2 e o jogo como `libmain.so`.
- `runner/android/android_main.c`: a casca Android (tela, som, toque e controle).
- `android/`: projeto Gradle, com `GameActivity` (roda o jogo) e `RomPickerActivity` (escolhe a ROM).
- `.github/workflows/build.yml`: compila o APK no GitHub Actions.

## Como usar

1. No seu fork de `DKC1Recomp`, troque o `CMakeLists.txt` e adicione as pastas `cmake/`, `runner/android/`, `android/` e `.github/workflows/`.
2. **Coloque a sua ROM** em `rom/game.sfc` (ROM USA 1.0). O gerador só aceita o hash dessa versão.
   - Isso vai contra o que eu disse antes de nunca subir a ROM pro GitHub. Como o gerador roda no build, ela precisa estar onde o Actions consegue ler. Use **somente um repositório privado** e não divulgue o APK.
3. Na aba **Actions**, rode **Build Android APK (debug)**. Ao terminar, baixe o artefato `dkc1-android-apk` e instale.
4. Abra o app e escolha a ROM. Ela é copiada pro armazenamento privado do app.

## Limites conhecidos

- Não testado: não foi compilado nem rodado.
- Controles de toque e posição dos botões são palpites, e precisam de ajuste no celular.
- Áudio e taxa de quadros podem estar fora do ideal.
- Só ARM 64 bits (`arm64-v8a`), o que cobre o Redmi Note 14.
- Gera o APK de debug (assinado com a chave de debug), só para uso pessoal.
