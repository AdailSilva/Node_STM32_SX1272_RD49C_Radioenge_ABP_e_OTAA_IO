import 'package:flutter/material.dart';

/// Mesmas cores das páginas web dos firmwares.
class AppColors {
  static const accent = Color(0xFF0B7A5F);
  static const accentDark = Color(0xFF35B98F);
  static const header = Color(0xFF0E2A24);
  static const ok = Color(0xFF1F9D55);
  static const okDark = Color(0xFF3FBF7F);
  static const warn = Color(0xFFC27803);
  static const warnDark = Color(0xFFE3A33B);
  static const err = Color(0xFFC0362C);
  static const errDark = Color(0xFFF07A6E);
  static const term = Color(0xFF0F1513);
  static const termFg = Color(0xFFCFE3DA);
  static const termErr = Color(0xFFFF8A7A);
  static const termWarn = Color(0xFFFFD166);
  static const termOk = Color(0xFF7FD4B0);
  static const termInfo = Color(0xFF8EC5FF);

  static bool dark(BuildContext c) => Theme.of(c).brightness == Brightness.dark;
  static Color okOf(BuildContext c) => dark(c) ? okDark : ok;
  static Color warnOf(BuildContext c) => dark(c) ? warnDark : warn;
  static Color errOf(BuildContext c) => dark(c) ? errDark : err;
  static Color accentOf(BuildContext c) => dark(c) ? accentDark : accent;
}

const String monoFont = 'monospace';

ThemeData buildTheme(Brightness b) {
  final scheme = ColorScheme.fromSeed(
    seedColor: AppColors.accent,
    brightness: b,
  ).copyWith(primary: b == Brightness.dark ? AppColors.accentDark : AppColors.accent);
  return ThemeData(
    useMaterial3: true,
    colorScheme: scheme,
    scaffoldBackgroundColor:
        b == Brightness.dark ? const Color(0xFF0F151B) : const Color(0xFFF2F4F6),
    appBarTheme: const AppBarTheme(
      backgroundColor: AppColors.header,
      foregroundColor: Color(0xFFE8F1EE),
      elevation: 0,
      centerTitle: false,
    ),
    visualDensity: VisualDensity.standard,
  );
}
