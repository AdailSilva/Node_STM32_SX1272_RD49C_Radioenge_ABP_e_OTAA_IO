// Componentes visuais comuns aos três apps (mesmo visual das páginas web).
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'theme.dart';

enum Level { none, ok, warn, err }

Color levelColor(BuildContext c, Level l) {
  switch (l) {
    case Level.ok:
      return AppColors.okOf(c);
    case Level.warn:
      return AppColors.warnOf(c);
    case Level.err:
      return AppColors.errOf(c);
    case Level.none:
      return Theme.of(c).colorScheme.outline;
  }
}

/// Faixa de status com a borda esquerda colorida (classe .st da página).
class StatusBanner extends StatelessWidget {
  const StatusBanner(this.text, {super.key, this.level = Level.none, this.trailing});

  final String text;
  final Level level;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.only(bottom: 12),
      decoration: BoxDecoration(
        color: cs.surfaceContainerHighest.withAlpha(110),
        borderRadius: BorderRadius.circular(9),
        border: Border(
          left: BorderSide(color: levelColor(context, level), width: 3),
        ),
      ),
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
      child: Row(
        children: [
          Expanded(child: Text(text, style: const TextStyle(fontSize: 13.5))),
          if (trailing != null) trailing!,
        ],
      ),
    );
  }
}

/// Bloco de seção (cartão com título, descrição e etiqueta).
class SectionCard extends StatelessWidget {
  const SectionCard({
    super.key,
    required this.title,
    this.subtitle,
    this.tag,
    this.tagNow = true,
    required this.children,
  });

  final String title;
  final String? subtitle;
  final String? tag;
  final bool tagNow;
  final List<Widget> children;

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
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Expanded(
                  child: Text(title,
                      style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                ),
                if (tag != null) Tag(tag!, color: tagNow ? AppColors.okOf(context) : AppColors.warnOf(context)),
              ],
            ),
            if (subtitle != null) ...[
              const SizedBox(height: 4),
              Text(subtitle!, style: TextStyle(fontSize: 13, color: cs.onSurfaceVariant)),
            ],
            const SizedBox(height: 12),
            ...children,
          ],
        ),
      ),
    );
  }
}

class Tag extends StatelessWidget {
  const Tag(this.text, {super.key, required this.color});

  final String text;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: color.withAlpha(140)),
      ),
      child: Text(text, style: TextStyle(fontSize: 11.5, color: color)),
    );
  }
}

class KvItem {
  const KvItem(this.label, this.value, [this.sub = '']) : copy = false;

  /// Valor em hexadecimal (EUI, DevAddr...): mostra o botão Copiar no card.
  const KvItem.copyable(this.label, this.value, [this.sub = '']) : copy = true;

  final String label;
  final String value;
  final String sub;
  final bool copy;
}

/// Grade de indicadores (classe .kv da página).
class KvGrid extends StatelessWidget {
  const KvGrid(this.items, {super.key});

  final List<KvItem> items;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return LayoutBuilder(builder: (context, c) {
      final cols = c.maxWidth >= 720 ? 4 : c.maxWidth >= 480 ? 3 : 2;
      const gap = 10.0;
      final w = (c.maxWidth - gap * (cols - 1)) / cols;
      return Wrap(
        spacing: gap,
        runSpacing: gap,
        children: [
          for (final it in items)
            Container(
              width: w,
              padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
              decoration: BoxDecoration(
                color: cs.surfaceContainerHighest.withAlpha(90),
                borderRadius: BorderRadius.circular(10),
                border: Border.all(color: cs.outlineVariant),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  if (it.copy && isHexValue(it.value))
                    Row(
                      children: [
                        Expanded(
                            child: Text(it.label, style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant))),
                        CopyIcon(value: () => it.value, name: it.label, hex: false),
                      ],
                    )
                  else
                    Text(it.label, style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant)),
                  const SizedBox(height: 2),
                  // valor longo (MAC, versão, "aguardando MQTT"...) diminui a fonte em vez de ser cortado
                  Tooltip(
                    message: it.value.isEmpty ? '-' : it.value,
                    child: FittedBox(
                      fit: BoxFit.scaleDown,
                      alignment: Alignment.centerLeft,
                      child: Text(
                        it.value,
                        maxLines: 1,
                        softWrap: false,
                        style: const TextStyle(
                            fontFamily: monoFont, fontSize: 17, fontWeight: FontWeight.w600),
                      ),
                    ),
                  ),
                  if (it.sub.isNotEmpty) ...[
                    const SizedBox(height: 1),
                    Text(it.sub,
                        style: TextStyle(fontSize: 11.5, color: cs.onSurfaceVariant, height: 1.3)),
                  ],
                ],
              ),
            ),
        ],
      );
    });
  }
}

