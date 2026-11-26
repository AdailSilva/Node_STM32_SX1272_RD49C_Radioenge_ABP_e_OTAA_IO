/*
 * node_diag.c — autoteste do nó (comandos "diag" e "jdiag").
 *
 * O módulo Radioenge não tem rede IP: ping, traceroute e nslookup rodam no
 * computador (página WEB) ou no celular (app). Aqui ficam os testes que só o
 * próprio nó sabe fazer: rádio no SPI, configuração na EEPROM, sessão LoRaWAN,
 * tráfego, relógio, alimentação, pilha, log e causa do último reinício.
 *
 *   diag    texto, uma linha por teste:  [ ok ] Rádio SX1272 ....... versão 0x22
 *   jdiag   modo app: "@D {"items":[{"n":"...","l":"ok|aviso|falha|info","v":"..."}],...}"
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "board-ext.h"
#include "LmHandler.h"
#include "node_build.h"
#include "node_config.h"
#include "node_log.h"
#include "node_diag.h"
#include "app.h"

typedef enum { L_OK, L_WARN, L_FAIL, L_INFO } Level_t;

static bool Json;
static bool First;
static uint8_t Nok, Nwarn, Nfail;

static void Item( const char *name, Level_t lv, const char *fmt, ... ) __attribute__( ( format( printf, 3, 4 ) ) );

static void Item( const char *name, Level_t lv, const char *fmt, ... )
{
    char v[96];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( v, sizeof( v ), fmt, ap );
    va_end( ap );
    static const char *const key[] = { "ok", "aviso", "falha", "info" };
    static const char *const tag[] = { "[ ok ]", "[AVISO]", "[FALHA]", "[info]" };
    if( lv == L_OK ) Nok++;
    if( lv == L_WARN ) Nwarn++;
    if( lv == L_FAIL ) Nfail++;
    if( Json )
    {
        printf( "%s{\"n\":\"%s\",\"l\":\"%s\",\"v\":\"%s\"}", First ? "" : ",", name, key[lv], v );
    }
    else
    {
        /* nome alinhado em 22 colunas (os acentos ocupam 2 bytes em UTF-8) */
        int w = 0;
        for( const char *c = name; *c; c++ )
        {
            if( ( ( unsigned char )*c & 0xC0 ) != 0x80 ) w++;
        }
        printf( "  %-7s %s ", tag[lv], name );
        for( int i = w; i < 22; i++ )
        {
            putchar( '.' );
        }
        printf( " %s\r\n", v );
    }
    First = false;
}

static bool AllZero( const uint8_t *b, size_t n )
{
    for( size_t i = 0; i < n; i++ )
    {
        if( b[i] ) return false;
    }
    return true;
}

