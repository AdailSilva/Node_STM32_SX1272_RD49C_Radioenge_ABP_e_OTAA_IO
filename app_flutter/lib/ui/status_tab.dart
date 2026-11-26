// Status do nó (comando jstatus, a cada 5 s).
import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show Clipboard, ClipboardData; // só o necessário (services.dart também exporta um appFlavor)

import '../core/app_info.dart';
import '../core/node_link.dart';
import 'theme.dart';
import 'widgets.dart';

class StatusTab extends StatelessWidget {
  const StatusTab({super.key, required this.link});

  final NodeLink link;

  static String clockName(String c) {
    switch (c) {
      case 'lse':
        return 'LSE (cristal)';
      case 'lsi_hse':
        return 'LSI (ref. HSE)';
      case 'lsi_hsi':
        return 'LSI (ref. HSI16)';
      default:
        return '-';
    }
  }

  static String beaconName(String b) {
    switch (b) {
      case 'A':
        return 'adquirindo';
      case 'R':
        return 'recebendo';
      case 'N':
        return 'perdeu o último';
      case 'L':
        return 'perdido';
      default:
        return '-';
    }
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: link,
      builder: (context, _) {
        final s = link.status;
        final joined = bv(s['joined']);
        // maccls: a classe que a LoRaMac-node usa de fato (firmwares antigos só mandam cls)
        final cls = sv(s['maccls']).isNotEmpty ? sv(s['maccls']) : sv(s['cls']);
        final cfgCls = sv(s['cfgcls']);
        final lp = iv(s['lowpower']);
        final ldata = sv(s['ldata']);
        final ping = iv(s['ping']);
        final ppm = iv(s['ppm']);
        final clk = sv(s['clk']);
        final noLse = clk.isNotEmpty && clk != 'lse';

        return RefreshIndicator(
          onRefresh: link.refreshStatus,
          child: ListView(
            padding: const EdgeInsets.fromLTRB(14, 14, 14, 24),
            children: [
              if (s.isEmpty)
                const Padding(padding: EdgeInsets.all(24), child: Center(child: CircularProgressIndicator())),
              if (s.isNotEmpty) ...[
                if (cfgCls == 'B' && noLse)
                  const StatusBanner(
                    'Classe B sem cristal de 32,768 kHz: o relógio do módulo é o LSI calibrado e '
                    'os ping slots podem ser perdidos.',
                    level: Level.warn,
                  ),
                SectionCard(
                  title: 'LoRaWAN',
                  subtitle: 'Atualiza a cada 5 s. Puxe para baixo para atualizar agora.',
                  children: [
                    KvGrid([
                      KvItem('Sessão', joined ? 'ativa' : (bv(s['joining']) ? 'JOIN…' : 'sem sessão'),
                          link.otaa ? '${iv(s['joins'])} JOIN(s) · DevNonce ${iv(s['devnonce'])}' : 'ABP'),
                      KvItem.copyable('DevAddr', joined ? sv(s['devaddr']) : '-'),
                      KvItem('Classe', cls.isEmpty ? '-' : cls,
                          cls == cfgCls ? 'em uso no nó · gravada: $cfgCls' : 'gravada: $cfgCls (trocando…)'),
                      if (cls == 'B' || cfgCls == 'B')
                        KvItem('Beacon', beaconName(sv(s['beacon'])),
                            '${iv(s['brx'])} recebidos · ${iv(s['bmiss'])} perdidos · ping ${1 << ping} s'),
                      KvItem('Uplinks', '${iv(s['upl'])}',
                          '${iv(s['uperr'])} erros · ${iv(s['acks'])} ACKs · FCnt ${iv(s['fcntup'])}'),
                      KvItem('Próximo envio', joined ? (iv(s['next']) > 0 ? 'em ${iv(s['next'])} s' : 'em instantes') : '-',
                          'DR${iv(s['dr'])}'
                          '${iv(s['freq']) > 0 ? ' · ${iv(s['txpow'])} dBm · ${(iv(s['freq']) / 1e6).toStringAsFixed(1)} MHz' : ''}'
                          ' · a cada ${iv(s['interval'])} s'),
                      KvItem('Downlinks', '${iv(s['dnl'])}', 'FCntDown ${iv(s['fcntdn'])}'),
                      if (iv(s['dnl']) > 0)
                        KvItem('Último downlink', sv(s['lslot']),
                            'porta ${iv(s['lport'])} · ${ldata.isEmpty ? 'sem dados' : ldata} · '
                            '${iv(s['rssi'])} dBm · SNR ${iv(s['snr'])} dB'),
                    ]),
                  ],
                ),
                SectionCard(
                  title: 'Módulo',
                  children: [
                    KvGrid([
                      KvItem('Ligado há', fmtUptime(iv(s['up']))),
                      KvItem('Hora GPS', '${iv(s['gps'])} s', bv(s['synced']) ? 'sincronizada' : 'não sincronizada'),
                      KvItem('Relógio', clockName(clk), '${iv(s['clkhz'])} Hz · ~$ppm ppm'),
                      KvItem('VDD', '${iv(s['vdd'])} mV',
                          '${bv(s['ext']) ? 'alimentação externa' : 'bateria'}${link.io != null ? ' · AT+BAT ${iv(link.io!['bat'])}' : ''}'),
                      KvItem('Energia', lp == 0 ? 'sem STOP' : (lp == 1 ? 'STOP' : 'STOP econ.'),
                          lp == 1 ? 'console sempre pronto' : (lp == 2 ? '1º caractere só acorda' : 'para depurar')),
                      KvItem('Firmware', sv(link.info['ver']), sv(link.info['stack'])),
                    ]),
                  ],
                ),
                if (link.keys.containsKey('valid')) SessionKeysCard(link: link),
                if (bv(s['dirty']))
                  const Hint('Há alterações feitas pelo console sem "save".'),
              ],
              DevFooter(sv(link.info['fw']).isEmpty ? appTitle : sv(link.info['fw'])),
            ],
          ),
        );
      },
    );
  }
}

