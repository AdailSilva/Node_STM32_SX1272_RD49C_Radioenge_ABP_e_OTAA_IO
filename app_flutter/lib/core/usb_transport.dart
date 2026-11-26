// USB OTG: adaptador USB-serial (CP2102, CH340/CH9102, FTDI, PL2303) ligado aos
// pinos 2 (RX) e 3 (TX) do módulo, com o GND em comum.
import 'dart:async';
import 'dart:typed_data';

import 'package:usb_serial/usb_serial.dart';

import 'transport.dart';

class UsbTransport extends LineTransport {
  UsbTransport._(this._port, this._name);

  final UsbPort _port;
  final String _name;
  StreamSubscription<Uint8List>? _sub;

  @override
  String get name => 'USB · $_name';

  static Future<List<UsbDevice>> list() => UsbSerial.listDevices();

  static String describe(UsbDevice d) {
    final name = (d.productName ?? '').trim();
    final maker = (d.manufacturerName ?? '').trim();
    final ids = 'VID ${_hex(d.vid)} · PID ${_hex(d.pid)}';
    if (name.isEmpty && maker.isEmpty) return ids;
    return '${[maker, name].where((s) => s.isNotEmpty).join(' ')} ($ids)';
  }

  static String _hex(int? v) => v == null ? '?' : v.toRadixString(16).padLeft(4, '0').toUpperCase();

  /// Abre o adaptador. O Android pede a permissão de USB na primeira vez.
  static Future<UsbTransport> open(UsbDevice device, {int baud = 115200}) async {
    final port = await device.create();
    if (port == null) {
      throw Exception('Não foi possível usar este adaptador USB (driver não suportado).');
    }
    final ok = await port.open();
    if (!ok) {
      throw Exception('O Android não liberou o adaptador USB (permissão negada?).');
    }
    await port.setDTR(false);
    await port.setRTS(false);
    await port.setPortParameters(baud, UsbPort.DATABITS_8, UsbPort.STOPBITS_1, UsbPort.PARITY_NONE);

    final t = UsbTransport._(port, (device.productName ?? 'USB-serial').trim());
    t._sub = port.inputStream?.listen(t.feed, onDone: t.markClosed, onError: (_) => t.markClosed());
    return t;
  }

  @override
  Future<bool> setBaud(int baud) async {
    if (isClosed) return false;
    await _port.setPortParameters(baud, UsbPort.DATABITS_8, UsbPort.STOPBITS_1, UsbPort.PARITY_NONE);
    return true;
  }

  @override
  Future<void> sendRaw(List<int> bytes) async {
    if (isClosed) throw Exception('USB desconectado.');
    await _port.write(Uint8List.fromList(bytes));
  }

  @override
  Future<void> close() async {
    await _sub?.cancel();
    try {
      await _port.close();
    } catch (_) {}
    markClosed();
  }
}
