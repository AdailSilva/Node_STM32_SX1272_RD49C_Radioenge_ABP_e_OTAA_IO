/*
 * console.c — comandos de texto na UART do módulo (pinos 2/3, 115200 8N1).
 *
 *   help                    lista os comandos
 *   status                  sessão, classe, beacon, contadores, relógio
 *   config                  configuração em uso
 *   set <campo> <valor>     altera um campo (só na RAM; "save" grava)
 *   save                    grava na EEPROM e aplica
 *   class <A|B|C>           troca a classe agora e grava
 *   send [texto|hex:AABB]   uplink agora (sem argumento: payload configurado)
 *   join                    refaz o JOIN (OTAA) / reativa a sessão (ABP)
 *   devtime                 pede a hora ao servidor (DeviceTimeReq)
 *   clock [recal]           relógio do RTC; "recal" mede o LSI de novo
 *   factory [nonce]         padrão de fábrica; "nonce" zera também o DevNonce
 *   reboot                  reinicia
 *   log [clear] | jlog [de] log do nó em RAM (texto / blocos "@L" para a página e o app)
 *   diag | jdiag            autoteste do nó (rádio, EEPROM, sessão, relógio, VDD, RAM...)
 *   jio                     GPIOs, bateria e serial transparente (JSON "@IO" para a página e o app)
 *   classes                 resumo das classes LoRaWAN A, B e C e o que este nó suporta
 *   rele [canal|todos acao] relés: estado; liga/desliga/alterna ("rele pulso c e ms": pulso)
 *   pzem [ler [canal]]      medidores PZEM-004T: últimas leituras; "ler" lê agora
 *   jdev                    relés, PZEM e protocolo V1 (JSON "@DV" para a página e o app)
 *   AT, AT+CMD=?, AT+CMD=x  comandos AT do firmware original da Radioenge (at_cmd.c)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "board.h"
#include "board-ext.h"
#include "uart.h"
#include "systime.h"
#include "LmHandler.h"
#include "node_build.h"
#include "node_config.h"
#include "console.h"
#include "node_log.h"
#include "node_diag.h"
#include "app.h"
#include "node_io.h"
#include "at_cmd.h"
#include "node_relay.h"
#include "node_pzem.h"
#include "node_v1.h"

extern Uart_t ConsoleUart;

#define LINE_MAX                520   /* AT+SENDB com 222 bytes (444 dígitos hex) */

static char Line[LINE_MAX];
static uint16_t LineLen = 0;
static bool Dirty = false;         /* há "set" sem "save" */
static bool AppMode = false;       /* "mode app": sem eco nem prompt, respostas @OK/@ERR */
static bool CmdOk = true;
static char CmdErr[72];
static const char *CmdNote = "";

/* Registra a falha do comando em andamento (resposta "@ERR <msg>" no modo app) */
static void Fail( const char *msg )
{
    if( CmdOk )
    {
        CmdOk = false;
        snprintf( CmdErr, sizeof( CmdErr ), "%s", msg );
    }
}
static bool NeedRestart = false;   /* credenciais/canais mudaram */
static bool MacDirty = false;      /* PNM, DCS ou atrasos do JOIN mudaram ("set" sem "save") */
static bool RxDirty = false;       /* janelas RX1/RX2 mudaram ("set" sem "save") */
static bool BaudPending = false;   /* "save" mudou a velocidade: troca depois da resposta */
static uint8_t AppliedBaud = 0;    /* AT+BAUDRATE / set baud em uso */

static void Prompt( void )
{
    if( !AppMode )
    {
        printf( "%s> ", Dirty ? "*" : "" );
    }
}

void ConsoleInit( void )
{
    setvbuf( stdout, NULL, _IONBF, 0 );   /* sem buffer: o prompt aparece na hora */
    AppliedBaud = NodeCfg.baudIdx;
    LineLen = 0;
    Prompt( );
}

bool ConsoleHasInput( void )
{
    return IsFifoEmpty( &ConsoleUart.FifoRx ) == false;
}

/* ------------------------------------------------------------ helpers */
static void PrintHexField( const char *name, const uint8_t *b, size_t n )
{
    char hex[2 * 16 + 1];
    BytesToHex( b, n, hex );
    printf( "  %-10s %s\r\n", name, hex );
}

static bool ParseU32( const char *s, uint32_t *v, uint32_t min, uint32_t max )
{
    char *end;
    unsigned long x = strtoul( s, &end, 0 );
    if( ( *s == '\0' ) || ( *end != '\0' ) || ( x < min ) || ( x > max ) )
    {
        return false;
    }
    *v = ( uint32_t )x;
    return true;
}

static bool ParseBool( const char *s, uint8_t *v )
{
    if( !strcmp( s, "1" ) || !strcmp( s, "on" ) || !strcmp( s, "sim" ) )
    {
        *v = 1;
        return true;
    }
    if( !strcmp( s, "0" ) || !strcmp( s, "off" ) || !strcmp( s, "nao" ) || !strcmp( s, "não" ) )
    {
        *v = 0;
        return true;
    }
    return false;
}

static const char* ClassBeaconText( char b )
{
    switch( b )
    {
    case 'A': return "adquirindo";
    case 'R': return "recebendo";
    case 'N': return "perdeu o último";
    case 'L': return "perdido";
    default:  return "-";
    }
}

/* ----------------------------------------------------------- comandos */
static void CmdHelp( void )
{
    printf( "Comandos:\r\n"
            "  status                  sessão, classe, beacon, contadores e relógio\r\n"
            "  config                  configuração em uso\r\n"
            "  set <campo> <valor>     altera um campo (veja 'set' sem argumentos)\r\n"
            "  save                    grava na EEPROM e aplica\r\n"
            "  class <A|B|C>           troca a classe agora e grava\r\n"
            "  send [texto|hex:A1B2]   uplink agora (sem argumento: payload configurado)\r\n"
            "  join                    %s\r\n"
            "  devtime                 pede a hora ao servidor (DeviceTimeReq)\r\n"
            "  clock [recal]           relógio do RTC; 'recal' mede o LSI de novo\r\n"
            "  factory [nonce]         padrão de fábrica ('nonce' zera também o DevNonce)\r\n"
            "  reboot                  reinicia o módulo\r\n"
            "  log [clear]             log do nó guardado em RAM (%u bytes); 'clear' apaga\r\n"
            "  diag                    autoteste: rádio, EEPROM, sessão, tráfego, relógio, VDD, RAM\r\n"
            "  classes                 classes LoRaWAN A, B e C e o que este nó suporta\r\n"
            "  rele [canal|todos liga|desliga|alterna]  relés (sem argumento: estado)\r\n"
            "  rele pulso <canal> <0|1> <ms>            pulso (volta ao estado anterior)\r\n"
            "  pzem [ler [canal]]      medidores PZEM-004T: últimas leituras; 'ler' lê agora\r\n"
            "  mode app|human          modo do app (sem eco, respostas @OK/@ERR) ou humano\r\n"
            "  keys                    chaves da sessão LoRaWAN em uso (DevAddr, NwkSKey, AppSKey)\r\n"
            "  info | jstatus | jconfig | jkeys | jio | jdev | jlog [de] | jdiag  respostas para a página e o app\r\n"
            "  AT? | AT+CMD=? | AT+CMD=x  comandos AT do firmware original da Radioenge ('AT?' lista)\r\n",
            NodeIsOtaa( ) ? "refaz o JOIN (OTAA)" : "reativa a sessão ABP",
            ( unsigned )NODE_LOG_SIZE );
}

static void CmdSetHelp( void )
{
    printf( "Campos do 'set':\r\n"
            "  njm <0 ABP|1 OTAA>  ajoin <0|1>\r\n"
            "  OTAA: deveui <16 hex|auto>  joineui <16 hex>  appkey <32 hex>  nwkkey <32 hex> (LoRaWAN 1.1)\r\n"
            "  ABP:  devaddr <8 hex>  nwkskey <32 hex>  appskey <32 hex>  netid <hex>\r\n"
            "  subband <0..7|8=todos>  ch500 <0|1>  adr <0|1>  dr <2..6>  txpower <2..20 dBm>\r\n"
            "  class <A|B|C>  ping <0..7 (2^n s)>\r\n"
            "  interval <0=desligado|5..86400 s>  port <1..223>  confirmed <0|1>  payload <texto>  payloadhex <hex>\r\n"
            "  nbtrials <0..79>  atport <0|1..223>  baud <9600|19200|43000|115200>\r\n"
            "  txp <0..15|255 = usar txpower>  dcs <0|1>  pnm <0|1>  chmask <24 hex>\r\n"
            "  rx2fq <923300000..927500000 Hz>  rx2dr <8..13>  rx1dl / rx2dl <ms>  jn1dl / jn2dl <ms>\r\n"
            "  keepalive <habilitado:porta:confirmado:periodo_ms>\r\n"
            "  rst <0|1>  rstport <1..223>  rstcmd <hex>  rstdelay <ms>  cmdport <0..223>\r\n"
            "  lowpower <0 sem STOP|1 STOP + console|2 STOP econômico>  extpower <0|1>  leds <0|1>\r\n"
            "  verbose <0|1>  (diagnóstico de sessão a cada envio: chaves, Frame raw, FCnt e MIC; vale na hora)\r\n"
            "  uplinkmode <0 payload fixo na 'port'|1 telemetria ADAILSILVA_V1 na porta 20>\r\n"
            "Depois: 'save' (credenciais, canais e njm reiniciam a sessão LoRaWAN).\r\n"
            "GPIOs: AT+GPIOC / AT+WPIN / AT+RPIN / AT+ADC (valem na hora); 'jio' mostra o estado.\r\n"
            "Relés e PZEM: AT+RELECFG / AT+PZEMCFG (valem na hora e gravam); 'rele', 'pzem' e 'jdev' mostram.\r\n" );
}

