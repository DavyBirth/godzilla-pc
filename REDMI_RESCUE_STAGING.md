# Redmi Rescue — branch temporária de compilação

Este branch é uma área isolada para compilar o aplicativo Android Redmi Rescue sem computador. **A branch `main` do Godzilla não foi modificada.**

O código-fonte do app fica no arquivo compactado `redmi-rescue-source.tar.xz.base64` (tar.xz codificado em base64). O workflow `.github/workflows/redmi-rescue-apk.yml` extrai o projeto, faz verificações estáticas e tenta compilar um APK de teste com Gradle.

O projeto ainda é beta. Ele não promete limpar caches privados do HyperOS e não faz limpeza automática de fotos ou conversas. Para instalar no Android, use apenas o arquivo `app-debug.apk` gerado no workflow se a compilação passar. Esta branch não modifica nem libera novas versões do jogo Godzilla.