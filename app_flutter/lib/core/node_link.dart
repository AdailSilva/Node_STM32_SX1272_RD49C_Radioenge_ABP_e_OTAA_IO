// Conversa com o firmware pelo console, no "modo app":
//   mode app  -> sem eco nem prompt; cada comando termina com "@OK" ou "@ERR <msg>"
//   info      -> "@I {json}"   (firmware, versão, DevEUI...)
//   jstatus   -> "@S {json}"   (sessão, classe, contadores, relógio...)
//   jconfig   -> "@C {json}"   (configuração em uso)
//   jkeys     -> "@K {json}"   (chaves da sessão: DevAddr, NwkSKey, AppSKey)
//   jdiag     -> "@D {json}"   (autoteste do nó: rádio, EEPROM, sessão, relógio, VDD, RAM)
//   jlog <de> -> "@L <texto>" por linha + "@LN {start,next,end,size}" (log em RAM no nó)
//   jio       -> "@IO {json}"  (GPIOs, bateria AT+BAT, serial transparente)
//   jdev      -> "@DV {json}"  (relés, medidores PZEM-004T e protocolo ADAILSILVA_V1)
// Os comandos AT da Radioenge (AT+GPIOC, AT+WPIN, AT+SENDB...) também terminam em
// "@OK"/"@ERR", com a resposta original ("AT OK", "AT PARAM ERROR"...) numa linha antes.
// As outras linhas (eventos [TX], [RX], [JOIN], [Classe]...) vão para o log.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';

import 'transport.dart';

class CmdResult {
  const CmdResult(this.ok, this.message, {this.at = ''});

  final bool ok;

  /// Resposta original de um comando AT ("AT OK", "AT TX OK", "AT PARAM ERROR"...).
  final String at;

  /// "@OK restart" -> "restart"; "@ERR valor não aceito" -> "valor não aceito"
  final String message;
}

class NodeLink extends ChangeNotifier {
  NodeLink(this.transport) {
    _sub = transport.lines.listen(_onLine);
    transport.closed.then((_) {
      connected = false;
      _failPending('ligação perdida');
      notifyListeners();
    });
  }

  final NodeTransport transport;
  StreamSubscription<String>? _sub;
  Timer? _poll;

  bool connected = true;
  bool ready = false;
  Map<String, dynamic> info = {};
  Map<String, dynamic> status = {};
  Map<String, dynamic> config = {};

  /// Conta cada jconfig recebido: a aba Configuração compara para saber que ele mudou.
  int configSeq = 0;

  /// Última revisão da configuração vista no jstatus ("cfgrev", muda a cada alteração
  /// do NodeCfg no nó). Mudou -> relê o jconfig sozinho.
  int? _cfgRev;
  bool _cfgSync = false;

  /// A aba Configuração liga enquanto salva (os "set" mudam a revisão).
  bool holdConfigSync = false;

  /// Chaves da sessão LoRaWAN em uso (jkeys): valid, mode, devaddr, nwk, app, age, restored.
  Map<String, dynamic> keys = {};
  DateTime? statusAt;

  /// Último autoteste (jdiag): {"items":[{"n","l","v"}],"ok","warn","fail"}.
  Map<String, dynamic>? diag;

  /// GPIOs (jio): {"con2","bat","tr","g":[[modo,pull,valor],...]}; null = firmware sem jio.
  Map<String, dynamic>? io;
  DateTime? ioAt;

  /// Pede o jio junto com o status (a aba que mostra os GPIOs liga).
  bool ioPolling = false;

  /// Relés e PZEM (jdev): {"umode","rq","rmap","rpul","rcfg","pq","pon","pbusy","padr":[..],
  /// "pz":[{"ok","age","v","i","p","e","f","pf","al"}],"v1c","v1u","boot"}; null = firmware sem jdev.
  Map<String, dynamic>? dev;
  DateTime? devAt;

  String _lastAt = '';

  /// Ativação em uso no nó (AT+NJM). Antes do "info", vale o "njm" do jconfig e, sem
  /// ele, a ativação de fábrica do firmware (OTAA).
  bool get otaa => info['mode'] != null
      ? sv(info['mode']) == 'otaa'
      : (config.containsKey('njm') ? bv(config['njm']) : true);
  String get modeName => otaa ? 'OTAA' : 'ABP';
  String get modeKey => otaa ? 'otaa' : 'abp';

  // blocos do "jlog" em andamento
  final List<String> _jl = [];
  Map<String, dynamic>? _jlm;

  /// Log (o que o nó imprime), no máximo [maxLog] linhas.
  final List<String> log = [];
  static const int maxLog = 3000;
  bool logPaused = false;

  /// Avisa a UI quando o nó reiniciou (para recarregar a configuração).
  int reboots = 0;

  Completer<CmdResult>? _pending;
  Future<void> _queue = Future.value();