static void CmdStatus( void )
{
    const RtcClockInfo_t *ci = RtcGetClockInfo( );
    uint32_t up = AppUptimeMs( ) / 1000;
    SysTime_t t = SysTimeGet( );

    printf( "%s v%s · %s\r\n", NODE_FW_NAME, NODE_FW_VERSION, NODE_STACK_VERSION );
    printf( "  uptime      %lu d %02lu:%02lu:%02lu\r\n", ( unsigned long )( up / 86400 ), ( unsigned long )( ( up / 3600 ) % 24 ),
            ( unsigned long )( ( up / 60 ) % 60 ), ( unsigned long )( up % 60 ) );
    printf( "  sessão      %s", AppStatus.joined ? "ativa" : ( AppStatus.joining ? "JOIN em andamento" : "sem sessão" ) );
    if( AppStatus.joined )
    {
        printf( " · DevAddr %08lX", ( unsigned long )AppStatus.devAddr );
    }
    printf( "\r\n" );
    if( NodeIsOtaa( ) )
    {
        printf( "  JOIN        %lu tentativa(s) · DevNonce %u\r\n", ( unsigned long )AppStatus.joinAttempts, AppGetDevNonce( ) );
    }
    printf( "  classe      %c em uso (configurada: %c)", AppCurrentClass( ), NodeCfg.devClass );
    if( NodeCfg.devClass == 'B' || AppStatus.activeClass == 'B' )
    {
        printf( " · beacon %s · %lu recebidos · %lu perdidos · ping 2^%u = %u s",
                ClassBeaconText( AppStatus.beacon ), ( unsigned long )AppStatus.beaconsRx,
                ( unsigned long )AppStatus.beaconsMissed, NodeCfg.pingPeriodicity, 1U << NodeCfg.pingPeriodicity );
    }
    printf( "\r\n" );
    {
        uint32_t ago;
        const char *why;
        uint32_t n = AppRadioRecoveries( &ago, &why );
        if( n == 0 )
        {
            printf( "  rádio       ok, nenhuma recuperação desde o boot\r\n" );
        }
        else
        {
            printf( "  rádio       %lu recuperação(ões) desde o boot · última há %lu s (%s)\r\n",
                    ( unsigned long )n, ( unsigned long )ago, why );
        }
    }
    printf( "  uplinks     %lu ok · %lu erros · %lu ACKs · FCntUp %lu · DR%d\r\n", ( unsigned long )AppStatus.uplinks,
            ( unsigned long )AppStatus.uplinkErrors, ( unsigned long )AppStatus.acks, ( unsigned long )AppStatus.fcntUp,
            LmHandlerGetCurrentDatarate( ) );
    if( AppStatus.joined && ( NodeCfg.txIntervalS == 0 ) )
    {
        printf( "  próximo     uplink periódico desligado (interval 0)\r\n" );
    }
    else if( AppStatus.joined )
    {
        int32_t next = ( int32_t )( AppStatus.nextTxMs - AppUptimeMs( ) );
        printf( "  próximo     em %ld s (a cada %lu s, porta %u)\r\n", ( long )( ( next > 0 ) ? next / 1000 : 0 ),
                ( unsigned long )NodeCfg.txIntervalS, NodeCfg.fport );
    }
    printf( "  downlinks   %lu · FCntDown %lu", ( unsigned long )AppStatus.downlinks, ( unsigned long )AppStatus.fcntDown );
    if( AppStatus.downlinks )
    {
        char hex[2 * 32 + 1];
        BytesToHex( AppStatus.lastData, ( AppStatus.lastLen > 32 ) ? 32 : AppStatus.lastLen, hex );
        printf( " · último: %s, porta %u, %u bytes %s, RSSI %d dBm, SNR %d dB", AppStatus.lastSlot, AppStatus.lastPort,
                AppStatus.lastLen, hex, AppStatus.lastRssi, AppStatus.lastSnr );
    }
    printf( "\r\n" );
    printf( "  hora GPS    %lu s%s\r\n", ( unsigned long )t.Seconds, AppStatus.timeSynced ? " (sincronizada)" : " (não sincronizada)" );
    printf( "  relógio     %s · %lu Hz · ~%lu ppm\r\n", AppClockSourceName( ), ( unsigned long )( ci->RtcClkHz + 0.5f ),
            ( unsigned long )ci->PpmEstimate );
    printf( "  AT/E-S      keepalive %s (%lu enviados) · interrupções %lu · transparente %s (%lu) · AT por downlink %lu\r\n",
            NodeCfg.kaEnable ? "ligado" : "desligado", ( unsigned long )AppStatus.keepalives, ( unsigned long )AppStatus.irqUplinks,
            NodeIoTransparentActive( ) ? "ligada" : "desligada", ( unsigned long )AppStatus.transpUplinks,
            ( unsigned long )AppStatus.atDownlinks );
    printf( "  energia     %s · VDD %lu mV · %s\r\n",
            ( NodeCfg.lowPower == 0 ) ? "sem STOP" : ( NodeCfg.lowPower == 1 ) ? "STOP entre eventos (console pronto)" : "STOP econômico",
            ( unsigned long )BoardGetBatteryVoltage( ), NodeCfg.extPower ? "alimentação externa" : "bateria" );
    printf( "  V1          %s · %lu comando(s) na porta 10 · %lu uplink(s) nas portas 11/20 · boot %u\r\n",
            ( NodeCfg.uplinkMode == NODE_UPLINK_V1 ) ? "telemetria periódica na porta 20" : "uplink periódico com payload fixo",
            ( unsigned long )AppStatus.v1Commands, ( unsigned long )AppStatus.v1Uplinks, NodeV1BootReason( ) );
    printf( "  relés       %u · mapa %04X%s\r\n", NodeRelayCount( ), NodeRelayMap( ), NodeRelayPulsing( ) ? " · em pulso" : "" );
    printf( "  PZEM        %u medidor(es)%s\r\n", NodeCfg.pzemCount,
            NodeCfg.pzemCount ? ( NodePzemActive( ) ? " em GPIO0/GPIO1" : " (serial desligada)" ) : " (modo PZEM desligado)" );
}

/* ---------------------------------------------------- relés e PZEM ---- */
static void CmdRelayShow( void )
{
    uint16_t map = NodeRelayMap( ), pul = NodeRelayPulsing( );
    if( NodeRelayCount( ) == 0 )
    {
        printf( "Nenhum relé configurado (AT+RELECFG=4,5:1 cria dois, em GPIO4 e GPIO5, ativos em 0).\r\n" );
        return;
    }
    printf( "Relés: %u · mapa %04X\r\n", NodeRelayCount( ), map );
    for( uint8_t ch = 0; ch < NodeRelayCount( ); ch++ )
    {
        printf( "  %u  GPIO%u  %-9s ativo em %u%s\r\n", ch, NodeCfg.relayPin[ch], ( ( map >> ch ) & 1 ) ? "ligado" : "desligado",
                ( ( NodeCfg.relayActiveLow >> ch ) & 1 ) ? 0 : 1, ( ( pul >> ch ) & 1 ) ? " · em pulso" : "" );
    }
}

static void CmdRelay( char *a1, char *a2, char *a3 )
{
    if( a1 == NULL )
    {
        CmdRelayShow( );
        return;
    }
    NodeRelayStatus_t st;
    uint32_t ch = 0, v = 0, ms = 0;
    if( !strcmp( a1, "pulso" ) )
    {
        if( !a2 || !a3 || !ParseU32( a2, &ch, 0, NODE_RELAY_MAX - 1 ) )
        {
            printf( "Uso: rele pulso <canal> <0|1> <ms>\r\n" );
            Fail( "uso: rele pulso <canal> <0|1> <ms>" );
            return;
        }
        /* "1 5000" chega junto no a3 (o Execute separa só 4 argumentos) */
        char *sp = strchr( a3, ' ' );
        if( !sp || ( *sp = '\0', !ParseU32( a3, &v, 0, 1 ) ) || !ParseU32( sp + 1, &ms, NODE_RELAY_PULSE_MIN_MS, NODE_RELAY_PULSE_MAX_MS ) )
        {
            printf( "Uso: rele pulso <canal> <0|1> <ms 100..86400000>\r\n" );
            Fail( "uso: rele pulso <canal> <0|1> <ms>" );
            return;
        }
        st = NodeRelayPulse( ( uint8_t )ch, ( uint8_t )v, ms );
    }
    else
    {
        uint8_t act;
        if( !strcmp( a1, "todos" ) || !strcmp( a1, "all" ) ) ch = NODE_RELAY_ALL;
        else if( !ParseU32( a1, &ch, 0, NODE_RELAY_MAX - 1 ) ) ch = 0x100;
        if( !a2 ) act = 0xFF;
        else if( !strcmp( a2, "liga" ) || !strcmp( a2, "on" ) || !strcmp( a2, "1" ) ) act = NODE_RELAY_ON;
        else if( !strcmp( a2, "desliga" ) || !strcmp( a2, "off" ) || !strcmp( a2, "0" ) ) act = NODE_RELAY_OFF;
        else if( !strcmp( a2, "alterna" ) || !strcmp( a2, "toggle" ) || !strcmp( a2, "2" ) ) act = NODE_RELAY_TOGGLE;
        else act = 0xFF;
        if( ( ch > NODE_RELAY_ALL ) || ( act == 0xFF ) )
        {
            printf( "Uso: rele <canal|todos> <liga|desliga|alterna>  ou  rele pulso <canal> <0|1> <ms>\r\n" );
            Fail( "uso: rele <canal|todos> <liga|desliga|alterna>" );
            return;
        }
        st = NodeRelaySet( ( uint8_t )ch, act );
    }
    if( st != NODE_RELAY_OK )
    {
        printf( "%s\r\n", ( st == NODE_RELAY_ABSENT ) ? "Relé não configurado." : "Valor fora da faixa." );
        Fail( ( st == NODE_RELAY_ABSENT ) ? "relé não configurado" : "valor fora da faixa" );
        return;
    }
    NodeRelayRequestReport( );              /* telemetria com o estado novo */
    CmdRelayShow( );
}

