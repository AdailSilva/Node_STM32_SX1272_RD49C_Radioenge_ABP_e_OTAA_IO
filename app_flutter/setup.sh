#!/bin/sh
# Gera a estrutura Android que falta (o flutter create NÃO sobrescreve lib/,
# pubspec.yaml, AndroidManifest.xml, MainActivity.kt nem res/xml deste repositório)
# e baixa as dependências. A documentação do app fica no README.md do firmware
# (seção App Android): o README.md de modelo que o flutter create gera é apagado.
set -e
cd "$(dirname "$0")"
command -v flutter >/dev/null || { echo "flutter não encontrado no PATH"; exit 1; }
flutter create --org br.com.adailsilva --project-name radioenge_node_atuadores_sensores_app --platforms android .
rm -f README.md
flutter pub get
echo
echo "Pronto. Rode:  flutter run    (ou: flutter build apk --release)"
