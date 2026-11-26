#!/usr/bin/env node
// =============================================================================
//  Testes do codec ADAILSILVA_V1 (codec_adailsilva_v1_chirpstack_v4.js e a versão
//  do ChirpStack v3, codec_adailsilva_v1_chirpstack_v3.js) com os
//  vetores da seção 6 do doc/PROTOCOLO_ADAILSILVA_V1.md, os objetos da seção 7 e
//  amostras das portas legadas 2, 5 e 103.
//
//    node doc/codec/teste_codec.js
//
//  Sai com código 1 se algum teste falhar.
// =============================================================================
"use strict";
const assert = require("assert");
const path = require("path");
const { decodeUplink, encodeDownlink } = require(path.join(__dirname, "codec_adailsilva_v1_chirpstack_v4.js"));
const v3 = require(path.join(__dirname, "codec_adailsilva_v1_chirpstack_v3.js"));

const hexToBytes = h => h.replace(/\s+/g, "").match(/../g).map(x => parseInt(x, 16));
const bytesToHex = b => b.map(x => x.toString(16).padStart(2, "0")).join(" ").toUpperCase();
const b64 = b => Buffer.from(b).toString("base64");

let ok = 0, fail = 0;
function t(name, fn) {
  try { fn(); ok++; console.log("  ok    " + name); }
  catch (e) { fail++; console.log("  FALHA " + name + "\n        " + String(e.message).split("\n").join("\n        ")); }
}

// ------------------------------------------------------------------ seção 6: downlinks
console.log("Seção 6 — downlinks (encodeDownlink, porta 10)");
const DL = [
  ["DL seq 7, RELE_DEFINIR relé 0 ligar", { seq: 7, comando: "RELE_DEFINIR", canal: 0, acao: "LIGAR" }, "07 20 00 01", "ByAAAQ=="],
  ["DL seq 8, RELE_PULSO relé 1 ligado por 5000 ms", { seq: 8, comando: "RELE_PULSO", canal: 1, estado: 1, duracao_ms: 5000 },
    "08 21 01 01 00 00 13 88", "CCEBAQAAE4g="],
  ["DL seq 9, RELE_MASCARA máscara 0x0003, valores 0x0001", { seq: 9, comando: "RELE_MASCARA", mascara: 0x0003, valores: 0x0001 },
    "09 22 00 03 00 01", "CSIAAwAB"],
  ["DL seq 10, PZEM_LER medidor 0", { seq: 10, comando: "PZEM_LER", canal: 0 }, "0A 30 00", "CjAA"],
  ["DL seq 11, INTERVALO 300 s", { seq: 11, comando: "INTERVALO", segundos: 300 }, "0B 02 00 00 01 2C", "CwIAAAEs"],
];
for (const [name, data, h, base64] of DL) {
  t(name, () => {
    const r = encodeDownlink({ data });
    assert.ok(!r.errors || !r.errors.length, "erros: " + (r.errors || []).join(", "));
    assert.strictEqual(bytesToHex(r.bytes), h);
    assert.strictEqual(b64(r.bytes), base64);
  });
}
t("seção 7: encodeDownlink do exemplo { seq 7, RELE_DEFINIR, canal 0, LIGAR }", () => {
  const r = encodeDownlink({ data: { seq: 7, comando: "RELE_DEFINIR", canal: 0, acao: "LIGAR" } });
  assert.deepStrictEqual(r.bytes, [0x07, 0x20, 0x00, 0x01]);
});

// ------------------------------------------------------------------ seção 6 e 7: uplinks
console.log("Seções 6 e 7 — uplinks (decodeUplink)");
t("UL resposta seq 7: OK, 2 relés, mapa 0x0001 (porta 11)", () => {
  const b = hexToBytes("07 20 00 02 00 01");
  assert.strictEqual(b64(b), "ByAAAgAB");
  const r = decodeUplink({ fPort: 11, bytes: b });
  assert.deepStrictEqual(r.data, {
    resposta: { seq: 7, opcode: 32, opcodeNome: "RELE_DEFINIR", status: 0, statusNome: "OK",
                reles: { quantidade: 2, estados: [true, false] } } });
});
t("UL telemetria: relés + PZEM 0 + BOOT energização (porta 20)", () => {
  const b = hexToBytes("01 00 03 02 00 01 10 00 12 08 FC 00 00 01 F4 00 00 04 B0 00 00 30 39 02 58 5F 00 31 00 01 01");
  assert.strictEqual(b64(b), "AQADAgABEAASCPwAAAH0AAAEsAAAMDkCWF8AMQABAQ==");
  const r = decodeUplink({ fPort: 20, bytes: b });
  assert.deepStrictEqual(r.data, {
    reles: { quantidade: 2, estados: [true, false] },
    pzem: [{ canal: 0, tensao: 230.0, corrente: 0.5, potencia: 120.0, energia: 12345, frequencia: 60.0, fp: 0.95, alarme: false }],
    boot: { motivo: 1, motivoNome: "ENERGIZACAO" } });
  assert.deepStrictEqual(r.warnings, []);
});