/// Texto de ajuda pequeno (classe .hint).
class Hint extends StatelessWidget {
  const Hint(this.text, {super.key, this.top = 6, this.color});

  final String text;
  final double top;
  final Color? color;

  @override
  Widget build(BuildContext context) {
    if (text.isEmpty) return const SizedBox.shrink();
    return Padding(
      padding: EdgeInsets.only(top: top),
      child: Text(text,
          style: TextStyle(
              fontSize: 12.5,
              height: 1.4,
              color: color ?? Theme.of(context).colorScheme.onSurfaceVariant)),
    );
  }
}

class FieldLabel extends StatelessWidget {
  const FieldLabel(this.text, {super.key});

  final String text;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: 6),
        child: Text(text, style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600)),
      );
}

InputDecoration fieldDecoration(BuildContext context,
    {String? label, String? hint, String? helper, Widget? suffix, String? error}) {
  return InputDecoration(
    labelText: label,
    hintText: hint,
    // com rótulo e exemplo juntos, o rótulo fica em cima e o exemplo aparece sem precisar tocar no campo
    floatingLabelBehavior: (label != null && hint != null) ? FloatingLabelBehavior.always : null,
    hintMaxLines: 4,
    helperText: helper,
    helperMaxLines: 8,
    errorText: error,
    errorMaxLines: 4,
    suffixIcon: suffix,
    isDense: true,
    border: const OutlineInputBorder(),
    filled: true,
    fillColor: Theme.of(context).colorScheme.surfaceContainerHighest.withAlpha(60),
  );
}

/// Campo de texto padrão do formulário.
class TextFieldX extends StatefulWidget {
  const TextFieldX({
    super.key,
    required this.controller,
    required this.label,
    this.hint,
    this.helper,
    this.mono = false,
    this.keyboard,
    this.password = false,
    this.maxLength,
    this.onChanged,
    this.enabled = true,
    this.maxLines = 1,
    this.upper = false,
    this.trailing,
    this.wrap = false,
    this.invalid = false,
  });

  final TextEditingController controller;
  final String label;
  final String? hint;
  final String? helper;
  final bool mono;
  final TextInputType? keyboard;
  final bool password;
  final int? maxLength;
  final ValueChanged<String>? onChanged;
  final bool enabled;
  final int maxLines;
  final bool upper;
  final List<Widget>? trailing;

  /// Valor comprido (chaves AES, EUIs): quebra em mais linhas em vez de rolar
  /// para o lado escondendo o começo. O Enter continua sendo "Concluído".
  final bool wrap;

  /// Destaca o campo em vermelho (o texto do erro vai em [helper]).
  final bool invalid;

  @override
  State<TextFieldX> createState() => _TextFieldXState();
}

/// Rótulo em cima do campo (como o <label> da página): quebra em várias linhas
/// em vez de ser cortado com "…" quando o celular é estreito, e deixa o exemplo
/// (placeholder) sempre visível dentro do campo.
class _TextFieldXState extends State<TextFieldX> {
  bool _show = false;

