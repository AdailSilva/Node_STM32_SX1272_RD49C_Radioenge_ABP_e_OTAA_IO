// App Android do nó LoRaWAN Radioenge com atuadores e sensores (STM32L071CZ + SX1272,
// LoRaMac-node): relés, medidores PZEM-004T v3 e o protocolo ADAILSILVA_V1. Um app só
// para as duas ativações (ABP e OTAA): a ativação em uso vem do nó (AT+NJM).
// Fala com o console da UART do módulo por cabo USB OTG ou por Bluetooth (BLE-serial):
// status, configuração, console e ações (classe, uplink, JOIN, reinício).
import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'core/app_info.dart';
import 'ui/connect_screen.dart';
import 'ui/theme.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const NodeApp());
}

class NodeApp extends StatelessWidget {
  const NodeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: appTitle,
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      locale: const Locale('pt', 'BR'),
      supportedLocales: const [Locale('pt', 'BR')],
      localizationsDelegates: const [
        GlobalMaterialLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
      ],
      home: const ConnectScreen(),
    );
  }
}
