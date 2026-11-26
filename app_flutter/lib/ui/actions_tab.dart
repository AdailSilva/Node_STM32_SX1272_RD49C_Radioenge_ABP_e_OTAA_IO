// Aba Manutenção: enviar agora (também por AT+SENDB /
// AT+TXBCFM, com FPort e confirmado), GPIOs (AT+GPIOC / AT+WPIN), relés e medidores
// PZEM-004T (AT+RELECFG / AT+RELE / AT+PZEMCFG, jdev), trocar a classe,
// JOIN, hora do servidor, relógio, reiniciar, exportar/importar a configuração e
// padrão de fábrica.
import 'dart:convert';

import 'package:flutter/material.dart';

import '../core/file_io.dart';
import '../core/app_info.dart';
import '../core/node_link.dart';
import 'devices_card.dart';
import 'gpio_card.dart';
import 'logs_tab.dart' show fileStamp;
import 'widgets.dart';

class ActionsTab extends StatefulWidget {
  const ActionsTab({super.key, required this.link, required this.onImport});

  final NodeLink link;

  /// Preenche a aba Configuração com o arquivo importado; devolve quantos campos.
  final int Function(Map<String, dynamic> json) onImport;

  @override
  State<ActionsTab> createState() => _ActionsTabState();
}

class _ActionsTabState extends State<ActionsTab> {
  final _payload = TextEditingController();
  final _port = TextEditingController();
  final _trials = TextEditingController();
  bool _hex = false;
  bool _cfm = false;
  bool _busy = false;

  NodeLink get l => widget.link;

  @override
  void dispose() {
    _payload.dispose();
    _port.dispose();
    _trials.dispose();
    super.dispose();
  }

  Future<void> _run(String cmd, String okText) async {
    if (_busy) return;
    setState(() => _busy = true);
    final r = await l.cmd(cmd);
    if (mounted) {
      setState(() => _busy = false);
      snack(context, r.ok ? okText : 'Erro: ${r.message}');
    }
    l.refreshStatus();
  }

  Future<void> _sendCustom() async {
    final t = _payload.text;
    if (_port.text.trim().isNotEmpty || _cfm) {
      await _sendAt(t);
      return;
    }
    if (t.isEmpty) {
      await _run('send', 'Uplink do payload configurado na fila.');
      return;
    }
    if (_hex) {
      if (!isHex(t)) {
        snack(context, 'Hexadecimal inválido.');
        return;
      }
      await _run('send hex:${cleanHex(t)}', 'Uplink na fila.');
    } else {
      await _run('send $t', 'Uplink na fila.');
    }
  }

  /// FPort ou confirmado: os comandos do firmware original (AT+SENDB / AT+TXBCFM).
  Future<void> _sendAt(String t) async {
    final fp = _port.text.trim().isNotEmpty ? _port.text.trim() : (sv(l.config['port']).isEmpty ? '1' : sv(l.config['port']));
    final p = int.tryParse(fp) ?? 0;
    if (p < 1 || p > 223) {
      snack(context, 'FPort: 1 a 223.');
      return;
    }
    final def = iv(l.config['nbtrials']).clamp(1, 15).toInt();
    final n = _trials.text.trim().isNotEmpty ? int.tryParse(_trials.text.trim()) ?? 0 : def;
    if (_cfm && (n < 1 || n > 15)) {
      snack(context, 'Tentativas: 1 a 15.');
      return;
    }
    if (_hex && t.isNotEmpty && !isHex(t)) {
      snack(context, 'Hexadecimal inválido.');
      return;
    }
    final data = (t.isNotEmpty ? (_hex ? cleanHex(t) : textToHex(t)) : sv(l.config['payload'])).toLowerCase();
    if (data.isEmpty) {
      snack(context, 'Escreva o payload (o configurado está vazio).');
      return;
    }
    if (_busy) return;
    setState(() => _busy = true);
    final r = await l.cmd(_cfm ? 'AT+TXBCFM=$p:1:$n:$data' : 'AT+SENDB=$p:$data', timeout: const Duration(seconds: 10));
    if (mounted) {
      setState(() => _busy = false);
      snack(context, r.ok
          ? '${r.at.isEmpty ? 'AT TX OK' : r.at} · porta $p${_cfm ? ' · confirmado: AT ACK OK/ERROR aparece nos Logs' : ''}'
          : 'Erro: ${r.message}');
    }
    l.refreshStatus();
  }