  @override
  Widget build(BuildContext context) {
    final multi = !widget.password && (widget.wrap || widget.maxLines != 1);
    final errColor = AppColors.errOf(context);
    var deco = fieldDecoration(
      context,
      hint: widget.hint,
      suffix: widget.password
          ? IconButton(
              tooltip: _show ? 'Ocultar' : 'Mostrar',
              icon: Icon(_show ? Icons.visibility_off : Icons.visibility),
              onPressed: () => setState(() => _show = !_show),
            )
          : null,
    );
    if (widget.invalid) {
      deco = deco.copyWith(
        enabledBorder: OutlineInputBorder(borderSide: BorderSide(color: errColor)),
        focusedBorder: OutlineInputBorder(borderSide: BorderSide(color: errColor, width: 2)),
      );
    }
    final field = Semantics(
      label: widget.label,
      child: TextField(
        controller: widget.controller,
        enabled: widget.enabled,
        keyboardType: widget.keyboard ?? (multi && widget.wrap ? TextInputType.text : null),
        textInputAction: widget.wrap ? TextInputAction.done : null,
        obscureText: widget.password && !_show,
        autocorrect: false,
        enableSuggestions: false,
        minLines: 1,
        maxLines: widget.password ? 1 : (widget.wrap ? 3 : widget.maxLines),
        textCapitalization: TextCapitalization.none,
        inputFormatters: [
          if (widget.wrap) FilteringTextInputFormatter.deny(RegExp(r'[\r\n]')),
          if (widget.maxLength != null) LengthLimitingTextInputFormatter(widget.maxLength),
          if (widget.upper) _UpperCaseFormatter(),
        ],
        style: widget.mono ? const TextStyle(fontFamily: monoFont, fontSize: 14) : null,
        onChanged: widget.onChanged,
        decoration: deco,
      ),
    );
    final t = widget.trailing;
    final help = widget.helper;
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          FieldLabel(widget.label),
          if (t == null || t.isEmpty)
            field
          else
            Row(
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                Expanded(child: field),
                for (final w in t) Padding(padding: const EdgeInsets.only(left: 4), child: w),
              ],
            ),
          if (help != null && help.isNotEmpty) Hint(help, color: widget.invalid ? errColor : null),
        ],
      ),
    );
  }
}

class _UpperCaseFormatter extends TextInputFormatter {
  @override
  TextEditingValue formatEditUpdate(TextEditingValue oldValue, TextEditingValue newValue) =>
      newValue.copyWith(text: newValue.text.toUpperCase());
}

/// Dois campos lado a lado em telas largas, um embaixo do outro no celular.
class Pair extends StatelessWidget {
  const Pair(this.a, this.b, {super.key, this.flexA = 1, this.flexB = 1});

  final Widget a;
  final Widget b;
  final int flexA;
  final int flexB;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, c) {
      // abaixo de 520 px cada campo ficaria com menos de ~250 px: um embaixo do outro
      if (c.maxWidth < 520) {
        return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [a, b]);
      }
      return Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Expanded(flex: flexA, child: a),
          const SizedBox(width: 12),
          Expanded(flex: flexB, child: b),
        ],
      );
    });
  }
}

class SegOption {
  const SegOption(this.value, this.label, [this.hint = '']);

  final String value;
  final String label;
  final String hint;
}

/// Botões segmentados (classe .seg). value null = nada marcado.
class SegChoice extends StatelessWidget {
  const SegChoice({
    super.key,
    this.label,
    required this.options,
    required this.value,
    required this.onChanged,
    this.showHint = true,
  });

  final String? label;
  final List<SegOption> options;
  final String? value;
  final ValueChanged<String> onChanged;
  final bool showHint;

  @override
  Widget build(BuildContext context) {
    final sel = options.where((o) => o.value == value);
    final hint = sel.isEmpty ? '' : sel.first.hint;
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (label != null) FieldLabel(label!),
          // botões segmentados quando cabem na largura; senão, opções que quebram a linha
          // (nunca corta o texto nem esconde opções numa rolagem lateral)
          LayoutBuilder(builder: (context, c) {
            final scaler = MediaQuery.textScalerOf(context);
            var need = 0.0;
            for (final o in options) {
              final tp = TextPainter(
                text: TextSpan(text: o.label, style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w500)),
                textDirection: TextDirection.ltr,
                textScaler: scaler,
              )..layout();
              need += tp.width + 34;
              tp.dispose();
            }
            if (need <= c.maxWidth) {
              return SegmentedButton<String>(
                segments: [
                  for (final o in options) ButtonSegment<String>(value: o.value, label: Text(o.label)),
                ],
                selected: value == null ? <String>{} : {value!},
                emptySelectionAllowed: true,
                showSelectedIcon: false,
                onSelectionChanged: (s) {
                  if (s.isNotEmpty) onChanged(s.first);
                },
              );
            }
            return Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                for (final o in options)
                  ChoiceChip(
                    label: Text(o.label),
                    selected: o.value == value,
                    showCheckmark: false,
                    onSelected: (_) => onChanged(o.value),
                  ),
              ],
            );
          }),
          if (showHint) Hint(hint),
        ],
      ),
    );
  }
}