static void CmdPzem( char *a1, char *a2 )
{
    if( NodeCfg.pzemCount == 0 )
    {
        printf( "Modo PZEM desligado (AT+PZEMCFG=f8 liga um medidor em GPIO0/GPIO1).\r\n" );
        if( a1 )
        {
            Fail( "modo PZEM desligado" );
        }
        return;
    }
    if( a1 && !strcmp( a1, "ler" ) )
    {
        uint32_t ch = 0;
        uint8_t from = 0, to = NodeCfg.pzemCount;
        if( a2 )
        {
            if( !ParseU32( a2, &ch, 0, NodeCfg.pzemCount - 1 ) )
            {
                Fail( "canal inválido" );
                return;
            }
            from = ( uint8_t )ch;
            to = ( uint8_t )( ch + 1 );
        }
        for( uint8_t c = from; c < to; c++ )
        {
            if( !NodePzemRequestLocal( NODE_PZEM_OP_READ, c, 0 ) )
            {
                Fail( "fila do PZEM cheia" );
                return;
            }
        }
        printf( "Leitura pedida: o resultado aparece como 'PZEMn: ...'.\r\n" );
        return;
    }
    for( uint8_t ch = 0; ch < NodeCfg.pzemCount; ch++ )
    {
        char s[120];
        NodePzemFormat( ch, s, sizeof( s ) );
        printf( "  PZEM%u (endereço %02X): %s\r\n", ch, NodeCfg.pzemAddr[ch], s );
    }
}

static void CmdConfig( void )
{
    char buf[2 * NODE_PAYLOAD_MAX + 1];

    printf( "Configuração%s:\r\n", Dirty ? " (com alterações não gravadas)" : "" );
    PrintHexField( "deveui", NodeCfg.devEui, 8 );
    if( NodeCfg.devEuiAuto )
    {
        printf( "             (auto: ID único do STM32)\r\n" );
    }
    printf( "  %-10s %u (%s)%s\r\n", "njm", NodeCfg.activation, NodeIsOtaa( ) ? "OTAA" : "ABP",
            ( NodeIsOtaa( ) != ( NODE_ACTIVATION == NODE_ACTIVATION_OTAA ) )
            ? ( ( NODE_ACTIVATION == NODE_ACTIVATION_OTAA ) ? " · padrão de fábrica: OTAA" : " · padrão de fábrica: ABP" ) : "" );
    if( NodeIsOtaa( ) )
    {
        PrintHexField( "joineui", NodeCfg.joinEui, 8 );
        PrintHexField( "appkey", NodeCfg.appKey, 16 );
        PrintHexField( "nwkkey", NodeCfg.nwkKey, 16 );
        printf( "  %-10s %u\r\n", "ajoin", NodeCfg.autoJoin );
    }
    else
    {
        printf( "  %-10s %08lX\r\n", "devaddr", ( unsigned long )NodeCfg.devAddr );
        PrintHexField( "nwkskey", NodeCfg.nwkSKey, 16 );
        PrintHexField( "appskey", NodeCfg.appSKey, 16 );
        printf( "  %-10s %06lX\r\n", "netid", ( unsigned long )NodeCfg.netId );
    }
    if( NodeCfg.subBand == NODE_SUBBAND_CUSTOM )
    {
        printf( "  %-10s 9 (máscara do AT+CHMASK: %04x:%04x:%04x:%04x:%04x:%04x)\r\n", "subband", NodeCfg.chMask[0],
                NodeCfg.chMask[1], NodeCfg.chMask[2], NodeCfg.chMask[3], NodeCfg.chMask[4], NodeCfg.chMask[5] );
    }
    else if( NodeCfg.subBand < NODE_SUBBAND_ALL )
    {
        printf( "  %-10s %u (canais %u-%u%s)\r\n", "subband", NodeCfg.subBand, NodeCfg.subBand * 8,
                NodeCfg.subBand * 8 + 7, NodeCfg.ch500 ? " + 500 kHz" : "" );
    }
    else
    {
        printf( "  %-10s 8 (todos os canais)\r\n", "subband" );
    }
    printf( "  %-10s %u\r\n", "ch500", NodeCfg.ch500 );
    printf( "  %-10s %u\r\n", "adr", NodeCfg.adr );
    printf( "  %-10s %u\r\n", "dr", NodeCfg.dr );
    printf( "  %-10s %d dBm\r\n", "txpower", NodeCfg.txPowDbm );
    printf( "  %-10s %c\r\n", "class", NodeCfg.devClass );
    printf( "  %-10s %u (ping slot a cada %u s)\r\n", "ping", NodeCfg.pingPeriodicity, 1U << NodeCfg.pingPeriodicity );
    printf( "  %-10s %lu s%s\r\n", "interval", ( unsigned long )NodeCfg.txIntervalS, NodeCfg.txIntervalS ? "" : " (desligado)" );
    printf( "  %-10s %u\r\n", "port", NodeCfg.fport );
    printf( "  %-10s %u\r\n", "confirmed", NodeCfg.confirmed );
    BytesToHex( NodeCfg.payload, NodeCfg.payloadLen, buf );
    printf( "  %-10s %u bytes · hex %s · texto \"", "payload", NodeCfg.payloadLen, buf );
    for( uint8_t i = 0; i < NodeCfg.payloadLen; i++ )
    {
        putchar( isprint( NodeCfg.payload[i] ) ? NodeCfg.payload[i] : '.' );
    }
    printf( "\"\r\n" );
    BytesToHex( NodeCfg.rstCmd, NodeCfg.rstLen, buf );
    printf( "  %-10s %s · porta %u · comando %s · espera %u ms\r\n", "rst", NodeCfg.rstEnable ? "ligado" : "desligado",
            NodeCfg.rstPort, buf, NodeCfg.rstDelayMs );
    printf( "  %-10s %u%s\r\n", "cmdport", NodeCfg.cmdPort, NodeCfg.cmdPort ? "" : " (desligado)" );
    printf( "  %-10s %u (%s)\r\n", "lowpower", NodeCfg.lowPower,
            ( NodeCfg.lowPower == 0 ) ? "sem STOP" : ( NodeCfg.lowPower == 1 ) ? "STOP, console pronto" : "STOP econômico" );
    printf( "  %-10s %u\r\n", "extpower", NodeCfg.extPower );
    printf( "  %-10s %u\r\n", "leds", NodeCfg.leds );
    printf( "  %-10s %u (diagnóstico de sessão a cada envio)\r\n", "verbose", NodeCfg.sessDiagOff ? 0 : 1 );
    printf( "  %-10s %u (%s)\r\n", "uplinkmode", NodeCfg.uplinkMode,
            ( NodeCfg.uplinkMode == NODE_UPLINK_V1 ) ? "telemetria ADAILSILVA_V1 na porta 20" : "payload fixo na 'port'" );
    {
        char rc[40];
        NodeRelayDescribe( rc, sizeof( rc ) );
        printf( "Relés e PZEM:\r\n  RELECFG %s (pinos:ativo-baixo) · PZEMCFG", rc );
        if( NodeCfg.pzemCount == 0 )
        {
            printf( " 0 (desligado)" );
        }
        for( uint8_t i = 0; i < NodeCfg.pzemCount; i++ )
        {
            printf( "%c%02x", i ? ',' : ' ', NodeCfg.pzemAddr[i] );
        }
        printf( "\r\n" );
    }
    {
        char io[64];
        NodeIoDescribe( io, sizeof( io ) );
        printf( "Comandos AT (firmware Radioenge):\r\n" );
        printf( "  TXP %u%s · DCS %u · PNM %u · RX2 %lu Hz DR%u · RX1DL %u · RX2DL %u · JN1DL %u · JN2DL %u ms\r\n",
                NodeCfg.txPowIdx, ( NodeCfg.txPowIdx == NODE_TXP_FROM_DBM ) ? " (usa txpower)" : "", NodeCfg.dutyCycle,
                NodeCfg.publicNet, ( unsigned long )NodeCfg.rx2Freq, NodeCfg.rx2Dr, NodeCfg.rx1DelayMs, NodeCfg.rx2DelayMs,
                NodeCfg.join1DelayMs, NodeCfg.join2DelayMs );
        printf( "  NBTRIALS %u · BAUDRATE %lu · KEEPALIVE %u:%u:%u:%lu · AT por downlink na porta %u%s\r\n",
                NodeCfg.nbTrials, ( unsigned long )NodeBaudrates[NodeCfg.baudIdx & 3], NodeCfg.kaEnable, NodeCfg.kaPort,
                NodeCfg.kaConfirmed, ( unsigned long )NodeCfg.kaPeriodMs, NodeCfg.atPort, NodeCfg.atPort ? "" : " (desligado)" );
        printf( "  GPIOC (modo:pull de GPIO0..9, '-' = não configurado) %s\r\n", io );
    }
}

