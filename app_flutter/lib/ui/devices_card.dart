// Relés e medidores PZEM-004T v3 (como a seção "Relés e PZEM" da página WEB):
// estado ao vivo (jdev -> "@DV {json}"), ligar/desligar/pulso (AT+RELE / AT+RELEPULSO),
// pinos dos relés (AT+RELECFG), endereços Modbus dos medidores (AT+PZEMCFG) e leitura
// na hora ("pzem ler").
import 'package:flutter/material.dart';

import '../core/node_link.dart';
import 'theme.dart';
import 'widgets.dart';

const _bootNames = ['desconhecido', 'energização', 'software', 'watchdog', 'queda de tensão', 'pino de reset'];

class DevicesCard extends StatefulWidget {
  const DevicesCard({super.key, required this.link});

  final NodeLink link;

  @override
  State<DevicesCard> createState() => _DevicesCardState();
}

class _DevicesCardState extends State<DevicesCard> {
  NodeLink get l => widget.link;
  bool _busy = false;

  // edição dos pinos dos relés (null = mostra o que está no nó)
  List<List<int>>? _relEdit; // [gpio, ativoBaixo]
  final _pzAddr = TextEditingController();
  bool _pzTouched = false;

  @override
  void dispose() {
    _pzAddr.dispose();
    super.dispose();
  }

  Map<String, dynamic> get _d => l.dev ?? const {};

  /// "4,5:1,0" -> [[4,1],[5,0]]; "0" -> []
  List<List<int>> _parseCfg(String s) {
    if (s.isEmpty || s == '0') return [];
    final parts = s.split(':');
    final pins = parts[0].split(',').map(iv).toList();
    final al = parts.length > 1 ? parts[1].split(',').map(iv).toList() : <int>[];
    return [for (var i = 0; i < pins.length; i++) [pins[i], i < al.length ? al[i] : 0]];
  }

  Future<void> _run(String cmd, String okText, {bool readBack = true}) async {
    if (_busy) return;
    setState(() => _busy = true);
    final r = await l.cmd(cmd);
    if (readBack) {
      await l.refreshDev();
      await l.refreshIo();
    }
    if (!mounted) return;
    setState(() => _busy = false);
    snack(context, r.ok ? okText : 'Erro: ${r.message}');
  }

  Future<void> _applyRelays() async {
    final e = _relEdit ?? [];
    final pins = e.map((x) => x[0]).toList();
    if (pins.toSet().length != pins.length) {
      snack(context, 'Cada relé precisa de um GPIO diferente.');
      return;
    }
    final cmd = e.isEmpty ? 'AT+RELECFG=0' : 'AT+RELECFG=${pins.join(',')}:${e.map((x) => x[1]).join(',')}';
    await _run(cmd, e.isEmpty ? 'Relés removidos (gravado).' : '${e.length} relé(s) configurado(s) (gravado).');
    l.refreshConfig();
    if (mounted) setState(() => _relEdit = null);
  }

  Future<void> _applyPzem(bool off) async {
    final a = _pzAddr.text.trim().toLowerCase();
    if (!off && !RegExp(r'^[0-9a-f]{1,2}(,[0-9a-f]{1,2}){0,3}$').hasMatch(a)) {
      snack(context, 'Endereços em hexadecimal separados por vírgula (ex.: f8 ou 01,02).');
      return;
    }
    await _run(off ? 'AT+PZEMCFG=0' : 'AT+PZEMCFG=$a',
        off ? 'Modo PZEM desligado (GPIO0/GPIO1 livres).' : 'PZEM em GPIO0/GPIO1 (gravado).');
    l.refreshConfig();
    if (mounted) setState(() => _pzTouched = false);
  }

  Future<void> _readPzem() async {
    if (_busy) return;
    setState(() => _busy = true);
    final r = await l.cmd('pzem ler');
    // cada medidor responde em até 0,5 s
    await Future<void>.delayed(Duration(milliseconds: 600 * (iv(_d['pq']) + 1)));
    await l.refreshDev();
    if (!mounted) return;
    setState(() => _busy = false);
    if (!r.ok) snack(context, 'Erro: ${r.message}');
  }