class OptionItem {
  const OptionItem(this.value, this.title, this.subtitle, {this.trailing});

  final String value;
  final String title;
  final String subtitle;
  final Widget? trailing;
}

/// Opções em cartão (classe .opt), uma por linha no celular.
class OptionCards extends StatelessWidget {
  const OptionCards({super.key, required this.items, required this.value, required this.onChanged});

  final List<OptionItem> items;
  final String? value;
  final ValueChanged<String> onChanged;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return LayoutBuilder(builder: (context, c) {
      final cols = c.maxWidth >= 560 ? 2 : 1;
      const gap = 8.0;
      final w = (c.maxWidth - gap * (cols - 1)) / cols;
      return Wrap(
        spacing: gap,
        runSpacing: gap,
        children: [
          for (final it in items)
            SizedBox(
              width: w,
              child: Material(
                color: it.value == value ? cs.primaryContainer.withAlpha(120) : cs.surface,
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(10),
                  side: BorderSide(color: it.value == value ? cs.primary : cs.outlineVariant),
                ),
                child: InkWell(
                  borderRadius: BorderRadius.circular(10),
                  onTap: () => onChanged(it.value),
                  child: Padding(
                    padding: const EdgeInsets.fromLTRB(10, 10, 12, 10),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Icon(
                          it.value == value ? Icons.radio_button_checked : Icons.radio_button_unchecked,
                          size: 20,
                          color: it.value == value ? cs.primary : cs.outline,
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(it.title, style: const TextStyle(fontWeight: FontWeight.w600)),
                              if (it.subtitle.isNotEmpty)
                                Text(it.subtitle,
                                    style: TextStyle(fontSize: 12.5, color: cs.onSurfaceVariant)),
                            ],
                          ),
                        ),
                        if (it.trailing != null) it.trailing!,
                      ],
                    ),
                  ),
                ),
              ),
            ),
        ],
      );
    });
  }
}

/// Lista de seleção (select da página). Um valor fora da lista vira item extra,
/// assim um valor salvo pelo console nunca derruba a tela.
class DropField<T> extends StatelessWidget {
  const DropField({
    super.key,
    required this.label,
    required this.items,
    required this.value,
    required this.onChanged,
    this.helper,
  });

  final String label;
  final List<MapEntry<T, String>> items;
  final T? value;
  final ValueChanged<T> onChanged;
  final String? helper;

  @override
  Widget build(BuildContext context) {
    final list = [...items];
    if (value != null && !list.any((e) => e.key == value)) {
      list.add(MapEntry(value as T, '$value'));
    }
    // rótulo em cima (quebra linha em vez de ser cortado); a lista aberta mostra o texto inteiro
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          FieldLabel(label),
          Semantics(
            label: label,
            child: InputDecorator(
              decoration: fieldDecoration(context),
              child: DropdownButtonHideUnderline(
                child: DropdownButton<T>(
                  value: value,
                  isExpanded: true,
                  isDense: true,
                  selectedItemBuilder: (_) => [
                    for (final e in list)
                      // "SB1 · canais 8–15 · 916,8–918,2 MHz" não cabe inteiro num celular estreito:
                      // diminui a fonte em vez de cortar
                      FittedBox(
                        fit: BoxFit.scaleDown,
                        alignment: Alignment.centerLeft,
                        child: Text(e.value,
                            maxLines: 1,
                            softWrap: false,
                            style: const TextStyle(fontFamily: monoFont, fontSize: 13.5)),
                      ),
                  ],
                  items: [
                    for (final e in list)
                      DropdownMenuItem<T>(
                        value: e.key,
                        // a lista aberta mostra o texto inteiro (quebra a linha, nunca corta)
                        child: Text(e.value,
                            style: const TextStyle(fontFamily: monoFont, fontSize: 13.5, height: 1.2)),
                      ),
                  ],
                  onChanged: (v) {
                    if (v != null) onChanged(v);
                  },
                ),
              ),
            ),
          ),
          if (helper != null && helper!.isNotEmpty) Hint(helper!),
        ],
      ),
    );
  }
}