static bool SetKey( const char *v, uint8_t *dst, size_t n )
{
    uint8_t tmp[16];
    if( HexToBytes( v, tmp, n ) != ( int )n )
    {
        printf( "Esperado %u bytes em hexadecimal (%u dígitos).\r\n", ( unsigned )n, ( unsigned )( 2 * n ) );
        Fail( ( n == 8 ) ? "esperado 16 dígitos hex" : "esperado 32 dígitos hex" );
        return false;
    }
    memcpy( dst, tmp, n );
    return true;
}

static void CmdSet( char *field, char *value )
{
    NodeConfig_t old = NodeCfg;
    uint32_t u;
    uint8_t b;
    bool ok = true;

    /* "set payload" / "set payloadhex" sem valor: payload vazio (uplink de 0 bytes) */
    if( ( field != NULL ) && ( value == NULL ) &&
        ( !strcmp( field, "payload" ) || !strcmp( field, "payloadhex" ) ) )
    {
        static char empty[1] = { 0 };
        value = empty;
    }

    if( ( field == NULL ) || ( value == NULL ) )
    {
        CmdSetHelp( );
        return;
    }

    if( !strcmp( field, "deveui" ) )
    {
        if( !strcmp( value, "auto" ) )
        {
            NodeCfg.devEuiAuto = 1;
            NodeDefaultDevEui( NodeCfg.devEui );
        }
        else if( ( ok = SetKey( value, NodeCfg.devEui, 8 ) ) )
        {
            NodeCfg.devEuiAuto = 0;
        }
    }
    else if( !strcmp( field, "joineui" ) )   ok = SetKey( value, NodeCfg.joinEui, 8 );
    else if( !strcmp( field, "appkey" ) )
    {
        bool sameNwk = ( memcmp( NodeCfg.appKey, NodeCfg.nwkKey, 16 ) == 0 );
        ok = SetKey( value, NodeCfg.appKey, 16 );
        if( ok && sameNwk )
        {
            memcpy( NodeCfg.nwkKey, NodeCfg.appKey, 16 );   /* LoRaWAN 1.0.x: NwkKey = AppKey */
        }
    }
    else if( !strcmp( field, "nwkkey" ) )    ok = SetKey( value, NodeCfg.nwkKey, 16 );
    else if( !strcmp( field, "nwkskey" ) )   ok = SetKey( value, NodeCfg.nwkSKey, 16 );
    else if( !strcmp( field, "appskey" ) )   ok = SetKey( value, NodeCfg.appSKey, 16 );
    else if( !strcmp( field, "devaddr" ) )
    {
        uint8_t a[4];
        if( ( ok = ( HexToBytes( value, a, 4 ) == 4 ) ) )
        {
            NodeCfg.devAddr = ( ( uint32_t )a[0] << 24 ) | ( ( uint32_t )a[1] << 16 ) | ( ( uint32_t )a[2] << 8 ) | a[3];
        }
    }
    else if( !strcmp( field, "netid" ) )     { ok = ParseU32( value, &u, 0, 0xFFFFFFFFUL ); if( ok ) NodeCfg.netId = u; }
    else if( !strcmp( field, "njm" ) )       { ok = ParseBool( value, &b ); if( ok ) NodeCfg.activation = b; }
    else if( !strcmp( field, "ajoin" ) )     { ok = ParseBool( value, &b ); if( ok ) NodeCfg.autoJoin = b; }
    else if( !strcmp( field, "nbtrials" ) )  { ok = ParseU32( value, &u, 0, 79 ); if( ok ) NodeCfg.nbTrials = ( uint8_t )u; }
    else if( !strcmp( field, "atport" ) )    { ok = ParseU32( value, &u, 0, 223 ); if( ok ) NodeCfg.atPort = ( uint8_t )u; }
    else if( !strcmp( field, "baud" ) )
    {
        ok = false;
        if( ParseU32( value, &u, 0, 1000000UL ) )
        {
            for( uint8_t i = 0; i < 4; i++ )
            {
                if( NodeBaudrates[i] == u )
                {
                    NodeCfg.baudIdx = i;
                    ok = true;
                }
            }
        }
    }
    else if( !strcmp( field, "subband" ) )   { ok = ParseU32( value, &u, 0, 8 ); if( ok ) NodeCfg.subBand = ( uint8_t )u; }
    else if( !strcmp( field, "ch500" ) )     { ok = ParseBool( value, &b ); if( ok ) NodeCfg.ch500 = b; }
    else if( !strcmp( field, "adr" ) )       { ok = ParseBool( value, &b ); if( ok ) NodeCfg.adr = b; }
    else if( !strcmp( field, "dr" ) )        { ok = ParseU32( value, &u, 2, 6 ); if( ok ) NodeCfg.dr = ( uint8_t )u; }
    else if( !strcmp( field, "txpower" ) )   { ok = ParseU32( value, &u, 2, 20 ); if( ok ) { NodeCfg.txPowDbm = ( int8_t )u; NodeCfg.txPowIdx = NODE_TXP_FROM_DBM; } }
    else if( !strcmp( field, "class" ) )
    {
        char c = ( char )toupper( ( unsigned char )value[0] );
        ok = ( value[1] == '\0' ) && ( c == 'A' || c == 'B' || c == 'C' );
        if( ok ) NodeCfg.devClass = c;
    }
    else if( !strcmp( field, "ping" ) )      { ok = ParseU32( value, &u, 0, 7 ); if( ok ) NodeCfg.pingPeriodicity = ( uint8_t )u; }
    else if( !strcmp( field, "interval" ) )  { ok = ParseU32( value, &u, 0, 86400 ); if( ok ) NodeCfg.txIntervalS = u; }
    else if( !strcmp( field, "port" ) )      { ok = ParseU32( value, &u, 1, 223 ); if( ok ) NodeCfg.fport = ( uint8_t )u; }
    else if( !strcmp( field, "confirmed" ) ) { ok = ParseBool( value, &b ); if( ok ) NodeCfg.confirmed = b; }
    else if( !strcmp( field, "payload" ) )
    {
        size_t n = strlen( value );
        ok = ( n <= NODE_PAYLOAD_MAX );
        if( ok )
        {
            memcpy( NodeCfg.payload, value, n );
            NodeCfg.payloadLen = ( uint8_t )n;
        }
    }
    else if( !strcmp( field, "payloadhex" ) )
    {
        int n = HexToBytes( value, NodeCfg.payload, NODE_PAYLOAD_MAX );
        ok = ( n >= 0 );
        if( ok ) NodeCfg.payloadLen = ( uint8_t )n;
    }
    else if( !strcmp( field, "rst" ) )       { ok = ParseBool( value, &b ); if( ok ) NodeCfg.rstEnable = b; }
    else if( !strcmp( field, "rstport" ) )   { ok = ParseU32( value, &u, 1, 223 ); if( ok ) NodeCfg.rstPort = ( uint8_t )u; }
    else if( !strcmp( field, "rstcmd" ) )
    {
        uint8_t tmp[NODE_RST_CMD_MAX];
        int n = HexToBytes( value, tmp, NODE_RST_CMD_MAX );
        ok = ( n >= 1 );
        if( ok )
        {
            memcpy( NodeCfg.rstCmd, tmp, n );
            NodeCfg.rstLen = ( uint8_t )n;
        }
    }
    else if( !strcmp( field, "rstdelay" ) )  { ok = ParseU32( value, &u, 0, 60000 ); if( ok ) NodeCfg.rstDelayMs = ( uint16_t )u; }
    else if( !strcmp( field, "cmdport" ) )   { ok = ParseU32( value, &u, 0, 223 ); if( ok ) NodeCfg.cmdPort = ( uint8_t )u; }
    else if( !strcmp( field, "lowpower" ) )  { ok = ParseU32( value, &u, 0, 2 ); if( ok ) NodeCfg.lowPower = ( uint8_t )u; }
    else if( !strcmp( field, "extpower" ) )  { ok = ParseBool( value, &b ); if( ok ) NodeCfg.extPower = b; }
    else if( !strcmp( field, "leds" ) )      { ok = ParseBool( value, &b ); if( ok ) NodeCfg.leds = b; }
    else if( !strcmp( field, "verbose" ) )   { ok = ParseBool( value, &b ); if( ok ) NodeCfg.sessDiagOff = b ? 0 : 1; }
    else if( !strcmp( field, "uplinkmode" ) ) { ok = ParseU32( value, &u, 0, 1 ); if( ok ) NodeCfg.uplinkMode = ( uint8_t )u; }
    /* campos dos comandos AT da Radioenge (mesmos limites do at_cmd.c), para a
     * página e o app gravarem pelo formulário com "save" */
    else if( !strcmp( field, "txp" ) )
    {
        ok = ParseU32( value, &u, 0, 255 ) && ( ( u <= 15 ) || ( u == NODE_TXP_FROM_DBM ) );
        if( ok ) NodeCfg.txPowIdx = ( uint8_t )u;
    }
    else if( !strcmp( field, "dcs" ) )       { ok = ParseBool( value, &b ); if( ok ) NodeCfg.dutyCycle = b; }
    else if( !strcmp( field, "pnm" ) )       { ok = ParseBool( value, &b ); if( ok ) NodeCfg.publicNet = b; }
    else if( !strcmp( field, "rx2fq" ) )     { ok = ParseU32( value, &u, 923300000UL, 927500000UL ); if( ok ) NodeCfg.rx2Freq = u; }
    else if( !strcmp( field, "rx2dr" ) )     { ok = ParseU32( value, &u, 8, 13 ); if( ok ) NodeCfg.rx2Dr = ( uint8_t )u; }
    else if( !strcmp( field, "rx1dl" ) )
    {
        /* como no AT+RX1DL: RX1 além da RX2 empurra a RX2 (RX2 = RX1 + 1 s) */
        ok = ParseU32( value, &u, 1000, 15000 );
        if( ok ) { NodeCfg.rx1DelayMs = ( uint16_t )u; if( NodeCfg.rx2DelayMs <= u ) NodeCfg.rx2DelayMs = ( uint16_t )( u + 1000 ); }
    }
    else if( !strcmp( field, "rx2dl" ) )     { ok = ParseU32( value, &u, 1000, 16000 ); if( ok ) NodeCfg.rx2DelayMs = ( uint16_t )u; }
    else if( !strcmp( field, "jn1dl" ) )
    {
        ok = ParseU32( value, &u, 1000, 15000 );
        if( ok ) { NodeCfg.join1DelayMs = ( uint16_t )u; if( NodeCfg.join2DelayMs <= u ) NodeCfg.join2DelayMs = ( uint16_t )( u + 1000 ); }
    }
    else if( !strcmp( field, "jn2dl" ) )     { ok = ParseU32( value, &u, 1000, 16000 ); if( ok ) NodeCfg.join2DelayMs = ( uint16_t )u; }
    else if( !strcmp( field, "keepalive" ) )
    {
        /* habilitado:porta:confirmado:periodo_ms, como o AT+KEEPALIVE */
        char *f[4] = { value, NULL, NULL, NULL };
        int nf = 1;
        for( char *p = value; *p && ( nf < 4 ); p++ )
        {
            if( *p == ':' ) { *p = '\0'; f[nf++] = p + 1; }
        }
        uint32_t en, port, cfm, per;
        ok = ( nf == 4 ) && ParseU32( f[0], &en, 0, 1 ) && ParseU32( f[1], &port, 1, 223 ) &&
             ParseU32( f[2], &cfm, 0, 1 ) && ParseU32( f[3], &per, 5000, 86400000UL );
        if( ok )
        {
            NodeCfg.kaEnable = ( uint8_t )en;
            NodeCfg.kaPort = ( uint8_t )port;
            NodeCfg.kaConfirmed = ( uint8_t )cfm;
            NodeCfg.kaPeriodMs = per;
        }
    }
    else if( !strcmp( field, "chmask" ) )
    {
        /* 6 palavras de 16 bits, como o AT+CHMASK (vira sub-banda quando bate) */
        uint8_t mb[12];
        uint16_t m[6];
        ok = ( HexToBytes( value, mb, sizeof( mb ) ) == 12 );
        for( int i = 0; ok && ( i < 6 ); i++ )
        {
            m[i] = ( uint16_t )( ( mb[2 * i] << 8 ) | mb[2 * i + 1] );
        }
        if( ok ) ok = NodeConfigSetChannelMask( &NodeCfg, m );
    }
    else
    {
        printf( "Campo desconhecido: %s\r\n", field );
        Fail( "campo desconhecido" );
        CmdSetHelp( );
        return;
    }

    char err[64];
    if( ok && !NodeConfigValidate( &NodeCfg, err, sizeof( err ) ) )
    {
        printf( "Inválido: %s\r\n", err );
        Fail( err );
        ok = false;
    }
    if( !ok )
    {
        NodeCfg = old;
        printf( "Valor não aceito.\r\n" );
        Fail( "valor não aceito" );
        return;
    }

    Dirty = true;
    NodeCfgRev++;
    if( NodeConfigCredFingerprint( &NodeCfg ) != NodeConfigCredFingerprint( &old ) )
    {
        NeedRestart = true;
    }
    /* parâmetros da MAC que o "save" precisa reaplicar (sem reiniciar) */
    if( ( old.rx1DelayMs != NodeCfg.rx1DelayMs ) || ( old.rx2DelayMs != NodeCfg.rx2DelayMs ) ||
        ( old.rx2Freq != NodeCfg.rx2Freq ) || ( old.rx2Dr != NodeCfg.rx2Dr ) )
    {
        RxDirty = true;
    }
    if( ( old.publicNet != NodeCfg.publicNet ) || ( old.dutyCycle != NodeCfg.dutyCycle ) ||
        ( old.join1DelayMs != NodeCfg.join1DelayMs ) || ( old.join2DelayMs != NodeCfg.join2DelayMs ) )
    {
        MacDirty = true;
    }
    printf( "OK (falta 'save').\r\n" );
}