  Future<void> _pulseDialog(int ch) async {
    final ms = TextEditingController(text: '1000');
    var state = 1;
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => StatefulBuilder(
        builder: (c, set) => AlertDialog(
          title: Text('Pulso no relé $ch'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SegChoice(
                label: 'Estado durante o pulso',
                options: const [SegOption('1', 'Ligado'), SegOption('0', 'Desligado')],
                value: '$state',
                showHint: false,
                onChanged: (v) => set(() => state = iv(v)),
              ),
              TextFieldX(controller: ms, label: 'Duração (ms)', hint: '100 a 86400000', keyboard: TextInputType.number,
                  mono: false, maxLength: 8),
              const Hint('Depois volta ao estado anterior e o nó manda uma telemetria com os relés.'),
            ],
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancelar')),
            FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Pulsar')),
          ],
        ),
      ),
    );
    final v = int.tryParse(ms.text.trim()) ?? 0;
    ms.dispose();
    if (ok != true) return;
    if (v < 100 || v > 86400000) {
      if (mounted) snack(context, 'Duração: 100 a 86400000 ms.');
      return;
    }
    await _run('AT+RELEPULSO=$ch:$state:$v', 'Pulso de $v ms no relé $ch.');
  }

  String _fmt(num v, int dec) => v.toStringAsFixed(dec).replaceAll('.', ',');

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: l,
      builder: (context, _) {
        if (l.dev == null) {
          return const SectionCard(
            title: 'Relés e PZEM',
            children: [Hint('Carregando… (firmware sem o comando jdev: atualize o nó para ver os relés e o PZEM aqui).', top: 0)],
          );
        }
        final cs = Theme.of(context).colorScheme;
        final d = _d;
        final rq = iv(d['rq']), map = iv(d['rmap']), pul = iv(d['rpul']);
        final cfg = _parseCfg(sv(d['rcfg']));
        final edit = _relEdit ?? cfg;
        final pq = iv(d['pq']);
        final padr = (d['padr'] is List) ? (d['padr'] as List).map(iv).toList() : <int>[];
        final pz = (d['pz'] is List) ? (d['pz'] as List) : const [];
        if (!_pzTouched) {
          _pzAddr.text = padr.isEmpty ? '' : padr.map((a) => a.toRadixString(16).padLeft(2, '0')).join(',');
        }
        final boot = iv(d['boot']);
        final free = [for (var g = 0; g < 10; g++) if (!(pq > 0 && g <= 1)) g];
        return AbsorbPointer(
          absorbing: _busy,
          child: SectionCard(
            title: 'Relés e PZEM',
            subtitle: 'Relés em GPIOs do módulo e medidores de energia PZEM-004T v3 (Modbus em GPIO0/GPIO1). '
                'Tudo vale na hora e fica gravado.',
            tag: 'aplica na hora e grava',
            children: [
              if (_busy) const LinearProgressIndicator(),
              KvGrid([
                KvItem('Relés', '$rq', rq == 0 ? 'AT+RELECFG' : 'mapa ${map.toRadixString(16).padLeft(4, '0').toUpperCase()}'),
                KvItem('PZEM-004T', '$pq', pq == 0 ? 'modo PZEM desligado' : (bv(d['pon']) ? 'GPIO0/GPIO1 · 9600 8N1' : 'serial parada')),
                KvItem('Uplink periódico', iv(d['umode']) == 1 ? 'telemetria V1' : 'payload fixo',
                    iv(d['umode']) == 1 ? 'porta 20 (TLV)' : 'FPort configurada'),
                KvItem('Protocolo V1', '${iv(d['v1c'])} cmd', '${iv(d['v1u'])} uplink(s) · boot: '
                    '${boot >= 0 && boot < _bootNames.length ? _bootNames[boot] : boot}'),
              ]),
              const SizedBox(height: 12),
              const FieldLabel('Relés · AT+RELE'),
              if (rq == 0) const Hint('Nenhum relé configurado.', top: 0),
              for (var ch = 0; ch < rq && ch < cfg.length; ch++)
                Container(
                  margin: const EdgeInsets.only(top: 6),
                  padding: const EdgeInsets.fromLTRB(12, 4, 8, 4),
                  decoration: BoxDecoration(
                      borderRadius: BorderRadius.circular(10), border: Border.all(color: cs.outlineVariant)),
                  child: Row(children: [
                    Expanded(
                      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                        Text('Relé $ch', style: const TextStyle(fontWeight: FontWeight.w600)),
                        Text('GPIO${cfg[ch][0]} · liga em nível ${cfg[ch][1] == 1 ? 0 : 1}'
                            '${(pul >> ch) & 1 == 1 ? ' · em pulso' : ''}',
                            style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant)),
                      ]),
                    ),
                    TextButton(onPressed: () => _pulseDialog(ch), child: const Text('Pulso')),
                    Switch(
                      value: (map >> ch) & 1 == 1,
                      onChanged: (v) => _run('AT+RELE=$ch:${v ? 1 : 0}', 'Relé $ch ${v ? 'ligado' : 'desligado'}.'),
                    ),
                  ]),
                ),
              if (rq > 0) ...[
                const SizedBox(height: 8),
                Wrap(spacing: 8, runSpacing: 8, children: [
                  OutlinedButton(onPressed: () => _run('AT+RELE=255:1', 'Todos ligados.'), child: const Text('Ligar todos')),
                  OutlinedButton(onPressed: () => _run('AT+RELE=255:0', 'Todos desligados.'), child: const Text('Desligar todos')),
                ]),
              ],
              const SizedBox(height: 14),
              const FieldLabel('Pinos dos relés · AT+RELECFG'),
              for (var i = 0; i < edit.length; i++)
                Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Expanded(
                    child: DropField<int>(
                      label: 'Relé $i',
                      items: [for (final g in free) MapEntry(g, 'GPIO$g')],
                      value: edit[i][0],
                      onChanged: (v) => setState(() {
                        _relEdit = [for (final x in edit) [...x]];
                        _relEdit![i][0] = v;
                      }),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: DropField<int>(
                      label: 'Liga com',
                      items: const [MapEntry(1, 'nível 0 (ativo-baixo)'), MapEntry(0, 'nível 1')],
                      value: edit[i][1],
                      onChanged: (v) => setState(() {
                        _relEdit = [for (final x in edit) [...x]];
                        _relEdit![i][1] = v;
                      }),
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.only(top: 26),
                    child: IconButton(
                      tooltip: 'Remover',
                      icon: const Icon(Icons.close),
                      onPressed: () => setState(() {
                        _relEdit = [for (final x in edit) [...x]]..removeAt(i);
                      }),
                    ),
                  ),
                ]),
              Wrap(spacing: 8, runSpacing: 8, children: [
                OutlinedButton.icon(
                  onPressed: edit.length >= 8
                      ? null
                      : () => setState(() {
                            final used = edit.map((x) => x[0]).toSet();
                            final g = free.firstWhere((x) => !used.contains(x), orElse: () => -1);
                            if (g >= 0) _relEdit = [for (final x in edit) [...x], [g, 1]];
                          }),
                  icon: const Icon(Icons.add),
                  label: const Text('Adicionar relé'),
                ),
                FilledButton(onPressed: _relEdit == null ? null : _applyRelays, child: const Text('Aplicar')),
                if (_relEdit != null) TextButton(onPressed: () => setState(() => _relEdit = null), child: const Text('Descartar')),
              ]),
              const Hint('O pino de um relé vira saída push-pull e fica só dele (AT+GPIOC e AT+WPIN nele dão erro). '
                  'Os estados ficam na EEPROM e voltam no boot. GPIO0/GPIO1 só sem PZEM e sem serial transparente.'),
              const SizedBox(height: 14),
              const FieldLabel('PZEM-004T v3 · AT+PZEMCFG'),
              TextFieldX(
                controller: _pzAddr,
                label: 'Endereços Modbus (hex)',
                hint: 'f8 (um medidor) ou 01,02',
                upper: false,
                maxLength: 11,
                onChanged: (_) => setState(() => _pzTouched = true),
              ),
              const SizedBox(height: 8),
              Wrap(spacing: 8, runSpacing: 8, children: [
                FilledButton(onPressed: () => _applyPzem(false), child: const Text('Aplicar')),
                OutlinedButton(onPressed: pq == 0 ? null : () => _applyPzem(true), child: const Text('Desligar PZEM')),
                OutlinedButton.icon(
                    onPressed: pq == 0 ? null : _readPzem, icon: const Icon(Icons.refresh), label: const Text('Ler agora')),
              ]),
              const Hint('O PZEM usa GPIO0 (RX) e GPIO1 (TX), os mesmos pinos da serial transparente: ligar um desliga o '
                  'outro. f8 é o endereço genérico (um medidor só no barramento). PZEM em 5 V: use conversor de nível.'),
              for (var ch = 0; ch < pq && ch < pz.length; ch++) ...[
                const SizedBox(height: 10),
                Text(
                    'PZEM $ch · endereço ${ch < padr.length ? padr[ch].toRadixString(16).padLeft(2, '0').toUpperCase() : '-'}'
                    '${pz[ch] is Map && bv((pz[ch] as Map)['ok']) ? ' · há ${iv((pz[ch] as Map)['age'])} s' : ''}',
                    style: const TextStyle(fontWeight: FontWeight.w600)),
                const SizedBox(height: 6),
                if (pz[ch] is Map && bv((pz[ch] as Map)['ok']))
                  KvGrid(() {
                    final m = pz[ch] as Map;
                    return [
                      KvItem('Tensão', '${_fmt(iv(m['v']) / 10, 1)} V'),
                      KvItem('Corrente', '${_fmt(iv(m['i']) / 1000, 3)} A'),
                      KvItem('Potência', '${_fmt(iv(m['p']) / 10, 1)} W'),
                      KvItem('Energia', '${iv(m['e'])} Wh'),
                      KvItem('Frequência', '${_fmt(iv(m['f']) / 10, 1)} Hz'),
                      KvItem('Fator de potência', _fmt(iv(m['pf']) / 100, 2)),
                      KvItem('Alarme de potência', bv(m['al']) ? 'sim' : 'não'),
                    ];
                  }())
                else
                  Hint(pz[ch] is Map && bv((pz[ch] as Map)['fail'])
                      ? 'Sem resposta na última leitura (FALHA_PERIFERICO): confira a fiação, o 5 V do PZEM e o endereço.'
                      : 'Sem leitura ainda: toque em Ler agora.', top: 0,
                      color: pz[ch] is Map && bv((pz[ch] as Map)['fail']) ? AppColors.warnOf(context) : null),
              ],
            ],
          ),
        );
      },
    );
  }
}