/// Faixa AU915: 64 canais de 125 kHz e 8 de 500 kHz (igual ao SVG da página).
class BandChart extends StatelessWidget {
  const BandChart({super.key, required this.on, required this.caption});

  /// Canais ligados, 0..71 (64..71 = 500 kHz).
  final Set<int> on;
  final String caption;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Container(
      margin: const EdgeInsets.only(top: 4, bottom: 12),
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 6),
      decoration: BoxDecoration(
        color: cs.surfaceContainerHighest.withAlpha(90),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: cs.outlineVariant),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(caption, style: TextStyle(fontSize: 12.5, color: cs.onSurfaceVariant)),
          const SizedBox(height: 8),
          SizedBox(
            height: 46,
            width: double.infinity,
            child: CustomPaint(
              painter: _BandPainter(
                on,
                AppColors.accentOf(context),
                AppColors.warnOf(context),
                cs.onSurfaceVariant,
              ),
            ),
          ),
          const SizedBox(height: 2),
          DefaultTextStyle(
            style: TextStyle(fontFamily: monoFont, fontSize: 11, color: cs.onSurfaceVariant),
            child: const Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [Text('915'), Text('918'), Text('921'), Text('924'), Text('928 MHz')],
            ),
          ),
        ],
      ),
    );
  }
}

class _BandPainter extends CustomPainter {
  _BandPainter(this.on, this.acc, this.warn, this.base);

  final Set<int> on;
  final Color acc;
  final Color warn;
  final Color base;

  @override
  void paint(Canvas canvas, Size size) {
    final sx = size.width / 1000, sy = size.height / 46;
    double x(double mhz) => (mhz - 915) / 13 * 1000;
    final line = Paint()
      ..color = base.withAlpha(77)
      ..strokeWidth = 1;
    canvas.drawLine(Offset(0, 40 * sy), Offset(size.width, 40 * sy), line);
    for (var i = 0; i < 64; i++) {
      final o = on.contains(i);
      final p = Paint()..color = o ? acc : base.withAlpha(64);
      final r = Rect.fromLTWH((x(915.2 + .2 * i) - 2.2) * sx, (o ? 8 : 26) * sy, 4.4 * sx, (o ? 32 : 14) * sy);
      canvas.drawRRect(RRect.fromRectAndRadius(r, const Radius.circular(1)), p);
    }
    for (var i = 0; i < 8; i++) {
      final o = on.contains(64 + i);
      final p = Paint()..color = o ? warn.withAlpha(230) : base.withAlpha(46);
      final r = Rect.fromLTWH((x(915.9 + 1.6 * i) - 9) * sx, (o ? 4 : 30) * sy, 18 * sx, (o ? 36 : 10) * sy);
      canvas.drawRRect(RRect.fromRectAndRadius(r, const Radius.circular(2)), p);
    }
  }

  @override
  bool shouldRepaint(covariant _BandPainter old) =>
      old.on.length != on.length || !old.on.containsAll(on) || old.acc != acc;
}

/// Linha de chips no topo (MQTT/LoRaWAN, IP, ID, Plano).
class ChipInfo {
  const ChipInfo(this.label, this.value, {this.level, this.copy = false});

  final String label;
  final String value;
  final Level? level;

  /// Valor em hexadecimal (EUI, DevAddr): tocar no chip copia o valor.
  final bool copy;
}

class InfoChips extends StatelessWidget {
  const InfoChips(this.chips, {super.key});

  final List<ChipInfo> chips;

  @override
  Widget build(BuildContext context) {
    return Wrap(
      spacing: 8,
      runSpacing: 6,
      children: [
        for (final c in chips) _chip(context, c),
      ],
    );
  }

  Widget _chip(BuildContext context, ChipInfo c) {
    final canCopy = c.copy && isHexValue(c.value);
    final body = Container(
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
            decoration: BoxDecoration(
              color: Colors.white.withAlpha(18),
              borderRadius: BorderRadius.circular(999),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (c.level != null) ...[
                  Container(
                    width: 8,
                    height: 8,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: c.level == Level.none ? const Color(0xFF7B8794) : levelColor(context, c.level!),
                    ),
                  ),
                  const SizedBox(width: 7),
                ],
                Text(c.label, style: const TextStyle(fontSize: 12.5, color: Color(0xFFE8F1EE))),
                if (c.value.isNotEmpty) ...[
                  const SizedBox(width: 5),
                  Text(c.value,
                      style: const TextStyle(
                          fontSize: 12.5,
                          fontFamily: monoFont,
                          fontWeight: FontWeight.w600,
                          color: Colors.white)),
                ],
                if (canCopy) ...[
                  const SizedBox(width: 6),
                  const Icon(Icons.copy, size: 13, color: Color(0xFFE8F1EE)),
                ],
              ],
            ),
          );
    if (!canCopy) return body;
    return Tooltip(
      message: 'Copiar ${c.label}',
      child: InkWell(
        borderRadius: BorderRadius.circular(999),
        onTap: () => copyHex(context, c.value, c.label),
        child: body,
      ),
    );
  }
}