  /// A configuração mudou no nó por fora (downlink de comando, AT por downlink, outro
  /// console, Ações)? Com "cfgrev" no jstatus compara a revisão; firmware sem ele:
  /// compara a classe gravada (cfgcls) com a do jconfig. Mudou -> pede o jconfig.
  void _checkConfig() {
    var changed = false;
    if (status.containsKey('cfgrev')) {
      final r = iv(status['cfgrev']);
      changed = _cfgRev != null && r != _cfgRev;
      _cfgRev = r;
    } else {
      final cfg = sv(status['cfgcls']);
      changed = cfg.isNotEmpty && config.containsKey('class') && cfg != sv(config['class']);
    }
    if (!changed || !ready || holdConfigSync || _cfgSync) return;
    _cfgSync = true;
    _addLog('[app] A configuração do nó mudou: lendo de novo (jconfig).');
    // depois do @OK do jstatus em andamento (a fila de comandos é serial)
    Future<void>(() async {
      try {
        await cmd('jconfig');
      } finally {
        _cfgSync = false;
      }
    });
  }

  // ---------------------------------------------------------------- linhas
  void _onLine(String line) {
    if (line.startsWith('@')) {
      _onReply(line);
      return;
    }
    if (line.trim().isEmpty) {
      // Linha em branco do firmware (separa cada envio e as chaves do "Frame raw"):
      // entra no log, mas sem repetir nem abrir o log com uma.
      if (log.isNotEmpty && log.last.isNotEmpty) _addLog('');
      return;
    }
    if (RegExp(r'^AT [A-Z]').hasMatch(line)) _lastAt = line;
    _addLog(line);
    // O firmware reiniciou (banner): volta ao modo app e recarrega tudo.
    if (line.contains("Digite 'help'")) {
      reboots++;
      ready = false;
      _failPending('o nó reiniciou');
      Future<void>.delayed(const Duration(milliseconds: 300), start);
    }
  }

  void _onReply(String line) {
    if (line.startsWith('@I ')) {
      info = _json(line.substring(3)) ?? info;
      notifyListeners();
    } else if (line.startsWith('@S ')) {
      status = _json(line.substring(3)) ?? status;
      statusAt = DateTime.now();
      _checkConfig();
      notifyListeners();
    } else if (line.startsWith('@C ')) {
      config = _json(line.substring(3)) ?? config;
      configSeq++;
      notifyListeners();
    } else if (line.startsWith('@K ')) {
      keys = _json(line.substring(3)) ?? keys;
      notifyListeners();
    } else if (line.startsWith('@IO ')) {
      io = _json(line.substring(4)) ?? io;
      ioAt = DateTime.now();
      notifyListeners();
    } else if (line.startsWith('@DV ')) {
      dev = _json(line.substring(4)) ?? dev;
      devAt = DateTime.now();
      notifyListeners();
    } else if (line.startsWith('@LN ')) {
      _jlm = _json(line.substring(4));
    } else if (line.startsWith('@L ') || line == '@L') {
      _jl.add(line.length > 3 ? line.substring(3) : '');
    } else if (line.startsWith('@D ')) {
      diag = _json(line.substring(3));
    } else if (line.startsWith('@OK')) {
      _complete(CmdResult(true, line.substring(3).trim()));
    } else if (line.startsWith('@ERR')) {
      _complete(CmdResult(false, line.length > 4 ? line.substring(4).trim() : 'erro'));
      _addLog(line);
    }
  }

  Map<String, dynamic>? _json(String s) {
    try {
      final v = jsonDecode(s);
      return v is Map<String, dynamic> ? v : null;
    } catch (_) {
      return null;
    }
  }

  void _addLog(String line) {
    log.add(line);
    if (log.length > maxLog) log.removeRange(0, log.length - maxLog);
    if (!logPaused) notifyListeners();
  }

  void clearLog() {
    log.clear();
    notifyListeners();
  }

  void _complete(CmdResult r) {
    final p = _pending;
    _pending = null;
    if (p != null && !p.isCompleted) p.complete(r);
  }

  void _failPending(String why) => _complete(CmdResult(false, why));

  // -------------------------------------------------------------- comandos
  /// Manda um comando e espera "@OK"/"@ERR". Um comando por vez.
  Future<CmdResult> cmd(String line, {Duration timeout = const Duration(seconds: 6)}) {
    final run = _queue.then((_) async {
      if (!line.toUpperCase().startsWith('AT')) return _send(line, timeout);
      _lastAt = '';
      final r = await _send(line, timeout);
      // no @ERR de um AT, o motivo é a resposta original ("AT PARAM ERROR")
      return CmdResult(r.ok, !r.ok && _lastAt.isNotEmpty ? _lastAt : r.message, at: _lastAt);
    });
    _queue = run.then((_) {}, onError: (_) {});
    return run;
  }

