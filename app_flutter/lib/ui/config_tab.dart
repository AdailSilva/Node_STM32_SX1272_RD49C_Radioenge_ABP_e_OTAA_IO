// Configuração do nó (jconfig -> formulário -> "set campo valor" + "save"), com os
// parâmetros dos comandos AT do firmware original da Radioenge (AT+NJM, AT+AJOIN,
// AT+TXP, AT+CHMASK, janelas RX/JOIN, AT+PNM, AT+DCS, AT+NBTRIALS, AT+KEEPALIVE,
// porta AT, AT+BAUDRATE). Os mesmos campos e regras da página WEB.
import 'package:flutter/material.dart';

import '../core/app_info.dart';
import '../core/node_link.dart';
import 'widgets.dart';

/// Máscara AU915 (6 palavras de 16 bits, como o AT+CHMASK) de uma sub-banda.
String subBandMask(int sb, bool ch500) {
  final m = List<int>.filled(6, 0);
  if (sb >= 8) {
    m[0] = m[1] = m[2] = m[3] = 0xFFFF;
    m[4] = ch500 ? 0x00FF : 0;
  } else {
    m[sb >> 1] = 0x00FF << (8 * (sb & 1));
    m[4] = ch500 ? 1 << sb : 0;
  }
  return m.map((w) => w.toRadixString(16).padLeft(4, '0')).join();
}

/// "ff00000000000000..." -> "ff00:0000:..."
String groupMask(String hex) {
  final h = hex.replaceAll(':', '').toLowerCase();
  final out = <String>[];
  for (var i = 0; i < h.length; i += 4) {
    out.add(h.substring(i, i + 4 > h.length ? h.length : i + 4));
  }
  return out.join(':');
}

/// Canais habilitados de uma máscara, em texto (o mesmo resumo da página WEB).
String maskSummary(String hex) {
  final h = cleanHex(hex);
  if (!RegExp(r'^[0-9A-F]{24}$').hasMatch(h)) {
    return '24 dígitos hexadecimais (ex.: 00ff:0000:0000:0000:0001:0000 = SB1, canais 8–15 e 65).';
  }
  final on = <int>[];
  for (var w = 0; w < 6; w++) {
    final v = int.parse(h.substring(4 * w, 4 * w + 4), radix: 16);
    for (var b = 0; b < 16; b++) {
      if (v & (1 << b) != 0) on.add(16 * w + b);
    }
  }
  String rng(List<int> a) {
    final o = <String>[];
    for (var i = 0; i < a.length; i++) {
      var j = i;
      while (j + 1 < a.length && a[j + 1] == a[j] + 1) {
        j++;
      }
      o.add(i == j ? '${a[i]}' : '${a[i]}–${a[j]}');
      i = j;
    }
    return o.join(', ');
  }

  final lo = on.where((c) => c < 64).toList();
  final hi = on.where((c) => c >= 64 && c < 72).toList();
  if (on.any((c) => c >= 72)) return 'Canais acima do 71 não existem no AU915.';
  if (on.isEmpty) return 'Nenhum canal habilitado.';
  return '${lo.length} canal(is) de 125 kHz${lo.isEmpty ? '' : ' (${rng(lo)})'}'
      '${hi.isEmpty ? '' : ' + ${hi.length} de 500 kHz (${rng(hi)})'}'
      '${lo.length > 8 ? '. Com mais de 8 canais, o gateway precisa escutar todos.' : '.'}';
}

class ConfigTab extends StatefulWidget {
  const ConfigTab({super.key, required this.link});

  final NodeLink link;

  @override
  State<ConfigTab> createState() => ConfigTabState();
}

class ConfigTabState extends State<ConfigTab> {
  NodeLink get l => widget.link;

  // campos de texto
  final _c = <String, TextEditingController>{};
  // campos de escolha
  final _v = <String, dynamic>{};
  bool _payloadHex = false;
  bool _hasVerbose = false;   // firmware com o diagnóstico de sessão ("verbose" no jconfig)
  bool _hasUplinkMode = false;   // firmware com relés/PZEM ("uplinkmode" no jconfig)
  bool _devEuiAuto = true;
  bool _saving = false;
  bool _loaded = false;
  int _seenReboots = -1;

  /// jconfig usado no último preenchimento: base para saber se o formulário tem
  /// edições quando chega um jconfig novo.
  Map<String, dynamic> _base = {};
  int _seenSeq = -1;

  /// A configuração mudou no nó enquanto o formulário tinha edições.
  bool _extChanged = false;
  List<String> _errors = [];

  static const _textKeys = [
    'deveui', 'joineui', 'appkey', 'nwkkey', 'devaddr', 'nwkskey', 'appskey', 'netid',
    'interval', 'port', 'payload', 'rstport', 'rstcmd', 'rstdelay', 'cmdport', 'txpower',
    'rx1dl', 'rx2dl', 'jn1dl', 'jn2dl', 'nbtrials', 'atport', 'chmask', 'kaport', 'kaper',
  ];
  static const _restartKeys = [
    'deveui', 'joineui', 'appkey', 'nwkkey', 'devaddr', 'nwkskey', 'appskey', 'netid', 'subband', 'ch500', 'chmask', 'njm',
  ];
  static const _rx2Freqs = [923300000, 923900000, 924500000, 925100000, 925700000, 926300000, 926900000, 927500000];
  static const _bauds = [115200, 43000, 19200, 9600];

