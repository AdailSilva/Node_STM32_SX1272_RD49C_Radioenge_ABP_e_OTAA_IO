// GPIOs do módulo Radioenge (como a seção GPIOs da página WEB): os 10 pinos com os
// modos do firmware original (AT+GPIOC), as saídas (AT+WPIN), as leituras digitais e
// analógicas (jio), a bateria (AT+BAT) e a serial transparente.
import 'package:flutter/material.dart';

import '../core/node_link.dart';
import 'theme.dart';
import 'widgets.dart';

/// Pino do conector e porta do STM32 de cada GPIO (node_io.h).
const _gpPins = [6, 7, 9, 10, 11, 12, 13, 14, 15, 16];
const _gpMcu = ['PA3', 'PA2', 'PA15', 'PA10', 'PA9', 'PA8', 'PB15', 'PA0', 'PA1', 'PB11'];
const _gpAdc = [0, 1, 7, 8];
const _gpIrq = [2, 4, 5, 6, 9];
const _gpAf = [0, 1];
const gpioModeNames = [
  'Entrada digital', 'Saída push-pull', 'Saída open-drain', 'Serial transparente', 'Serial transp. open-drain',
  'Entrada analógica', 'Interrupção na subida', 'Interrupção na descida', 'Interrupção nas duas bordas',
];

List<int> _modesOf(int g) => [
      0, 1, 2,
      if (_gpAf.contains(g)) ...[3, 4],
      if (_gpAdc.contains(g)) 5,
      if (_gpIrq.contains(g)) ...[6, 7, 8],
    ];

class GpioCard extends StatefulWidget {
  const GpioCard({super.key, required this.link});

  final NodeLink link;

  @override
  State<GpioCard> createState() => _GpioCardState();
}

class _GpioCardState extends State<GpioCard> {
  NodeLink get l => widget.link;

  // escolhas ainda não aplicadas: GPIO -> (modo, pull)
  final Map<int, List<int>> _edit = {};
  bool _busy = false;

  List<int> _node(int g) {
    final list = l.io?['g'];
    if (list is List && g < list.length && list[g] is List && (list[g] as List).length >= 3) {
      final r = list[g] as List;
      return [iv(r[0]), iv(r[1]), iv(r[2])];
    }
    return [-1, 0, -1];
  }

  Future<void> _apply(int g) async {
    final e = _edit[g];
    if (e == null || e[0] < 0) return;
    setState(() => _busy = true);
    final r = await l.cmd('AT+GPIOC=$g:${e[0]}:${e[1]}');
    await l.refreshIo();
    l.refreshConfig();
    if (!mounted) return;
    setState(() {
      _busy = false;
      _edit.remove(g);
    });
    snack(
        context,
        r.ok
            ? 'GPIO$g: ${gpioModeNames[e[0]]} (gravado).'
            : 'GPIO$g: ${r.message == 'AT PARAM ERROR' ? 'modo não permitido neste pino'
                '${g == 2 || g == 6 ? ' (GPIO2 e GPIO6 não podem ser interrupção juntos)' : ''}' : r.message}.');
  }

  Future<void> _write(int g, int lvl) async {
    final r = await l.cmd('AT+WPIN=$g:$lvl');
    await l.refreshIo();
    if (mounted && !r.ok) snack(context, 'GPIO$g: ${r.message}');
  }

