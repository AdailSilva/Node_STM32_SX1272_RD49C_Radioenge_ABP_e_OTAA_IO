// Aba Diagnósticos: Ping, Traceroute e Nslookup feitos PELO CELULAR (o módulo
// Radioenge não tem rede IP) e o Autoteste do nó (comando jdiag do firmware). Mesma
// disposição da aba Diagnósticos dos apps dos nós ESP32.
import 'package:flutter/material.dart';

import '../core/net_diag.dart';
import '../core/node_link.dart';
import 'term_view.dart';
import 'theme.dart';
import 'widgets.dart';

/// Destino padrão dos testes (o mesmo dos nós ESP32).
const String diagDefaultHost = 'chirpstack-v4.adailsilva.com.br';

class DiagTab extends StatefulWidget {
  const DiagTab({super.key, required this.link});

  final NodeLink link;

  @override
  State<DiagTab> createState() => _DiagTabState();
}

class _DiagTabState extends State<DiagTab> {
  final TextEditingController _host = TextEditingController(text: diagDefaultHost);
  final ScrollController _scroll = ScrollController();
  String _out = 'Escolha o destino e toque num dos testes.';
  String _status = '';
  bool _busy = false;
  DiagCancel? _cancel;

  static final _bad = RegExp(r'\[FALHA\]|não |sem resposta|erro|\*\*\*|!\w?$|\s\*$|100% packet loss|Unreachable|unknown host',
      caseSensitive: false);
  static final _good = RegExp(r'\[ ok \]| (time|tempo)=|^(Address|Endereço):|bytes from');

  NodeLink get l => widget.link;

  @override
  void dispose() {
    _cancel?.cancel();
    _host.dispose();
    _scroll.dispose();
    super.dispose();
  }

  Color? _color(String line) {
    if (line.contains('[AVISO]') || line.startsWith('AVISO:') || line.startsWith('ICMP: não testado')) return AppColors.termWarn;
    if (line.contains('[info]')) return null;
    if (_bad.hasMatch(line)) return AppColors.termErr;
    if (_good.hasMatch(line)) return AppColors.termOk;
    return null;
  }

  void _append(String t) {
    setState(() => _out += t);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
  }

  Future<void> _selfTest() async {
    final sw = Stopwatch()..start();
    final r = await l.runSelfTest();
    sw.stop();
    final d = l.diag;
    if (!mounted) return;
    if (!r.ok || d == null) {
      _append('*** o nó não respondeu ao autoteste: ${r.message} (firmware sem o comando jdiag?)\n');
      return;
    }
    const tag = {'ok': '[ ok ]', 'aviso': '[AVISO]', 'falha': '[FALHA]', 'info': '[info]'};
    final b = StringBuffer('Autoteste do nó · ${sv(l.info['fw'])} v${sv(l.info['ver'])} · ${l.transport.name}\n\n');
    for (final it in (d['items'] as List? ?? const [])) {
      if (it is! Map) continue;
      b.writeln('${(tag[sv(it['l'])] ?? '[info]').padRight(7)} ${'${sv(it['n'])} '.padRight(23, '.')} ${sv(it['v'])}');
    }
    b.writeln('\nResultado: ${iv(d['ok'])} ok · ${iv(d['warn'])} aviso(s) · ${iv(d['fail'])} falha(s) · '
        'resposta do console em ${sw.elapsedMilliseconds} ms');
    _append(b.toString());
  }