  bool get dirty => _loaded && _changes().isNotEmpty;

  /// Ativação escolhida no formulário (AT+NJM).
  bool get _otaa => _v['njm'] == true;
  bool get _custom => _v['subband'] == 9;

  @override
  void initState() {
    super.initState();
    for (final k in _textKeys) {
      _c[k] = TextEditingController();
    }
    l.addListener(_onLink);
    _onLink();
  }

  @override
  void dispose() {
    l.removeListener(_onLink);
    for (final c in _c.values) {
      c.dispose();
    }
    super.dispose();
  }

  void _onLink() {
    if (l.config.isEmpty || _saving) return;
    // carrega na primeira vez e depois de cada reinício do nó
    if (!_loaded || _seenReboots != l.reboots) {
      _fromNode();
      _seenReboots = l.reboots;
      return;
    }
    // jconfig novo (a configuração mudou no nó por fora: downlink de comando, AT
    // por downlink, Ações): sem edições, refaz o formulário com os valores reais;
    // com edições, mantém e avisa
    if (l.configSeq != _seenSeq) {
      _seenSeq = l.configSeq;
      if (_sameCfg(_base, l.config)) return;
      if (_changes(_base).isNotEmpty) {
        if (!_extChanged && mounted) setState(() => _extChanged = true);
        return;
      }
      _fromNode();
      if (mounted) snack(context, 'A configuração gravada no nó mudou: formulário atualizado.');
    }
  }

  /// Preenche com o jconfig atual do nó e guarda a base.
  void _fromNode() {
    _base = Map<String, dynamic>.from(l.config);
    _seenSeq = l.configSeq;
    _extChanged = false;
    _load(l.config);
  }

  static bool _sameCfg(Map<String, dynamic> a, Map<String, dynamic> b) {
    final ka = a.keys.where((k) => k != 'dirty').toSet(), kb = b.keys.where((k) => k != 'dirty').toSet();
    return ka.length == kb.length && ka.every((k) => kb.contains(k) && sv(a[k]) == sv(b[k]));
  }

  void _load(Map<String, dynamic> c) {
    _devEuiAuto = bv(c['devEuiAuto']);
    for (final k in ['deveui', 'joineui', 'appkey', 'nwkkey', 'devaddr', 'nwkskey', 'appskey', 'netid', 'rstcmd']) {
      _c[k]!.text = sv(c[k]);
    }
    for (final k in ['interval', 'port', 'rstport', 'rstdelay', 'cmdport', 'txpower',
                     'rx1dl', 'rx2dl', 'jn1dl', 'jn2dl', 'nbtrials', 'atport']) {
      _c[k]!.text = sv(c[k]);
    }
    final p = sv(c['payload']);
    _payloadHex = !isPrintableText(p);
    _c['payload']!.text = _payloadHex ? p : hexToText(p);
    for (final k in ['subband', 'dr', 'ping', 'lowpower', 'txp', 'rx2dr', 'rx2fq', 'baud']) {
      _v[k] = iv(c[k]);
    }
    for (final k in ['ch500', 'adr', 'confirmed', 'rst', 'extpower', 'leds', 'ajoin', 'dcs', 'pnm']) {
      _v[k] = bv(c[k]);
    }
    // diagnóstico de sessão: só existe nos firmwares novos (padrão ligado)
    _hasVerbose = c.containsKey('verbose');
    _v['verbose'] = _hasVerbose ? bv(c['verbose']) : true;
    _hasUplinkMode = c.containsKey('uplinkmode');
    _v['uplinkmode'] = _hasUplinkMode ? iv(c['uplinkmode']) : 0;
    _v['njm'] = c.containsKey('njm') ? bv(c['njm']) : l.otaa;
    _v['class'] = sv(c['class']).isEmpty ? 'A' : sv(c['class']);
    if (!c.containsKey('txp')) _v['txp'] = 255;
    _c['chmask']!.text = groupMask(sv(c['chmask']));
    final ka = sv(c['keepalive']).split(':');
    _v['kaen'] = ka.isNotEmpty && ka[0] == '1';
    _v['kacfm'] = ka.length > 2 && ka[2] == '1';
    _c['kaport']!.text = ka.length > 1 ? ka[1] : '5';
    _c['kaper']!.text = ka.length > 3 ? '${(iv(ka[3]) / 1000).round()}' : '60';
    _syncMask();
    _errors = [];
    _loaded = true;
    if (mounted) setState(() {});
  }

  /// Fora da máscara personalizada, o campo mostra a máscara da sub-banda escolhida.
  void _syncMask() {
    if (!_custom) _c['chmask']!.text = groupMask(subBandMask(iv(_v['subband']), _v['ch500'] == true));
  }

  /// Importar (aba Manutenção): junta o arquivo à configuração do nó e preenche o
  /// formulário, sem salvar. Devolve quantos campos o arquivo trouxe.
  int importJson(Map<String, dynamic> j) {
    final c = Map<String, dynamic>.from(l.config);
    var n = 0;
    for (final e in j.entries) {
      if (e.key.startsWith('_') || e.key == 'dirty') continue;
      c[e.key] = e.value;
      n++;
    }
    if (n > 0) _load(c);
    return n;
  }

  String _b(String k) => _v[k] == true ? '1' : '0';
  String _t(String k) => _c[k]!.text.trim();