  Future<CmdResult> _send(String line, Duration timeout) async {
    if (!connected) return const CmdResult(false, 'desconectado');
    final c = Completer<CmdResult>();
    _pending = c;
    try {
      // O nó dorme em STOP: o 1º byte só acorda o MCU e os seguintes que chegam
      // juntos se perdem. Acorda com "\r", espera e manda o comando.
      await transport.sendRaw(utf8.encode('\r'));
      await Future.delayed(const Duration(milliseconds: 60));
      await transport.sendRaw(utf8.encode('$line\r\n'));
    } catch (e) {
      _pending = null;
      return CmdResult(false, '$e');
    }
    try {
      return await c.future.timeout(timeout);
    } on TimeoutException {
      if (identical(_pending, c)) _pending = null;
      return const CmdResult(false, 'sem resposta do nó');
    }
  }

  /// Entra no modo app e carrega informações, configuração e status.
  Future<bool> start() async {
    var r = await cmd('mode app');
    if (!r.ok) {
      // primeira tentativa pode perder caracteres (MCU acordando): tenta de novo
      r = await cmd('mode app');
    }
    if (!r.ok) return false;
    await cmd('info');
    await cmd('jconfig');
    await cmd('jstatus');
    await cmd('jkeys');
    await cmd('jio');
    await cmd('jdev');
    ready = true;
    notifyListeners();
    return true;
  }

  /// Log que o firmware guarda em RAM, pedido em blocos ("jlog <de>"), como o
  /// /logs_download dos nós ESP32. null = o nó não respondeu (firmware sem jlog).
  Future<List<String>?> fetchNodeLog({void Function(int lines)? progress}) async {
    final out = <String>[];
    var from = 0;
    for (var i = 0; i < 80; i++) {
      _jl.clear();
      _jlm = null;
      final r = await cmd('jlog $from', timeout: const Duration(seconds: 10));
      if (!r.ok) return out.isEmpty ? null : out;
      out.addAll(_jl);
      progress?.call(out.length);
      final m = _jlm;
      if (m == null) break;
      final next = iv(m['next']);
      if (next >= iv(m['end']) || next == from) break;
      from = next;
    }
    return out;
  }

  /// Autoteste do nó (jdiag): o resultado fica em [diag].
  Future<CmdResult> runSelfTest() {
    diag = null;
    return cmd('jdiag', timeout: const Duration(seconds: 10));
  }

  Future<void> refreshStatus() async {
    if (!connected) return;
    await cmd('jstatus');
    await cmd('jkeys');
    if (ioPolling && io != null) await cmd('jio');
    if (ioPolling && dev != null) await cmd('jdev');
  }

  Future<void> refreshIo() async {
    if (connected) await cmd('jio');
  }

  Future<void> refreshDev() async {
    if (connected) await cmd('jdev');
  }

  Future<void> refreshConfig() async {
    if (connected) await cmd('jconfig');
  }

  /// Atualiza o status a cada 5 s enquanto [on] for true.
  void setPolling(bool on) {
    _poll?.cancel();
    _poll = null;
    if (on) {
      _poll = Timer.periodic(const Duration(seconds: 5), (_) {
        if (connected && ready && _pending == null) refreshStatus();
      });
    }
  }

  Future<void> disconnect() async {
    setPolling(false);
    connected = false;
    await _sub?.cancel();
    await transport.close();
    notifyListeners();
  }

  @override
  void dispose() {
    _poll?.cancel();
    _sub?.cancel();
    super.dispose();
  }
}

// ------------------------------------------------------------- utilitários
String sv(dynamic v) => v == null ? '' : '$v';
int iv(dynamic v) => v is int ? v : int.tryParse('$v') ?? 0;
bool bv(dynamic v) => v == true || v == 1 || v == '1';

String fmtUptime(int s) {
  final d = s ~/ 86400;
  final h = (s ~/ 3600) % 24;
  final m = (s ~/ 60) % 60;
  final ss = s % 60;
  final t = '${h.toString().padLeft(2, '0')}:${m.toString().padLeft(2, '0')}:${ss.toString().padLeft(2, '0')}';
  return d > 0 ? '$d d $t' : t;
}

bool isHex(String s, {int? bytes}) {
  final t = s.replaceAll(RegExp(r'[\s:-]'), '');
  if (t.isEmpty || t.length.isOdd || !RegExp(r'^[0-9a-fA-F]+$').hasMatch(t)) return false;
  return bytes == null || t.length == bytes * 2;
}

String cleanHex(String s) => s.replaceAll(RegExp(r'[\s:-]'), '').toUpperCase();

String hexToText(String hex) {
  final b = <int>[];
  for (var i = 0; i + 1 < hex.length; i += 2) {
    b.add(int.parse(hex.substring(i, i + 2), radix: 16));
  }
  return utf8.decode(b, allowMalformed: true);
}

String textToHex(String text) =>
    utf8.encode(text).map((e) => e.toRadixString(16).padLeft(2, '0')).join().toUpperCase();

bool isPrintableText(String hex) {
  for (var i = 0; i + 1 < hex.length; i += 2) {
    final v = int.tryParse(hex.substring(i, i + 2), radix: 16) ?? 0;
    if (v < 0x20 || v > 0x7E) return false;
  }
  return true;
}