static void CmdSave( void )
{
    if( !NodeConfigSave( ) )
    {
        printf( "Falha ao gravar na EEPROM.\r\n" );
        Fail( "falha ao gravar na EEPROM" );
        return;
    }
    Dirty = false;
    printf( "Gravado.\r\n" );
    if( NeedRestart )
    {
        NeedRestart = false;
        printf( "Credenciais ou canais mudaram: a sessão LoRaWAN recomeça do zero.\r\n" );
        CmdNote = " restart";
        AppReboot( 300 );
        return;
    }
    if( MacDirty || RxDirty )
    {
        AppApplyMacParams( RxDirty );
    }
    MacDirty = RxDirty = false;
    AppApplyRuntimeConfig( );
    if( NodeCfg.baudIdx != AppliedBaud )
    {
        /* troca depois da resposta (@OK na velocidade antiga, para a página e o app) */
        printf( "Serial agora em %lu bps.\r\n", ( unsigned long )NodeBaudrates[NodeCfg.baudIdx & 3] );
        CmdNote = " baud";
        BaudPending = true;
    }
}

static void ApplyPendingBaud( void )
{
    if( BaudPending )
    {
        BaudPending = false;
        UartMcuFlush( );
        AppSetConsoleBaudrate( NodeBaudrates[NodeCfg.baudIdx & 3] );
        AppliedBaud = NodeCfg.baudIdx;
    }
}

static void CmdSend( char *arg )
{
    if( !AppStatus.joined )
    {
        printf( "Sem sessão LoRaWAN ainda.\r\n" );
        Fail( "sem sessão LoRaWAN" );
        return;
    }
    if( ( arg == NULL ) || ( *arg == '\0' ) )
    {
        AppSendNow( NULL, 0, true );
        printf( "Uplink do payload configurado na fila.\r\n" );
        return;
    }
    uint8_t data[NODE_PAYLOAD_MAX];
    int n;
    if( !strncmp( arg, "hex:", 4 ) )
    {
        n = HexToBytes( arg + 4, data, sizeof( data ) );
        if( n < 0 )
        {
            printf( "Hexadecimal inválido.\r\n" );
            Fail( "hexadecimal inválido" );
            return;
        }
    }
    else
    {
        n = ( int )strlen( arg );
        if( n > NODE_PAYLOAD_MAX )
        {
            printf( "Até %d bytes.\r\n", NODE_PAYLOAD_MAX );
            Fail( "payload grande demais" );
            return;
        }
        memcpy( data, arg, n );
    }
    AppSendNow( data, ( uint8_t )n, false );
    printf( "Uplink de %d bytes na fila.\r\n", n );
}

static void CmdClock( char *arg )
{
    if( ( arg != NULL ) && !strcmp( arg, "recal" ) )
    {
        if( LoRaMacIsBusy( ) )
        {
            printf( "MAC ocupada (TX/RX em andamento): tente de novo em instantes.\r\n" );
            Fail( "MAC ocupada" );
        }
        else if( RtcRecalibrate( ) )
        {
            printf( "LSI medido de novo.\r\n" );
        }
        else if( RtcGetClockInfo( )->Source == RTC_CLOCK_LSE )
        {
            printf( "Nada a recalibrar (relógio no cristal LSE).\r\n" );
        }
        else
        {
            printf( "Medida do LSI falhou: mantida a calibração anterior.\r\n" );
        }
    }
    const RtcClockInfo_t *ci = RtcGetClockInfo( );
    uint32_t rateMilli = ( uint32_t )( ( ( uint64_t )ci->TickRateQ16 * 1000ULL ) >> 16 );
    printf( "Relógio do RTC: %s\r\n", AppClockSourceName( ) );
    printf( "  RTCCLK %lu Hz · PREDIV_A %lu · tick %lu.%03lu Hz (nominal 1024) · ~%lu ppm\r\n",
            ( unsigned long )( ci->RtcClkHz + 0.5f ), ( unsigned long )ci->PredivA, ( unsigned long )( rateMilli / 1000 ),
            ( unsigned long )( rateMilli % 1000 ), ( unsigned long )ci->PpmEstimate );
    if( ci->HseHz )
    {
        printf( "  cristal HSE de %lu Hz usado como referência\r\n", ( unsigned long )ci->HseHz );
    }
    if( ci->Source != RTC_CLOCK_LSE )
    {
        printf( "  Sem cristal de 32,768 kHz (PC14/PC15): a Classe B fica instável.\r\n" );
    }
}