  /// Valor atual de cada campo, no formato do comando "set".
  Map<String, String> _current() {
    final m = <String, String>{};
    m['njm'] = _otaa ? '1' : '0';
    m['deveui'] = _devEuiAuto ? 'auto' : cleanHex(_c['deveui']!.text);
    if (_otaa) {
      m['joineui'] = cleanHex(_c['joineui']!.text);
      m['appkey'] = cleanHex(_c['appkey']!.text);
      m['nwkkey'] = cleanHex(_c['nwkkey']!.text);
      m['ajoin'] = _b('ajoin');
    } else {
      m['devaddr'] = cleanHex(_c['devaddr']!.text);
      m['nwkskey'] = cleanHex(_c['nwkskey']!.text);
      m['appskey'] = cleanHex(_c['appskey']!.text);
      m['netid'] = '0x${cleanHex(_c['netid']!.text).isEmpty ? '0' : cleanHex(_c['netid']!.text)}';
    }
    // máscara personalizada: só o chmask (o firmware decide se vira sub-banda)
    if (_custom) {
      m['chmask'] = cleanHex(_c['chmask']!.text).toLowerCase();
    } else {
      m['subband'] = '${_v['subband']}';
      m['ch500'] = _b('ch500');
    }
    m['adr'] = _b('adr');
    m['dr'] = '${_v['dr']}';
    m['txp'] = '${_v['txp']}';
    if (_v['txp'] == 255) m['txpower'] = _t('txpower');
    m['class'] = '${_v['class']}';
    m['ping'] = '${_v['ping']}';
    m['rx1dl'] = _t('rx1dl');
    m['rx2dl'] = _t('rx2dl');
    m['rx2fq'] = '${_v['rx2fq']}';
    m['rx2dr'] = '${_v['rx2dr']}';
    m['jn1dl'] = _t('jn1dl');
    m['jn2dl'] = _t('jn2dl');
    m['pnm'] = _b('pnm');
    m['dcs'] = _b('dcs');
    m['interval'] = _t('interval');
    m['port'] = _t('port');
    m['confirmed'] = _b('confirmed');
    m['payloadhex'] = _payloadHex ? cleanHex(_c['payload']!.text) : textToHex(_c['payload']!.text);
    m['nbtrials'] = _t('nbtrials');
    m['keepalive'] = '${_b('kaen')}:${_t('kaport')}:${_b('kacfm')}:${(int.tryParse(_t('kaper')) ?? 0) * 1000}';
    m['rst'] = _b('rst');
    m['rstport'] = _t('rstport');
    m['rstcmd'] = cleanHex(_c['rstcmd']!.text);
    m['rstdelay'] = _t('rstdelay');
    m['cmdport'] = _t('cmdport');
    m['atport'] = _t('atport');
    m['lowpower'] = '${_v['lowpower']}';
    m['extpower'] = _b('extpower');
    m['leds'] = _b('leds');
    if (_hasVerbose) m['verbose'] = _b('verbose');
    if (_hasUplinkMode) m['uplinkmode'] = '${_v['uplinkmode']}';
    m['baud'] = '${_v['baud']}';
    return m;
  }

  /// O que o nó tem hoje, no mesmo formato.
  Map<String, String> _original([Map<String, dynamic>? base]) {
    final c = base ?? l.config;
    String b(String k) => bv(c[k]) ? '1' : '0';
    final m = <String, String>{};
    m['njm'] = c.containsKey('njm') ? b('njm') : (l.otaa ? '1' : '0');
    m['deveui'] = bv(c['devEuiAuto']) ? 'auto' : sv(c['deveui']);
    for (final k in ['joineui', 'appkey', 'nwkkey', 'devaddr', 'nwkskey', 'appskey']) {
      m[k] = sv(c[k]);
    }
    m['netid'] = '0x${sv(c['netid'])}';
    m['ajoin'] = b('ajoin');
    m['chmask'] = sv(c['chmask']).toLowerCase();
    for (final k in ['subband', 'dr', 'txp', 'txpower', 'class', 'ping', 'rx1dl', 'rx2dl', 'rx2fq', 'rx2dr', 'jn1dl', 'jn2dl',
                     'interval', 'port', 'nbtrials', 'rstport', 'rstcmd', 'rstdelay', 'cmdport', 'atport', 'lowpower', 'baud']) {
      m[k] = sv(c[k]);
    }
    for (final k in ['ch500', 'adr', 'pnm', 'dcs', 'confirmed', 'rst', 'extpower', 'leds']) {
      m[k] = b(k);
    }
    if (c.containsKey('verbose')) m['verbose'] = b('verbose');
    if (c.containsKey('uplinkmode')) m['uplinkmode'] = sv(c['uplinkmode']);
    m['payloadhex'] = sv(c['payload']);
    // período comparado em segundos, como no formulário
    final ka = sv(c['keepalive']).split(':');
    m['keepalive'] = ka.length == 4 ? '${ka[0]}:${ka[1]}:${ka[2]}:${(iv(ka[3]) / 1000).round() * 1000}' : '';
    return m;
  }

  String _normNetid(String v) {
    final t = v.replaceFirst(RegExp(r'^0x', caseSensitive: false), '').replaceFirst(RegExp(r'^0+'), '');
    return '0x${t.isEmpty ? '0' : t}';
  }

