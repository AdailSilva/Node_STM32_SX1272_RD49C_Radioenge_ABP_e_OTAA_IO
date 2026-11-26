// =============================================================================
//  Codec ADAILSILVA_V1 no formato do ChirpStack v3 (Decode / Encode) para o nó
//  LoRaWAN Radioenge com atuadores e sensores (relés + PZEM-004T v3).
//
//  Mesmo código de codec_adailsilva_v1_chirpstack_v4.js (copiado abaixo) mais as
//  duas funções de entrada do v3, no fim do arquivo. No ChirpStack v4 use o
//  arquivo _v4.
//
//  ChirpStack v3: Device profiles -> (perfil) -> Codec -> "Custom JavaScript codec
//  functions" e cole este arquivo inteiro. Downlinks (Encode) na porta 10.
//
//  SPDX-License-Identifier: MIT
// =============================================================================

var OPCODES = {
  0x01: "STATUS", 0x02: "INTERVALO", 0x03: "ENVIAR_AGORA", 0x04: "CONFIRMADO", 0x05: "CLASSE",
  0x06: "REINICIAR", 0x20: "RELE_DEFINIR", 0x21: "RELE_PULSO", 0x22: "RELE_MASCARA", 0x23: "RELE_LER",
  0x30: "PZEM_LER", 0x31: "PZEM_ZERAR_ENERGIA", 0x32: "PZEM_LIMITE_POTENCIA"
};
var STATUS = ["OK", "OPCODE_DESCONHECIDO", "PARAMETRO_INVALIDO", "PERIFERICO_AUSENTE", "FALHA_PERIFERICO", "OCUPADO"];
var BOOT = ["DESCONHECIDO", "ENERGIZACAO", "SOFTWARE", "WATCHDOG", "BROWNOUT", "PINO_RESET"];