/* ------------------------------------------------- saídas para o app ---- */
/* Uma linha por resposta: "@I {...}", "@S {...}", "@C {...}" (JSON). */
static void JsonHex( const char *key, const uint8_t *b, size_t n, bool comma )
{
    char hex[2 * NODE_PAYLOAD_MAX + 1];
    BytesToHex( b, n, hex );
    printf( "\"%s\":\"%s\"%s", key, hex, comma ? "," : "" );
}

/* Chaves da sessão: "keys" (humano) e "jkeys" (JSON para a página e o app) */
static void CmdKeys( bool json )
{
    uint32_t da = 0;
    uint8_t nwk[16], app[16];
    char hn[33], ha[33];
    bool ok = AppGetSessionKeys( &da, nwk, app );
    uint32_t age = ok ? ( AppUptimeMs( ) - AppStatus.sessionAtMs ) / 1000 : 0;
    const char *mode = NodeIsOtaa( ) ? "otaa" : "abp";
    if( ok )
    {
        BytesToHex( nwk, 16, hn );
        BytesToHex( app, 16, ha );
    }
    if( json )
    {
        if( ok )
        {
            printf( "@K {\"valid\":1,\"mode\":\"%s\",\"devaddr\":\"%08lX\",\"nwk\":\"%s\",\"app\":\"%s\",\"age\":%lu,\"restored\":%d}\r\n",
                    mode, ( unsigned long )da, hn, ha, ( unsigned long )age, AppStatus.sessionRestored );
        }
        else
        {
            printf( "@K {\"valid\":0,\"mode\":\"%s\"}\r\n", mode );
        }
        return;
    }
    if( !ok )
    {
        printf( "Sem sessão LoRaWAN: %s\r\n", NodeIsOtaa( )
                ? "as chaves são derivadas quando o JOIN for aceito." : "a sessão ABP ainda não foi ativada." );
        Fail( "sem sessão" );
        return;
    }
    printf( "Chaves de sessão (%s, %s há %lu s):\r\n", NodeIsOtaa( ) ? "OTAA" : "ABP",
            AppStatus.sessionRestored ? "restaurada da EEPROM" : ( NodeIsOtaa( ) ? "JOIN aceito" : "ativada" ),
            ( unsigned long )age );
    printf( "  DevAddr  %08lX\r\n  NwkSKey  %s\r\n  AppSKey  %s\r\n", ( unsigned long )da, hn, ha );
}

static void CmdInfo( void )
{
    char eui[17];
    BytesToHex( NodeCfg.devEui, 8, eui );
    printf( "@I {\"fw\":\"%s\",\"ver\":\"%s\",\"stack\":\"%s\",\"mode\":\"%s\",\"mcu\":\"STM32L071CZ\","
            "\"radio\":\"SX1272\",\"region\":\"AU915\",\"deveui\":\"%s\"}\r\n",
            NODE_FW_NAME, NODE_FW_VERSION, NODE_STACK_VERSION,
            NodeIsOtaa( ) ? "otaa" : "abp", eui );
}

static void CmdJsonStatus( void )
{
    const RtcClockInfo_t *ci = RtcGetClockInfo( );
    SysTime_t t = SysTimeGet( );
    int32_t next = ( int32_t )( AppStatus.nextTxMs - AppUptimeMs( ) );
    const char *src = ( ci->Source == RTC_CLOCK_LSE ) ? "lse" : ( ci->Source == RTC_CLOCK_LSI_HSE_REF ) ? "lsi_hse" : "lsi_hsi";

    printf( "@S {\"up\":%lu,\"joined\":%d,\"joining\":%d,\"devaddr\":\"%08lX\",\"joins\":%lu,\"devnonce\":%u,",
            ( unsigned long )( AppUptimeMs( ) / 1000 ), AppStatus.joined, AppStatus.joining,
            ( unsigned long )AppStatus.devAddr, ( unsigned long )AppStatus.joinAttempts, AppGetDevNonce( ) );
    /* cls = estado do firmware; maccls = a classe que a LoRaMac-node usa de fato */
    printf( "\"cls\":\"%c\",\"maccls\":\"%c\",\"cfgcls\":\"%c\",\"beacon\":\"%c\",\"brx\":%lu,\"bmiss\":%lu,\"ping\":%u,",
            AppStatus.activeClass, AppCurrentClass( ), NodeCfg.devClass, AppStatus.beacon, ( unsigned long )AppStatus.beaconsRx,
            ( unsigned long )AppStatus.beaconsMissed, NodeCfg.pingPeriodicity );
    printf( "\"upl\":%lu,\"uperr\":%lu,\"acks\":%lu,\"fcntup\":%lu,\"dr\":%d,\"next\":%ld,\"interval\":%lu,",
            ( unsigned long )AppStatus.uplinks, ( unsigned long )AppStatus.uplinkErrors, ( unsigned long )AppStatus.acks,
            ( unsigned long )AppStatus.fcntUp, LmHandlerGetCurrentDatarate( ), ( long )( ( next > 0 ) ? next / 1000 : 0 ),
            ( unsigned long )NodeCfg.txIntervalS );
    printf( "\"rrec\":%lu,", ( unsigned long )AppRadioRecoveries( NULL, NULL ) );
    printf( "\"dnl\":%lu,\"fcntdn\":%lu,\"lslot\":\"%s\",\"lport\":%u,\"rssi\":%d,\"snr\":%d,",
            ( unsigned long )AppStatus.downlinks, ( unsigned long )AppStatus.fcntDown, AppStatus.lastSlot,
            AppStatus.lastPort, AppStatus.lastRssi, AppStatus.lastSnr );
    JsonHex( "ldata", AppStatus.lastData, ( AppStatus.lastLen > 32 ) ? 32 : AppStatus.lastLen, true );
    printf( "\"gps\":%lu,\"synced\":%d,\"clk\":\"%s\",\"clkhz\":%lu,\"ppm\":%lu,\"hse\":%lu,",
            ( unsigned long )t.Seconds, AppStatus.timeSynced, src, ( unsigned long )( ci->RtcClkHz + 0.5f ),
            ( unsigned long )ci->PpmEstimate, ( unsigned long )ci->HseHz );
    printf( "\"freq\":%lu,\"txpow\":%d,",
            ( unsigned long )BoardGetLastTxFreq( ), BoardGetLastTxPower( ) );
    /* cfgrev: muda a cada alteração da configuração (veja NodeCfgRev) */
    printf( "\"vdd\":%lu,\"lowpower\":%u,\"ext\":%u,\"dirty\":%d,\"cfgrev\":%u}\r\n",
            ( unsigned long )BoardGetBatteryVoltage( ), NodeCfg.lowPower, NodeCfg.extPower, Dirty,
            ( unsigned )NodeCfgRev );
}

