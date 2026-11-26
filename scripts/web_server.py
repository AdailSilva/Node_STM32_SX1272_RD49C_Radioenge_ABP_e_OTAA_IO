#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
web_server.py — serve a página WEB do nó Radioenge (pasta web/) em http://localhost
e faz os diagnósticos de rede NESTE COMPUTADOR.

O módulo Radioenge não tem Wi-Fi nem IP: quem tem rede é o computador em que a
página está aberta. Por isso, aqui, Ping, Traceroute e Nslookup rodam no PC, com
esta API:

    GET /diag_info                     -> {"local":true,"os":"Linux","host":"...","dns":[...],"tools":{...}}
    GET /diag_run?t=ping|trace|dns&h=  -> texto, linha a linha, enquanto o teste roda

    ping   5 pacotes ICMP de 32 bytes, 1 por segundo (um "ping -c 1" do sistema por pacote)
    trace  até 15 saltos, 1 tentativa de 1 s por salto (ping com TTL = 1, 2, 3...)
    dns    resolução pelo resolvedor do sistema  (A/AAAA; PTR quando o destino é um IP)

A saída tem o mesmo formato da página /diag dos nós e gateways ESP32 ("PING host (ip):
32 bytes de dados", "40 bytes de ip: seq=0 ttl=52 tempo=38.412 ms", "seq=1 sem resposta
(1 s)", estatísticas...). Só IPv4, como no ESP32. O ICMP é o do sistema (programa ping),
então um destino com o ICMP bloqueado aparece sem resposta, igual ao terminal.

Só aceita conexões de 127.0.0.1 e destinos com letras, números, ".", "-", ":" e "_"
(nada passa por um shell). Uso:

    python3 scripts/web_server.py [porta]      # padrão 8101
    sh scripts/build.sh web [porta]            # o mesmo, e abre o navegador

SPDX-License-Identifier: MIT
"""
import http.server
import ipaddress
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.parse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "web"))
HOST_RE = re.compile(r"^[A-Za-z0-9._:-]{1,63}$")
IS_WIN = os.name == "nt"
IS_MAC = sys.platform == "darwin"
ENC = "oem" if IS_WIN else "utf-8"


def tools():
    return {
        "ping": shutil.which("ping") is not None,
    }


def dns_servers():
    """Servidores DNS do sistema (Linux/macOS: /etc/resolv.conf)."""
    out = []
    try:
        with open("/etc/resolv.conf", encoding="utf-8", errors="replace") as f:
            for line in f:
                p = line.split()
                if len(p) >= 2 and p[0] == "nameserver":
                    out.append(p[1])
    except OSError:
        pass
    return out


def is_ip(h):
    try:
        ipaddress.ip_address(h)
        return True
    except ValueError:
        return False


DATA = 32           # bytes de dados por pacote (como o DIAG_DATA do ESP32)
HOPS = 15           # saltos do traceroute
IPRE = r"(\d{1,3}(?:\.\d{1,3}){3})"


def resolve4(h):
    """IPv4 do destino (None se não resolver)."""
    try:
        return socket.getaddrinfo(h, None, socket.AF_INET)[0][4][0]
    except (socket.gaierror, OSError, IndexError):
        return None


def ping_once(ip, ttl=None, wait_s=1):
    """Um pacote ICMP pelo programa ping do sistema. Devolve (tipo, origem, ms, ttl):
    tipo = "reply", "ttl" (TTL esgotado num roteador), "unreach" ou None (sem resposta)."""
    if IS_WIN:
        cmd = ["ping", "-n", "1", "-w", str(wait_s * 1000), "-l", str(DATA)] + (["-i", str(ttl)] if ttl else []) + [ip]
    elif IS_MAC:
        cmd = ["ping", "-n", "-c", "1", "-W", str(wait_s * 1000), "-s", str(DATA)] + (["-m", str(ttl)] if ttl else []) + [ip]
    else:
        cmd = ["ping", "-n", "-c", "1", "-W", str(wait_s), "-s", str(DATA)] + (["-t", str(ttl)] if ttl else []) + [ip]
    env = None if IS_WIN else dict(os.environ, LC_ALL="C", LANG="C")
    t0 = time.perf_counter()
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           env=env, timeout=wait_s + 4)
        out = r.stdout.decode(ENC, errors="replace")
    except subprocess.TimeoutExpired:
        return None, None, None, None
    ms = (time.perf_counter() - t0) * 1000.0
    who = r"(?:bytes from|From|Reply from|Resposta de|Respuesta desde)\s+" + IPRE
    m = re.search(who + r"[^\n]*?(?:time|tempo|tiempo)\s*([=<])\s*([\d.,]+)\s*ms", out, re.I)
    if m:
        t = float(m.group(3).replace(",", "."))
        if m.group(2) == "<":
            t = min(t, ms)
        tt = re.search(r"ttl\s*=\s*(\d+)", out, re.I)
        return "reply", m.group(1), t, int(tt.group(1)) if tt else None
    m = re.search(who + r"[^\n]*(?:time to live exceeded|ttl exceeded|ttl expired|ttl expirado|tempo de vida)", out, re.I)
    if m:
        return "ttl", m.group(1), ms, None
    m = re.search(who + r"[^\n]*(?:unreachable|inacess|inalcan)", out, re.I)
    if m:
        return "unreach", m.group(1), ms, None
    return None, None, None, None


def fmt_ms(v):
    return "%.3f" % v


class Handler(http.server.SimpleHTTPRequestHandler):
    server_version = "RadioengeNodeWeb/1.0"

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, fmt, *args):
        if args and "/diag_" in str(args[0]):
            sys.stderr.write("[web] %s\n" % (fmt % args))

    def cors_origin(self):
        """A página também pode ser aberta direto do disco (file://, origem "null"),
        pelo GitHub Pages (https://<usuário>.github.io) ou por outra porta local: elas
        chamam http://127.0.0.1:8101/diag_* e só essas origens são liberadas."""
        o = self.headers.get("Origin", "")
        if o == "null" or re.match(r"^https://[A-Za-z0-9-]+\.github\.io$", o) or \
                re.match(r"^http://(localhost|127\.0\.0\.1)(:\d+)?$", o):
            return o
        return None

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        if self.path.startswith("/diag_"):
            o = self.cors_origin()
            if o:
                self.send_header("Access-Control-Allow-Origin", o)
                self.send_header("Access-Control-Allow-Private-Network", "true")
                self.send_header("Vary", "Origin")
        super().end_headers()

    def do_OPTIONS(self):
        # pré-voo do Chrome (Private Network Access) para file:// e https:// -> 127.0.0.1
        if self.client_address[0] not in ("127.0.0.1", "::1") or not self.path.startswith("/diag_") or not self.cors_origin():
            self.send_error(403, "origem nao permitida")
            return
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET")
        self.end_headers()

    # --------------------------------------------------------------- rotas
    def do_GET(self):
        if self.client_address[0] not in ("127.0.0.1", "::1"):
            self.send_error(403, "so localhost")
            return
        u = urllib.parse.urlsplit(self.path)
        if u.path == "/diag_info":
            return self.diag_info()
        if u.path == "/diag_run":
            q = urllib.parse.parse_qs(u.query)
            return self.diag_run(q.get("t", [""])[0], q.get("h", [""])[0].strip())
        return super().do_GET()

    def diag_info(self):
        body = json.dumps({
            "local": True,
            "os": "%s %s" % (platform.system(), platform.release()),
            "host": socket.gethostname(),
            "dns": dns_servers(),
            "tools": tools(),
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def start_text(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def say(self, text):
        self.wfile.write(text.encode("utf-8"))
        self.wfile.flush()

    def diag_run(self, t, h):
        if t not in ("ping", "trace", "dns"):
            self.send_error(400, "teste invalido")
            return
        if not HOST_RE.match(h) or h.startswith("-"):
            self.send_error(400, "destino invalido")
            return
        self.start_text()
        try:
            if t == "dns":
                self.nslookup(h)
            elif shutil.which("ping") is None:
                self.say("%s: o programa 'ping' não foi encontrado neste computador "
                         "(Linux: instale o pacote iputils-ping)\n" % ("ping" if t == "ping" else "traceroute"))
            elif t == "ping":
                self.ping(h)
            else:
                self.traceroute(h)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # Mesmo texto do diag_ping() / diag_traceroute() do ESP32 (main/node_diag.c)
    def ping(self, h, count=5):
        if ":" in h:
            self.say('ping: só IPv4 é suportado ("%s" é IPv6)\n' % h)
            return
        ip = resolve4(h)
        if ip is None:
            self.say('ping: não foi possível resolver "%s"\n' % h)
            return
        self.say("PING %s (%s): %d bytes de dados\n" % (h, ip, DATA))
        times, sent = [], 0
        for seq in range(count):
            t0 = time.time()
            kind, src, ms, ttl = ping_once(ip)
            sent += 1
            if kind == "reply":
                times.append(ms)
                self.say("%d bytes de %s: seq=%d ttl=%s tempo=%s ms\n"
                         % (DATA + 8, src, seq, ttl if ttl is not None else "?", fmt_ms(ms)))
            elif kind == "ttl":
                self.say("De %s: seq=%d TTL esgotado\n" % (src, seq))
            elif kind == "unreach":
                self.say("De %s: seq=%d destino inalcançável\n" % (src, seq))
            else:
                self.say("seq=%d sem resposta (1 s)\n" % seq)
            rest = 1.0 - (time.time() - t0)
            if seq + 1 < count and rest > 0:
                time.sleep(rest)
        self.say("\n--- estatísticas do ping para %s ---\n" % h)
        self.say("%d pacotes transmitidos, %d recebidos, %d%% de perda\n"
                 % (sent, len(times), (sent - len(times)) * 100 // sent))
        if times:
            self.say("ida e volta mín/méd/máx = %s/%s/%s ms\n"
                     % (fmt_ms(min(times)), fmt_ms(sum(times) / len(times)), fmt_ms(max(times))))

    def traceroute(self, h):
        if ":" in h:
            self.say('traceroute: só IPv4 é suportado ("%s" é IPv6)\n' % h)
            return
        ip = resolve4(h)
        if ip is None:
            self.say('traceroute: não foi possível resolver "%s"\n' % h)
            return
        self.say("traceroute para %s (%s), máximo de %d saltos, pacotes de %d bytes\n" % (h, ip, HOPS, 20 + 8 + DATA))
        for ttl in range(1, HOPS + 1):
            kind, src, ms, _ = ping_once(ip, ttl=ttl)
            if kind is None:
                self.say("%2d  *\n" % ttl)
                continue
            self.say("%2d  %s  %s ms%s\n" % (ttl, src, fmt_ms(ms), "  !H" if kind == "unreach" else ""))
            if kind in ("reply", "unreach"):
                break

    def nslookup(self, h):
        t0 = time.time()
        srv = dns_servers()
        self.say("Servidor:\t%s\n\n" % (", ".join(srv) if srv else "resolvedor do sistema"))
        if is_ip(h):
            try:
                name, aliases, _ = socket.gethostbyaddr(h)
                self.say("%s\tnome = %s\n" % (h, name))
                for a in aliases:
                    self.say("%s\tnome = %s\n" % (h, a))
            except (socket.herror, socket.gaierror, OSError):
                self.say("*** %s não tem nome reverso (PTR)\n" % h)
        else:
            self.say("Nome:\t\t%s\n" % h)
            seen = []
            try:
                for fam, _, _, _, addr in socket.getaddrinfo(h, None, proto=socket.IPPROTO_TCP):
                    ip = addr[0]
                    if ip not in seen:
                        seen.append(ip)
                        self.say("Endereço:\t%s\n" % ip)
            except socket.gaierror:
                pass
            if not seen:
                self.say("*** não foi possível resolver %s (sem resposta ou nome inexistente)\n" % h)
        self.say("\n(%d ms)\n" % int((time.time() - t0) * 1000))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8101
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print("Página WEB em http://localhost:%d/  (diagnósticos de rede neste computador; Ctrl+C para parar)" % port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
