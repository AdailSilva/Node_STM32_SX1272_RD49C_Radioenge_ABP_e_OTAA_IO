// Tela inicial: escolher como falar com o nó (cabo USB OTG ou Bluetooth BLE).
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:url_launcher/url_launcher.dart';
import 'package:usb_serial/usb_serial.dart';

import '../core/ble_transport.dart';
import '../core/app_info.dart';
import '../core/node_link.dart';
import '../core/transport.dart';
import '../core/usb_transport.dart';
import 'lorawan_classes_screen.dart';
import 'node_shell.dart';
import 'theme.dart';
import 'widgets.dart';

class ConnectScreen extends StatefulWidget {
  const ConnectScreen({super.key});

  @override
  State<ConnectScreen> createState() => _ConnectScreenState();
}

class _ConnectScreenState extends State<ConnectScreen> {
  List<UsbDevice> _usb = [];
  int _baud = 115200;
  bool _usbLoading = false;

  List<ScanResult> _ble = [];
  bool _scanning = false;
  bool _onlyUart = true;
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<bool>? _scanState;
  StreamSubscription<UsbEvent>? _usbEvents;

  bool _connecting = false;

  @override
  void initState() {
    super.initState();
    _refreshUsb();
    _usbEvents = UsbSerial.usbEventStream?.listen((_) => _refreshUsb());
    _scanSub = FlutterBluePlus.scanResults.listen((r) {
      if (mounted) setState(() => _ble = r);
    });
    _scanState = FlutterBluePlus.isScanning.listen((s) {
      if (mounted) setState(() => _scanning = s);
    });
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _scanState?.cancel();
    _usbEvents?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  Future<void> _refreshUsb() async {
    setState(() => _usbLoading = true);
    try {
      final l = await UsbTransport.list();
      if (mounted) setState(() => _usb = l);
    } catch (_) {
      if (mounted) setState(() => _usb = []);
    }
    if (mounted) setState(() => _usbLoading = false);
  }

  Future<void> _scan() async {
    if (_scanning) {
      await FlutterBluePlus.stopScan();
      return;
    }
    if (!await BleTransport.ensurePermissions()) {
      if (mounted) {
        messageDialog(context, 'Bluetooth', 'Sem permissão para procurar dispositivos Bluetooth.');
      }
      return;
    }
    if (await FlutterBluePlus.isSupported == false) {
      if (mounted) messageDialog(context, 'Bluetooth', 'Este celular não tem Bluetooth Low Energy.');
      return;
    }
    final st = await FlutterBluePlus.adapterState.first;
    if (st != BluetoothAdapterState.on) {
      try {
        await FlutterBluePlus.turnOn();
      } catch (_) {
        if (mounted) messageDialog(context, 'Bluetooth', 'Ligue o Bluetooth do celular e tente de novo.');
        return;
      }
    }
    setState(() => _ble = []);
    await FlutterBluePlus.startScan(
      timeout: const Duration(seconds: 10),
      withServices: _onlyUart ? [nusService, hm10Service] : [],
    );
  }

  Future<void> _open(Future<NodeTransport> Function() opener) async {
    if (_connecting) return;
    setState(() => _connecting = true);
    await FlutterBluePlus.stopScan();
    NodeTransport? t;
    try {
      t = await opener();
      final link = NodeLink(t);
      final ok = await link.start();
      if (!ok) {
        await link.disconnect();
        throw Exception('O nó não respondeu. Confira os fios (TX do adaptador no pino 2, RX no pino 3, GND), '
            'a velocidade (115200 de fábrica; 9600, 19200 ou 43000 se foi trocada por AT+BAUDRATE) '
            'e se o firmware Radioenge ABP e OTAA está gravado.');
      }
      // Um firmware só para ABP e OTAA: o AT+NJM troca a ativação (a Configuração
      // mostra e edita a ativação em uso no nó)
      if (!mounted) return;
      setState(() => _connecting = false);
      await Navigator.of(context).push(MaterialPageRoute(builder: (_) => NodeShell(link: link)));
      await link.disconnect();
      _refreshUsb();
    } catch (e) {
      try {
        await t?.close();
      } catch (_) {}
      if (mounted) {
        setState(() => _connecting = false);
        messageDialog(context, 'Não conectou', '$e'.replaceFirst('Exception: ', ''));
      }
    }
  }

  void _about() {
    showAboutDialog(
      context: context,
      applicationName: appTitle,
      applicationVersion: '1.0.0',
      applicationIcon: Icon(Icons.sensors, color: AppColors.accentOf(context)),
      children: [
        const Text('Configura e acompanha o nó LoRaWAN no módulo Radioenge (STM32L071CZ + SX1272, '
            'LoRaMac-node) pelo console da UART: cabo USB OTG ou módulo Bluetooth BLE-serial.'),
        const SizedBox(height: 12),
        TextButton.icon(
          onPressed: () => launchUrl(Uri.parse('https://k8s.adailsilva.com.br/'), mode: LaunchMode.externalApplication),
          icon: const Icon(Icons.open_in_new, size: 18),
          label: const Text('Desenvolvido por AdailSilva'),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Scaffold(
      appBar: AppBar(
        title: Text(appTitle),
        actions: [
          const ClassesButton(),
          IconButton(onPressed: _about, icon: const Icon(Icons.info_outline), tooltip: 'Sobre'),
        ],
      ),
      body: Stack(
        children: [
          ListView(
            padding: const EdgeInsets.fromLTRB(14, 14, 14, 40),
            children: [
              SectionCard(
                title: 'USB OTG',
                subtitle: 'Adaptador USB-serial de 3,3 V ligado ao módulo: TX do adaptador no pino 2, '
                    'RX no pino 3 e GND no pino 1. Ligue no celular por um cabo OTG.',
                children: [
                  DropField<int>(
                    label: 'Velocidade',
                    items: const [
                      MapEntry(115200, '115200 (padrão do firmware)'),
                      MapEntry(43000, '43000'),
                      MapEntry(19200, '19200'),
                      MapEntry(9600, '9600 (padrão do firmware original)'),
                    ],
                    value: _baud,
                    onChanged: (v) => setState(() => _baud = v),
                  ),
                  const SizedBox(height: 8),
                  if (_usbLoading)
                    const Padding(padding: EdgeInsets.all(8), child: LinearProgressIndicator())
                  else if (_usb.isEmpty)
                    const Hint('Nenhum adaptador USB encontrado. Ligue o cabo OTG e toque em Atualizar.')
                  else
                    for (final d in _usb)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: Icon(Icons.usb, color: AppColors.accentOf(context)),
                        title: Text(UsbTransport.describe(d)),
                        subtitle: Text(d.deviceName),
                        trailing: const Icon(Icons.chevron_right),
                        onTap: () => _open(() => UsbTransport.open(d, baud: _baud)),
                      ),
                  Align(
                    alignment: Alignment.centerRight,
                    child: TextButton.icon(
                      onPressed: _refreshUsb,
                      icon: const Icon(Icons.refresh),
                      label: const Text('Atualizar'),
                    ),
                  ),
                ],
              ),
              SectionCard(
                title: 'Bluetooth (BLE)',
                subtitle: 'Módulo BLE-serial (HM-10, JDY-08/23, ou Nordic UART) ligado aos pinos 2 e 3, '
                    'na mesma velocidade do console (115200; o HM-10 vem em 9600: use AT+BAUD4).',
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: SwitchListTile(
                          contentPadding: EdgeInsets.zero,
                          title: const Text('Só módulos UART', style: TextStyle(fontSize: 14)),
                          value: _onlyUart,
                          onChanged: (v) => setState(() => _onlyUart = v),
                        ),
                      ),
                      const SizedBox(width: 8),
                      FilledButton.icon(
                        onPressed: _scan,
                        icon: Icon(_scanning ? Icons.stop : Icons.bluetooth_searching),
                        label: Text(_scanning ? 'Parar' : 'Procurar'),
                      ),
                    ],
                  ),
                  if (_scanning) const LinearProgressIndicator(),
                  if (!_scanning && _ble.isEmpty)
                    const Hint('Toque em Procurar. Se o módulo não aparecer, desligue "Só módulos UART": '
                        'alguns clones não anunciam o serviço.'),
                  for (final r in _ble)
                    ListTile(
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(Icons.bluetooth, color: AppColors.accentOf(context)),
                      title: Text(BleTransport.describe(r)),
                      subtitle: Text('${r.device.remoteId.str} · ${r.rssi} dBm'),
                      trailing: const Icon(Icons.chevron_right),
                      onTap: () => _open(() => BleTransport.open(r.device, BleTransport.describe(r))),
                    ),
                ],
              ),
              Hint(
                'O app usa o mesmo console da UART (comandos "mode app", "jstatus", "jconfig"). '
                'Nada vai para a internet.',
                color: cs.onSurfaceVariant,
              ),
              DevFooter(appTitle),
            ],
          ),
          if (_connecting)
            Container(
              color: Colors.black.withAlpha(90),
              alignment: Alignment.center,
              child: const Card(
                child: Padding(
                  padding: EdgeInsets.all(20),
                  child: Row(mainAxisSize: MainAxisSize.min, children: [
                    CircularProgressIndicator(),
                    SizedBox(width: 16),
                    Text('Conectando ao nó…'),
                  ]),
                ),
              ),
            ),
        ],
      ),
    );
  }
}
