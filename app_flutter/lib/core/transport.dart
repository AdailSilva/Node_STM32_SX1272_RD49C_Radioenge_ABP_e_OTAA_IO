// Ligação com o console do nó (UART do módulo, 115200 8N1): USB OTG ou BLE.
// Cada transporte entrega linhas de texto completas e manda linhas terminadas em CRLF.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

abstract class NodeTransport {
  /// "USB · CP2102", "BLE · HM-10"...
  String get name;

  /// Linhas recebidas, sem o fim de linha.
  Stream<String> get lines;

  /// Fecha quando a ligação cai (cabo solto, Bluetooth fora de alcance).
  Future<void> get closed;

  Future<void> sendRaw(List<int> bytes);

  Future<void> close();

  /// Troca a velocidade da porta depois de "save" com outra velocidade
  /// (AT+BAUDRATE / set baud). false = o transporte não controla a velocidade.
  Future<bool> setBaud(int baud);
}

/// Junta os bytes recebidos em linhas (UTF-8, \r\n ou \n).
abstract class LineTransport implements NodeTransport {
  final StreamController<String> _lines = StreamController<String>.broadcast();
  final Completer<void> _closed = Completer<void>();
  final List<int> _buf = [];

  @override
  Stream<String> get lines => _lines.stream;

  @override
  Future<void> get closed => _closed.future;

  bool get isClosed => _closed.isCompleted;

  void feed(Uint8List data) {
    for (final b in data) {
      if (b == 0x0A) {
        final line = utf8.decode(_buf, allowMalformed: true).replaceAll('\r', '');
        _buf.clear();
        _lines.add(line);
      } else if (_buf.length < 4096) {
        _buf.add(b);
      }
    }
  }

  /// O transporte avisa aqui quando a ligação caiu.
  /// Troca a velocidade da porta (USB-serial). false = não suportado (Bluetooth).
  Future<bool> setBaud(int baud) async => false;

  void markClosed() {
    if (!_closed.isCompleted) _closed.complete();
    if (!_lines.isClosed) _lines.close();
  }
}

/// Divide em pedaços (BLE manda poucos bytes por vez).
List<Uint8List> chunked(List<int> bytes, int size) {
  final out = <Uint8List>[];
  for (var i = 0; i < bytes.length; i += size) {
    final end = (i + size < bytes.length) ? i + size : bytes.length;
    out.add(Uint8List.fromList(bytes.sublist(i, end)));
  }
  return out;
}
