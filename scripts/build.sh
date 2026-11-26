#!/bin/sh
# =============================================================================
#  scripts/build.sh — compila, grava pelo SWD (ST-LINK) e abre o console do nó
#  Radioenge (STM32L071CZ + SX1272). Roda com sh (dash), bash ou zsh.
#
#  Uso:  sh scripts/build.sh <comando> [porta do console]
#
#    build            compila                  -> build/radioenge_node_abp_otaa.{elf,hex,bin}
#    clean            apaga build/
#    unlock           tira a proteção de leitura (RDP 0xBB -> 0xAA). Só na 1ª gravação.
#                     APAGA o firmware AT de fábrica da Radioenge, que não pode ser regravado!
#    erase            apaga a flash e a EEPROM (configuração, sessão LoRaWAN, DevNonce)
#    upload           compila e grava (sem abrir o console)
#    flash [porta]    compila, grava e abre o console          (porta padrão: /dev/ttyUSB0)
#    monitor [porta]  só abre o console (115200 8N1)
#    info             mostra o ST-LINK e o STM32 conectados
#    web [porta TCP]  abre a página WEB (web/index.html) em http://localhost:8101,
#                     com ping/traceroute/nslookup neste computador (scripts/web_server.py)
#    all              compila (igual a build; existe para o laço "compilar todos")
#
#  Variáveis opcionais:
#    TOOL=cubeprog|openocd|stflash   ferramenta de gravação (padrão: a primeira encontrada,
#                                    nesta ordem: STM32CubeProgrammer, OpenOCD, st-flash)
#    CONSOLE_UART=1|2                1 = pinos 2/3 do módulo (padrão); 2 = GPIO1/GPIO0
#    RTC_FORCE_LSI=ON                testa o modo sem cristal de 32,768 kHz
#    USE_DEBUGGER=ON                 mantém o SWD ativo em SLEEP/STOP (gasta mais): com ele o
#                                    OpenOCD conecta sem o fio de RST e sem segurar o RESET
#    CONFIRM=APAGAR                  responde a pergunta do unlock (para scripts)
#    OCD_RESET=sw|hw                 OpenOCD: sw (padrão) conecta sem o fio de RST, tentando
#                                    parar o núcleo por 10 s; hw usa o RST do ST-LINK (sob reset)
# =============================================================================
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
CMD=${1:-build}
PORT=${2:-/dev/ttyUSB0}
BAUD=${BAUD:-115200}      # console; outra velocidade (AT+BAUDRATE): BAUD=9600 sh scripts/build.sh monitor
EEPROM_ADDR=0x08080000
EEPROM_SIZE=6144

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mERRO:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# Um firmware só para ABP e OTAA (a ativação é trocada por AT+NJM, sem regravar)
FW=radioenge_node_abp_otaa

# --------------------------------------------- STM32CubeCLT nos caminhos padrão
# Se o compilador não estiver no PATH, usa o do STM32CubeCLT (a versão mais nova
# instalada): GNU-tools-for-STM32, CMake, Ninja e STM32CubeProgrammer.
find_cubeclt() {
    [ -n "${STM32CLT_PATH:-}" ] && [ -d "$STM32CLT_PATH" ] && { echo "$STM32CLT_PATH"; return 0; }
    for d in $(ls -d /opt/st/stm32cubeclt_* /opt/ST/STM32CubeCLT_* /opt/ST/STM32CubeCLT \
                     /c/ST/STM32CubeCLT_* /c/ST/STM32CubeCLT/STM32CubeCLT* 2>/dev/null | sort -rV); do
        [ -d "$d/GNU-tools-for-STM32/bin" ] && { echo "$d"; return 0; }
    done
    return 1
}
if CLT=$(find_cubeclt); then
    have arm-none-eabi-gcc || PATH="$CLT/GNU-tools-for-STM32/bin:$PATH"
    have cmake             || PATH="$CLT/CMake/bin:$PATH"
    have ninja             || PATH="$CLT/Ninja/bin:$PATH"
    have STM32_Programmer_CLI || PATH="$CLT/STM32CubeProgrammer/bin:$PATH"
    export PATH
fi

