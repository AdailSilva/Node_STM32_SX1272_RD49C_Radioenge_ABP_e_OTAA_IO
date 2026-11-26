// Terminal sem quebra de linha (logs e diagnóstico). Cada linha do
// equipamento ocupa uma linha só na tela: a fonte diminui (de 10 até 8)
// para a linha mais longa caber na largura; se nem assim couber, as linhas
// longas rolam para o lado (arrastar na horizontal) em vez de quebrar.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'theme.dart';

/// Tamanho da fonte dos terminais (logs e diagnóstico): máximo e mínimo.
const double termFontSize = 10;
const double termMinFontSize = 8;
const double termLineHeight = 1.35;

const TextStyle termTextStyle = TextStyle(
  fontFamily: monoFont,
  fontSize: termFontSize,
  height: termLineHeight,
);

class TermLines extends StatefulWidget {
  const TermLines({super.key, required this.lines, required this.colorOf, this.controller});

  /// Linhas já separadas (sem '\n').
  final List<String> lines;

  /// Cor de cada linha; null usa a cor padrão do terminal.
  final Color? Function(String line) colorOf;

  /// Controle da rolagem vertical (rolar automático para o fim).
  final ScrollController? controller;

  @override
  State<TermLines> createState() => _TermLinesState();
}

class _TermLinesState extends State<TermLines> {
  final ScrollController _h = ScrollController();

  @override
  void dispose() {
    _h.dispose();
    super.dispose();
  }

  /// Largura de um caractere na fonte do terminal (já com a escala de texto do sistema).
  double _charWidth(BuildContext context) {
    final tp = TextPainter(
      text: const TextSpan(text: '0000000000', style: termTextStyle),
      textDirection: TextDirection.ltr,
      textScaler: MediaQuery.textScalerOf(context),
    )..layout();
    final w = tp.width / 10;
    tp.dispose();
    return w;
  }

  @override
  Widget build(BuildContext context) {
    final lines = widget.lines;
    var maxLen = 0;
    for (final l in lines) {
      final n = l.length + '\t'.allMatches(l).length * 3;
      if (n > maxLen) maxLen = n;
    }
    final cw = _charWidth(context); // largura de um caractere com a fonte em termFontSize
    return LayoutBuilder(builder: (context, c) {
      // fonte que faz a linha mais longa caber, limitada entre o mínimo e o máximo
      final fit = maxLen == 0 ? termFontSize : (c.maxWidth - 12) / (maxLen * cw) * termFontSize;
      final fs = fit.clamp(termMinFontSize, termFontSize).toDouble();
      final width = math.max(c.maxWidth, maxLen * cw * fs / termFontSize + 12);
      return Scrollbar(
        controller: _h,
        child: SingleChildScrollView(
          controller: _h,
          scrollDirection: Axis.horizontal,
          child: SizedBox(
            width: width,
            height: c.maxHeight,
            child: ListView.builder(
              controller: widget.controller,
              itemCount: lines.length,
              itemBuilder: (_, i) {
                final l = lines[i];
                return Text(
                  // linha em branco: um espaço garante a altura de uma linha
                  l.isEmpty ? ' ' : l.replaceAll('\t', '    '),
                  softWrap: false,
                  maxLines: 1,
                  overflow: TextOverflow.visible,
                  style: termTextStyle.copyWith(fontSize: fs, color: widget.colorOf(l) ?? AppColors.termFg),
                );
              },
            ),
          ),
        ),
      );
    });
  }
}