static void CmdJsonConfig( void )
{
    printf( "@C {\"devEuiAuto\":%u,", NodeCfg.devEuiAuto );
    JsonHex( "deveui", NodeCfg.devEui, 8, true );
    JsonHex( "joineui", NodeCfg.joinEui, 8, true );
    JsonHex( "appkey", NodeCfg.appKey, 16, true );
    JsonHex( "nwkkey", NodeCfg.nwkKey, 16, true );
    printf( "\"devaddr\":\"%08lX\",", ( unsigned long )NodeCfg.devAddr );
    JsonHex( "nwkskey", NodeCfg.nwkSKey, 16, true );
    JsonHex( "appskey", NodeCfg.appSKey, 16, true );
    printf( "\"netid\":\"%06lX\",\"subband\":%u,\"ch500\":%u,\"adr\":%u,\"dr\":%u,\"txpower\":%d,",
            ( unsigned long )NodeCfg.netId, NodeCfg.subBand, NodeCfg.ch500, NodeCfg.adr, NodeCfg.dr, NodeCfg.txPowDbm );
    printf( "\"class\":\"%c\",\"ping\":%u,\"interval\":%lu,\"port\":%u,\"confirmed\":%u,",
            NodeCfg.devClass, NodeCfg.pingPeriodicity, ( unsigned long )NodeCfg.txIntervalS, NodeCfg.fport, NodeCfg.confirmed );
    JsonHex( "payload", NodeCfg.payload, NodeCfg.payloadLen, true );
    printf( "\"rst\":%u,\"rstport\":%u,", NodeCfg.rstEnable, NodeCfg.rstPort );
    JsonHex( "rstcmd", NodeCfg.rstCmd, NodeCfg.rstLen, true );
    printf( "\"rstdelay\":%u,\"cmdport\":%u,\"lowpower\":%u,\"extpower\":%u,\"leds\":%u,\"verbose\":%u,",
            NodeCfg.rstDelayMs, NodeCfg.cmdPort, NodeCfg.lowPower, NodeCfg.extPower, NodeCfg.leds, NodeCfg.sessDiagOff ? 0 : 1 );
    {
        uint16_t m[6];
        char io[64];
        NodeConfigChannelMask( &NodeCfg, m );
        NodeIoDescribe( io, sizeof( io ) );
        printf( "\"njm\":%u,\"ajoin\":%u,\"txp\":%u,\"dcs\":%u,\"pnm\":%u,\"rx2fq\":%lu,\"rx2dr\":%u,",
                NodeCfg.activation, NodeCfg.autoJoin, NodeCfg.txPowIdx, NodeCfg.dutyCycle, NodeCfg.publicNet,
                ( unsigned long )NodeCfg.rx2Freq, NodeCfg.rx2Dr );
        printf( "\"rx1dl\":%u,\"rx2dl\":%u,\"jn1dl\":%u,\"jn2dl\":%u,\"nbtrials\":%u,\"baud\":%lu,",
                NodeCfg.rx1DelayMs, NodeCfg.rx2DelayMs, NodeCfg.join1DelayMs, NodeCfg.join2DelayMs, NodeCfg.nbTrials,
                ( unsigned long )NodeBaudrates[NodeCfg.baudIdx & 3] );
        printf( "\"keepalive\":\"%u:%u:%u:%lu\",\"chmask\":\"%04x%04x%04x%04x%04x%04x\",\"gpio\":\"%s\",\"atport\":%u,",
                NodeCfg.kaEnable, NodeCfg.kaPort, NodeCfg.kaConfirmed, ( unsigned long )NodeCfg.kaPeriodMs,
                m[0], m[1], m[2], m[3], m[4], m[5], io, NodeCfg.atPort );
    }
    {
        /* relés ("4,5:1,1" / "0"), PZEM ("f8" / "01,02" / "0") e conteúdo do uplink */
        char rc[40], pz[16] = "0";
        uint8_t n = 0;
        NodeRelayDescribe( rc, sizeof( rc ) );
        for( uint8_t i = 0; i < NodeCfg.pzemCount; i++ )
        {
            n += ( uint8_t )snprintf( pz + n, sizeof( pz ) - n, "%s%02x", i ? "," : "", NodeCfg.pzemAddr[i] );
        }
        printf( "\"uplinkmode\":%u,\"relecfg\":\"%s\",\"pzemcfg\":\"%s\",", NodeCfg.uplinkMode, rc, pz );
    }
    printf( "\"dirty\":%d}\r\n", Dirty );
}

/* GPIOs ao vivo para a página e o app (seção GPIOs):
 *   @IO {"con2":0,"bat":200,"tr":0,"g":[[modo,pull,valor],...]}
 * modo -1 = não configurado; valor -1 = sem leitura (função alternativa ou não
 * configurado), 0/1 nas digitais, 0..4095 nas analógicas. */
static void CmdJsonIo( void )
{
    printf( "@IO {\"con2\":%d,\"bat\":%u,\"tr\":%d,\"pz\":%d,\"rel\":%u,\"g\":[", ( CONSOLE_UART == 2 ) ? 1 : 0,
            NodeIoBatteryLevel( ), NodeIoTransparentActive( ) ? 1 : 0, NodeCfg.pzemCount ? 1 : 0,
            ( unsigned )NodeConfigRelayPins( &NodeCfg ) );
    for( uint8_t g = 0; g < NODE_GPIO_COUNT; g++ )
    {
        uint16_t v = 0;
        uint8_t m = NodeCfg.gpioMode[g];
        bool rd = ( m != NODE_GPIO_UNUSED ) && ( NodeIoRead( g, &v ) == NODE_IO_OK );
        printf( "%s[%d,%u,%d]", g ? "," : "", ( m == NODE_GPIO_UNUSED ) ? -1 : ( int )m, NodeCfg.gpioPull[g],
                rd ? ( int )v : -1 );
    }
    printf( "]}\r\n" );
}

/* Relés, PZEM e protocolo V1 ao vivo para a página e o app (seção Relés e PZEM):
 *   @DV {"umode":1,"rq":2,"rmap":1,"rpul":0,"rcfg":"4,5:1,1","pq":1,"pon":1,"pbusy":0,
 *        "padr":[248],"pz":[{"ok":1,"age":12,"v":2300,"i":500,"p":1200,"e":12345,"f":600,"pf":95,"al":0}],
 *        "v1c":3,"v1u":5,"boot":1}
 * Unidades do protocolo: v 0,1 V · i 0,001 A · p 0,1 W · e Wh · f 0,1 Hz · pf 0,01.
 * Medidor sem leitura: {"ok":0,"fail":0|1}. */
static void CmdJsonDev( void )
{
    char rc[40];
    NodeRelayDescribe( rc, sizeof( rc ) );
    printf( "@DV {\"umode\":%u,\"rq\":%u,\"rmap\":%u,\"rpul\":%u,\"rcfg\":\"%s\",\"pq\":%u,\"pon\":%d,\"pbusy\":%d,\"padr\":[",
            NodeCfg.uplinkMode, NodeRelayCount( ), NodeRelayMap( ), NodeRelayPulsing( ), rc, NodeCfg.pzemCount,
            NodePzemActive( ) ? 1 : 0, NodePzemBusy( ) ? 1 : 0 );
    for( uint8_t i = 0; i < NodeCfg.pzemCount; i++ )
    {
        printf( "%s%u", i ? "," : "", NodeCfg.pzemAddr[i] );
    }
    printf( "],\"pz\":[" );
    for( uint8_t ch = 0; ch < NodeCfg.pzemCount; ch++ )
    {
        uint8_t b[NODE_PZEM_BLOCK_LEN];
        uint32_t age;
        if( NodePzemLast( ch, b, &age ) )
        {
            printf( "%s{\"ok\":1,\"age\":%lu,\"v\":%u,\"i\":%lu,\"p\":%lu,\"e\":%lu,\"f\":%u,\"pf\":%u,\"al\":%u}", ch ? "," : "",
                    ( unsigned long )age, ( unsigned )( ( b[0] << 8 ) | b[1] ),
                    ( unsigned long )( ( ( uint32_t )b[2] << 24 ) | ( ( uint32_t )b[3] << 16 ) | ( ( uint32_t )b[4] << 8 ) | b[5] ),
                    ( unsigned long )( ( ( uint32_t )b[6] << 24 ) | ( ( uint32_t )b[7] << 16 ) | ( ( uint32_t )b[8] << 8 ) | b[9] ),
                    ( unsigned long )( ( ( uint32_t )b[10] << 24 ) | ( ( uint32_t )b[11] << 16 ) | ( ( uint32_t )b[12] << 8 ) | b[13] ),
                    ( unsigned )( ( b[14] << 8 ) | b[15] ), b[16], b[17] );
        }
        else
        {
            printf( "%s{\"ok\":0,\"fail\":%d}", ch ? "," : "", NodePzemLastFailed( ch ) ? 1 : 0 );
        }
    }
    printf( "],\"v1c\":%lu,\"v1u\":%lu,\"boot\":%u}\r\n", ( unsigned long )AppStatus.v1Commands,
            ( unsigned long )AppStatus.v1Uplinks, NodeV1BootReason( ) );
}

/* Resumo das classes LoRaWAN (o mesmo da página WEB e do app, seção Classes) */
static void CmdClasses( void )
{
    printf( "Classes LoRaWAN: a classe define QUANDO o nó escuta downlinks. O uplink é igual nas três.\r\n"
            "Todo nó precisa da Classe A; B e C são modos adicionais. O gateway não tem classe.\r\n\r\n"
            "  A  Fala, depois escuta  · RX1 e RX2 logo depois de cada uplink\r\n"
            "     atraso até o próximo uplink · consumo mínimo · sensores a bateria\r\n"
            "  B  Horários combinados  · + ping slots a cada 1..128 s, sincronizados pelo beacon\r\n"
            "     atraso até o próximo ping slot · exige gateway com GPS + PPS · fechadura, válvula\r\n"
            "  C  Escuta o tempo todo  · recepção contínua na RX2, exceto no TX\r\n"
            "     downlink quase imediato · ~10-12 mA só no rádio · relé, iluminação, alarme\r\n\r\n"
            "Neste nó (%s):\r\n"
            "  A  implementada (padrão de fábrica)\r\n"
            "  B  implementada: DeviceTimeReq -> beacon -> PingSlotInfoReq (ping 2^n s, 'set ping')\r\n"
            "     sem o cristal de 32,768 kHz o relógio é o LSI calibrado e os ping slots podem ser perdidos\r\n"
            "  C  implementada: RX2 contínua em 923,3 MHz (DR8)\r\n"
            "Classe em uso: %c (configurada: %c). Troque com 'class A|B|C' ou downlink 01 00/01/02 na FPort %u.\r\n"
            "No ChirpStack v4 a classe fica no perfil do dispositivo (Class-B / Class-C).\r\n",
            NODE_STACK_VERSION, AppCurrentClass( ), NodeCfg.devClass, NodeCfg.cmdPort );
}

/* Comandos que só consultam (não entram no log em RAM) */
static bool IsQuery( const char *c )
{
    static const char *const q[] = { "help", "?", "status", "config", "info", "jstatus", "jconfig", "keys", "jkeys",
                                     "log", "jlog", "diag", "jdiag", "classes", "mode", "jio", "jdev", NULL };
    for( int i = 0; q[i]; i++ )
    {
        if( !strcmp( c, q[i] ) ) return true;
    }
    return false;
}