// ------------------------------------------------------------------ Copiar
// Todo valor em hexadecimal (EUI, DevAddr, chaves, NetID, payload, comando)
// tem um "Copiar", como nas páginas web.

/// Valor pronto para colar no ChirpStack: sem espaços, ":" ou "-", sem "0x" e
/// em maiúsculas. Se não for hexadecimal (ex.: payload em texto), copia como está.
String hexForCopy(String v) {
  final t = v.trim();
  final c = t.replaceFirst(RegExp(r'^0x', caseSensitive: false), '').replaceAll(RegExp(r'[\s:,\-]'), '');
  return c.isNotEmpty && RegExp(r'^[0-9A-Fa-f]+$').hasMatch(c) ? c.toUpperCase() : t;
}

/// Texto que é um valor hexadecimal (com ou sem espaços, ":" ou "-"): "sem sessão", "-" etc. não são.
bool isHexValue(String v) {
  final c = v.trim().replaceFirst(RegExp(r'^0x', caseSensitive: false), '').replaceAll(RegExp(r'[\s:,\-]'), '');
  return c.isNotEmpty && RegExp(r'^[0-9A-Fa-f]+$').hasMatch(c);
}

/// Copia [value] (normalizado com [hexForCopy] se [hex]) e avisa com um snack.
Future<void> copyHex(BuildContext context, String value, String name, {bool hex = true}) async {
  final v = hex ? hexForCopy(value) : value.trim();
  if (v.isEmpty || v == '-') {
    snack(context, '$name está vazio.');
    return;
  }
  await Clipboard.setData(ClipboardData(text: v));
  if (context.mounted) snack(context, '$name copiado: $v');
}

/// Botão Copiar ao lado de um campo em hexadecimal (use em `trailing` do [TextFieldX]).
/// Ícone, como os outros botões ao lado dos campos (Inverter, Do MAC), para caber no celular.
class CopyHexButton extends StatelessWidget {
  const CopyHexButton({super.key, required this.value, required this.name, this.hex = true});

  /// Lido na hora do toque (o campo pode ter mudado).
  final String Function() value;
  final String name;

  /// false: copia o texto como está (payload em modo Texto).
  final bool hex;

  @override
  Widget build(BuildContext context) => IconButton(
        tooltip: 'Copiar $name',
        onPressed: () => copyHex(context, value(), name, hex: hex),
        icon: const Icon(Icons.copy),
      );
}

/// Ícone pequeno de copiar (cards de status, valores só de leitura).
class CopyIcon extends StatelessWidget {
  const CopyIcon({super.key, required this.value, required this.name, this.hex = true});

  final String Function() value;
  final String name;
  final bool hex;

  @override
  Widget build(BuildContext context) => IconButton(
        tooltip: 'Copiar $name',
        visualDensity: VisualDensity.compact,
        padding: EdgeInsets.zero,
        constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
        iconSize: 16,
        icon: const Icon(Icons.copy),
        onPressed: () => copyHex(context, value(), name, hex: hex),
      );
}

/// Barra inferior com "alterações não salvas" + botões (classe .bar).
class SaveBar extends StatelessWidget {
  const SaveBar({
    super.key,
    required this.dirty,
    required this.saving,
    required this.onSave,
    required this.onReboot,
    required this.rebootLabel,
    this.onRead,
  });

