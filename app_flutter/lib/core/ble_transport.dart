// Bluetooth: módulo BLE-serial ligado aos pinos 2 (RX) e 3 (TX) do módulo.
// Serviços reconhecidos:
//   - Nordic UART Service (NUS): 6E400001… (escrita 6E400002, notificação 6E400003)
//   - HM-10 / JDY-08 / JDY-23 / CC254x: serviço FFE0, característica FFE1
// O módulo BLE precisa estar na mesma velocidade do console (115200 por padrão;
// o HM-10 sai de fábrica em 9600: AT+BAUD4 muda para 115200).
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'transport.dart';

final Guid nusService = Guid('6E400001-B5A3-F393-E0A9-E50E24DCCA9E');
final Guid nusRx = Guid('6E400002-B5A3-F393-E0A9-E50E24DCCA9E'); // app -> módulo
final Guid nusTx = Guid('6E400003-B5A3-F393-E0A9-E50E24DCCA9E'); // módulo -> app
final Guid hm10Service = Guid('FFE0');
final Guid hm10Char = Guid('FFE1');

class BleTransport extends LineTransport {
  BleTransport._(this._device, this._write, this._label);

  final BluetoothDevice _device;
  final BluetoothCharacteristic _write;
  final String _label;
  final List<StreamSubscription<dynamic>> _subs = [];

  @override
  String get name => 'BLE · $_label';

  /// Pede as permissões do Bluetooth (Android 12+: SCAN/CONNECT; antes: localização).
  static Future<bool> ensurePermissions() async {
    if (!Platform.isAndroid) return true;
    final res = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
    final scan = res[Permission.bluetoothScan];
    final conn = res[Permission.bluetoothConnect];
    final loc = res[Permission.locationWhenInUse];
    // Android 12+ usa SCAN/CONNECT; versões antigas usam a localização
    final newOk = (scan?.isGranted ?? false) && (conn?.isGranted ?? false);
    final oldOk = loc?.isGranted ?? false;
    return newOk || oldOk;
  }

  static String describe(ScanResult r) {
    final n = r.advertisementData.advName.isNotEmpty ? r.advertisementData.advName : r.device.platformName;
    return n.isEmpty ? r.device.remoteId.str : n;
  }

  static Future<BleTransport> open(BluetoothDevice device, String label) async {
    await device.connect(timeout: const Duration(seconds: 15), autoConnect: false);
    final services = await device.discoverServices();

    BluetoothCharacteristic? write;
    BluetoothCharacteristic? notify;
    for (final s in services) {
      if (s.uuid == nusService) {
        for (final c in s.characteristics) {
          if (c.uuid == nusRx) write = c;
          if (c.uuid == nusTx) notify = c;
        }
      } else if (s.uuid == hm10Service) {
        for (final c in s.characteristics) {
          if (c.uuid == hm10Char) {
            write = c;
            notify = c;
          }
        }
      }
    }
    if (write == null || notify == null) {
      await device.disconnect();
      throw Exception('Este dispositivo não tem serviço UART (Nordic UART ou HM-10 FFE0/FFE1).');
    }

    final t = BleTransport._(device, write, label);
    await notify.setNotifyValue(true);
    t._subs.add(notify.onValueReceived.listen((v) => t.feed(Uint8List.fromList(v))));
    t._subs.add(device.connectionState.listen((s) {
      if (s == BluetoothConnectionState.disconnected) t.markClosed();
    }));
    return t;
  }

  @override
  Future<void> sendRaw(List<int> bytes) async {
    if (isClosed) throw Exception('Bluetooth desconectado.');
    final noResp = _write.properties.writeWithoutResponse;
    for (final part in chunked(bytes, 20)) {
      await _write.write(part, withoutResponse: noResp);
    }
  }

  @override
  Future<void> close() async {
    for (final s in _subs) {
      await s.cancel();
    }
    try {
      await _device.disconnect();
    } catch (_) {}
    markClosed();
  }
}
