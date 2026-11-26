// Tela "Classes LoRaWAN": o que são as Classes A, B e C, quando usar cada uma e
// o que este nó suporta. Resumo da seção "Classes LoRaWAN" do README do firmware (mantenha os dois
// iguais). Não acessa o nó.
import 'package:flutter/material.dart';

import 'theme.dart';
import 'widgets.dart';

/// Abre a tela (usado pelos botões 🎓 das AppBars).
void openLoRaWanClasses(BuildContext context) {
  Navigator.of(context).push(MaterialPageRoute(builder: (_) => const LoRaWanClassesScreen()));
}

/// Botão 🎓 da AppBar.
class ClassesButton extends StatelessWidget {
  const ClassesButton({super.key});

  @override
  Widget build(BuildContext context) => IconButton(
        onPressed: () => openLoRaWanClasses(context),
        icon: const Icon(Icons.school_outlined),
        tooltip: 'Classes LoRaWAN',
      );
}

// ------------------------------------------------------------------ conteúdo

/// Segmento da linha do tempo: 't' uplink, 'r' escutando, 'b' beacon, 'g' dormindo.
class _Seg {
  const _Seg(this.kind, this.flex);
  final String kind;
  final int flex;
}

class _ClassInfo {
  const _ClassInfo({
    required this.letter,
    required this.title,
    required this.subtitle,
    required this.timeline,
    required this.rows,
    required this.useWhen,
    required this.examples,
  });
  final String letter, title, subtitle, useWhen, examples;
  final List<_Seg> timeline;
  final List<List<String>> rows;
}

const _classes = <_ClassInfo>[
  _ClassInfo(
    letter: 'A',
    title: 'Fala, depois escuta',
    subtitle: 'obrigatória em todo nó',
    timeline: [_Seg('t', 3), _Seg('g', 2), _Seg('r', 1), _Seg('g', 2), _Seg('r', 1), _Seg('g', 18), _Seg('t', 3)],
    rows: [
      ['Escuta', 'Só em 2 janelas curtas (RX1 e RX2) logo depois de cada uplink'],
      ['Atraso', 'Até o **próximo uplink** (segundos a horas)'],
      ['Consumo', '🔋 mínimo'],
    ],
    useWhen: 'o nó é a bateria, quase só envia dados e o downlink pode esperar.',
    examples: 'medidores de energia/água/gás, temperatura, nível, estação meteorológica, rastreador.',
  ),
  _ClassInfo(
    letter: 'B',
    title: 'Escuta em horários combinados',
    subtitle: 'beacon a cada 128 s + ping slots',
    timeline: [
      _Seg('b', 2), _Seg('g', 3), _Seg('r', 1), _Seg('g', 3), _Seg('r', 1), _Seg('g', 3), _Seg('r', 1), //
      _Seg('g', 3), _Seg('r', 1), _Seg('g', 3), _Seg('r', 1), _Seg('g', 3), _Seg('b', 2),
    ],
    rows: [
      ['Escuta', 'Janelas A + **ping slots** a cada 1, 2, 4 … 128 s, sincronizados pelo beacon (hora GPS)'],
      ['Atraso', 'Até o próximo ping slot (**1 s a 128 s**)'],
      ['Consumo', '🔋🔋 baixo/médio'],
      ['Exige', 'Gateways com **GPS + PPS**'],
    ],
    useWhen: 'o nó é a bateria mas precisa receber comandos com atraso previsível.',
    examples: 'fechadura, válvula de irrigação a bateria, sirene/avisador, multicast agendado.',
  ),
  _ClassInfo(
    letter: 'C',
    title: 'Escuta o tempo todo',
    subtitle: 'recepção contínua na RX2',
    timeline: [_Seg('t', 3), _Seg('r', 13), _Seg('t', 3), _Seg('r', 11)],
    rows: [
      ['Escuta', '**Sempre**, exceto enquanto transmite'],
      ['Atraso', '**Quase imediato** (só a rede)'],
      ['Consumo', '🔌 alto: ~10–12 mA o tempo todo só no rádio'],
      ['Exige', 'Alimentação contínua'],
    ],
    useWhen: 'o nó está na rede elétrica e precisa reagir rápido.',
    examples: 'relé/disjuntor remoto, iluminação pública, tomada inteligente, alarme, FUOTA multicast.',
  ),
];

const _compare = <List<String>>[
  ['', 'Classe A', 'Classe B', 'Classe C'],
  ['Obrigatória', 'Sim, em todo nó', 'Opcional', 'Opcional'],
  ['Janelas de recepção', 'RX1 + RX2 depois de cada uplink', 'RX1/RX2 + ping slots (1–128 s)', 'Contínua, exceto no TX'],
  ['Quem inicia o downlink', 'Só depois de um uplink', 'Servidor, no próximo ping slot', 'Servidor, a qualquer momento'],
  ['Atraso máximo', 'Intervalo entre uplinks', 'Periodicidade do ping slot', '~imediato'],
  ['Alimentação típica', 'Bateria (anos)', 'Bateria (meses a anos)', 'Rede elétrica / fonte'],
  ['Exige do gateway', 'Nada especial', 'GPS + PPS (beacon)', 'Nada especial'],
  ['Downlink do gateway', 'tmst (RX1/RX2)', 'beacon + tmms (hora GPS)', '"imme": true'],
  ['Multicast (FUOTA)', 'Não', 'Sim', 'Sim'],
];

