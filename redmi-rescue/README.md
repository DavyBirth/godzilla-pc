# Redmi Rescue — Android beta

Projeto experimental para Redmi Note 13 5G / HyperOS. Esta é uma versão **nativa, sem PC ou Termux**, que pode ser compilada pelo GitHub Actions.

## Funções reais
- Lê espaço total/livre, RAM disponível, temperatura da bateria e nível térmico informado pelo Android.
- Indexa mídias autorizadas pelo usuário e lista arquivos grandes sem apagar nada.
- Identifica grupos de duplicatas byte a byte por hash SHA-256 (até 256 MiB por arquivo e 768 MiB por análise). Os resultados podem ser parciais.
- Permite escolher um arquivo pelo seletor nativo para revisá-lo, **sem excluir**.
- Compartilha relatório resumido, sem nomes dos arquivos, apenas por ação do usuário.
- Não utiliza internet, root, acessibilidade, acesso irrestrito, limpeza automática ou acesso aos dados privados de terceiros.
- Ignora nomes e pastas que indicam WhatsApp/Signal/Telegram/backups, mas isso não garante identificação completa de todos os arquivos privados.
- O app não verifica se imagens foram copiadas para a nuvem. Não é capaz de garantir melhorias de velocidade.

## APK
O GitHub Actions, workflow `Redmi Rescue APK (beta)`, compila o APK de debug. Entre na aba Actions deste repositório, abra a execução da branch `redmi-rescue-beta` e baixe o artefato `redmi-rescue-beta-apk` (contém `app-debug.apk`).

O projeto está numa branch experimental **separada do código principal de Godzilla** porque a integração atual não permite criar repositórios novos. Não altere ou mescle a branch na `main`.

## Segurança e limitações
Este primeiro build é intencionalmente **somente leitura**. A próxima etapa, se o aplicativo compilar e os testes demonstrarem segurança, será adicionar exclusões com confirmação e lixeira do Android, sem tocar em conversas ou fotos não protegidas. O aplicativo nunca foi testado em um Redmi físico neste ambiente. Não se declara superior a outros aplicativos sem benchmark real.

## Comparação
Registre espaço livre e tempo de abertura da câmera (cronômetro externo) antes e depois de mudanças feitas manualmente. A leitura da RAM livre não mede diretamente desempenho.

## Desenvolvimento
Java 17 + Android SDK 35 + Android Gradle Plugin 8.7.3. Execute `gradle -p redmi-rescue :app:assembleDebug` em ambiente com SDK configurado, ou o workflow incluído.