static void RunTests( void )
{
    /* 1. Rádio no SPI */
    uint8_t ver = AppRadioVersion( );
    if( ver == 0x22 )
    {
        Item( "Rádio SX1272", L_OK, "responde no SPI (versão 0x%02X)", ver );
    }
    else
    {
        Item( "Rádio SX1272", L_FAIL, "versão 0x%02X (esperado 0x22): SPI ou rádio com problema", ver );
    }

    /* 2. Configuração e credenciais */
    Item( "Configuração", AppConfigFromEeprom( ) ? L_OK : L_WARN, "%s",
          AppConfigFromEeprom( ) ? "lida da EEPROM" : "padrão de fábrica (EEPROM vazia ou inválida)" );
    if( NodeIsOtaa( ) )
    {
        bool zero = AllZero( NodeCfg.appKey, 16 ) || AllZero( NodeCfg.joinEui, 8 );
        Item( "Credenciais OTAA", zero ? L_FAIL : L_OK, "%s", zero ? "AppKey ou JoinEUI zerados" : "JoinEUI e AppKey preenchidos" );
    }
    else
    {
        bool zero = ( NodeCfg.devAddr == 0 ) || AllZero( NodeCfg.nwkSKey, 16 ) || AllZero( NodeCfg.appSKey, 16 );
        Item( "Credenciais ABP", zero ? L_FAIL : L_OK, "%s", zero ? "DevAddr, NwkSKey ou AppSKey zerados" : "DevAddr e chaves preenchidos" );
    }

    /* 3. Sessão LoRaWAN */
    if( AppStatus.joined )
    {
        Item( "Sessão LoRaWAN", L_OK, "ativa · DevAddr %08lX%s", ( unsigned long )AppStatus.devAddr,
              AppStatus.sessionRestored ? " (restaurada da EEPROM)" : "" );
    }
    else if( AppStatus.joining )
    {
        Item( "Sessão LoRaWAN", L_WARN, "JOIN em andamento (%lu tentativa(s))", ( unsigned long )AppStatus.joinAttempts );
    }
    else
    {
        Item( "Sessão LoRaWAN", L_FAIL, "sem sessão" );
    }
    Item( "MAC LoRaWAN", L_INFO, "%s · classe %c em uso (configurada %c)", AppMacBusy( ) ? "ocupada (TX/RX agora)" : "livre",
          AppStatus.activeClass, NodeCfg.devClass );

    /* 4. Tráfego */
    uint32_t tot = AppStatus.uplinks + AppStatus.uplinkErrors;
    if( tot == 0 )
    {
        Item( "Uplinks", L_INFO, "nenhum desde que ligou" );
    }
    else
    {
        uint32_t pct = ( AppStatus.uplinkErrors * 100u ) / tot;
        Item( "Uplinks", ( pct > 10 ) ? L_WARN : L_OK, "%lu enviados · %lu erros (%lu %%) · FCntUp %lu",
              ( unsigned long )AppStatus.uplinks, ( unsigned long )AppStatus.uplinkErrors, ( unsigned long )pct,
              ( unsigned long )AppStatus.fcntUp );
    }
    if( NodeCfg.confirmed && ( AppStatus.uplinks >= 4 ) )
    {
        bool few = ( AppStatus.acks * 2u ) < AppStatus.uplinks;
        Item( "ACKs (confirmados)", few ? L_WARN : L_OK, "%lu ACKs para %lu uplinks%s", ( unsigned long )AppStatus.acks,
              ( unsigned long )AppStatus.uplinks, few ? ": gateway longe ou sub-banda errada?" : "" );
    }
    if( AppStatus.downlinks )
    {
        Item( "Último downlink", AppStatus.lastRssi < -115 ? L_WARN : L_OK, "%s · RSSI %d dBm · SNR %d dB",
              AppStatus.lastSlot, AppStatus.lastRssi, AppStatus.lastSnr );
    }
    else
    {
        Item( "Downlinks", L_INFO, "nenhum desde que ligou" );
    }

    /* 5. Relógio e hora */
    const RtcClockInfo_t *ci = RtcGetClockInfo( );
    bool lse = ( ci->Source == RTC_CLOCK_LSE );
    bool wantB = ( NodeCfg.devClass == 'B' );
    Item( "Relógio do RTC", lse ? L_OK : ( wantB ? L_WARN : L_OK ), "%s · %lu Hz · ~%lu ppm%s", AppClockSourceName( ),
          ( unsigned long )( ci->RtcClkHz + 0.5f ), ( unsigned long )ci->PpmEstimate,
          ( !lse && wantB ) ? " · Classe B instável" : "" );
    Item( "Hora da rede", AppStatus.timeSynced ? L_OK : ( wantB ? L_WARN : L_INFO ), "%s",
          AppStatus.timeSynced ? "sincronizada (DeviceTimeAns ou beacon)" : "não sincronizada" );

    /* 6. Alimentação */
    uint32_t vdd = BoardGetBatteryVoltage( );
    Item( "Alimentação (VDD)", ( vdd < 2200 ) ? L_FAIL : ( vdd < 2700 ) ? L_WARN : L_OK, "%lu mV · %s", ( unsigned long )vdd,
          NodeCfg.extPower ? "externa" : "bateria" );

    /* 7. Memória */
    uint32_t peak, total;
    AppStackInfo( &peak, &total );
    Item( "Pilha (RAM)", ( total > peak && ( total - peak ) < 512 ) ? L_WARN : L_OK, "pico %lu de %lu bytes livres · heap %lu bytes",
          ( unsigned long )peak, ( unsigned long )total, ( unsigned long )AppHeapUsed( ) );
    Item( "Log em RAM", L_INFO, "%lu de %u bytes", ( unsigned long )NodeLogUsed( ), ( unsigned )NODE_LOG_SIZE );

    /* 8. Reinício e tempo ligado */
    const char *rk = AppResetCauseKey( );
    Item( "Último reinício", ( !strcmp( rk, "wdg" ) || !strcmp( rk, "lpwr" ) ) ? L_WARN : L_INFO, "%s", AppResetCauseText( ) );
    uint32_t up = AppUptimeMs( ) / 1000;
    Item( "Ligado há", L_INFO, "%lu d %02lu:%02lu:%02lu", ( unsigned long )( up / 86400 ), ( unsigned long )( ( up / 3600 ) % 24 ),
          ( unsigned long )( ( up / 60 ) % 60 ), ( unsigned long )( up % 60 ) );
}

void NodeDiagRun( bool json )
{
    Json = json;
    First = true;
    Nok = Nwarn = Nfail = 0;
    if( json )
    {
        printf( "@D {\"items\":[" );
        RunTests( );
        printf( "],\"ok\":%u,\"warn\":%u,\"fail\":%u}\r\n", Nok, Nwarn, Nfail );
        return;
    }
    printf( "Autoteste do nó (%s v%s):\r\n", NODE_FW_NAME, NODE_FW_VERSION );
    RunTests( );
    printf( "Resultado: %u ok · %u aviso(s) · %u falha(s).\r\n", Nok, Nwarn, Nfail );
    printf( "Ping, traceroute e nslookup: na página WEB (Diagnósticos) ou no app, pelo computador/celular.\r\n" );
}