  Map<String, String> _changes([Map<String, dynamic>? base]) {
    final cur = _current();
    final org = _original(base);
    final out = <String, String>{};
    cur.forEach((k, v) {
      var a = (org[k] ?? '').toUpperCase(), b = v.toUpperCase();
      if (k == 'netid') {
        a = _normNetid(a);
        b = _normNetid(b);
      }
      if (k == 'chmask') {
        a = cleanHex(a);
        b = cleanHex(b);
      }
      if (a != b) out[k] = v;
    });
    return out;
  }

  List<String> _validate() {
    final e = <String>[];
    int? n(String k) => int.tryParse(_t(k));
    bool num(String k, int min, int max) {
      final v = n(k);
      return v != null && v >= min && v <= max;
    }

    if (!_devEuiAuto && !isHex(_c['deveui']!.text, bytes: 8)) e.add('DevEUI: 16 dígitos hexadecimais.');
    if (_otaa) {
      if (!isHex(_c['joineui']!.text, bytes: 8)) e.add('JoinEUI: 16 dígitos hexadecimais.');
      if (!isHex(_c['appkey']!.text, bytes: 16)) e.add('AppKey: 32 dígitos hexadecimais.');
      if (!isHex(_c['nwkkey']!.text, bytes: 16)) e.add('NwkKey: 32 dígitos hexadecimais.');
    } else {
      if (!isHex(_c['devaddr']!.text, bytes: 4)) e.add('DevAddr: 8 dígitos hexadecimais.');
      if (!isHex(_c['nwkskey']!.text, bytes: 16)) e.add('NwkSKey: 32 dígitos hexadecimais.');
      if (!isHex(_c['appskey']!.text, bytes: 16)) e.add('AppSKey: 32 dígitos hexadecimais.');
      final id = cleanHex(_c['netid']!.text);
      if (id.isNotEmpty && (!RegExp(r'^[0-9A-F]{1,6}$').hasMatch(id))) e.add('NetID: até 6 dígitos hexadecimais.');
    }
    if (_custom) {
      final h = cleanHex(_c['chmask']!.text);
      if (!RegExp(r'^[0-9A-F]{16}00[0-9A-F]{2}0000$').hasMatch(h) || !RegExp(r'[1-9A-F]').hasMatch(h)) {
        e.add('Máscara de canais: 24 dígitos hexadecimais, pelo menos um canal, só até o canal 71 (a 6ª palavra é 0000).');
      }
    }
    if (_v['txp'] == 255 && !num('txpower', 2, 20)) e.add('Potência: 2 a 20 dBm.');
    if (!num('rx1dl', 1000, 15000)) e.add('Atraso da RX1: 1000 a 15000 ms.');
    if (!num('rx2dl', 1000, 16000) || (n('rx2dl') ?? 0) <= (n('rx1dl') ?? 0)) {
      e.add('Atraso da RX2: até 16000 ms e maior que o da RX1.');
    }
    if (!num('jn1dl', 1000, 15000)) e.add('JOIN, janela 1: 1000 a 15000 ms.');
    if (!num('jn2dl', 1000, 16000) || (n('jn2dl') ?? 0) <= (n('jn1dl') ?? 0)) {
      e.add('JOIN, janela 2: até 16000 ms e maior que a janela 1.');
    }
    if (!num('interval', 5, 86400)) e.add('Intervalo: 5 a 86400 s.');
    if (!num('port', 1, 223)) e.add('FPort do uplink: 1 a 223.');
    if (!num('nbtrials', 0, 79)) e.add('Tentativas: 0 a 79.');
    if (!num('kaport', 1, 223)) e.add('FPort do keepalive: 1 a 223.');
    if (!num('kaper', 5, 86400)) e.add('Período do keepalive: 5 a 86400 s.');
    if (!num('rstport', 1, 223)) e.add('FPort do reset: 1 a 223.');
    if (!num('rstdelay', 0, 60000)) e.add('Espera do reset: 0 a 60000 ms.');
    if (!num('cmdport', 0, 223)) e.add('FPort de comandos: 0 (desligado) a 223.');
    final at = n('atport');
    if (!num('atport', 0, 223) || at == 4 || (at != 0 && at == n('cmdport'))) {
      e.add('FPort dos comandos AT: 0 (desligado) ou 1 a 223, diferente de 4 (serial transparente) e da FPort de comandos.');
    }
    final rc = cleanHex(_c['rstcmd']!.text);
    if (!isHex(rc) || rc.length > 32) e.add('Comando de reset: 1 a 16 bytes em hexadecimal.');
    final pl = _c['payload']!.text;
    if (_payloadHex) {
      if (pl.trim().isNotEmpty && !isHex(pl)) e.add('Payload: hexadecimal inválido.');
      if (cleanHex(pl).length > 102) e.add('Payload: até 51 bytes.');
    } else if (textToHex(pl).length > 102) {
      e.add('Payload: até 51 bytes.');
    }
    return e;
  }