  Future<void> _run(String t) async {
    final h = _host.text.trim();
    if (t != 'self' && !diagHostOk(h)) {
      setState(() => _status = 'destino inválido');
      return;
    }
    if (t == 'self' && !(l.connected && l.ready)) {
      setState(() => _status = 'conecte o nó primeiro');
      return;
    }
    FocusScope.of(context).unfocus();
    final c = DiagCancel();
    final sw = Stopwatch()..start();
    setState(() {
      _busy = true;
      _cancel = c;
      _out = '';
      _status = 'executando…';
    });
    try {
      if (t == 'self') {
        await _selfTest();
      } else {
        final s = t == 'ping' ? diagPing(h, c) : t == 'trace' ? diagTrace(h, c) : diagDns(h);
        await for (final chunk in s) {
          if (!mounted || c.cancelled) break;
          _append(chunk);
        }
      }
      if (mounted) {
        setState(() => _status = c.cancelled ? 'interrompido' : 'concluído em ${(sw.elapsedMilliseconds / 1000).toStringAsFixed(1).replaceAll('.', ',')} s');
      }
    } catch (e) {
      if (mounted) setState(() => _status = c.cancelled ? 'interrompido' : 'falhou: $e');
    } finally {
      if (mounted) {
        setState(() {
          _busy = false;
          _cancel = null;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 12, 12, 4),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              TextField(
                controller: _host,
                enabled: !_busy,
                autocorrect: false,
                keyboardType: TextInputType.url,
                style: const TextStyle(fontFamily: monoFont),
                onSubmitted: (_) => _run('ping'),
                decoration: fieldDecoration(context, label: 'Endereço ou nome do host'),
              ),
              const SizedBox(height: 6),
              Wrap(spacing: 6, runSpacing: 4, children: [
                for (final s in const [diagDefaultHost, '8.8.8.8', 'openwrt.org'])
                  ActionChip(
                    visualDensity: VisualDensity.compact,
                    label: Text(s, style: const TextStyle(fontFamily: monoFont, fontSize: 12)),
                    onPressed: _busy ? null : () => setState(() => _host.text = s),
                  ),
              ]),
              const SizedBox(height: 8),
              // três testes de rede dividindo a largura, como nos apps dos nós ESP32
              Row(children: [
                for (final b in const [
                  ['ping', 'Ping', '5 pacotes, 1 por segundo'],
                  ['trace', 'Traceroute', 'Até 15 saltos, 1 tentativa de 1 s por salto'],
                  ['dns', 'Nslookup', 'Resolve o nome pelo DNS deste celular'],
                ]) ...[
                  if (b[0] != 'ping') const SizedBox(width: 6),
                  Expanded(
                    child: Tooltip(
                      message: b[2],
                      child: FilledButton.tonal(
                        style: FilledButton.styleFrom(padding: const EdgeInsets.symmetric(horizontal: 8)),
                        onPressed: _busy ? null : () => _run(b[0]),
                        child: FittedBox(fit: BoxFit.scaleDown, child: Text(b[1], maxLines: 1)),
                      ),
                    ),
                  ),
                ],
              ]),
              const SizedBox(height: 6),
              Row(children: [
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: _busy || !l.connected ? null : () => _run('self'),
                    icon: const Icon(Icons.fact_check_outlined),
                    label: const Text('Autoteste do nó'),
                  ),
                ),
                if (_busy) ...[
                  const SizedBox(width: 6),
                  TextButton.icon(
                    onPressed: () => _cancel?.cancel(),
                    icon: const Icon(Icons.stop),
                    label: const Text('Parar'),
                  ),
                ],
              ]),
              Padding(
                padding: const EdgeInsets.only(top: 6),
                child: Text(
                  'Ping, Traceroute e Nslookup usam a rede deste celular: o módulo Radioenge não tem IP. '
                  'O Autoteste roda no firmware (rádio, EEPROM, sessão, relógio, VDD, RAM).'
                  '${_status.isEmpty ? '' : ' · $_status'}',
                  style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant),
                ),
              ),
            ],
          ),
        ),
        Expanded(
          child: Container(
            width: double.infinity,
            margin: const EdgeInsets.fromLTRB(8, 6, 8, 8),
            padding: const EdgeInsets.all(10),
            decoration: BoxDecoration(color: AppColors.term, borderRadius: BorderRadius.circular(10)),
            child: SelectionArea(
              child: TermLines(lines: _out.split('\n'), controller: _scroll, colorOf: _color),
            ),
          ),
        ),
      ],
    );
  }
}