// ------------------------------------------------------------------ outros casos do protocolo
console.log("Outros casos do protocolo");
t("telemetria com PZEM_FALHA, ALIMENTACAO e tipo desconhecido (pulado pelo tamanho)", () => {
  const b = hexToBytes("11 01 00  7F 00 02 AA BB  30 00 02 0C E4");
  const r = decodeUplink({ fPort: 20, bytes: b });
  assert.deepStrictEqual(r.data, { pzemFalha: [1], alimentacao: { milivolts: 3300, tensao: 3.3 } });
  assert.strictEqual(r.warnings.length, 1);
});
t("telemetria dividida em dois uplinks (blocos inteiros) decodifica as duas partes", () => {
  const a = decodeUplink({ fPort: 20, bytes: hexToBytes("01 00 03 02 00 01") }).data;
  const c = decodeUplink({ fPort: 20, bytes: hexToBytes("10 00 12 08 FC 00 00 01 F4 00 00 04 B0 00 00 30 39 02 58 5F 00") }).data;
  assert.deepStrictEqual(a.reles.estados, [true, false]);
  assert.strictEqual(c.pzem[0].energia, 12345);
});
t("resposta STATUS (19 bytes)", () => {
  const b = hexToBytes("05 01 00  01 00 01 00  00 00 0E 10  00 00 01 2C  43 00 02 00 02 01 01");
  const r = decodeUplink({ fPort: 11, bytes: b }).data.resposta;
  assert.deepStrictEqual(r.estado, { versaoProtocolo: 1, firmware: "0.1.0", uptime: 3600, intervalo: 300, classe: "C",
    confirmado: false, reles: { quantidade: 2, estados: [false, true] }, pzemQuantidade: 1, boot: { motivo: 1, motivoNome: "ENERGIZACAO" } });
});
t("resposta truncada a 3 bytes (não coube no DR)", () => {
  const r = decodeUplink({ fPort: 11, bytes: hexToBytes("0A 30 00") }).data.resposta;
  assert.strictEqual(r.opcodeNome, "PZEM_LER");
  assert.strictEqual(r.truncada, true);
});
t("resposta PZEM_LER completa e FALHA_PERIFERICO", () => {
  const r = decodeUplink({ fPort: 11, bytes: hexToBytes("0A 30 00 00 08 FC 00 00 01 F4 00 00 04 B0 00 00 30 39 02 58 5F 00") }).data.resposta;
  assert.strictEqual(r.pzem.tensao, 230);
  const f = decodeUplink({ fPort: 11, bytes: hexToBytes("0B 30 04") }).data.resposta;
  assert.strictEqual(f.statusNome, "FALHA_PERIFERICO");
  assert.strictEqual(f.truncada, undefined);
});
t("encodeDownlink: STATUS, ENVIAR_AGORA, CONFIRMADO, CLASSE, REINICIAR, ZERAR, LIMITE", () => {
  const e = d => bytesToHex(encodeDownlink({ data: d }).bytes);
  assert.strictEqual(e({ seq: 1, comando: "STATUS" }), "01 01");
  assert.strictEqual(e({ seq: 2, comando: "ENVIAR_AGORA" }), "02 03");
  assert.strictEqual(e({ seq: 3, comando: "CONFIRMADO", confirmado: true }), "03 04 01");
  assert.strictEqual(e({ seq: 4, comando: "CLASSE", classe: "C" }), "04 05 43");
  assert.strictEqual(e({ seq: 5, comando: "REINICIAR", atraso_ms: 2000 }), "05 06 07 D0");
  assert.strictEqual(e({ seq: 6, comando: "PZEM_ZERAR_ENERGIA", canal: 0 }), "06 31 00");
  assert.strictEqual(e({ seq: 7, comando: "PZEM_LIMITE_POTENCIA", canal: 0, watts: 2300 }), "07 32 00 08 FC");
  assert.strictEqual(e({ seq: 8, comando: "RELE_DEFINIR", canal: "todos", acao: "ALTERNAR" }), "08 20 FF 02");
});
t("encodeDownlink: erros (classe B, intervalo fora da faixa, comando desconhecido)", () => {
  assert.ok(encodeDownlink({ data: { seq: 1, comando: "CLASSE", classe: "B" } }).errors.length);
  assert.ok(encodeDownlink({ data: { seq: 1, comando: "INTERVALO", segundos: 4 } }).errors.length);
  assert.ok(encodeDownlink({ data: { seq: 1, comando: "XYZ" } }).errors.length);
});

