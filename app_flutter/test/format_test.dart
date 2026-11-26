import 'package:flutter_test/flutter_test.dart';
import 'package:radioenge_node_atuadores_sensores_app/core/node_link.dart';
import 'package:radioenge_node_atuadores_sensores_app/core/transport.dart';

void main() {
  test('hex', () {
    expect(isHex('A1b2'), true);
    expect(isHex('A1B', bytes: 2), false);
    expect(isHex('00 11 22 33', bytes: 4), true);
    expect(cleanHex('ad:a1-de ad'), 'ADA1DEAD');
    expect(textToHex('AdailSilva-IoT'), '416461696C53696C76612D496F54');
    expect(hexToText('416461696C53696C76612D496F54'), 'AdailSilva-IoT');
    expect(isPrintableText('41420D0A'), false);
  });

  test('uptime', () {
    expect(fmtUptime(59), '00:00:59');
    expect(fmtUptime(90061), '1 d 01:01:01');
  });

  test('chunks', () {
    final c = chunked(List<int>.generate(45, (i) => i), 20);
    expect(c.length, 3);
    expect(c.last.length, 5);
  });
}
