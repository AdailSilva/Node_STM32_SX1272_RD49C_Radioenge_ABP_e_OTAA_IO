// Salvar (exportar configuração, logs) e abrir arquivos (importar, certificados).
import 'dart:convert';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';

class FileIO {
  FileIO._();

  /// Abre o seletor "Salvar como". Devolve o caminho, ou null se cancelado.
  static Future<String?> saveBytes(String fileName, Uint8List bytes) {
    return FilePicker.platform.saveFile(
      dialogTitle: 'Salvar $fileName',
      fileName: fileName,
      bytes: bytes,
    );
  }

  static Future<String?> saveText(String fileName, String text) =>
      saveBytes(fileName, Uint8List.fromList(utf8.encode(text)));

  /// Lê um arquivo de texto escolhido pelo usuário (null = cancelado).
  static Future<String?> pickText({List<String>? extensions}) async {
    final r = await FilePicker.platform.pickFiles(
      type: extensions == null ? FileType.any : FileType.custom,
      allowedExtensions: extensions,
      withData: true,
    );
    if (r == null || r.files.isEmpty) return null;
    final b = r.files.first.bytes;
    if (b == null) return null;
    return utf8.decode(b, allowMalformed: true);
  }
}