// ------------------------------------------------------------------ portas legadas
console.log("Portas legadas (2, 5, 103)");
t("porta 2: interrupção de GPIO (8 bytes, Tabela 13)", () => {
  const r = decodeUplink({ fPort: 2, bytes: hexToBytes("C8 00 04 00 10 F9 FF 9C") });
  assert.deepStrictEqual(r.data, { interrupcao: { bateria: { nivel: 200, tensao: 3.22 }, gpio: 4, entradas: 0x0010, snr: -7, rssi: -100 } });
});
t("porta 5: keepalive (16 bytes, Tabela 9)", () => {
  const r = decodeUplink({ fPort: 5, bytes: hexToBytes("00 00 FF 00 01 07 FF 00 00 00 00 08 00 05 FF 88") });
  assert.deepStrictEqual(r.data, { keepalive: { bateria: { nivel: 0, fonte: "externa" }, entradas: 1, adc: [2047, 0, 0, 2048],
                                                snr: 5, rssi: -120 } });
});
t("porta 103: respostas de AT em linhas", () => {
  const b = Array.from(Buffer.from("AT OK\n2:0001\nAT OK"));
  assert.deepStrictEqual(decodeUplink({ fPort: 103, bytes: b }).data, { at: { linhas: ["AT OK", "2:0001", "AT OK"] } });
});

// ------------------------------------------------------------------ ChirpStack v3 (Decode / Encode)
console.log("ChirpStack v3 — codec_adailsilva_v1_chirpstack_v3.js dá o mesmo resultado do v4");
t("v3 Encode = v4 encodeDownlink (vetores da seção 6)", () => {
  for (const [, data, h] of DL) assert.strictEqual(bytesToHex(v3.Encode(10, data)), h);
});
t("v3 Decode = v4 decodeUplink (resposta, telemetria e portas legadas)", () => {
  const casos = [
    [11, "07 20 00 02 00 01"],
    [20, "01 00 03 02 00 01"],
    [20, "10 00 12 08 FC 00 00 01 F4 00 00 04 B0 00 00 30 39 02 58 5F 00"],
    [101, "48 65 6C 6C 6F"],
  ];
  for (const [port, h] of casos)
    assert.deepStrictEqual(v3.Decode(port, hexToBytes(h), {}), decodeUplink({ fPort: port, bytes: hexToBytes(h) }).data);
});
t("v3 Encode com comando inválido lança erro", () => {
  assert.throws(() => v3.Encode(10, { seq: 1, comando: "NAO_EXISTE" }));
});

// ------------------------------------------------------------------ Modbus (seção 5.3)
console.log("Modbus CRC-16 da seção 5.3 (o mesmo algoritmo do app/node_pzem.c)");
function crc16(b) { let c = 0xFFFF; for (const x of b) { c ^= x; for (let i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1; } return c; }
const withCrc = b => { const c = crc16(b); return bytesToHex(b.concat([c & 255, c >> 8])); };
t("ler 10 registradores: F8 04 00 00 00 0A 64 64", () => assert.strictEqual(withCrc(hexToBytes("F8 04 00 00 00 0A")), "F8 04 00 00 00 0A 64 64"));
t("zerar energia: F8 42 C2 41", () => assert.strictEqual(withCrc(hexToBytes("F8 42")), "F8 42 C2 41"));
t("limite 2300 W: F8 06 00 01 08 FC CB E2", () => assert.strictEqual(withCrc(hexToBytes("F8 06 00 01 08 FC")), "F8 06 00 01 08 FC CB E2"));

console.log(`\n${ok} ok, ${fail} falha(s)`);
process.exit(fail ? 1 : 0);