/// Chaves da sessão LoRaWAN (comando jkeys), como na página WEB e nos nós ESP32:
/// ocultas até tocar em "Mostrar"; cada uma pode ser copiada.
class SessionKeysCard extends StatefulWidget {
  const SessionKeysCard({super.key, required this.link});

  final NodeLink link;

  @override
  State<SessionKeysCard> createState() => _SessionKeysCardState();
}

class _SessionKeysCardState extends State<SessionKeysCard> {
  bool _show = false;

  static String _dur(int t) {
    if (t < 60) return '$t s';
    if (t < 3600) return '${t ~/ 60} min ${t % 60} s';
    if (t < 86400) return '${t ~/ 3600} h ${(t ~/ 60) % 60} min';
    return '${t ~/ 86400} d ${(t ~/ 3600) % 24} h';
  }

  Future<void> _copy(String value, String done) async {
    await Clipboard.setData(ClipboardData(text: value));
    if (mounted) snack(context, done);
  }

  @override
  Widget build(BuildContext context) {
    final k = widget.link.keys;
    final ok = bv(k['valid']);
    final age = _dur(iv(k['age']));
    final String sub;
    if (widget.link.otaa) {
      sub = !ok
          ? 'Sem sessão: são derivadas quando o JOIN for aceito (e renovadas a cada novo JOIN).'
          : bv(k['restored'])
              ? 'Restauradas da EEPROM no boot, há $age · renovadas a cada novo JOIN.'
              : 'Derivadas da AppKey no JOIN aceito há $age · renovadas a cada novo JOIN.';
    } else {
      sub = ok
          ? 'Configuradas manualmente em Ativação ABP · fixas até você alterá-las.'
          : 'Sessão ABP ainda não ativada.';
    }
    final cs = Theme.of(context).colorScheme;
    const hidden = '•••••••••••••••• (oculta)';

    Widget row(String label, String shown, String raw, String done) {
      return Padding(
        padding: const EdgeInsets.only(top: 6),
        child: Row(
          children: [
            SizedBox(
              width: 72,
              child: Text(label, style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant)),
            ),
            Expanded(
              child: SelectableText(
                shown,
                style: const TextStyle(fontFamily: monoFont, fontSize: 14, fontWeight: FontWeight.w600),
              ),
            ),
            TextButton(
              onPressed: ok && raw.isNotEmpty ? () => _copy(raw, done) : null,
              child: const Text('Copiar'),
            ),
          ],
        ),
      );
    }

    final da = sv(k['devaddr']), nwk = sv(k['nwk']), app = sv(k['app']);
    return SectionCard(
      title: 'Chaves de sessão',
      subtitle: sub,
      children: [
        Align(
          alignment: Alignment.centerLeft,
          child: OutlinedButton(
            onPressed: ok ? () => setState(() => _show = !_show) : null,
            child: Text(_show ? 'Ocultar' : 'Mostrar'),
          ),
        ),
        row('DevAddr', ok ? da : '-', da, 'DevAddr copiado.'),
        row('NwkSKey', !ok ? '-' : (_show ? nwk : hidden), nwk, 'NwkSKey copiada.'),
        row('AppSKey', !ok ? '-' : (_show ? app : hidden), app, 'AppSKey copiada.'),
      ],
    );
  }
}