// ------------------------------------------------------------------ utilidades
function u16(b, i) { return (b[i] << 8) | b[i + 1]; }
function u32(b, i) { return ((b[i] << 24) >>> 0) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]; }
function i8(v) { return v > 127 ? v - 256 : v; }
function i16(v) { return v > 32767 ? v - 65536 : v; }
function hex(b, from, to) {
  var s = "";
  for (var i = from || 0; i < (to === undefined ? b.length : to); i++) s += (b[i] < 16 ? "0" : "") + b[i].toString(16);
  return s.toUpperCase();
}
function text(b) {
  var s = "";
  for (var i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
  try { return decodeURIComponent(escape(s)); } catch (e) { return s; }   // UTF-8
}
function printable(b) {
  for (var i = 0; i < b.length; i++) if ((b[i] < 0x20 || b[i] > 0x7e) && b[i] !== 0x0a && b[i] !== 0x0d) return false;
  return true;
}
function relays(qtd, map) {
  var e = [];
  for (var i = 0; i < qtd; i++) e.push(((map >> i) & 1) === 1);
  return { quantidade: qtd, estados: e };
}
function boot(m) { return { motivo: m, motivoNome: BOOT[m] || "DESCONHECIDO" }; }
// bloco PZEM de 18 bytes (5.2)
function pzemBlock(b, i, canal) {
  return {
    canal: canal,
    tensao: u16(b, i) / 10,
    corrente: u32(b, i + 2) / 1000,
    potencia: u32(b, i + 6) / 10,
    energia: u32(b, i + 10),
    frequencia: u16(b, i + 14) / 10,
    fp: b[i + 16] / 100,
    alarme: b[i + 17] !== 0
  };
}
// nível de bateria do firmware original (AT+BAT): 0 = externa, 1 = 1,8 V ... 254 = 3,6 V, 255 = sem medida
function battery(n) {
  var o = { nivel: n };
  if (n >= 1 && n <= 254) o.tensao = Math.round((1.8 + (n - 1) * 1.8 / 253) * 100) / 100;
  else o.fonte = n === 0 ? "externa" : "sem medida";
  return o;
}

// ------------------------------------------------------------------ porta 20
function decodeTelemetry(b, warnings) {
  var d = {}, i = 0;
  while (i + 3 <= b.length) {
    var t = b[i], ch = b[i + 1], len = b[i + 2], v = i + 3;
    if (v + len > b.length) { warnings.push("bloco 0x" + hex([t]) + " cortado"); break; }
    if (t === 0x01 && len >= 3) d.reles = relays(b[v], u16(b, v + 1));
    else if (t === 0x02 && len >= 3) d.entradas = relays(b[v], u16(b, v + 1));
    else if (t === 0x10 && len >= 18) (d.pzem = d.pzem || []).push(pzemBlock(b, v, ch));
    else if (t === 0x11) (d.pzemFalha = d.pzemFalha || []).push(ch);
    else if (t === 0x30 && len >= 2) d.alimentacao = { milivolts: u16(b, v), tensao: u16(b, v) / 1000 };
    else if (t === 0x31 && len >= 1) d.boot = boot(b[v]);
    else warnings.push("tipo TLV 0x" + hex([t]) + " desconhecido (" + len + " bytes): ignorado");
    i = v + len;
  }
  return d;
}

// ------------------------------------------------------------------ porta 11
function decodeReply(b, warnings) {
  if (b.length < 3) { warnings.push("resposta com menos de 3 bytes"); return { resposta: { hex: hex(b) } }; }
  var op = b[1], st = b[2], r = {
    seq: b[0], opcode: op, opcodeNome: OPCODES[op] || "DESCONHECIDO", status: st, statusNome: STATUS[st] || "DESCONHECIDO"
  };
  var d = 3, n = b.length - 3, need = 0;
  if (st === 0) {
    if (op === 0x01) {
      need = 19;
      if (n >= 19) r.estado = {
        versaoProtocolo: b[d], firmware: b[d + 1] + "." + b[d + 2] + "." + b[d + 3],
        uptime: u32(b, d + 4), intervalo: u32(b, d + 8), classe: String.fromCharCode(b[d + 12]),
        confirmado: b[d + 13] === 1, reles: relays(b[d + 14], u16(b, d + 15)), pzemQuantidade: b[d + 17], boot: boot(b[d + 18])
      };
    } else if (op === 0x02) {
      need = 4;
      if (n >= 4) r.intervalo = u32(b, d);
    } else if (op >= 0x20 && op <= 0x23) {
      need = 3;
      if (n >= 3) r.reles = relays(b[d], u16(b, d + 1));
    } else if (op === 0x30) {
      need = 19;
      if (n >= 19) r.pzem = pzemBlock(b, d + 1, b[d]);
    }
    if (need && n < need) r.truncada = true;   // não coube no DR: os dados vêm na telemetria
  }
  return { resposta: r };
}

// ------------------------------------------------------------------ legado
function decodeIrq(b) {
  if (b.length < 8) return { hex: hex(b) };
  return { interrupcao: { bateria: battery(b[0]), gpio: b[2], entradas: u16(b, 3), snr: i8(b[5]), rssi: i16(u16(b, 6)) } };
}
function decodeKeepalive(b) {
  if (b.length < 16) return { hex: hex(b) };
  return { keepalive: { bateria: battery(b[0]), entradas: u16(b, 3), adc: [u16(b, 5), u16(b, 7), u16(b, 9), u16(b, 11)],
                        snr: i8(b[13]), rssi: i16(u16(b, 14)) } };
}
function decodeAt(b) {
  var lines = text(b).split(/\r?\n/).filter(function (l) { return l.length > 0; });
  return { at: { linhas: lines } };
}

function decodeUplink(input) {
  var b = input.bytes || [], port = input.fPort, warnings = [], data;
  if (port === 20) data = decodeTelemetry(b, warnings);
  else if (port === 11) data = decodeReply(b, warnings);
  else if (port === 2) data = decodeIrq(b);
  else if (port === 5) data = decodeKeepalive(b);
  else if (port === 103) data = decodeAt(b);
  else if (port === 4) data = { serial: printable(b) ? { hex: hex(b), texto: text(b) } : { hex: hex(b) } };
  else if (port === 101) data = { payload: printable(b) ? { hex: hex(b), texto: text(b) } : { hex: hex(b) } };
  else data = { hex: hex(b) };
  return { data: data, warnings: warnings };
}

// ------------------------------------------------------------------ downlink (porta 10)
// Parâmetros de cada comando (nomes aceitos no objeto de entrada)
var COMANDOS = {
  STATUS: 0x01, INTERVALO: 0x02, ENVIAR_AGORA: 0x03, CONFIRMADO: 0x04, CLASSE: 0x05, REINICIAR: 0x06,
  RELE_DEFINIR: 0x20, RELE_PULSO: 0x21, RELE_MASCARA: 0x22, RELE_LER: 0x23,
  PZEM_LER: 0x30, PZEM_ZERAR_ENERGIA: 0x31, PZEM_LIMITE_POTENCIA: 0x32
};
var ACOES = { DESLIGAR: 0, LIGAR: 1, ALTERNAR: 2 };

function num(v, nome, min, max) {
  if (typeof v === "boolean") v = v ? 1 : 0;
  if (typeof v === "string" && /^(0x[0-9a-f]+|\d+)$/i.test(v)) v = Number(v);
  if (typeof v !== "number" || !isFinite(v) || Math.floor(v) !== v) throw new Error(nome + " ausente ou inválido");
  if (v < min || v > max) throw new Error(nome + " fora da faixa " + min + ".." + max);
  return v;
}
function pick(d, names) { for (var i = 0; i < names.length; i++) if (d[names[i]] !== undefined) return d[names[i]]; return undefined; }
function canal(d) {
  var c = d.canal;
  if (c === "todos" || c === "TODOS") return 0xFF;
  return num(c, "canal", 0, 255);
}

function encodeDownlink(input) {
  var d = input.data || {}, errors = [];
  try {
    var cmd = d.comando !== undefined ? d.comando : d.opcode;
    var op = typeof cmd === "number" ? cmd : COMANDOS[String(cmd || "").toUpperCase()];
    if (op === undefined) throw new Error("comando desconhecido: " + cmd);
    var seq = num(d.seq === undefined ? 0 : d.seq, "seq", 0, 255), b = [seq, op], v;
    switch (op) {
      case 0x02:
        v = num(pick(d, ["segundos", "intervalo"]), "segundos", 5, 86400);
        b.push((v >>> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
        break;
      case 0x04:
        b.push(num(pick(d, ["confirmado", "valor"]), "confirmado", 0, 1));
        break;
      case 0x05:
        v = String(pick(d, ["classe"]) || "").toUpperCase();
        if (v !== "A" && v !== "C") throw new Error("classe deve ser A ou C");
        b.push(v.charCodeAt(0));
        break;
      case 0x06:
        v = num(pick(d, ["atraso_ms", "atrasoMs", "atraso"]) || 0, "atraso_ms", 0, 60000);
        b.push((v >> 8) & 255, v & 255);
        break;
      case 0x20:
        v = pick(d, ["acao"]);
        if (typeof v === "string") v = ACOES[v.toUpperCase()];
        b.push(canal(d), num(v, "acao", 0, 2));
        break;
      case 0x21:
        v = pick(d, ["estado"]);
        if (typeof v === "string") v = /^(1|LIGADO|LIGAR|ON)$/i.test(v) ? 1 : 0;
        b.push(canal(d), num(v === undefined ? 1 : v, "estado", 0, 1));
        v = num(pick(d, ["duracao_ms", "duracaoMs", "duracao"]), "duracao_ms", 100, 86400000);
        b.push((v >>> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
        break;
      case 0x22:
        v = num(pick(d, ["mascara"]), "mascara", 0, 0xFFFF);
        var w = num(pick(d, ["valores"]), "valores", 0, 0xFFFF);
        b.push((v >> 8) & 255, v & 255, (w >> 8) & 255, w & 255);
        break;
      case 0x30:
      case 0x31:
        b.push(num(d.canal === undefined ? 0 : d.canal, "canal", 0, 255));
        break;
      case 0x32:
        b.push(num(d.canal === undefined ? 0 : d.canal, "canal", 0, 255));
        v = num(pick(d, ["watts", "potencia"]), "watts", 0, 23000);
        b.push((v >> 8) & 255, v & 255);
        break;
      default:
        break;      // STATUS, ENVIAR_AGORA, RELE_LER: sem parâmetros
    }
    return { bytes: b };     // porta 10 (escolhida na fila de downlink do ChirpStack)
  } catch (e) {
    errors.push(e.message);
    return { bytes: [], errors: errors };
  }
}

// ------------------------------------------------------------------ ChirpStack v3
// Decode(fPort, bytes, variables) -> objeto
function Decode(fPort, bytes, variables) {
  var r = decodeUplink({ fPort: fPort, bytes: bytes, variables: variables });
  if (r.errors && r.errors.length) return { erros: r.errors };
  return r.data;
}

// Encode(fPort, obj, variables) -> bytes (enfileire na porta 10)
function Encode(fPort, obj, variables) {
  var r = encodeDownlink({ data: obj, variables: variables });
  if (r.errors && r.errors.length) throw new Error(r.errors.join("; "));
  return r.bytes;
}

// Node.js (doc/codec/teste_codec.js); o ChirpStack ignora
if (typeof module !== "undefined" && module.exports) {
  module.exports = { Decode: Decode, Encode: Encode };
}