find_cubeprog() {
    if have STM32_Programmer_CLI; then command -v STM32_Programmer_CLI; return 0; fi
    for p in "$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI" \
             "/opt/st/stm32cubeprogrammer/bin/STM32_Programmer_CLI" \
             "/usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI" \
             "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI" \
             "/c/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"; do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

pick_tool() {
    if [ -n "${TOOL:-}" ]; then echo "$TOOL"; return 0; fi
    if find_cubeprog >/dev/null; then echo cubeprog; return 0; fi
    if have openocd; then echo openocd; return 0; fi
    if have st-flash; then echo stflash; return 0; fi
    die "nenhuma ferramenta SWD encontrada: instale o STM32CubeCLT (ou o STM32CubeProgrammer), o OpenOCD ou o stlink-tools (README: Preparar o ambiente)."
}

# OpenOCD com ST-LINK.
#  OCD_RESET=sw (padrão): não depende do pino RST do ST-LINK (em muitos clones do V2 ele
#    não funciona em SWD). Liga o debug nos modos SLEEP/STOP/STANDBY (DBGMCU_CR = 7) e tenta
#    parar o núcleo a cada 100 ms por 10 s; o reset depois da gravação é por software
#    (SYSRESETREQ). Se nem assim conectar, segure o RESET do módulo no GND e solte quando
#    aparecer ">>> Conectando".
#  Depois de parar, faz um reset por software ("reset halt") e limpa as flags de erro da
#  flash (FLASH_SR): um firmware anterior parado no meio pode deixar a flash travada, e aí o
#  apagamento falha ("failed erasing sectors"). DBGMCU_CR sobrevive a esse reset.
#  target/stm32l0_dual_bank.cfg: o L071CZ tem 2 bancos de 96 KB (0x08000000 e 0x08018000);
#  o stm32l0.cfg só declara o primeiro, e o firmware passa de 96 KB.
#  OCD_RESET=hw: conexão sob reset pelo pino RST do ST-LINK ligado no RESET do módulo.
ocd() {
    if [ "${OCD_RESET:-sw}" = "hw" ]; then
        openocd -f interface/stlink.cfg -f target/stm32l0_dual_bank.cfg \
            -c "reset_config srst_only srst_nogate connect_assert_srst" "$@"
    else
        openocd -f interface/stlink.cfg -f target/stm32l0_dual_bank.cfg \
            -c "reset_config none" "$@"
    fi
}

# Para o núcleo (Tcl do OpenOCD). Os "timed out while waiting for target halted"
# durante as tentativas são normais; só é erro se nenhuma das 100 tentativas parar.
OCD_HALT='echo ">>> Conectando (até 10 s). Se não conectar, segure o RESET no GND e solte agora."
for {set i 0} {$i < 100} {incr i} { catch {mww 0x40015804 7}; if {![catch {halt 100}]} { break } }
if {[stm32l0.cpu curstate] ne "halted"} { echo "ERRO: não consegui parar o núcleo"; shutdown error }
reset_cfg_halt_retry
echo ">>> Núcleo parado"'
OCD_HALT=$(printf '%s' "$OCD_HALT" | sed 's/^reset_cfg_halt_retry$/catch {reset halt}\
for {set i 0} {$i < 50} {incr i} { catch {mww 0x40015804 7}; if {![catch {halt 100}]} { break } }\
mww 0x40022018 0x00032F02/')

# ---------------------------------------------------------------------- build
do_build() {
    have arm-none-eabi-gcc || die "arm-none-eabi-gcc não encontrado (instale o STM32CubeCLT; README: Preparar o ambiente)."
    have cmake || die "cmake não encontrado (vem no STM32CubeCLT)."
    say "Compilando $FW (arm-none-eabi-gcc $(arm-none-eabi-gcc -dumpversion))"
    # build/ configurado em outra pasta (o workspace mudou de lugar): o CMake recusa
    # o cache antigo. Apaga e configura de novo.
    if [ -f "$BUILD/CMakeCache.txt" ]; then
        cached=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
        if [ -n "$cached" ] && [ "$cached" != "$ROOT" ] && [ "$cached" != "$(cd "$ROOT" && pwd -P)" ]; then
            say "build/ foi configurado em $cached: apagando (cache de outra pasta)"
            rm -rf "$BUILD"
        fi
    fi
    if have ninja && [ ! -f "$BUILD/Makefile" ]; then
        set -- -G Ninja
    else
        set --
    fi
    cmake -S "$ROOT" -B "$BUILD" "$@" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-arm-none-eabi.cmake" \
        -DCONSOLE_UART="${CONSOLE_UART:-1}" \
        -DRTC_FORCE_LSI="${RTC_FORCE_LSI:-OFF}" \
        -DUSE_DEBUGGER="${USE_DEBUGGER:-OFF}"
    cmake --build "$BUILD"
    say "Pronto: build/$FW.hex  (e build/$FW.bin, build/$FW.elf)"
}

# ------------------------------------------------------------------- gravação
do_unlock() {
    warn "Isto remove a proteção de leitura (RDP) e APAGA toda a flash e a EEPROM do STM32,"
    warn "inclusive o firmware AT original da Radioenge, que não pode ser regravado."
    ok=${CONFIRM:-}
    if [ -z "$ok" ]; then
        printf 'Digite APAGAR para continuar: '
        read -r ok
    fi
    [ "$ok" = "APAGAR" ] || die "cancelado."
    tool=$(pick_tool)
    case "$tool" in
        cubeprog)
            cp=$(find_cubeprog)
            "$cp" -c port=SWD mode=UR reset=HWrst -ob displ || true
            "$cp" -c port=SWD mode=UR reset=HWrst -ob RDP=0xAA ;;
        openocd)
            ocd -c "init" -c "$OCD_HALT" -c "mdw 0x1FF80000" -c "stm32lx unlock 0" -c "shutdown" ;;
        stflash)
            die "o st-flash não altera o RDP: use TOOL=cubeprog (STM32CubeProgrammer) ou TOOL=openocd." ;;
        *) die "TOOL desconhecida: $tool" ;;
    esac
    say "RDP = 0xAA (sem proteção). Desligue e ligue o módulo antes de gravar."
}