  Future<void> _save() async {
    final errs = _validate();
    setState(() => _errors = errs);
    if (errs.isNotEmpty) return;
    final ch = _changes();
    if (ch.isEmpty) {
      snack(context, 'Nada mudou.');
      return;
    }
    final restart = ch.keys.any(_restartKeys.contains);
    if (restart &&
        !await confirmDialog(
            context,
            'Salvar',
            ch.containsKey('njm')
                ? 'A ativação muda para ${_otaa ? 'OTAA' : 'ABP'}: o nó reinicia e a sessão LoRaWAN recomeça do zero.'
                : 'Credenciais ou canais mudaram: o nó vai reiniciar e a sessão LoRaWAN recomeça do zero.',
            confirm: 'Salvar e reiniciar')) {
      return;
    }
    final newBaud = ch.containsKey('baud') ? int.tryParse(ch['baud']!) ?? 0 : 0;
    if (!mounted) return;
    if (newBaud > 0 &&
        !await confirmDialog(
            context,
            'Velocidade da serial',
            l.transport.name.startsWith('BLE')
                ? 'O console passa para $newBaud bps. Configure o módulo Bluetooth na mesma velocidade, '
                    'senão a ligação para de funcionar.'
                : 'O console passa para $newBaud bps. O app troca a velocidade do adaptador USB junto.',
            confirm: 'Salvar')) {
      return;
    }
    setState(() => _saving = true);
    l.holdConfigSync = true;
    final fails = <String>[];
    for (final e in ch.entries) {
      // valor vazio (ex.: payload): o firmware aceita "set payloadhex" sem valor
      final line = e.value.isEmpty ? 'set ${e.key}' : 'set ${e.key} ${e.value}';
      final r = await l.cmd(line);
      if (!r.ok) fails.add('${e.key}: ${r.message}');
    }
    if (fails.isNotEmpty) {
      await l.cmd('jconfig');
      l.holdConfigSync = false;
      setState(() {
        _saving = false;
        _errors = fails;
      });
      return;
    }
    final r = await l.cmd('save');
    l.holdConfigSync = false;
    // o nó responde na velocidade antiga e troca logo depois ("@OK baud"); com
    // "restart" a nova velocidade vale no boot
    var baudOk = true;
    if (r.ok && newBaud > 0 && (r.message.contains('baud') || r.message.contains('restart'))) {
      baudOk = await l.transport.setBaud(newBaud);
    }
    setState(() => _saving = false);
    if (!mounted) return;
    if (!r.ok) {
      setState(() => _errors = ['save: ${r.message}']);
      return;
    }
    if (!baudOk) {
      await messageDialog(context, 'Velocidade da serial',
          'O nó passou para $newBaud bps. Configure o módulo Bluetooth nessa velocidade e conecte de novo.');
      return;
    }
    if (r.message.contains('restart')) {
      snack(context, 'Gravado. O nó está reiniciando…');
    } else {
      await l.cmd('jconfig');
      await l.refreshStatus();
      if (mounted) snack(context, newBaud > 0 ? 'Gravado. Serial agora em $newBaud bps.' : 'Gravado e aplicado.');
      _fromNode();
    }
  }

  void _discard() => _fromNode();

  /// "Ler do nó": pede a configuração gravada de novo (jconfig) e refaz o formulário com ela
  Future<void> _readFromNode() async {
    if (dirty &&
        !await confirmDialog(context, 'Ler do nó', 'Descartar as alterações não salvas e ler a configuração gravada no nó?',
            confirm: 'Ler')) {
      return;
    }
    final r = await l.cmd('jconfig');
    if (!mounted) return;
    if (!r.ok) {
      snack(context, 'Erro: ${r.message}');
      return;
    }
    await l.refreshStatus();
    if (!mounted) return;
    setState(() => _errors = []);
    _fromNode();
    snack(context, 'Configuração lida do nó.');
  }