  /// Igual à página WEB: a configuração gravada no nó (jconfig), inclusive as chaves
  /// LoRaWAN, num JSON radioenge_<abp|otaa>_<deveui>_<AAAAMMDD>.json.
  Future<void> _export() async {
    final r = await l.cmd('jconfig');
    if (!mounted) return;
    if (!r.ok || l.config.isEmpty) {
      snack(context, 'Não foi possível ler a configuração do nó.');
      return;
    }
    final i = l.info;
    final id = sv(l.config['deveui']).isNotEmpty ? sv(l.config['deveui']) : sv(i['deveui']);
    final j = <String, dynamic>{
      '_mode': l.modeKey,
      '_fw': sv(i['fw']),
      '_ver': sv(i['ver']),
      '_stack': sv(i['stack']),
      '_mcu': sv(i['mcu']),
      '_radio': sv(i['radio']),
      '_region': sv(i['region']),
      '_deveui': id,
      '_exported': DateTime.now().toUtc().toIso8601String(),
      ...l.config,
    }..remove('dirty');
    final name = 'radioenge_${l.modeKey}_${id.isEmpty ? 'node' : id}_${fileStamp().substring(0, 8)}.json'.toLowerCase();
    try {
      final path = await FileIO.saveText(name, const JsonEncoder.withIndent('  ').convert(j));
      if (mounted && path != null) snack(context, 'Salvo: $name (contém as chaves LoRaWAN: guarde com cuidado)');
    } catch (e) {
      if (mounted) snack(context, 'Não foi possível salvar: $e');
    }
  }

  /// Só preenche a Configuração: o usuário revisa e salva.
  Future<void> _import() async {
    final text = await FileIO.pickText(extensions: const ['json']);
    if (text == null || !mounted) return;
    dynamic j;
    try {
      j = jsonDecode(text);
    } catch (_) {
      j = null;
    }
    if (j is! Map<String, dynamic>) {
      snack(context, 'Arquivo JSON inválido.');
      return;
    }
    // Arquivos exportados pelos antigos firmwares separados (só ABP ou só OTAA) não têm
    // o "njm": a ativação vem do "_mode", para o nó continuar na mesma ativação.
    final mode = sv(j['_mode']);
    if (!j.containsKey('njm') && (mode == 'abp' || mode == 'otaa')) j['njm'] = mode == 'otaa' ? 1 : 0;
    final fileMode = j.containsKey('njm') ? (bv(j['njm']) ? 'OTAA' : 'ABP') : l.modeName;
    if (fileMode != l.modeName &&
        !await confirmDialog(context, 'Importar',
            'Este arquivo é de um nó em $fileMode e este nó está em ${l.modeName}. Importar mesmo assim? '
            'A ativação do formulário muda para $fileMode (AT+NJM) e, ao salvar, o nó reinicia nela.',
            confirm: 'Importar')) {
      return;
    }
    final n = widget.onImport(j);
    if (mounted) {
      snack(context, n > 0 ? 'Configuração carregada ($n campos). Revise e toque em Salvar no nó.' : 'Nenhum campo reconhecido no arquivo.');
    }
  }

