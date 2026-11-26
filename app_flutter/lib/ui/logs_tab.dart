// Aba Logs: tudo o que o nó imprime ([TX], [RX], [JOIN], [Classe], [Beacon]...),
// com os mesmos controles da aba Logs dos apps dos nós ESP32 (Pausar, Limpar tela,
// Baixar do nó / sessão, Rolar automático, filtro) e, embaixo, o campo de comandos
// do console. "Baixar do nó" traz o log que o firmware guarda em RAM (jlog).
import 'package:flutter/material.dart';

import '../core/file_io.dart';
import '../core/node_link.dart';
import 'term_view.dart';
import 'theme.dart';
import 'widgets.dart';

/// Cores das linhas (as mesmas da página WEB).
Color? radioengeLogColor(String line) {
  if (RegExp(r'ERRO|FALHOU|Falha|@ERR|inválid|recusad', caseSensitive: false).hasMatch(line)) {
    return AppColors.termErr;
  }
  if (RegExp(r'AVISO|ATENÇÃO|Perdido|Adiado|Sem Join-accept').hasMatch(line)) return AppColors.termWarn;
  if (line.contains('[RX]') || line.contains('[Beacon] Recebido')) return AppColors.termInfo;
  if (line.contains('[TX]') || line.contains('[JOIN] Conectado') || line.contains('[Classe] Agora')) {
    return AppColors.termOk;
  }
  // diagnóstico de sessão (como nos nós ESP32)
  if (line.startsWith('EV_TXSTART') || line == 'Packet queued.') return AppColors.termOk;
  return null;
}

String _two(int n) => n.toString().padLeft(2, '0');

/// AAAAMMDD-hhmmss, para os nomes dos arquivos.
String fileStamp([DateTime? d]) {
  d ??= DateTime.now();
  return '${d.year}${_two(d.month)}${_two(d.day)}-${_two(d.hour)}${_two(d.minute)}${_two(d.second)}';
}

String clockText([DateTime? d]) {
  d ??= DateTime.now();
  return '${_two(d.hour)}:${_two(d.minute)}:${_two(d.second)}';
}

class LogsTab extends StatefulWidget {
  const LogsTab({super.key, required this.link, required this.active});

  final NodeLink link;
  final bool active;

  @override
  State<LogsTab> createState() => _LogsTabState();
}

class _LogsTabState extends State<LogsTab> {
  final _scroll = ScrollController();
  final _input = TextEditingController();
  final _flt = TextEditingController();
  bool _auto = true;
  bool _paused = false;
  List<String> _frozen = const [];
  String _filter = '';
  String _status = '';
  bool _downloading = false;

  NodeLink get l => widget.link;

  @override
  void initState() {
    super.initState();
    l.addListener(_onLink);
  }

  @override
  void dispose() {
    l.removeListener(_onLink);
    _scroll.dispose();
    _input.dispose();
    _flt.dispose();
    super.dispose();
  }

  void _onLink() {
    if (!widget.active || !mounted || _paused) return;
    setState(() => _status = l.connected ? '${l.transport.name} · atualizado ${clockText()}' : 'desconectado');
    _scrollEnd();
  }

  void _scrollEnd() {
    if (!_auto) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
  }

  List<String> get _all => _paused ? _frozen : l.log;

  List<String> get _visible {
    final f = _filter.toLowerCase();
    if (f.isEmpty) return _all;
    return _all.where((x) => x.toLowerCase().contains(f)).toList();
  }

  String get _id {
    final e = sv(l.config['deveui']).isNotEmpty ? sv(l.config['deveui']) : sv(l.info['deveui']);
    return e.isEmpty ? 'node' : e.toLowerCase();
  }

  Future<void> _downloadNode() async {
    if (_downloading) return;
    setState(() {
      _downloading = true;
      _status = 'baixando o log do nó…';
    });
    try {
      final lines = await l.fetchNodeLog(progress: (n) {
        if (mounted) setState(() => _status = 'baixando o log do nó… $n linhas');
      });
      if (!mounted) return;
      if (lines == null) {
        snack(context, 'O nó não respondeu ao jlog (firmware antigo?).');
        return;
      }
      final i = l.info;
      final head = '# ${sv(i['fw'])} v${sv(i['ver'])} · DevEUI ${_id.toUpperCase()} · log em RAM do nó '
          '(${lines.length} linhas, [hh:mm:ss] = tempo ligado)\n'
          '# baixado em ${DateTime.now()} por ${l.transport.name}\n';
      final name = 'radioenge_${l.modeKey}_${_id}_${fileStamp()}.log';
      final path = await FileIO.saveText(name, '$head${lines.join('\n')}\n');
      if (mounted && path != null) snack(context, 'Salvo: $name (${lines.length} linhas)');
    } catch (e) {
      if (mounted) snack(context, 'Não foi possível salvar: $e');
    } finally {
      if (mounted) {
        setState(() {
          _downloading = false;
          _status = l.connected ? '${l.transport.name} · atualizado ${clockText()}' : 'desconectado';
        });
      }
    }
  }