/* Linha "> comando" no log em RAM (as chaves ficam ocultas) */
static void LogCommand( const char *line )
{
    char cmd[12] = { 0 }, f[12] = { 0 };
    sscanf( line, " %11s %11s", cmd, f );
    for( char *c = cmd; *c; c++ ) *c = ( char )tolower( ( unsigned char )*c );
    for( char *c = f; *c; c++ ) *c = ( char )tolower( ( unsigned char )*c );
    if( !cmd[0] || IsQuery( cmd ) || ( !strcmp( cmd, "clock" ) && strcmp( f, "recal" ) ) )
    {
        return;
    }
    if( AtIsAtLine( line ) )
    {
        /* AT: leituras (=?) e ajuda não entram; chaves ficam ocultas */
        const char *eq = strchr( line, '=' );
        if( ( eq == NULL ) ? ( strchr( line, '?' ) != NULL ) : ( strchr( eq, '?' ) != NULL && eq[1] == '?' ) )
        {
            return;
        }
        if( ( eq != NULL ) && ( strstr( cmd, "key" ) != NULL ) )
        {
            NodeLogLine( "> %.*s= (oculta)%s", ( int )( eq - line ), line, CmdOk ? "" : " -> erro" );
            return;
        }
        NodeLogLine( "> %.80s%s", line, CmdOk ? "" : " -> erro" );
        return;
    }
    if( !strcmp( cmd, "set" ) && ( !strcmp( f, "appkey" ) || !strcmp( f, "nwkkey" ) || !strcmp( f, "nwkskey" ) || !strcmp( f, "appskey" ) ) )
    {
        NodeLogLine( "> set %s (oculta)%s", f, CmdOk ? "" : " -> erro" );
        return;
    }
    NodeLogLine( "> %.80s%s", line, CmdOk ? "" : " -> erro" );
}

static void Execute( char *line )
{
    char *argv[4] = { 0 };
    int argc = 0;
    char *p = line;

    while( *p && ( argc < 4 ) )
    {
        while( *p == ' ' )
        {
            p++;
        }
        if( !*p )
        {
            break;
        }
        argv[argc++] = p;
        /* o último argumento de "set payload" e de "send" pode ter espaços */
        bool rest = ( argc == 3 && !strcmp( argv[0], "set" ) && argv[1] && !strcmp( argv[1], "payload" ) ) ||
                    ( argc == 2 && !strcmp( argv[0], "send" ) ) ||
                    ( argc == 4 && !strcmp( argv[0], "rele" ) );      /* rele pulso <c> "<e> <ms>" */
        if( rest )
        {
            break;
        }
        while( *p && ( *p != ' ' ) )
        {
            p++;
        }
        if( *p )
        {
            *p++ = '\0';
        }
    }
    if( argc == 0 )
    {
        return;
    }
    for( char *c = argv[0]; *c; c++ )
    {
        *c = ( char )tolower( ( unsigned char )*c );
    }
    if( argc >= 2 && !strcmp( argv[0], "set" ) )
    {
        for( char *c = argv[1]; *c; c++ )
        {
            *c = ( char )tolower( ( unsigned char )*c );
        }
    }

    if( !strcmp( argv[0], "help" ) || !strcmp( argv[0], "?" ) )  CmdHelp( );
    else if( !strcmp( argv[0], "status" ) )                       CmdStatus( );
    else if( !strcmp( argv[0], "config" ) )                       CmdConfig( );
    else if( !strcmp( argv[0], "set" ) )                          CmdSet( argv[1], argv[2] );
    else if( !strcmp( argv[0], "save" ) )                         CmdSave( );
    else if( !strcmp( argv[0], "class" ) )
    {
        char c = argv[1] ? ( char )toupper( ( unsigned char )argv[1][0] ) : 0;
        if( ( c != 'A' ) && ( c != 'B' ) && ( c != 'C' ) )
        {
            printf( "Uso: class A|B|C\r\n" );
            Fail( "uso: class A|B|C" );
        }
        else
        {
            NodeCfg.devClass = c;
            NodeConfigSave( );
            printf( "Classe %c gravada.\r\n", c );
            AppRequestClass( c );
        }
    }
    else if( !strcmp( argv[0], "send" ) )                         CmdSend( argv[1] );
    else if( !strcmp( argv[0], "join" ) )                         AppRejoin( );
    else if( !strcmp( argv[0], "devtime" ) )                      AppRequestDeviceTime( );
    else if( !strcmp( argv[0], "clock" ) )                        CmdClock( argv[1] );
    else if( !strcmp( argv[0], "factory" ) )
    {
        bool zeroNonce = ( argv[1] != NULL ) && !strcmp( argv[1], "nonce" );
        printf( "Voltando ao padrão de fábrica%s...\r\n", zeroNonce ? " (DevNonce zerado)" : " (DevNonce mantido)" );
        AppFactoryReset( !zeroNonce );
    }
    else if( !strcmp( argv[0], "reboot" ) )                       AppReboot( 300 );
    else if( !strcmp( argv[0], "mode" ) )
    {
        if( argv[1] && !strcmp( argv[1], "app" ) )        AppMode = true;
        else if( argv[1] && !strcmp( argv[1], "human" ) ) AppMode = false;
        else                                             Fail( "uso: mode app|human" );
    }
    else if( !strcmp( argv[0], "info" ) )                         CmdInfo( );
    else if( !strcmp( argv[0], "jstatus" ) )                      CmdJsonStatus( );
    else if( !strcmp( argv[0], "jconfig" ) )                      CmdJsonConfig( );
    else if( !strcmp( argv[0], "keys" ) )                         CmdKeys( false );
    else if( !strcmp( argv[0], "jkeys" ) )                        CmdKeys( true );
    else if( !strcmp( argv[0], "jio" ) )                          CmdJsonIo( );
    else if( !strcmp( argv[0], "jdev" ) )                         CmdJsonDev( );
    else if( !strcmp( argv[0], "rele" ) )                         CmdRelay( argv[1], argv[2], argv[3] );
    else if( !strcmp( argv[0], "pzem" ) )                         CmdPzem( argv[1], argv[2] );
    else if( !strcmp( argv[0], "log" ) )
    {
        if( argv[1] && !strcmp( argv[1], "clear" ) )
        {
            NodeLogClear( );
            printf( "Log em RAM apagado.\r\n" );
        }
        else
        {
            NodeLogDump( );
        }
    }
    else if( !strcmp( argv[0], "jlog" ) )
    {
        uint32_t from = 0;
        if( argv[1] && !ParseU32( argv[1], &from, 0, 0xFFFFFFFFUL ) )
        {
            Fail( "uso: jlog [byte inicial]" );
        }
        else
        {
            NodeLogDumpApp( from );
        }
    }
    else if( !strcmp( argv[0], "diag" ) )                         NodeDiagRun( false );
    else if( !strcmp( argv[0], "jdiag" ) )                        NodeDiagRun( true );
    else if( !strcmp( argv[0], "classes" ) )                      CmdClasses( );
    else
    {
        printf( "Comando desconhecido: %s ('help' lista os comandos)\r\n", argv[0] );
        Fail( "comando desconhecido" );
    }
}

void ConsoleProcess( void )
{
    uint8_t c;
    static char Copy[100];

    /* eco, prompt e respostas dos comandos não entram no log em RAM */
    NodeLogPause( true );

    while( UartGetChar( &ConsoleUart, &c ) == 0 )
    {
        if( ( c == '\r' ) || ( c == '\n' ) )
        {
            if( ( c == '\n' ) && ( LineLen == 0 ) )
            {
                continue;   /* \n depois de \r */
            }
            if( !AppMode )
            {
                printf( "\r\n" );
            }
            Line[LineLen] = '\0';
            CmdOk = true;
            CmdErr[0] = '\0';
            CmdNote = "";
            bool empty = ( LineLen == 0 );
            size_t cl = ( LineLen < sizeof( Copy ) - 1 ) ? LineLen : sizeof( Copy ) - 1;
            memcpy( Copy, Line, cl );
            Copy[cl] = '\0';
            if( !empty )
            {
                AppConsoleActivity( );          /* LED amarelo, como no original */
            }
            if( AtIsAtLine( Line ) )
            {
                if( !AtExecute( Line ) )
                {
                    Fail( "AT" );
                }
                if( AtSavedConfig( ) )
                {
                    Dirty = false;              /* o AT grava a configuração inteira */
                }
                AppliedBaud = NodeCfg.baudIdx;  /* AT+BAUDRATE já trocou a velocidade */
            }
            else
            {
                Execute( Line );
            }
            LogCommand( Copy );
            if( AppMode && !empty )
            {
                if( CmdOk )
                {
                    printf( "@OK%s\r\n", CmdNote );
                }
                else
                {
                    printf( "@ERR %s\r\n", CmdErr );
                }
            }
            ApplyPendingBaud( );
            LineLen = 0;
            Prompt( );
        }
        else if( ( c == 0x08 ) || ( c == 0x7F ) )
        {
            if( LineLen > 0 )
            {
                LineLen--;
                if( !AppMode )
                {
                    printf( "\b \b" );
                }
            }
        }
        else if( ( c >= 0x20 ) && ( LineLen < LINE_MAX - 1 ) )
        {
            Line[LineLen++] = ( char )c;
            if( !AppMode )
            {
                putchar( c );
            }
        }
    }
    fflush( stdout );
    NodeLogPause( false );
}
