// Tela do nó conectado: Status, Configuração, Logs, Diagnósticos e Manutenção
// (os mesmos menus dos apps dos nós ESP32; Classes LoRaWAN no ícone do topo).
import 'package:flutter/material.dart';

import '../core/node_link.dart';
import 'actions_tab.dart';
import 'config_tab.dart';
import 'diag_tab.dart';
import 'logs_tab.dart';
import 'lorawan_classes_screen.dart';
import 'status_tab.dart';
import 'widgets.dart';

class NodeShell extends StatefulWidget {
  const NodeShell({super.key, required this.link});

  final NodeLink link;

  @override
  State<NodeShell> createState() => _NodeShellState();
}

class _NodeShellState extends State<NodeShell> with WidgetsBindingObserver {
  int _tab = 0;
  bool _fg = true;
  bool _lostShown = false;
  final GlobalKey<ConfigTabState> _cfgKey = GlobalKey<ConfigTabState>();

  NodeLink get l => widget.link;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    l.addListener(_onLink);
    _applyPolling();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    l.removeListener(_onLink);
    l.setPolling(false);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _fg = state == AppLifecycleState.resumed;
    _applyPolling();
  }

  void _applyPolling() {
    l.ioPolling = _tab == 4;              // GPIOs ficam na aba Manutenção
    l.setPolling(_fg && (_tab == 0 || _tab == 4));
  }

  void _onLink() {
    if (!l.connected && !_lostShown && mounted) {
      _lostShown = true;
      Navigator.of(context).popUntil((r) => r is PageRoute);
      messageDialog(context, 'Ligação perdida', 'O cabo USB ou o Bluetooth desconectou.').then((_) {
        if (mounted) Navigator.of(context).popUntil((r) => r.isFirst);
      });
    }
  }

  Future<void> _leave() async {
    final dirty = _cfgKey.currentState?.dirty ?? false;
    final ok = await confirmDialog(
      context,
      'Desconectar',
      dirty ? 'Há alterações não salvas na Configuração. Desconectar mesmo assim?' : 'Desconectar do nó?',
      confirm: 'Desconectar',
    );
    if (ok && mounted) Navigator.of(context).pop();
  }

  void _setTab(int i) {
    setState(() => _tab = i);
    _applyPolling();
  }

  @override
  Widget build(BuildContext context) {
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) _leave();
      },
      child: AnimatedBuilder(
        animation: l,
        builder: (context, _) {
          final s = l.status;
          final joined = bv(s['joined']);
          final joining = bv(s['joining']);
          final otaa = l.otaa;
          final idLabel = joined || !otaa ? 'DevAddr' : 'DevEUI';
          final idValue = joined || !otaa ? sv(s['devaddr'] ?? l.config['devaddr']) : sv(l.info['deveui']);
          final cls = sv(s['cls']);
          return Scaffold(
            appBar: AppBar(
              title: Row(children: [
                Flexible(
                  child: FittedBox(fit: BoxFit.scaleDown, alignment: Alignment.centerLeft, child: Text('Radioenge ${l.modeName}', maxLines: 1, softWrap: false)),
                ),
                const SizedBox(width: 8),
                Tag(l.transport.name.startsWith('BLE') ? 'BLE' : 'USB', color: const Color(0xFFE8F1EE)),
              ]),
              actions: [
                const ClassesButton(),
                IconButton(onPressed: _leave, icon: const Icon(Icons.link_off), tooltip: 'Desconectar'),
              ],
            ),
            body: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                // chips com quebra de linha (nunca cortados nem escondidos numa rolagem lateral)
                Container(
                  color: Theme.of(context).appBarTheme.backgroundColor,
                  padding: const EdgeInsets.fromLTRB(12, 0, 12, 10),
                  child: InfoChips([
                      ChipInfo(
                        joined ? 'LoRaWAN ativo' : (joining ? 'JOIN…' : 'sem sessão'),
                        '',
                        level: joined ? Level.ok : (joining ? Level.warn : Level.err),
                      ),
                      ChipInfo(idLabel, idValue.isEmpty ? '-' : idValue, copy: true),
                      ChipInfo('Classe', cls.isEmpty ? '-' : cls),
                      const ChipInfo('Plano', 'AU915'),
                    ]),
                ),
                Expanded(
                  child: IndexedStack(
              index: _tab,
              children: [
                StatusTab(link: l),
                ConfigTab(key: _cfgKey, link: l),
                LogsTab(link: l, active: _fg && _tab == 2),
                DiagTab(link: l),
                ActionsTab(
                  link: l,
                  onImport: (j) {
                    final n = _cfgKey.currentState?.importJson(j) ?? 0;
                    if (n > 0) _setTab(1);
                    return n;
                  },
                ),
              ],
            ),
                ),
              ],
            ),
            bottomNavigationBar: NavigationBar(
              selectedIndex: _tab,
              onDestinationSelected: _setTab,
              labelBehavior: NavigationDestinationLabelBehavior.alwaysShow,
              destinations: const [
                NavigationDestination(icon: Icon(Icons.monitor_heart_outlined), label: 'Status'),
                NavigationDestination(icon: Icon(Icons.tune), label: 'Config.'),
                NavigationDestination(icon: Icon(Icons.article_outlined), label: 'Logs'),
                NavigationDestination(icon: Icon(Icons.network_ping), label: 'Diagnóst.'),
                NavigationDestination(icon: Icon(Icons.build_outlined), label: 'Manutenção'),
              ],
            ),
          );
        },
      ),
    );
  }
}