  Widget _value(BuildContext context, int g, List<int> n) {
    final cs = Theme.of(context).colorScheme;
    final m = n[0], v = n[2];
    final small = TextStyle(fontSize: 12, color: cs.onSurfaceVariant);
    Widget dot(bool on) => Container(
          width: 11,
          height: 11,
          decoration: BoxDecoration(shape: BoxShape.circle, color: on ? AppColors.okOf(context) : cs.outlineVariant),
        );
    if ((iv(l.io?['rel']) >> g) & 1 == 1) {
      return Row(children: [
        dot(v == 1),
        const SizedBox(width: 8),
        Flexible(child: Text('relé (AT+RELE; nível $v no pino)', style: small)),
      ]);
    }
    if (m == 1 || m == 2) {
      return Row(children: [
        dot(v == 1),
        const SizedBox(width: 8),
        SegmentedButton<int>(
          segments: const [ButtonSegment(value: 0, label: Text('0')), ButtonSegment(value: 1, label: Text('1'))],
          selected: {v == 1 ? 1 : 0},
          showSelectedIcon: false,
          onSelectionChanged: (s) => _write(g, s.first),
        ),
        const SizedBox(width: 8),
        Text('AT+WPIN', style: small),
      ]);
    }
    if (m == 5) {
      if (v < 0) return const Text('-');
      final vdd = iv(l.status['vdd']) > 0 ? iv(l.status['vdd']) : 3300;
      final volts = (v * vdd / 4095 / 1000).toStringAsFixed(2).replaceAll('.', ',');
      return Row(children: [
        Text('$v', style: const TextStyle(fontFamily: monoFont, fontWeight: FontWeight.w600)),
        const SizedBox(width: 8),
        Expanded(child: LinearProgressIndicator(value: v / 4095, minHeight: 6)),
        const SizedBox(width: 8),
        Text('~$volts V', style: small),
      ]);
    }
    if (m == 0 || m >= 6) {
      if (v < 0) return const Text('-');
      return Row(children: [
        dot(v == 1),
        const SizedBox(width: 8),
        Text('$v', style: const TextStyle(fontFamily: monoFont, fontWeight: FontWeight.w600)),
        if (m >= 6) ...[const SizedBox(width: 8), Text('uplink na porta 2', style: small)],
      ]);
    }
    if (m == 3 || m == 4) return Text('${g == 0 ? 'RX' : 'TX'} da serial transparente', style: small);
    if (g <= 1 && bv(l.io?['pz'])) return Text('${g == 0 ? 'RX' : 'TX'} do PZEM-004T (Modbus 9600 8N1)', style: small);
    return Text('não configurado (alta impedância)', style: small);
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: l,
      builder: (context, _) {
        final io = l.io;
        if (io == null) {
          return const SectionCard(
            title: 'GPIOs',
            children: [Hint('Carregando… (firmware sem o comando jio: atualize o nó para ver os GPIOs aqui).', top: 0)],
          );
        }
        final con2 = bv(io['con2']);
        final tr = bv(io['tr']);
        final bat = iv(io['bat']);
        final nodes = [for (var g = 0; g < 10; g++) _node(g)];
        final inputs = [for (var g = 0; g < 10; g++) if ((nodes[g][0] == 0 || nodes[g][0] >= 6) && nodes[g][2] == 1) 'GPIO$g'];
        int count(List<int> modes) => nodes.where((n) => modes.contains(n[0])).length;
        final lvl = bat == 255
            ? 'sem medida'
            : bat == 0
                ? 'externa'
                : '~${(1.8 + (bat - 1) * 1.8 / 253).toStringAsFixed(2).replaceAll('.', ',')} V';
        return SectionCard(
          title: 'GPIOs',
          subtitle: 'Os 10 pinos do módulo com os modos do firmware original. Aplicar configura o pino na hora '
              'e grava o modo na EEPROM (AT+GPIOC).',
          tag: 'aplica na hora e grava',
          children: [
            if (bv(io['pz']))
              const StatusBanner(
                'GPIO0 (RX) e GPIO1 (TX) estão com o PZEM-004T (Modbus). Configurar um deles aqui desliga o modo PZEM.',
                level: Level.ok,
              ),
            if (tr)
              const StatusBanner(
                'Serial transparente ativa em GPIO0 (RX) e GPIO1 (TX): os bytes recebidos vão num uplink na porta 4, '
                'e o downlink na porta 4 sai por ela.',
                level: Level.ok,
              ),
            KvGrid([
              KvItem('Bateria · AT+BAT', '$bat', '1 = 1,8 V … 254 = 3,6 V · $lvl'),
              KvItem('Entradas em 1', inputs.isEmpty ? 'nenhuma' : inputs.join(' '), 'digitais e interrupções'),
              KvItem('Configurados', '${nodes.where((n) => n[0] >= 0).length} de 10',
                  '${count([1, 2])} saída(s) · ${count([5])} analógica(s) · ${count([6, 7, 8])} interrupção(ões)'),
              KvItem('Serial transparente', tr ? 'ativa' : 'desligada',
                  tr ? 'porta 4 · ${sv(l.config['baud'])} bps' : 'AT+GPIOC=0:3:0 liga'),
            ]),
            const SizedBox(height: 8),
            for (var g = 0; g < 10; g++)
              if (!(con2 && g <= 1)) _row(context, g, nodes[g]),
            const Hint('Analógicas só em GPIO0, 1, 7 e 8; interrupção em GPIO2, 4, 5, 6 e 9 (GPIO2 e GPIO6 dividem a '
                'mesma linha: um por vez; GPIO1 fica com o rádio). A serial transparente usa GPIO0 (RX) e GPIO1 (TX) '
                'juntos, na velocidade da serial do console. As saídas voltam a 0 quando o nó reinicia.'),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: _busy ? null : l.refreshIo,
              icon: const Icon(Icons.refresh),
              label: const Text('Ler agora'),
            ),
          ],
        );
      },
    );
  }

  Widget _row(BuildContext context, int g, List<int> n) {
    final cs = Theme.of(context).colorScheme;
    final e = _edit[g] ?? [n[0], n[1]];
    final changed = _edit.containsKey(g) && (e[0] != n[0] || e[1] != n[1]) && e[0] >= 0;
    return Container(
      margin: const EdgeInsets.only(top: 8),
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: changed ? AppColors.warnOf(context) : cs.outlineVariant),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            Text('GPIO$g', style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 15)),
            const SizedBox(width: 8),
            Flexible(
              child: Text('pino ${_gpPins[g]} · ${_gpMcu[g]}${_gpAdc.contains(g) ? ' · ADC' : ''}',
                  style: TextStyle(fontFamily: monoFont, fontSize: 12, color: cs.onSurfaceVariant)),
            ),
          ]),
          const SizedBox(height: 8),
          if ((iv(l.io?['rel']) >> g) & 1 == 1)
            const Hint('Pino de relé: mude em Relés e PZEM (AT+RELECFG).', top: 0)
          else ...[
          DropField<int>(
            label: 'Modo · AT+GPIOC',
            items: [
              if (e[0] < 0) const MapEntry(-1, 'Não configurado'),
              for (final m in _modesOf(g)) MapEntry(m, '$m · ${gpioModeNames[m]}'),
            ],
            value: e[0],
            onChanged: (v) => setState(() => _edit[g] = [v, e[1]]),
          ),
          Row(children: [
            Expanded(
              child: DropField<int>(
                label: 'Pull',
                items: const [MapEntry(0, 'Sem pull'), MapEntry(1, 'Pull-up'), MapEntry(2, 'Pull-down')],
                value: e[1],
                onChanged: (v) => setState(() => _edit[g] = [e[0], v]),
              ),
            ),
            const SizedBox(width: 12),
            Padding(
              padding: const EdgeInsets.only(top: 10),
              child: FilledButton(onPressed: changed && !_busy ? () => _apply(g) : null, child: const Text('Aplicar')),
            ),
          ]),
          ],
          _value(context, g, n),
        ],
      ),
    );
  }
}