  Widget _text(String key, String label, {String? hint, String? helper, bool mono = true, bool number = false,
      int? maxLength, bool upper = false, bool enabled = true, bool copy = false, bool copyHexValue = true,
      ValueChanged<String>? onChanged}) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: TextFieldX(
        controller: _c[key]!,
        label: label,
        hint: hint,
        helper: helper,
        mono: mono,
        keyboard: number ? TextInputType.number : null,
        maxLength: maxLength,
        upper: upper,
        wrap: mono && (maxLength ?? 0) > 20,
        enabled: enabled,
        onChanged: (v) => setState(() => onChanged?.call(v)),
        // todo campo em hexadecimal tem Copiar (sem espaços, como no ChirpStack); funciona
        // mesmo com o campo desativado (DevEUI automático)
        trailing: copy
            ? [
                CopyHexButton(
                  value: () => key == 'deveui' && _c[key]!.text.trim().isEmpty ? sv(l.info['deveui']) : _c[key]!.text,
                  name: label.split(' ').first,
                  hex: copyHexValue,
                ),
              ]
            : null,
      ),
    );
  }

  Widget _switch(String key, String title, [String subtitle = '', VoidCallback? after]) {
    return SwitchListTile(
      contentPadding: EdgeInsets.zero,
      title: Text(title),
      subtitle: subtitle.isEmpty ? null : Text(subtitle, style: const TextStyle(fontSize: 12.5)),
      value: _v[key] == true,
      onChanged: (v) => setState(() {
        _v[key] = v;
        after?.call();
      }),
    );
  }

  @override
  Widget build(BuildContext context) {
    if (!_loaded) return const Center(child: CircularProgressIndicator());
    final otaa = _otaa;
    final njmChanged = l.config.containsKey('njm') && otaa != bv(l.config['njm']);
    final subBands = <MapEntry<int, String>>[
      for (var i = 0; i <= 7; i++) MapEntry(i, 'Sub-banda $i (canais ${i * 8}–${i * 8 + 7} + ${64 + i})'),
      const MapEntry(8, 'Todos os canais (0–63)'),
      const MapEntry(9, 'Máscara personalizada (AT+CHMASK)'),
    ];
    const drs = <MapEntry<int, String>>[
      MapEntry(2, 'DR2 · SF10/125 kHz (até 11 bytes)'),
      MapEntry(3, 'DR3 · SF9/125 kHz (até 53 bytes)'),
      MapEntry(4, 'DR4 · SF8/125 kHz'),
      MapEntry(5, 'DR5 · SF7/125 kHz'),
      MapEntry(6, 'DR6 · SF8/500 kHz'),
    ];
    final txps = <MapEntry<int, String>>[
      const MapEntry(255, 'Usar a potência em dBm'),
      for (var i = 0; i <= 15; i++) MapEntry(i, i == 0 ? '0 · potência máxima' : '$i · máxima − ${2 * i} dB'),
    ];
    const rx2drs = <MapEntry<int, String>>[
      MapEntry(8, 'DR8 · SF12/500 kHz (padrão)'),
      MapEntry(9, 'DR9 · SF11/500 kHz'),
      MapEntry(10, 'DR10 · SF10/500 kHz'),
      MapEntry(11, 'DR11 · SF9/500 kHz'),
      MapEntry(12, 'DR12 · SF8/500 kHz'),
      MapEntry(13, 'DR13 · SF7/500 kHz'),
    ];
    final rx2fqs = <MapEntry<int, String>>[
      for (final f in _rx2Freqs) MapEntry(f, '${(f / 1e6).toStringAsFixed(1)} MHz${f == 923300000 ? ' (padrão)' : ''}'),
    ];

    return Column(
      children: [
        Expanded(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(14, 14, 14, 24),
            children: [
              if (_errors.isNotEmpty) ErrorBox(_errors),
              if (_extChanged)
                const StatusBanner(
                  'A configuração gravada no nó mudou por fora (downlink de comando, AT por downlink ou Ações) '
                  'enquanto o formulário tinha alterações não salvas. "Ler do nó" mostra os valores reais '
                  '(descarta as alterações); "Salvar" grava as suas por cima.',
                  level: Level.warn,
                ),
              SectionCard(
                title: otaa ? 'Ativação OTAA' : 'Ativação ABP',
                subtitle: otaa
                    ? 'Os mesmos valores do dispositivo no ChirpStack (MSB primeiro).'
                    : 'Copie do ChirpStack (aba Activation). LoRaWAN 1.0.x: a NwkSKey vale para as 3 chaves de rede.',
                tag: 'reinicia a sessão',
                tagNow: false,
                children: [
                  SegChoice(
                    label: 'Ativação (AT+NJM)',
                    options: const [SegOption('abp', 'ABP'), SegOption('otaa', 'OTAA')],
                    value: otaa ? 'otaa' : 'abp',
                    showHint: false,
                    onChanged: (v) => setState(() => _v['njm'] = v == 'otaa'),
                  ),
                  Hint(njmChanged
                      ? 'Ao salvar, o nó reinicia em ${otaa ? 'OTAA' : 'ABP'}: preencha as credenciais ${otaa ? 'OTAA' : 'ABP'} abaixo.'
                      : 'Troca ABP ↔ OTAA sem regravar o firmware. Ao salvar, o nó reinicia com a outra ativação.',
                      top: 0),
                  const SizedBox(height: 8),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('DevEUI automático'),
                    subtitle: const Text('Gerado do ID único do STM32', style: TextStyle(fontSize: 12.5)),
                    value: _devEuiAuto,
                    onChanged: (v) => setState(() => _devEuiAuto = v),
                  ),
                  _text('deveui', 'DevEUI', hint: '16 dígitos hex', maxLength: 16, upper: true, enabled: !_devEuiAuto, copy: true),
                  if (otaa) ...[
                    _text('joineui', 'JoinEUI (AppEUI)', hint: '16 dígitos hex', maxLength: 16, upper: true, copy: true),
                    _text('appkey', 'AppKey', hint: '32 dígitos hex', maxLength: 32, upper: true, copy: true),
                    _text('nwkkey', 'NwkKey', hint: '32 dígitos hex', maxLength: 32, upper: true, copy: true,
                        helper: 'Só LoRaWAN 1.1. No ChirpStack com MAC 1.0.x, deixe igual à AppKey.'),
                    _switch('ajoin', 'JOIN automático no boot',
                        'AT+AJOIN · desligado, o JOIN só acontece com AT+JOIN ou Refazer JOIN'),
                  ] else ...[
                    _text('devaddr', 'DevAddr', hint: '8 dígitos hex', maxLength: 8, upper: true, copy: true),
                    _text('nwkskey', 'NwkSKey', hint: '32 dígitos hex', maxLength: 32, upper: true, copy: true),
                    _text('appskey', 'AppSKey', hint: '32 dígitos hex', maxLength: 32, upper: true, copy: true),
                    _text('netid', 'NetID', hint: '000000', maxLength: 6, upper: true, copy: true),
                  ],
                ],
              ),
              SectionCard(
                title: 'Rádio LoRa',
                subtitle: 'Use os mesmos canais que o gateway escuta.',
                children: [
                  DropField<int>(
                    label: 'Plano AU915',
                    items: subBands,
                    value: _v['subband'] as int?,
                    onChanged: (v) => setState(() {
                      _v['subband'] = v;
                      _syncMask();
                    }),
                    helper: 'Mudar a sub-banda ou a máscara reinicia a sessão.',
                  ),
                  _text('chmask', 'Máscara de canais (AT+CHMASK)', maxLength: 29, enabled: _custom, copy: true,
                      helper: '${_custom ? '' : 'Máscara da sub-banda escolhida: '}${maskSummary(_c['chmask']!.text)}'),
                  if (!_custom) _switch('ch500', 'Canal de 500 kHz da sub-banda', '64 + sub-banda (padrão: 65)', _syncMask),
                  _switch('adr', 'ADR', 'O servidor ajusta DR e potência'),
                  DropField<int>(
                    label: 'Data rate (sem ADR)',
                    items: drs,
                    value: _v['dr'] as int?,
                    onChanged: (v) => setState(() => _v['dr'] = v),
                    helper: 'AU915 com dwell time de 400 ms: DR0 e DR1 não são permitidos.',
                  ),
                  DropField<int>(
                    label: 'Potência por índice (AT+TXP)',
                    items: txps,
                    value: _v['txp'] as int?,
                    onChanged: (v) => setState(() => _v['txp'] = v),
                  ),
                  _text('txpower', 'Potência (dBm, conduzida)', number: true, maxLength: 2, enabled: _v['txp'] == 255,
                      helper: '2 a 20. Acima de 17 dBm o SX1272 usa o modo +20 dBm (≤ 1 % do tempo).'),
                ],
              ),
              SectionCard(
                title: 'Janelas de recepção e rede',
                subtitle: 'Padrão do ChirpStack e da TTN: RX1 1000 ms, RX2 2000 ms, JOIN 5000/6000 ms, '
                    'RX2 em 923,3 MHz DR8. Everynet: RX1 5000 e RX2 6000 ms. No OTAA o Join-accept define os atrasos.',
                children: [
                  Pair(
                    _text('rx1dl', 'Atraso da RX1 (ms) · RX1DL', number: true, mono: false),
                    _text('rx2dl', 'Atraso da RX2 (ms) · RX2DL', number: true, mono: false),
                  ),
                  DropField<int>(
                    label: 'Frequência da RX2 · RX2FQ',
                    items: rx2fqs,
                    value: _v['rx2fq'] as int?,
                    onChanged: (v) => setState(() => _v['rx2fq'] = v),
                  ),
                  DropField<int>(
                    label: 'Data rate da RX2 · RX2DR',
                    items: rx2drs,
                    value: _v['rx2dr'] as int?,
                    onChanged: (v) => setState(() => _v['rx2dr'] = v),
                  ),
                  Pair(
                    _text('jn1dl', 'JOIN, janela 1 (ms) · JN1DL', number: true, mono: false),
                    _text('jn2dl', 'JOIN, janela 2 (ms) · JN2DL', number: true, mono: false),
                  ),
                  _switch('pnm', 'Rede pública', 'AT+PNM · sync word 0x34; desligado: rede privada (0x12)'),
                  _switch('dcs', 'Duty cycle', 'AT+DCS · limite de ocupação do canal (ETSI); o AU915 não exige'),
                ],
              ),
              SectionCard(
                title: 'Classe LoRaWAN',
                subtitle: 'Quando o nó escuta downlinks. Ícone 🎓 no topo: explicação completa.',
                children: [
                  SegChoice(
                    options: const [
                      SegOption('A', 'Classe A', 'Escuta só depois de cada uplink. Menor consumo.'),
                      SegOption('B', 'Classe B', 'Ping slots sincronizados por beacon (gateway com GPS + PPS).'),
                      SegOption('C', 'Classe C', 'Escuta o tempo todo. Precisa de alimentação contínua.'),
                    ],
                    value: _v['class'] as String?,
                    onChanged: (v) => setState(() => _v['class'] = v),
                  ),
                  const Hint('Mostra a classe gravada no nó (lida com jconfig). Se ela muda no nó (Ações, downlink de comando, AT), '
                      'o formulário se refaz sozinho.'),
                  const Hint('ChirpStack: ele não pergunta a classe ao nó. No LoRaWAN 1.0.x, a cada JOIN ele usa a do '
                      'perfil do dispositivo: perfil com Class-C marcada vira deviceClassEnabled: CLASS_C e os downlinks '
                      'saem na hora, mesmo com o nó em A (e se perdem). Deixe o perfil igual à classe daqui e refaça o JOIN.'),
                  const SizedBox(height: 10),
                  DropField<int>(
                    label: 'Ping slot (Classe B)',
                    items: [for (var i = 0; i <= 7; i++) MapEntry(i, 'a cada ${1 << i} s (2^$i)')],
                    value: _v['ping'] as int?,
                    onChanged: (v) => setState(() => _v['ping'] = v),
                    helper: 'Igual à "ping-slot periodicity" do perfil no ChirpStack.',
                  ),
                ],
              ),
              SectionCard(
                title: 'Uplink',
                children: [
                  if (_hasUplinkMode)
                    SegChoice(
                      label: 'Conteúdo do uplink periódico · AT+TELEM',
                      options: const [
                        SegOption('1', 'Telemetria V1',
                            'Protocolo ADAILSILVA_V1 na porta 20: relés, PZEM, alimentação e motivo do boot (TLV). '
                                'Com PZEM use DR3 ou maior.'),
                        SegOption('0', 'Payload fixo', 'O payload abaixo, na FPort abaixo.'),
                      ],
                      value: '${_v['uplinkmode']}',
                      onChanged: (v) => setState(() => _v['uplinkmode'] = iv(v)),
                    ),
                  Pair(
                    _text('interval', 'Intervalo (s)', number: true, mono: false),
                    _text('port', 'FPort', number: true, mono: false),
                  ),
                  _switch('confirmed', 'Uplink confirmado', 'Pede ACK ao servidor'),
                  _text('nbtrials', 'Tentativas · NBTRIALS', number: true, mono: false,
                      helper: 'Uplink confirmado: até 15 tentativas (LoRaWAN 1.0.4). No AT+JOIN vale o número todo (0 a 79).'),
                  SegChoice(
                    label: 'Payload',
                    options: const [SegOption('txt', 'Texto'), SegOption('hex', 'Hexadecimal')],
                    value: _payloadHex ? 'hex' : 'txt',
                    showHint: false,
                    onChanged: (v) => setState(() {
                      final t = _c['payload']!.text;
                      if (v == 'hex' && !_payloadHex) {
                        _c['payload']!.text = textToHex(t);
                      } else if (v == 'txt' && _payloadHex && isHex(t) && isPrintableText(cleanHex(t))) {
                        _c['payload']!.text = hexToText(cleanHex(t));
                      }
                      _payloadHex = v == 'hex';
                    }),
                  ),
                  const SizedBox(height: 8),
                  _text('payload', 'Conteúdo', mono: _payloadHex, maxLength: _payloadHex ? 102 : 51,
                      upper: _payloadHex, helper: 'Até 51 bytes. Com DR2, só 11 bytes cabem.',
                      copy: true, copyHexValue: _payloadHex),
                ],
              ),
              SectionCard(
                title: 'Keepalive',
                subtitle: 'AT+KEEPALIVE · pacote de 16 bytes do firmware original (Tabela 9 do manual): '
                    'bateria, RSSI, SNR, entradas e analógicas.',
                children: [
                  _switch('kaen', 'Keepalive ligado'),
                  Pair(
                    _text('kaport', 'FPort do keepalive', number: true, mono: false, enabled: _v['kaen'] == true),
                    _text('kaper', 'Período (s)', number: true, mono: false, enabled: _v['kaen'] == true),
                  ),
                  _switch('kacfm', 'Keepalive confirmado'),
                ],
              ),
              SectionCard(
                title: 'Downlinks',
                subtitle: 'Reset remoto, comandos binários e comandos AT por downlink.',
                children: [
                  _switch('rst', 'Reset remoto'),
                  Pair(
                    _text('rstport', 'FPort do reset', number: true, mono: false),
                    _text('rstdelay', 'Espera (ms)', number: true, mono: false),
                  ),
                  _text('rstcmd', 'Comando de reset (hex)', maxLength: 32, upper: true, copy: true),
                  _text('cmdport', 'FPort de comandos (0 = desligado)', number: true, mono: false,
                      helper: '01 00/01/02 = classe A/B/C · 02 hh ll = intervalo · 03 nn = ping · '
                          '04 00/01 = confirmado · 05 = uplink agora'),
                  _text('atport', 'FPort dos comandos AT (0 = desligado)', number: true, mono: false,
                      helper: 'O downlink é o texto do comando (AT+WPIN=3:1; vários separados por ";"). '
                          'A resposta volta num uplink na mesma porta.'),
                  Hint('Portas em uso: 2 interrupção de GPIO · 4 serial transparente · '
                      '${_t('kaport')} keepalive · ${_t('port')} uplink · ${_t('rstport')} reset · '
                      '${_t('cmdport')} comandos · ${_t('atport')} AT.'),
                ],
              ),
              SectionCard(
                title: 'Energia e serial',
                children: [
                  DropField<int>(
                    label: 'Modo de baixo consumo',
                    items: const [
                      MapEntry(0, '0 · sem STOP (depuração)'),
                      MapEntry(1, '1 · STOP, console sempre pronto'),
                      MapEntry(2, '2 · STOP econômico (1º caractere só acorda)'),
                    ],
                    value: _v['lowpower'] as int?,
                    onChanged: (v) => setState(() => _v['lowpower'] = v),
                  ),
                  _switch('extpower', 'Alimentação externa', 'Informa nível de bateria 0 ao servidor'),
                  _switch('leds', 'LEDs', 'Vermelho TX, verde RX, amarelo serial (como o original)'),
                  if (_hasVerbose)
                    _switch('verbose', 'Diagnóstico de sessão',
                        'Chaves, Frame raw, FCnt e MIC a cada envio, nos Logs (como nos nós ESP32)'),
                  DropField<int>(
                    label: 'Velocidade da serial · AT+BAUDRATE',
                    items: [
                      for (final b in _bauds)
                        MapEntry(b, b == 115200 ? '115200 (fábrica deste firmware)' : (b == 9600 ? '9600 (fábrica do original)' : '$b')),
                    ],
                    value: _v['baud'] as int?,
                    onChanged: (v) => setState(() => _v['baud'] = v),
                    helper: 'Vale também para a serial transparente. Pela USB o app troca junto; '
                        'com Bluetooth, configure o módulo BLE na mesma velocidade.',
                  ),
                ],
              ),
              DevFooter(appTitle),
            ],
          ),
        ),
        SaveBar(
          dirty: dirty,
          saving: _saving,
          onSave: _save,
          onReboot: _discard,
          rebootLabel: 'Descartar',
          onRead: _readFromNode,
        ),
      ],
    );
  }
}
