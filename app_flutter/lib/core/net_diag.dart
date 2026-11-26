// Diagnósticos de rede feitos PELO CELULAR (o módulo Radioenge não tem rede IP).
// ICMP de verdade, com o programa "ping" do Android, e o mesmo texto da página de
// diagnósticos dos nós e gateways ESP32:
//   ping     5 pacotes ICMP de 32 bytes, 1 por segundo ("ping -c 1" por pacote)
//   trace    "ping -c 1 -t <ttl>" de 1 a 15 saltos (o Android não tem traceroute)
//   dns      resolvedor do sistema (A/AAAA) ou nome reverso (PTR) de um IP
import 'dart:convert';
import 'dart:io';

class DiagCancel {
  bool cancelled = false;
  Process? _proc;
  void cancel() {
    cancelled = true;
    _proc?.kill();
  }
}

final RegExp diagHostRe = RegExp(r'^[A-Za-z0-9._:-]{1,63}$');

bool diagHostOk(String h) => diagHostRe.hasMatch(h) && !h.startsWith('-');


const int _data = 32; // bytes de dados por pacote (como o DIAG_DATA do ESP32)
const int _hops = 15;
const String _ip4 = r'(\d{1,3}(?:\.\d{1,3}){3})';
final RegExp _who = RegExp(r'(?:bytes from|From)\s+' + _ip4, caseSensitive: false);

/// Um pacote ICMP pelo "ping" do Android (iputils/toybox): ("reply"|"ttl"|"unreach"|null, origem, ms, ttl).
Future<(String?, String?, double, int?)> _pingOnce(String ip, {int? ttl}) async {
  final sw = Stopwatch()..start();
  final r = await Process.run('ping', ['-n', '-c', '1', '-W', '1', '-s', '$_data', if (ttl != null) ...['-t', '$ttl'], ip]);
  sw.stop();
  final out = '${r.stdout}\n${r.stderr}';
  final ms = sw.elapsedMicroseconds / 1000;
  final rep = RegExp(r'bytes from\s+' + _ip4 + r'[^\n]*?time\s*[=<]\s*([\d.]+)\s*ms', caseSensitive: false).firstMatch(out);
  if (rep != null) {
    final t = RegExp(r'ttl\s*=\s*(\d+)', caseSensitive: false).firstMatch(out);
    return ('reply', rep.group(1), double.tryParse(rep.group(2)!) ?? ms, t == null ? null : int.parse(t.group(1)!));
  }
  final w = _who.firstMatch(out);
  if (w != null && RegExp(r'time to live exceeded|ttl exceeded', caseSensitive: false).hasMatch(out)) {
    return ('ttl', w.group(1), ms, null);
  }
  if (w != null && RegExp(r'unreachable', caseSensitive: false).hasMatch(out)) return ('unreach', w.group(1), ms, null);
  return (null, null, ms, null);
}

Future<String?> _resolve4(String host) async {
  try {
    final a = await InternetAddress.lookup(host, type: InternetAddressType.IPv4);
    return a.isEmpty ? null : a.first.address;
  } catch (_) {
    return null;
  }
}