  Future<void> _factory() async {
    var zeroNonce = false;
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => StatefulBuilder(
        builder: (c, set) => AlertDialog(
          title: const Text('Padrão de fábrica'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('Apaga a configuração e a sessão LoRaWAN e volta aos valores do node_build.h. '
                  'O nó reinicia.'),
              if (l.otaa)
                CheckboxListTile(
                  contentPadding: EdgeInsets.zero,
                  value: zeroNonce,
                  onChanged: (v) => set(() => zeroNonce = v ?? false),
                  title: const Text('Zerar também o DevNonce'),
                  subtitle: const Text('O ChirpStack vai recusar os JOINs até você limpar os DevNonces lá.'),
                ),
            ],
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancelar')),
            FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Restaurar')),
          ],
        ),
      ),
    );
    if (ok == true) await _run(zeroNonce ? 'factory nonce' : 'factory', 'Restaurando… o nó vai reiniciar.');
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: l,
      builder: (context, _) {
        final s = l.status;
        final joined = bv(s['joined']);
        final cls = sv(s['maccls']).isNotEmpty ? sv(s['maccls']) : sv(s['cls']);
        final i = l.info;
        return AbsorbPointer(
          absorbing: _busy,
          child: ListView(
            padding: const EdgeInsets.fromLTRB(14, 14, 14, 24),
            children: [
              if (_busy) const LinearProgressIndicator(),
              SectionCard(
                title: 'Uplink agora',
                subtitle: joined
                    ? 'Vazio: manda o payload configurado. Com FPort ou Confirmado, vai por AT+SENDB / AT+TXBCFM '
                        'e a resposta (AT TX OK, AT ACK OK...) aparece nos Logs.'
                    : 'Sem sessão LoRaWAN ainda.',
                children: [
                  SegChoice(
                    options: const [SegOption('txt', 'Texto'), SegOption('hex', 'Hexadecimal')],
                    value: _hex ? 'hex' : 'txt',
                    showHint: false,
                    onChanged: (v) => setState(() => _hex = v == 'hex'),
                  ),
                  const SizedBox(height: 8),
                  TextFieldX(controller: _payload, label: 'Payload (opcional)', mono: _hex, upper: _hex, maxLength: _hex ? 102 : 51),
                  const SizedBox(height: 8),
                  Pair(
                    TextFieldX(controller: _port, label: 'FPort', hint: 'a configurada', keyboard: TextInputType.number, maxLength: 3),
                    TextFieldX(controller: _trials, label: 'Tentativas (confirmado)', hint: 'NBTRIALS',
                        keyboard: TextInputType.number, maxLength: 2, enabled: _cfm),
                  ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Confirmado (AT+TXBCFM)'),
                    value: _cfm,
                    onChanged: (v) => setState(() => _cfm = v),
                  ),
                  FilledButton.icon(
                    onPressed: joined ? _sendCustom : null,
                    icon: const Icon(Icons.upload),
                    label: const Text('Enviar agora'),
                  ),
                ],
              ),
              GpioCard(link: l),
              DevicesCard(link: l),
              SectionCard(
                title: 'Classe',
                subtitle: 'Troca agora e grava. Em uso: ${cls.isEmpty ? '-' : cls}. '
                    'Troque também o perfil do dispositivo no ChirpStack (Class-C / Class-B) e refaça o JOIN: '
                    'é só no JOIN que ele lê a classe.',
                children: [
                  SegChoice(
                    options: const [
                      SegOption('A', 'Classe A'),
                      SegOption('B', 'Classe B'),
                      SegOption('C', 'Classe C'),
                    ],
                    value: sv(s['cfgcls']).isEmpty ? null : sv(s['cfgcls']),
                    showHint: false,
                    onChanged: (v) => _run('class $v', 'Classe $v pedida.'),
                  ),
                  if (sv(s['clk']).isNotEmpty && sv(s['clk']) != 'lse')
                    const Hint('Sem cristal de 32,768 kHz: a Classe B pode perder ping slots.'),
                ],
              ),
              SectionCard(
                title: 'LoRaWAN',
                children: [
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      OutlinedButton.icon(
                        onPressed: () async {
                          if (await confirmDialog(context, l.otaa ? 'Refazer JOIN' : 'Reativar sessão',
                              l.otaa ? 'Fazer um novo JOIN agora?' : 'Reativar a sessão ABP agora?')) {
                            _run('join', l.otaa ? 'JOIN pedido.' : 'Sessão reativada.');
                          }
                        },
                        icon: const Icon(Icons.login),
                        label: Text(l.otaa ? 'Refazer JOIN' : 'Reativar ABP'),
                      ),
                      OutlinedButton.icon(
                        onPressed: joined ? () => _run('devtime', 'DeviceTimeReq no próximo uplink.') : null,
                        icon: const Icon(Icons.schedule),
                        label: const Text('Pedir a hora (DeviceTimeReq)'),
                      ),
                      OutlinedButton.icon(
                        onPressed: () => _run('clock recal', 'Relógio medido de novo.'),
                        icon: const Icon(Icons.av_timer),
                        label: const Text('Recalibrar relógio'),
                      ),
                    ],
                  ),
                ],
              ),
              SectionCard(
                title: 'Manutenção',
                subtitle: 'A exportação leva as chaves LoRaWAN: guarde o arquivo com cuidado. '
                    'Importar só preenche a Configuração; revise e salve.',
                children: [
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      OutlinedButton.icon(
                        onPressed: () async {
                          if (await confirmDialog(context, 'Reiniciar', 'Reiniciar o nó agora?', confirm: 'Reiniciar')) {
                            _run('reboot', 'Reiniciando…');
                          }
                        },
                        icon: const Icon(Icons.restart_alt),
                        label: const Text('Reiniciar'),
                      ),
                      OutlinedButton.icon(
                        onPressed: _export,
                        icon: const Icon(Icons.download),
                        label: const Text('Exportar configuração'),
                      ),
                      OutlinedButton.icon(
                        onPressed: _import,
                        icon: const Icon(Icons.upload_file),
                        label: const Text('Importar…'),
                      ),
                      OutlinedButton.icon(
                        onPressed: _factory,
                        icon: const Icon(Icons.settings_backup_restore),
                        label: const Text('Padrão de fábrica'),
                      ),
                    ],
                  ),
                ],
              ),
              SectionCard(
                title: 'Firmware',
                children: [
                  KvGrid([
                    KvItem('Firmware', sv(i['fw']), 'v${sv(i['ver'])}'),
                    KvItem('Pilha', sv(i['stack'])),
                    KvItem('Hardware', '${sv(i['mcu'])} + ${sv(i['radio'])}', sv(i['region'])),
                    KvItem.copyable('DevEUI', sv(i['deveui'])),
                    KvItem('Ligação', l.transport.name),
                  ]),
                ],
              ),
              DevFooter(appTitle),
            ],
          ),
        );
      },
    );
  }
}