  Future<void> _downloadSession() async {
    if (l.log.isEmpty) {
      snack(context, 'Nada recebido ainda.');
      return;
    }
    try {
      final name = 'radioenge_${l.modeKey}_sessao_${fileStamp()}.log';
      final path = await FileIO.saveText(name, '${l.log.join('\n')}\n');
      if (mounted && path != null) snack(context, 'Salvo: $name');
    } catch (e) {
      if (mounted) snack(context, 'Não foi possível salvar: $e');
    }
  }

  Future<void> _send() async {
    final t = _input.text.trim();
    if (t.isEmpty) return;
    if (t == 'mode human') {
      snack(context, 'O app precisa do modo app: use um terminal serial para o modo humano.');
      return;
    }
    _input.clear();
    l.log.add('> $t');
    final r = await l.cmd(t);
    if (!r.ok && mounted) snack(context, 'Erro: ${r.message}');
  }

  @override
  Widget build(BuildContext context) {
    final lines = _visible;
    final cs = Theme.of(context).colorScheme;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(10, 10, 10, 6),
          child: Wrap(
            spacing: 8,
            runSpacing: 6,
            crossAxisAlignment: WrapCrossAlignment.center,
            children: [
              FilledButton.tonalIcon(
                onPressed: () => setState(() {
                  _paused = !_paused;
                  _frozen = _paused ? List.of(l.log) : const [];
                  if (!_paused) _scrollEnd();
                }),
                icon: Icon(_paused ? Icons.play_arrow : Icons.pause),
                label: Text(_paused ? 'Continuar' : 'Pausar'),
              ),
              OutlinedButton.icon(
                onPressed: () {
                  _frozen = const [];
                  l.clearLog();
                  setState(() {});
                },
                icon: const Icon(Icons.clear_all),
                label: const Text('Limpar tela'),
              ),
              PopupMenuButton<String>(
                tooltip: 'Baixar',
                enabled: !_downloading,
                onSelected: (v) => v == 'dev' ? _downloadNode() : _downloadSession(),
                itemBuilder: (_) => [
                  PopupMenuItem(
                    value: 'dev',
                    enabled: l.connected,
                    child: const Text('Baixar do nó (log em RAM, até 3 KB)'),
                  ),
                  const PopupMenuItem(value: 'ses', child: Text('Baixar sessão (o que o app recebeu)')),
                ],
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
                  child: Row(mainAxisSize: MainAxisSize.min, children: [
                    _downloading
                        ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                        : const Icon(Icons.download),
                    const SizedBox(width: 4),
                    const Text('Baixar'),
                  ]),
                ),
              ),
              Row(mainAxisSize: MainAxisSize.min, children: [
                Switch(
                  value: _auto,
                  onChanged: (v) {
                    setState(() => _auto = v);
                    _scrollEnd();
                  },
                ),
                const Text('Rolar automático', style: TextStyle(fontSize: 13)),
              ]),
            ],
          ),
        ),
        // filtro com a largura toda e o estado embaixo (como nos apps dos nós ESP32)
        Padding(
          padding: const EdgeInsets.fromLTRB(10, 0, 10, 6),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              TextField(
                controller: _flt,
                onChanged: (v) => setState(() => _filter = v),
                decoration: fieldDecoration(context,
                    hint: 'Filtrar (ex.: [TX], [RX], JOIN, Classe)',
                    suffix: _filter.isEmpty
                        ? const Icon(Icons.filter_alt_outlined)
                        : IconButton(
                            tooltip: 'Limpar filtro',
                            icon: const Icon(Icons.close),
                            onPressed: () => setState(() {
                              _flt.clear();
                              _filter = '';
                            }),
                          )),
              ),
              Padding(
                padding: const EdgeInsets.only(top: 4, left: 2),
                child: Text(
                  '${_paused ? 'pausado · ' : ''}$_status'
                  '${_filter.isEmpty ? '' : ' · ${lines.length} de ${_all.length} linhas'}',
                  style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant),
                ),
              ),
            ],
          ),
        ),
        Expanded(
          child: Container(
            margin: const EdgeInsets.fromLTRB(8, 0, 8, 4),
            padding: const EdgeInsets.all(8),
            decoration: BoxDecoration(color: AppColors.term, borderRadius: BorderRadius.circular(10)),
            // sem quebra de linha: linhas longas rolam para o lado
            child: SelectionArea(
              child: TermLines(lines: lines, controller: _scroll, colorOf: radioengeLogColor),
            ),
          ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.fromLTRB(10, 4, 10, 10),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _input,
                    enabled: l.connected,
                    autocorrect: false,
                    enableSuggestions: false,
                    style: const TextStyle(fontFamily: monoFont),
                    decoration: const InputDecoration(
                      isDense: true,
                      labelText: 'Comando do console ou AT (ex.: AT+SEND=5:teste)',
                      helperText: 'ex.: status, class C, send hex:0102, log, diag',
                      helperMaxLines: 3,
                      border: OutlineInputBorder(),
                    ),
                    onSubmitted: (_) => _send(),
                  ),
                ),
                const SizedBox(width: 8),
                FilledButton(onPressed: l.connected ? _send : null, child: const Icon(Icons.send)),
              ],
            ),
          ),
        ),
      ],
    );
  }
}