/// Ping ICMP de verdade (o "ping" do Android), um pacote por segundo, com o mesmo
/// texto do diag_ping() dos nós e gateways ESP32.
Stream<String> diagPing(String host, DiagCancel c, {int count = 5}) async* {
  if (host.contains(':')) {
    yield 'ping: só IPv4 é suportado ("$host" é IPv6)\n';
    return;
  }
  final ip = await _resolve4(host);
  if (ip == null) {
    yield 'ping: não foi possível resolver "$host"\n';
    return;
  }
  yield 'PING $host ($ip): $_data bytes de dados\n';
  final ts = <double>[];
  var sent = 0;
  for (var seq = 0; seq < count && !c.cancelled; seq++) {
    final sw = Stopwatch()..start();
    (String?, String?, double, int?) r;
    try {
      r = await _pingOnce(ip);
    } on ProcessException {
      yield 'ping: o programa ping não existe neste aparelho\n';
      return;
    }
    sent++;
    switch (r.$1) {
      case 'reply':
        ts.add(r.$3);
        yield '${_data + 8} bytes de ${r.$2}: seq=$seq ttl=${r.$4 ?? '?'} tempo=${r.$3.toStringAsFixed(3)} ms\n';
      case 'ttl':
        yield 'De ${r.$2}: seq=$seq TTL esgotado\n';
      case 'unreach':
        yield 'De ${r.$2}: seq=$seq destino inalcançável\n';
      default:
        yield 'seq=$seq sem resposta (1 s)\n';
    }
    final left = 1000 - sw.elapsedMilliseconds;
    if (seq + 1 < count && left > 0) await Future<void>.delayed(Duration(milliseconds: left));
  }
  yield '\n--- estatísticas do ping para $host ---\n'
      '$sent pacotes transmitidos, ${ts.length} recebidos, ${sent == 0 ? 0 : (sent - ts.length) * 100 ~/ sent}% de perda\n';
  if (ts.isNotEmpty) {
    final mn = ts.reduce((a, b) => a < b ? a : b), mx = ts.reduce((a, b) => a > b ? a : b);
    final av = ts.reduce((a, b) => a + b) / ts.length;
    yield 'ida e volta mín/méd/máx = ${mn.toStringAsFixed(3)}/${av.toStringAsFixed(3)}/${mx.toStringAsFixed(3)} ms\n';
  }
}

/// Traceroute com o ping do sistema (TTL 1, 2, 3... até 15), como o diag_traceroute() do ESP32.
Stream<String> diagTrace(String host, DiagCancel c) async* {
  if (host.contains(':')) {
    yield 'traceroute: só IPv4 é suportado ("$host" é IPv6)\n';
    return;
  }
  final ip = await _resolve4(host);
  if (ip == null) {
    yield 'traceroute: não foi possível resolver "$host"\n';
    return;
  }
  yield 'traceroute para $host ($ip), máximo de $_hops saltos, pacotes de ${20 + 8 + _data} bytes\n';
  for (var ttl = 1; ttl <= _hops && !c.cancelled; ttl++) {
    (String?, String?, double, int?) r;
    try {
      r = await _pingOnce(ip, ttl: ttl);
    } on ProcessException {
      yield 'traceroute: o programa ping não existe neste aparelho\n';
      return;
    }
    final t = ttl.toString().padLeft(2);
    if (r.$1 == null) {
      yield '$t  *\n';
      continue;
    }
    yield '$t  ${r.$2}  ${r.$3.toStringAsFixed(3)} ms${r.$1 == 'unreach' ? '  !H' : ''}\n';
    if (r.$1 == 'reply' || r.$1 == 'unreach') return;
  }
}

/// Nslookup pelo resolvedor do sistema.
Stream<String> diagDns(String host) async* {
  final sw = Stopwatch()..start();
  yield 'Servidor:\tresolvedor do sistema (Android)\n\n';
  final ip = InternetAddress.tryParse(host);
  if (ip != null) {
    try {
      final r = await ip.reverse();
      yield '$host\tnome = ${r.host}\n';
    } catch (_) {
      yield '*** $host não tem nome reverso (PTR)\n';
    }
  } else {
    yield 'Nome:\t\t$host\n';
    try {
      final a = await InternetAddress.lookup(host);
      final seen = <String>{};
      for (final x in a) {
        if (seen.add(x.address)) yield 'Endereço:\t${x.address}\n';
      }
      if (seen.isEmpty) yield '*** não foi possível resolver $host (sem resposta ou nome inexistente)\n';
    } catch (_) {
      yield '*** não foi possível resolver $host (sem resposta ou nome inexistente)\n';
    }
  }
  yield '\n(${sw.elapsedMilliseconds} ms)\n';
}