do_erase() {
    tool=$(pick_tool)
    zeros="$BUILD/eeprom_zeros.bin"
    mkdir -p "$BUILD"
    dd if=/dev/zero of="$zeros" bs="$EEPROM_SIZE" count=1 2>/dev/null
    say "Apagando a flash e a EEPROM com $tool"
    case "$tool" in
        cubeprog)
            cp=$(find_cubeprog)
            "$cp" -c port=SWD mode=UR reset=HWrst -e all
            "$cp" -c port=SWD mode=UR reset=HWrst -w "$zeros" "$EEPROM_ADDR" ;;
        openocd)
            # Sem o "stm32lx mass_erase": no L0 ele sobe o RDP para 1 e desce para 0, e o recarregamento
            # das option bytes derruba a conexão sem o fio de RST ("stm32lx mass erase failed").
            # Apaga os dois bancos por setor e zera a EEPROM de dados (PEKEYR destrava o PECR;
            # PELOCK = 1 trava de novo), o mesmo resultado do cubeprog e do st-flash.
            ocd -c "init" -c "$OCD_HALT" \
                -c "flash erase_sector 0 0 last" -c "flash erase_sector 1 0 last" \
                -c "mww 0x4002200C 0x89ABCDEF" -c "mww 0x4002200C 0x02030405" \
                -c "load_image $zeros $EEPROM_ADDR bin" -c "verify_image $zeros $EEPROM_ADDR bin" \
                -c "mww 0x40022004 0x00000001" \
                -c "reset run" -c "shutdown" ;;
        stflash)
            st-flash --connect-under-reset erase
            st-flash --connect-under-reset write "$zeros" "$EEPROM_ADDR" ;;
        *) die "TOOL desconhecida: $tool" ;;
    esac
    say "Flash e EEPROM apagadas. O próximo firmware sobe com os valores do app/node_build.h."
}