const _howToChoose = <String>[
  'Quase só envia dados e o downlink pode esperar o próximo envio? **Classe A**: o padrão para a maioria dos sensores.',
  'Precisa receber comandos com atraso previsível, a bateria? **Classe B**, se todos os gateways da área tiverem GPS com PPS.',
  'Está na rede elétrica e precisa obedecer rápido? **Classe C**.',
  'Só quer um downlink mais rápido de vez em quando? Fique na **Classe A** e diminua o intervalo de uplink.',
];

// -------------------------------------------- o que muda de um app para outro
const _deviceTitle = 'Neste nó (LoRaMac-node v4.7.1)';

/// [classe, estado (✅ 🧪 🗓️ ➖ ❌), detalhe]
const _deviceRows = <List<String>>[
  ['A', '✅', '**Implementada.** RX1/RX2 depois de cada uplink. Padrão de fábrica.'],
  ['B', '✅', '**Implementada.** DeviceTimeReq → beacon → PingSlotInfoReq. Exige gateway com GPS + PPS e, no módulo, o cristal de 32,768 kHz: sem ele o relógio é o LSI calibrado e os ping slots podem ser perdidos.'],
  ['C', '✅', '**Implementada.** Recepção contínua na RX2 (923,3 MHz, DR8). Exige alimentação contínua.'],
];

const _deviceNotes = <String>[
  'Troque a classe na aba Manutenção, na Configuração, pelo console (class A|B|C) ou por downlink na FPort 102 (01 00 / 01 01 / 01 02). O comando classes no console mostra este resumo.',
  'No ChirpStack, o perfil do dispositivo precisa combinar com a classe do nó: Class-B ou Class-C marcadas.',
];

const _footer = 'LoRaWAN Node Radioenge';

// --------------------------------------------------------------------- tela

class LoRaWanClassesScreen extends StatelessWidget {
  const LoRaWanClassesScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Scaffold(
      appBar: AppBar(title: const Text('Classes LoRaWAN')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(14, 14, 14, 24),
        children: [
          Padding(
            padding: const EdgeInsets.only(bottom: 14),
            child: _md(
              context,
              'A **classe** define **quando o nó escuta downlinks** (mensagens do servidor para o nó: comandos, '
              'ACK, ADR, reset remoto). O uplink é igual nas três. Todo nó precisa da **Classe A**; B e C são '
              'modos adicionais. O **gateway não tem classe**: ele atende nós A, B e C ao mesmo tempo.',
              color: cs.onSurfaceVariant,
            ),
          ),
          for (final c in _classes) _ClassCard(c),
          const _Legend(),
          const _DeviceCard(),
          SectionCard(
            title: 'Comparativo',
            children: [
              _compareTable(context),
            ],
          ),
          SectionCard(
            title: 'Qual classe usar',
            children: [
              for (var i = 0; i < _howToChoose.length; i++)
                Padding(
                  padding: const EdgeInsets.only(bottom: 6),
                  child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    SizedBox(width: 22, child: Text('${i + 1}.', style: const TextStyle(fontWeight: FontWeight.w600))),
                    Expanded(child: _md(context, _howToChoose[i])),
                  ]),
                ),
              const Hint(
                'B e C não devem ser usadas ao mesmo tempo no mesmo nó. No ChirpStack v4 a classe é escolhida no '
                'perfil do dispositivo (abas Class-B e Class-C); sem nenhuma marcada, o dispositivo é Classe A. '
                'Explicação completa no README do firmware, seção Classes LoRaWAN.',
              ),
            ],
          ),
          const DevFooter(_footer),
        ],
      ),
    );
  }

  Widget _compareTable(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Table(
      defaultColumnWidth: const FixedColumnWidth(150),
      columnWidths: const {0: FlexColumnWidth(1.25), 1: FlexColumnWidth(1), 2: FlexColumnWidth(1), 3: FlexColumnWidth(1)},
      border: TableBorder(horizontalInside: BorderSide(color: cs.outlineVariant)),
      children: [
        for (var r = 0; r < _compare.length; r++)
          TableRow(children: [
            for (var c = 0; c < 4; c++)
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 7),
                child: Text(
                  _compare[r][c],
                  style: TextStyle(
                    fontSize: 13,
                    fontWeight: r == 0 ? FontWeight.w600 : FontWeight.normal,
                    color: (r == 0 || c == 0) ? cs.onSurfaceVariant : null,
                  ),
                ),
              ),
          ]),
      ],
    );
  }
}