  final bool dirty;
  final bool saving;
  final VoidCallback onSave;
  final VoidCallback onReboot;
  final String rebootLabel;
  /// "Ler do nó": lê de novo a configuração gravada (opcional)
  final VoidCallback? onRead;

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Material(
      elevation: 6,
      color: cs.surface,
      child: SafeArea(
        top: false,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(14, 10, 14, 10),
          child: LayoutBuilder(builder: (context, c) {
            final text = dirty ? 'Alterações não salvas' : 'Nenhuma alteração';
            final dot = Tooltip(
              message: text,
              child: Container(
                width: 8,
                height: 8,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: dirty ? AppColors.warnOf(context) : cs.outlineVariant,
                ),
              ),
            );
            final reboot = OutlinedButton(
              onPressed: saving ? null : onReboot,
              child: FittedBox(fit: BoxFit.scaleDown, child: Text(rebootLabel, maxLines: 1)),
            );
            final read = onRead == null
                ? null
                : OutlinedButton(
                    onPressed: saving ? null : onRead,
                    child: const FittedBox(fit: BoxFit.scaleDown, child: Text('Ler do nó', maxLines: 1)),
                  );
            final save = FilledButton(
              onPressed: saving ? null : onSave,
              child: saving
                  ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                  : const FittedBox(fit: BoxFit.scaleDown, child: Text('Salvar', maxLines: 1)),
            );
            // celular estreito (ou fonte grande): só a bolinha, e os dois botões dividem a largura
            // (como a página faz abaixo de 560 px), sem espremer o texto em várias linhas
            if (c.maxWidth < 380) {
              return Row(children: [
                dot,
                const SizedBox(width: 10),
                if (read != null) ...[Expanded(child: read), const SizedBox(width: 8)],
                Expanded(child: reboot),
                const SizedBox(width: 8),
                Expanded(child: save),
              ]);
            }
            return Row(
              children: [
                dot,
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    text,
                    style: TextStyle(fontSize: 13, color: dirty ? cs.onSurface : cs.onSurfaceVariant),
                  ),
                ),
                if (read != null) ...[read, const SizedBox(width: 8)],
                reboot,
                const SizedBox(width: 8),
                save,
              ],
            );
          }),
        ),
      ),
    );
  }
}

// ----------------------------- diálogos -----------------------------

Future<bool> confirmDialog(BuildContext context, String title, String message,
    {String confirm = 'Confirmar', bool danger = false}) async {
  final r = await showDialog<bool>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(title),
      content: Text(message),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancelar')),
        FilledButton(
          style: danger ? FilledButton.styleFrom(backgroundColor: AppColors.errOf(c)) : null,
          onPressed: () => Navigator.pop(c, true),
          child: Text(confirm),
        ),
      ],
    ),
  );
  return r ?? false;
}

Future<void> messageDialog(BuildContext context, String title, String message,
    {List<Widget>? actions}) {
  return showDialog<void>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(title),
      content: SingleChildScrollView(child: Text(message)),
      actions: actions ?? [TextButton(onPressed: () => Navigator.pop(c), child: const Text('OK'))],
    ),
  );
}

void snack(BuildContext context, String text) {
  final m = ScaffoldMessenger.maybeOf(context);
  m?.hideCurrentSnackBar();
  m?.showSnackBar(SnackBar(content: Text(text)));
}

/// Erros de validação (caixa vermelha #err da página).
class ErrorBox extends StatelessWidget {
  const ErrorBox(this.errors, {super.key});

  final List<String> errors;

  @override
  Widget build(BuildContext context) {
    if (errors.isEmpty) return const SizedBox.shrink();
    final c = AppColors.errOf(context);
    return Container(
      width: double.infinity,
      margin: const EdgeInsets.only(bottom: 14),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: c.withAlpha(28),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: c.withAlpha(90)),
      ),
      child: Text(errors.join('\n'), style: TextStyle(color: c, fontSize: 13.5)),
    );
  }
}

/// Rodapé "Desenvolvido por AdailSilva".
class DevFooter extends StatelessWidget {
  const DevFooter(this.product, {super.key});

  final String product;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 18),
      child: Center(
        child: Text.rich(
          TextSpan(
            text: '$product · Desenvolvido por ',
            children: const [TextSpan(text: 'AdailSilva', style: TextStyle(fontWeight: FontWeight.w600))],
          ),
          textAlign: TextAlign.center,
          style: TextStyle(fontSize: 12.5, color: Theme.of(context).colorScheme.onSurfaceVariant),
        ),
      ),
    );
  }
}