do_upload() {
    do_build
    tool=$(pick_tool)
    say "Gravando $FW pelo SWD com $tool"
    case "$tool" in
        cubeprog)
            "$(find_cubeprog)" -c port=SWD mode=UR reset=HWrst -w "$BUILD/$FW.hex" -v -rst ;;
        openocd)
            # sem o "program" do OpenOCD: ele sempre faz "reset init", que falha sem o fio de RST
            ocd -c "init" -c "$OCD_HALT" \
                -c "flash write_image erase $BUILD/$FW.elf" \
                -c "verify_image $BUILD/$FW.elf" \
                -c "reset run" -c "shutdown" ;;
        stflash)
            st-flash --reset --connect-under-reset --format ihex write "$BUILD/$FW.hex" ;;
        *) die "TOOL desconhecida: $tool" ;;
    esac
    say "Gravado."
}

do_info() {
    tool=$(pick_tool)
    case "$tool" in
        cubeprog) cp=$(find_cubeprog); "$cp" -l st-link; "$cp" -c port=SWD mode=UR reset=HWrst ;;
        openocd)  ocd -c "init" -c "$OCD_HALT" -c "flash info 0" -c "flash info 1" -c "mdw 0x1FF80000" -c "resume" -c "shutdown" ;;
        stflash)  st-info --probe --connect-under-reset ;;
        *) die "TOOL desconhecida: $tool" ;;
    esac
}

# ------------------------------------------------------------------ página WEB
# O Web Serial e o Web Bluetooth só funcionam em https:// ou em http://localhost,
# por isso a página é servida localmente (não abra o arquivo direto com file://).
# O servidor local (scripts/web_server.py) também roda os diagnósticos de rede
# (ping, traceroute, nslookup) neste computador, para a seção Diagnósticos.
do_web() {
    wport=${2:-8101}
    have python3 || die "python3 não encontrado (precisa só para servir a página)."
    url="http://localhost:$wport/"
    say "Página WEB em $url  (Chrome, Edge ou Opera; Ctrl+C para parar)"
    if have xdg-open; then (sleep 1; xdg-open "$url" >/dev/null 2>&1) &
    elif have open; then (sleep 1; open "$url" >/dev/null 2>&1) &
    fi
    # web_server.py: serve web/ e faz Ping, Traceroute e Nslookup neste computador
    # (o módulo não tem rede IP), com a mesma API /diag_run dos nós ESP32
    exec python3 "$ROOT/scripts/web_server.py" "$wport"
}

# -------------------------------------------------------------------- console
do_monitor() {
    # No Linux a mesma porta pode ser aberta por dois programas ao mesmo tempo (picocom e
    # navegador, por exemplo): os bytes se dividem entre eles e os dois veem texto picotado.
    if have fuser && [ -e "$PORT" ] && fuser "$PORT" >/dev/null 2>&1; then
        warn "$PORT já está aberta por outro programa (página WEB? outro terminal?):"
        fuser -v "$PORT" 2>&1 | sed 's/^/   /' >&2 || true
        warn "feche-o antes, senão os dois perdem caracteres."
    fi
    [ -e "$PORT" ] || die "porta $PORT não existe. Confira com: ls /dev/ttyUSB* /dev/ttyACM* (macOS: ls /dev/cu.*)"
    if have picocom; then
        say "Console em $PORT, $BAUD 8N1 (para sair: Ctrl+A e depois Ctrl+X)"
        picocom -b "$BAUD" --imap lfcrlf --omap crlf "$PORT"
    elif python3 -c 'import serial' 2>/dev/null; then
        say "Console em $PORT, $BAUD 8N1 (para sair: Ctrl+])"
        python3 -m serial.tools.miniterm --eol CRLF "$PORT" "$BAUD"
    else
        die "instale o picocom ou o pyserial (python3 -m pip install pyserial) para abrir o console."
    fi
}

case "$CMD" in
    build|all) do_build ;;
    clean)     rm -rf "$BUILD"; say "build/ apagado." ;;
    unlock)    do_unlock ;;
    erase)     do_erase ;;
    upload)    do_upload ;;
    flash)     do_upload; sleep 1; do_monitor ;;
    monitor)   do_monitor ;;
    info)      do_info ;;
    web)       do_web "$@" ;;
    -h|--help|help) sed -n '2,29p' "$0" ;;
    *) die "comando desconhecido: $CMD (use: build, clean, unlock, erase, upload, flash, monitor, info, web)" ;;
esac