/// Texto com trechos em **negrito**.
Widget _md(BuildContext context, String text, {Color? color, double size = 14}) {
  final parts = text.split('**');
  return Text.rich(
    TextSpan(children: [
      for (var i = 0; i < parts.length; i++)
        TextSpan(text: parts[i], style: i.isOdd ? const TextStyle(fontWeight: FontWeight.w600) : null),
    ]),
    style: TextStyle(fontSize: size, height: 1.4, color: color),
  );
}

Color _segColor(BuildContext context, String k) {
  switch (k) {
    case 't':
      return AppColors.accentOf(context);
    case 'r':
      return AppColors.dark(context) ? const Color(0xFF6AA5FF) : const Color(0xFF2F6FD0);
    case 'b':
      return AppColors.warnOf(context);
    default:
      return Theme.of(context).colorScheme.outlineVariant.withAlpha(90);
  }
}

class _ClassCard extends StatelessWidget {
  const _ClassCard(this.c);
  final _ClassInfo c;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Card(
      margin: const EdgeInsets.only(bottom: 14),
      elevation: 0,
      color: cs.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(color: cs.outlineVariant),
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Container(
              width: 38,
              height: 38,
              alignment: Alignment.center,
              decoration: BoxDecoration(color: AppColors.accentOf(context), borderRadius: BorderRadius.circular(10)),
              child: Text(c.letter,
                  style: const TextStyle(color: Colors.white, fontSize: 20, fontWeight: FontWeight.w700)),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text('Classe ${c.letter}: ${c.title}',
                    style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                Text(c.subtitle, style: TextStyle(fontSize: 12.5, color: cs.onSurfaceVariant)),
              ]),
            ),
          ]),
          const SizedBox(height: 10),
          ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: SizedBox(
              height: 14,
              child: Row(children: [
                for (final s in c.timeline)
                  Expanded(flex: s.flex, child: Container(color: _segColor(context, s.kind))),
              ]),
            ),
          ),
          const SizedBox(height: 10),
          for (final r in c.rows)
            Padding(
              padding: const EdgeInsets.only(bottom: 4),
              child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                SizedBox(
                    width: 74,
                    child: Text(r[0], style: TextStyle(fontSize: 13.5, color: cs.onSurfaceVariant))),
                Expanded(child: _md(context, r[1], size: 13.5)),
              ]),
            ),
          const SizedBox(height: 6),
          _md(context, '**Use quando** ${c.useWhen}', size: 13, color: cs.onSurfaceVariant),
          const SizedBox(height: 2),
          _md(context, '**Exemplos:** ${c.examples}', size: 13, color: cs.onSurfaceVariant),
        ]),
      ),
    );
  }
}

class _Legend extends StatelessWidget {
  const _Legend();

  @override
  Widget build(BuildContext context) {
    Widget item(String k, String label) => Row(mainAxisSize: MainAxisSize.min, children: [
          Container(
            width: 10,
            height: 10,
            decoration: BoxDecoration(color: _segColor(context, k), borderRadius: BorderRadius.circular(2)),
          ),
          const SizedBox(width: 5),
          Text(label, style: TextStyle(fontSize: 12, color: Theme.of(context).colorScheme.onSurfaceVariant)),
        ]);
    return Padding(
      padding: const EdgeInsets.only(bottom: 14),
      child: Wrap(spacing: 12, runSpacing: 6, children: [
        item('t', 'uplink (TX)'),
        item('r', 'nó escutando (RX)'),
        item('b', 'beacon do gateway'),
        item('g', 'rádio dormindo'),
      ]),
    );
  }
}

class _DeviceCard extends StatelessWidget {
  const _DeviceCard();

  Color _stColor(BuildContext context, String s) {
    switch (s) {
      case '✅':
        return AppColors.okOf(context);
      case '❌':
        return AppColors.errOf(context);
      case '➖':
        return Theme.of(context).colorScheme.onSurfaceVariant;
      default:
        return AppColors.warnOf(context);
    }
  }

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final acc = AppColors.accentOf(context);
    return Card(
      margin: const EdgeInsets.only(bottom: 14),
      elevation: 0,
      color: cs.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12), side: BorderSide(color: acc, width: 2)),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text(_deviceTitle, style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: acc)),
          const SizedBox(height: 10),
          for (final r in _deviceRows)
            Padding(
              padding: const EdgeInsets.only(bottom: 10),
              child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                SizedBox(
                  width: 64,
                  child: Text.rich(TextSpan(children: [
                    TextSpan(text: '${r[0]}  ', style: const TextStyle(fontWeight: FontWeight.w700)),
                    TextSpan(text: r[1], style: TextStyle(color: _stColor(context, r[1]))),
                  ])),
                ),
                Expanded(child: _md(context, r[2], size: 13.5)),
              ]),
            ),
          for (final n in _deviceNotes) Hint(n, top: 4),
          const Hint('✅ em uso / disponível · 🧪 disponível, falta validar em campo · 🗓️ planejado · '
              '➖ não ativado · ❌ não existe', top: 10),
        ]),
      ),
    );
  }
}
