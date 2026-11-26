/*
 * at_cmd.c — comandos AT do Módulo LoRaWAN EndDevice Radioenge (veja at_cmd.h).
 *
 * Fontes: manual "Mote LoRa Radioenge — Manual de Utilização" (rev. mar/2019 e
 * abr/2021) e a tabela de comandos do firmware Lora_AT_v1r3s35 (o .rad é
 * cifrado; o comportamento vem dos manuais e da tabela).
 *
 * Diferenças conscientes em relação ao original (documentadas no README):
 *   - AT+DR aceita 2..6 (AU915 com dwell time de 400 ms: DR0/DR1 recusados);
 *   - portas de aplicação 1..223 (0 e 224..255 são da MAC / certificação);
 *   - AT+NBTRIALS vale até 15 tentativas por uplink confirmado (limite da
 *     LoRaMac-node 1.0.4); no AT+JOIN vale o número todo (0..79);
 *   - AT+CLASS aceita também B; AT+CHMASK=? e AT+GPIOC=? existem;
 *   - credenciais, canais e AT+NJM reiniciam o módulo 5 s depois do último
 *     comando (ou na hora, com AT+JOIN/ATZ), para criar a sessão nova.
 *
 * Comandos novos deste firmware (não existem no original): AT+TELEM, AT+RELECFG,
 * AT+RELE, AT+RELEPULSO, AT+PZEMCFG, AT+PZEM, AT+PZEMZERA, AT+PZEMLIM, AT+PZEMEND
 * (relés, PZEM-004T v3 e protocolo ADAILSILVA_V1; veja doc/COMANDOS_AT_RADIOENGE.md).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include "board.h"
#include "board-ext.h"
#include "LoRaMac.h"
#include "LmHandler.h"
#include "node_build.h"
#include "node_config.h"
#include "node_io.h"
#include "app.h"
#include "at_cmd.h"
#include "node_relay.h"
#include "node_pzem.h"

typedef enum { OP_GET, OP_SET, OP_RUN, OP_HELP } AtOp_t;

typedef enum
{
    R_NONE = 0,         /* a resposta (valor) já foi escrita */
    R_OK,
    R_ERROR,
    R_PARAM,
    R_BUSY,
    R_TX_OK,
    R_TX_LENGTH,
    R_NOT_JOINED,
    R_ALREADY_JOINED,
}AtRes_t;

static const char *const ResText[] =
{
    "", "AT OK", "AT ERROR", "AT PARAM ERROR", "AT BUSY ERROR", "AT TX OK",
    "AT TX LENGTH ERROR", "AT NO NETWORK JOINED", "AT ALREADY JOINED",
};

typedef AtRes_t ( *AtHandler_t )( AtOp_t op, char *arg );

typedef struct
{
    const char *name;
    AtHandler_t fn;
    const char *help;
}AtCmd_t;

/* ------------------------------------------------------------- saída --- */
static char *CapBuf = NULL;            /* != NULL: capturando (AT por downlink) */
static uint16_t CapMax = 0, CapLen = 0;
static bool SavedCfg = false;

static void Out( const char *fmt, ... )
{
    char tmp[96];
    va_list ap;
    va_start( ap, fmt );
    int n = vsnprintf( tmp, sizeof( tmp ), fmt, ap );
    va_end( ap );
    if( n < 0 )
    {
        return;
    }
    if( n >= ( int )sizeof( tmp ) )
    {
        n = sizeof( tmp ) - 1;
    }
    if( CapBuf != NULL )
    {
        for( int i = 0; ( i < n ) && ( CapLen < CapMax ); i++ )
        {
            CapBuf[CapLen++] = tmp[i];
        }
        return;
    }
    fputs( tmp, stdout );
}

static void OutLine( const char *s )
{
    if( CapBuf != NULL )
    {
        if( CapLen && ( CapLen < CapMax ) )
        {
            CapBuf[CapLen++] = '\n';
        }
        Out( "%s", s );
        return;
    }
    printf( "%s\r\n", s );
}

static void OutVal( const char *fmt, ... )
{
    char tmp[96];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( tmp, sizeof( tmp ), fmt, ap );
    va_end( ap );
    OutLine( tmp );
}

static void OutHexColon( const uint8_t *b, uint8_t n )
{
    char s[3 * 16 + 1];
    uint8_t p = 0;
    for( uint8_t i = 0; i < n; i++ )
    {
        p += ( uint8_t )snprintf( s + p, sizeof( s ) - p, "%s%02x", i ? ":" : "", b[i] );
    }
    OutLine( s );
}

void AtPrintAsync( const char *msg )
{
    printf( "%s\r\n", msg );
}

void AtPrintRx( const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr )
{
    printf( "RX:" );
    for( uint8_t i = 0; i < len; i++ )
    {
        printf( "%02x", data[i] );
    }
    printf( ":%d:%d\r\n", rssi, snr );
}

/* --------------------------------------------------------- utilitários --- */
static bool ParseU32( const char *s, uint32_t *v, uint32_t min, uint32_t max )
{
    char *end;
    while( *s == ' ' )
    {
        s++;
    }
    if( !isdigit( ( unsigned char )*s ) )
    {
        return false;
    }
    unsigned long x = strtoul( s, &end, 10 );
    while( *end == ' ' )
    {
        end++;
    }
    if( ( *end != '\0' ) || ( x < min ) || ( x > max ) )
    {
        return false;
    }
    *v = ( uint32_t )x;
    return true;
}

/* Separa até n campos por ':' (o último fica com o resto da linha) */
static int Split( char *arg, char **f, int n )
{
    int c = 0;
    if( arg == NULL )
    {
        return 0;
    }
    f[c++] = arg;
    while( ( c < n ) && ( arg = strchr( arg, ':' ) ) != NULL )
    {
        *arg++ = '\0';
        f[c++] = arg;
    }
    return c;
}

static bool ParseHexExact( const char *s, uint8_t *out, size_t n )
{
    uint8_t tmp[16];
    if( HexToBytes( s, tmp, sizeof( tmp ) ) != ( int )n )
    {
        return false;
    }
    memcpy( out, tmp, n );
    return true;
}

/* Grava a configuração alterada (com validação). Credenciais/canais/NJM:
 * reinício adiado; o resto vale na hora. */
static AtRes_t Commit( const NodeConfig_t *old, bool macParams )
{
    char err[64];
    if( !NodeConfigValidate( &NodeCfg, err, sizeof( err ) ) )
    {
        NodeCfg = *old;
        return R_PARAM;
    }
    if( memcmp( old, &NodeCfg, sizeof( NodeCfg ) ) == 0 )
    {
        return R_OK;
    }
    if( !NodeConfigSave( ) )
    {
        NodeCfg = *old;
        return R_ERROR;
    }
    SavedCfg = true;
    if( NodeConfigCredFingerprint( &NodeCfg ) != NodeConfigCredFingerprint( old ) )
    {
        AppScheduleRestart( );
        return R_OK;
    }
    if( macParams )
    {
        AppApplyMacParams( true );
    }
    AppApplyRuntimeConfig( );
    return R_OK;
}

#define FIELD( f )      ( ( uint8_t* )&NodeCfg + offsetof( NodeConfig_t, f ) )
#define BEGIN_SET()     NodeConfig_t old = NodeCfg
#define COMMIT()        return Commit( &old, false )
#define COMMIT_MAC()    return Commit( &old, true )

/* Comando de 0/1 gravado num campo uint8_t */
static AtRes_t Flag( AtOp_t op, char *arg, uint8_t *field, bool mac )
{
    if( op == OP_GET )
    {
        OutVal( "%u", *field );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint32_t v;
    if( !ParseU32( arg, &v, 0, 1 ) )
    {
        return R_PARAM;
    }
    BEGIN_SET( );
    *field = ( uint8_t )v;
    return Commit( &old, mac );
}

/* Número gravado num campo (8, 16 ou 32 bits) */
static AtRes_t Number( AtOp_t op, char *arg, void *field, uint8_t size, uint32_t min, uint32_t max, bool mac )
{
    /* NodeConfig_t é "packed": campos de 16/32 bits podem estar desalinhados
     * (o Cortex-M0+ não aceita acesso desalinhado), por isso memcpy */
    uint32_t cur = 0;
    if( size == 1 )      { cur = *( uint8_t* )field; }
    else if( size == 2 ) { uint16_t t; memcpy( &t, field, 2 ); cur = t; }
    else                 { memcpy( &cur, field, 4 ); }
    if( op == OP_GET )
    {
        OutVal( "%lu", ( unsigned long )cur );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint32_t v;
    if( !ParseU32( arg, &v, min, max ) )
    {
        return R_PARAM;
    }
    if( mac && AppMacBusy( ) )
    {
        return R_BUSY;
    }
    BEGIN_SET( );
    if( size == 1 )      { *( uint8_t* )field = ( uint8_t )v; }
    else if( size == 2 ) { uint16_t t = ( uint16_t )v; memcpy( field, &t, 2 ); }
    else                 { memcpy( field, &v, 4 ); }
    return Commit( &old, mac );
}

static AtRes_t Key( AtOp_t op, char *arg, uint8_t *field, uint8_t n )
{
    if( op == OP_GET )
    {
        OutHexColon( field, n );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    BEGIN_SET( );
    if( !ParseHexExact( arg, field, n ) )
    {
        return R_PARAM;
    }
    COMMIT( );
}

/* ------------------------------------------------------------ comandos --- */
static AtRes_t C_Deui( AtOp_t op, char *arg )
{
    if( ( op == OP_SET ) && arg && !strcmp( arg, "auto" ) )
    {
        BEGIN_SET( );
        NodeCfg.devEuiAuto = 1;
        NodeDefaultDevEui( NodeCfg.devEui );
        COMMIT( );
    }
    AtRes_t r = Key( op, arg, NodeCfg.devEui, 8 );
    if( ( op == OP_SET ) && ( r == R_OK ) )
    {
        NodeCfg.devEuiAuto = 0;
        NodeConfigSave( );
    }
    return r;
}

static AtRes_t C_Daddr( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        uint32_t a = NodeIsOtaa( ) ? ( AppStatus.joined ? AppStatus.devAddr : 0 ) : NodeCfg.devAddr;
        uint8_t b[4] = { ( uint8_t )( a >> 24 ), ( uint8_t )( a >> 16 ), ( uint8_t )( a >> 8 ), ( uint8_t )a };
        OutHexColon( b, 4 );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint8_t b[4];
    if( !ParseHexExact( arg, b, 4 ) )
    {
        return R_PARAM;
    }
    BEGIN_SET( );
    NodeCfg.devAddr = ( ( uint32_t )b[0] << 24 ) | ( ( uint32_t )b[1] << 16 ) | ( ( uint32_t )b[2] << 8 ) | b[3];
    COMMIT( );
}

static AtRes_t C_AppKey( AtOp_t op, char *arg )
{
    if( op != OP_SET )
    {
        return Key( op, arg, NodeCfg.appKey, 16 );
    }
    BEGIN_SET( );
    if( !ParseHexExact( arg, NodeCfg.appKey, 16 ) )
    {
        return R_PARAM;
    }
    if( memcmp( old.appKey, old.nwkKey, 16 ) == 0 )
    {
        memcpy( NodeCfg.nwkKey, NodeCfg.appKey, 16 );   /* LoRaWAN 1.0.x: NwkKey = AppKey */
    }
    COMMIT( );
}

/* NwkSKey / AppSKey: no OTAA com sessão, mostra as chaves derivadas no JOIN */
static AtRes_t SessionKey( AtOp_t op, char *arg, uint8_t *field, bool nwk )
{
    if( ( op == OP_GET ) && NodeIsOtaa( ) )
    {
        uint32_t da;
        uint8_t n[16], a[16];
        if( AppGetSessionKeys( &da, n, a ) )
        {
            OutHexColon( nwk ? n : a, 16 );
            return R_NONE;
        }
    }
    return Key( op, arg, field, 16 );
}

static AtRes_t C_AppSKey( AtOp_t op, char *arg ) { return SessionKey( op, arg, NodeCfg.appSKey, false ); }
static AtRes_t C_NwkSKey( AtOp_t op, char *arg ) { return SessionKey( op, arg, NodeCfg.nwkSKey, true ); }
static AtRes_t C_AppEui( AtOp_t op, char *arg )  { return Key( op, arg, NodeCfg.joinEui, 8 ); }
static AtRes_t C_Adr( AtOp_t op, char *arg )     { return Flag( op, arg, &NodeCfg.adr, false ); }
static AtRes_t C_Cfm( AtOp_t op, char *arg )     { return Flag( op, arg, &NodeCfg.confirmed, false ); }
static AtRes_t C_Dcs( AtOp_t op, char *arg )     { return Flag( op, arg, &NodeCfg.dutyCycle, true ); }
static AtRes_t C_Pnm( AtOp_t op, char *arg )     { return Flag( op, arg, &NodeCfg.publicNet, true ); }
static AtRes_t C_AJoin( AtOp_t op, char *arg )   { return Flag( op, arg, &NodeCfg.autoJoin, false ); }

static AtRes_t C_Txp( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        /* índice em uso: o gravado por AT+TXP ou o equivalente à potência em dBm */
        uint8_t idx = NodeCfg.txPowIdx;
        if( idx == NODE_TXP_FROM_DBM )
        {
            int i = ( int )( ( 27.85f - ( float )NodeCfg.txPowDbm ) / 2.0f + 0.5f );
            idx = ( uint8_t )( ( i < 0 ) ? 0 : ( i > 14 ) ? 14 : i );
        }
        OutVal( "%u", idx );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint32_t v;
    if( !ParseU32( arg, &v, 0, 15 ) )
    {
        return R_PARAM;
    }
    BEGIN_SET( );
    NodeCfg.txPowIdx = ( uint8_t )v;
    COMMIT( );
}

static AtRes_t C_Dr( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        OutVal( "%u", NodeCfg.adr ? ( unsigned )LmHandlerGetCurrentDatarate( ) : NodeCfg.dr );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint32_t v;
    if( !ParseU32( arg, &v, 0, 13 ) || ( v < 2 ) || ( v > 6 ) )
    {
        return R_PARAM;   /* uplink AU915: DR2..DR6 (dwell time de 400 ms) */
    }
    BEGIN_SET( );
    NodeCfg.dr = ( uint8_t )v;
    COMMIT( );
}

static AtRes_t C_Rx2Fq( AtOp_t op, char *arg )  { return Number( op, arg, FIELD( rx2Freq ), 4, 923300000UL, 927500000UL, true ); }
static AtRes_t C_Rx2Dr( AtOp_t op, char *arg )  { return Number( op, arg, FIELD( rx2Dr ), 1, 8, 13, true ); }
static AtRes_t C_Jn1Dl( AtOp_t op, char *arg )
{
    uint32_t v;
    if( ( op == OP_SET ) && ParseU32( arg, &v, 1000, 15000 ) && ( v >= NodeCfg.join2DelayMs ) )
    {
        NodeCfg.join2DelayMs = ( uint16_t )( v + 1000 );   /* JN2 = JN1 + 1 s */
    }
    return Number( op, arg, FIELD( join1DelayMs ), 2, 1000, 15000, true );
}
static AtRes_t C_Jn2Dl( AtOp_t op, char *arg )  { return Number( op, arg, FIELD( join2DelayMs ), 2, 1000, 16000, true ); }
static AtRes_t C_NbTrials( AtOp_t op, char *arg ) { return Number( op, arg, FIELD( nbTrials ), 1, 0, 79, false ); }

/* RX1DL/RX2DL: leitura do valor em uso na MAC (no OTAA vem do Join-accept) */
static AtRes_t RxDelay( AtOp_t op, char *arg, void *field, Mib_t mib )
{
    if( op == OP_GET )
    {
        MibRequestConfirm_t m;
        m.Type = mib;
        uint16_t t;
        memcpy( &t, field, 2 );
        uint32_t v = t;
        if( LoRaMacMibGetRequestConfirm( &m ) == LORAMAC_STATUS_OK )
        {
            v = ( mib == MIB_RECEIVE_DELAY_1 ) ? m.Param.ReceiveDelay1 : m.Param.ReceiveDelay2;
        }
        OutVal( "%lu", ( unsigned long )v );
        return R_NONE;
    }
    return Number( op, arg, field, 2, 1000, 16000, true );
}

static AtRes_t C_Rx1Dl( AtOp_t op, char *arg )
{
    /* RX2 = RX1 + 1 s (LoRaWAN): subir a RX1 além da RX2 (ex. Everynet 5000/6000)
     * empurra a RX2 junto, para a sequência AT+RX1DL / AT+RX2DL funcionar */
    uint32_t v;
    if( ( op == OP_SET ) && ParseU32( arg, &v, 1000, 15000 ) && ( v >= NodeCfg.rx2DelayMs ) )
    {
        NodeCfg.rx2DelayMs = ( uint16_t )( v + 1000 );
    }
    return RxDelay( op, arg, FIELD( rx1DelayMs ), MIB_RECEIVE_DELAY_1 );
}
static AtRes_t C_Rx2Dl( AtOp_t op, char *arg ) { return RxDelay( op, arg, FIELD( rx2DelayMs ), MIB_RECEIVE_DELAY_2 ); }

static AtRes_t C_Njm( AtOp_t op, char *arg )   { return Flag( op, arg, &NodeCfg.activation, false ); }

static AtRes_t C_NwkId( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        uint32_t n = NodeCfg.netId;
        uint8_t b[4] = { ( uint8_t )( n >> 24 ), ( uint8_t )( n >> 16 ), ( uint8_t )( n >> 8 ), ( uint8_t )n };
        OutHexColon( b, 4 );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint8_t b[4];
    if( !ParseHexExact( arg, b, 4 ) )
    {
        return R_PARAM;
    }
    BEGIN_SET( );
    NodeCfg.netId = ( ( uint32_t )b[0] << 24 ) | ( ( uint32_t )b[1] << 16 ) | ( ( uint32_t )b[2] << 8 ) | b[3];
    COMMIT( );
}

static AtRes_t C_Class( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        OutVal( "%c", NodeCfg.devClass );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    char c = ( char )toupper( ( unsigned char )arg[0] );
    if( ( arg[1] != '\0' ) || ( ( c != 'A' ) && ( c != 'B' ) && ( c != 'C' ) ) )
    {
        return R_PARAM;
    }
    BEGIN_SET( );
    NodeCfg.devClass = c;
    COMMIT( );                       /* AppApplyRuntimeConfig( ) pede a classe */
}

static AtRes_t C_Join( AtOp_t op, char *arg )
{
    ( void )arg;
    if( op != OP_RUN )
    {
        return R_ERROR;
    }
    switch( AppAtJoin( ) )
    {
    case APP_JOIN_STARTED:   return R_NONE;          /* AT JOIN OK / AT JOIN ERROR depois */
    case APP_JOIN_RESTART:   return R_NONE;          /* reinicia, faz o JOIN e responde */
    case APP_JOIN_ALREADY:   return R_ALREADY_JOINED;
    default:                 return R_ERROR;
    }
}

static AtRes_t C_Njs( AtOp_t op, char *arg )
{
    ( void )arg;
    if( op != OP_GET )
    {
        return R_ERROR;
    }
    OutVal( "%d", AppStatus.joined ? 1 : 0 );
    return R_NONE;
}

static AtRes_t TxResult( AppTxResult_t r )
{
    switch( r )
    {
    case APP_TX_OK:         return R_TX_OK;
    case APP_TX_NOT_JOINED: return R_NOT_JOINED;
    case APP_TX_BUSY:       return R_BUSY;
    case APP_TX_LENGTH:     return R_TX_LENGTH;
    default:                return R_ERROR;
    }
}

/* SEND/SENDB/TXCFM/TXBCFM: porta[:confirmado:tentativas]:payload */
static AtRes_t SendCommon( AtOp_t op, char *arg, bool hex, bool explicitCfm )
{
    if( ( op != OP_SET ) || ( arg == NULL ) )
    {
        return R_PARAM;
    }
    char *f[4];
    int nf = Split( arg, f, explicitCfm ? 4 : 2 );
    if( nf != ( explicitCfm ? 4 : 2 ) )
    {
        return R_PARAM;
    }
    uint32_t port, cfm = NodeCfg.confirmed, trials = NodeCfg.nbTrials;
    if( !ParseU32( f[0], &port, 1, 223 ) )
    {
        return R_PARAM;
    }
    if( explicitCfm && ( !ParseU32( f[1], &cfm, 0, 1 ) || !ParseU32( f[2], &trials, 0, 79 ) ) )
    {
        return R_PARAM;
    }
    char *payload = f[nf - 1];
    static uint8_t data[242];
    int n;
    if( hex )
    {
        n = HexToBytes( payload, data, sizeof( data ) );
        if( n < 0 )
        {
            return R_PARAM;
        }
    }
    else
    {
        n = ( int )strlen( payload );
        if( n > ( int )sizeof( data ) )
        {
            return R_TX_LENGTH;
        }
        memcpy( data, payload, n );
    }
    return TxResult( AppSendFrame( ( uint8_t )port, data, ( uint8_t )n, cfm != 0, ( uint8_t )trials, true ) );
}

static AtRes_t C_Send( AtOp_t op, char *arg )   { return SendCommon( op, arg, false, false ); }
static AtRes_t C_SendB( AtOp_t op, char *arg )  { return SendCommon( op, arg, true, false ); }
static AtRes_t C_TxCfm( AtOp_t op, char *arg )  { return SendCommon( op, arg, false, true ); }
static AtRes_t C_TxBCfm( AtOp_t op, char *arg ) { return SendCommon( op, arg, true, true ); }

static AtRes_t C_Ver( AtOp_t op, char *arg )
{
    ( void )arg;
    if( op != OP_GET )
    {
        return R_ERROR;
    }
    OutLine( NODE_FW_VERSION );
    return R_NONE;
}

static AtRes_t C_Snr( AtOp_t op, char *arg )
{
    ( void )arg;
    if( op != OP_GET )
    {
        return R_ERROR;
    }
    OutVal( "%d", AppStatus.lastSnr );
    return R_NONE;
}

static AtRes_t C_Rssi( AtOp_t op, char *arg )
{
    ( void )arg;
    if( op != OP_GET )
    {
        return R_ERROR;
    }
    OutVal( "%d", AppStatus.lastRssi );
    return R_NONE;
}

static AtRes_t C_Bat( AtOp_t op, char *arg )
{
    ( void )arg;
    if( op != OP_GET )
    {
        return R_ERROR;
    }
    OutVal( "%u", NodeIoBatteryLevel( ) );
    return R_NONE;
}

static AtRes_t C_Baud( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        OutVal( "%lu", ( unsigned long )NodeBaudrates[NodeCfg.baudIdx & 3] );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint32_t v;
    if( !ParseU32( arg, &v, 0, 1000000UL ) )
    {
        return R_PARAM;
    }
    /* 0..3 = índice (firmware antigo); senão o valor em bps. Outro valor: 9600 (manual) */
    uint8_t idx = 0;
    if( v <= 3 )
    {
        idx = ( uint8_t )v;
    }
    else
    {
        for( uint8_t i = 0; i < 4; i++ )
        {
            if( NodeBaudrates[i] == v )
            {
                idx = i;
            }
        }
    }
    BEGIN_SET( );
    NodeCfg.baudIdx = idx;
    AtRes_t r = Commit( &old, false );
    if( ( r == R_OK ) && ( old.baudIdx != idx ) )
    {
        OutLine( ResText[R_OK] );               /* responde na velocidade antiga... */
        UartMcuFlush( );
        AppSetConsoleBaudrate( NodeBaudrates[idx] );   /* ...e troca */
        return R_NONE;
    }
    return r;
}

static AtRes_t C_KeepAlive( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        OutVal( "%u:%u:%u:%lu", NodeCfg.kaEnable, NodeCfg.kaPort, NodeCfg.kaConfirmed, ( unsigned long )NodeCfg.kaPeriodMs );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    char *f[4];
    uint32_t en, port, cfm, per = NodeCfg.kaPeriodMs;
    int nf = Split( arg, f, 4 );
    if( ( nf < 1 ) || !ParseU32( f[0], &en, 0, 1 ) )
    {
        return R_PARAM;
    }
    port = NodeCfg.kaPort;
    cfm = NodeCfg.kaConfirmed;
    if( ( nf >= 2 ) && !ParseU32( f[1], &port, 1, 223 ) ) return R_PARAM;
    if( ( nf >= 3 ) && !ParseU32( f[2], &cfm, 0, 1 ) )    return R_PARAM;
    if( ( nf >= 4 ) && !ParseU32( f[3], &per, 5000, 86400000UL ) ) return R_PARAM;
    BEGIN_SET( );
    NodeCfg.kaEnable = ( uint8_t )en;
    NodeCfg.kaPort = ( uint8_t )port;
    NodeCfg.kaConfirmed = ( uint8_t )cfm;
    NodeCfg.kaPeriodMs = per;
    COMMIT( );
}

static AtRes_t C_ChMask( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        uint16_t m[6];
        NodeConfigChannelMask( &NodeCfg, m );
        OutVal( "%04x%04x%04x%04x%04x%04x", m[0], m[1], m[2], m[3], m[4], m[5] );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint8_t b[12];
    if( !ParseHexExact( arg, b, 12 ) )
    {
        return R_PARAM;   /* aceita 00ff:0000:0000:0000:0001:0000 ou 24 dígitos seguidos */
    }
    uint16_t m[6];
    for( int i = 0; i < 6; i++ )
    {
        m[i] = ( uint16_t )( ( b[2 * i] << 8 ) | b[2 * i + 1] );
    }
    BEGIN_SET( );
    if( !NodeConfigSetChannelMask( &NodeCfg, m ) )
    {
        return R_PARAM;
    }
    COMMIT( );
}

static AtRes_t IoResult( NodeIoStatus_t s )
{
    return ( s == NODE_IO_OK ) ? R_OK : ( s == NODE_IO_PARAM_ERROR ) ? R_PARAM : R_ERROR;
}

static AtRes_t C_Gpioc( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        char s[64];
        NodeIoDescribe( s, sizeof( s ) );
        OutLine( s );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    char *f[3];
    uint32_t pin, mode, pull = 0;
    int nf = Split( arg, f, 3 );
    if( ( nf < 2 ) || !ParseU32( f[0], &pin, 0, 9 ) || !ParseU32( f[1], &mode, 0, 8 ) ||
        ( ( nf == 3 ) && !ParseU32( f[2], &pull, 0, 2 ) ) )
    {
        return R_PARAM;
    }
    BEGIN_SET( );
    NodeIoStatus_t s = NodeIoConfigure( ( uint8_t )pin, ( uint8_t )mode, ( uint8_t )pull );
    if( s != NODE_IO_OK )
    {
        return IoResult( s );
    }
    COMMIT( );
}

static AtRes_t C_Wpin( AtOp_t op, char *arg )
{
    if( op != OP_SET )
    {
        return R_PARAM;
    }
    char *f[2];
    uint32_t pin, lvl;
    if( ( Split( arg, f, 2 ) != 2 ) || !ParseU32( f[0], &pin, 0, 9 ) || !ParseU32( f[1], &lvl, 0, 1 ) )
    {
        return R_PARAM;
    }
    return IoResult( NodeIoWrite( ( uint8_t )pin, ( uint8_t )lvl ) );
}

static AtRes_t ReadPin( AtOp_t op, char *arg, bool adcOnly )
{
    if( op != OP_SET )
    {
        return R_PARAM;
    }
    uint32_t pin;
    uint16_t v;
    if( !ParseU32( arg, &pin, 0, 9 ) )
    {
        return R_PARAM;
    }
    NodeIoStatus_t s = adcOnly ? NodeIoReadAdc( ( uint8_t )pin, &v ) : NodeIoRead( ( uint8_t )pin, &v );
    if( s != NODE_IO_OK )
    {
        return IoResult( s );
    }
    OutVal( "%u", v );
    return R_NONE;
}

static AtRes_t C_Rpin( AtOp_t op, char *arg ) { return ReadPin( op, arg, false ); }
static AtRes_t C_Adc( AtOp_t op, char *arg )  { return ReadPin( op, arg, true ); }

/* ------------------------------------------- relés, PZEM e telemetria V1 --- */
static AtRes_t C_Telem( AtOp_t op, char *arg ) { return Flag( op, arg, &NodeCfg.uplinkMode, false ); }

/* Lista "a,b,c" de números (dec) ou bytes (hex) */
static int ParseList( char *s, uint8_t *out, int max, bool hex, uint32_t lo, uint32_t hi )
{
    int n = 0;
    char *tok = s;
    while( tok != NULL )
    {
        char *next = strchr( tok, ',' );
        if( next != NULL )
        {
            *next++ = '\0';
        }
        uint32_t v;
        if( n >= max )
        {
            return -1;
        }
        if( hex )
        {
            /* 1 ou 2 dígitos hexadecimais ("f8", "1", "0x01") */
            char *end;
            while( *tok == ' ' ) tok++;
            if( !isxdigit( ( unsigned char )*tok ) )
            {
                return -1;
            }
            unsigned long x = strtoul( tok, &end, 16 );
            while( *end == ' ' ) end++;
            if( ( *end != '\0' ) || ( x < lo ) || ( x > hi ) )
            {
                return -1;
            }
            v = ( uint32_t )x;
        }
        else if( !ParseU32( tok, &v, lo, hi ) )
        {
            return -1;
        }
        out[n++] = ( uint8_t )v;
        tok = next;
    }
    return n;
}

static AtRes_t C_ReleCfg( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        char s[40];
        NodeRelayDescribe( s, sizeof( s ) );
        OutLine( s );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint8_t pins[NODE_RELAY_MAX], al[NODE_RELAY_MAX];
    int np = 0, na = 0;
    char *f[2];
    int nf = Split( arg, f, 2 );
    if( !( ( nf == 1 ) && !strcmp( f[0], "0" ) ) )
    {
        np = ParseList( f[0], pins, NODE_RELAY_MAX, false, 0, 9 );
        na = ( nf == 2 ) ? ParseList( f[1], al, NODE_RELAY_MAX, false, 0, 1 ) : 0;
        if( ( np <= 0 ) || ( na < 0 ) || ( ( na > 1 ) && ( na != np ) ) )
        {
            return R_PARAM;
        }
    }
    uint8_t mask = 0;
    for( int i = 0; i < np; i++ )
    {
        if( ( na == 1 ) ? al[0] : ( na > 1 ) ? al[i] : 0 )
        {
            mask |= ( uint8_t )( 1U << i );
        }
    }
    char err[64];
    BEGIN_SET( );
    if( !NodeRelayConfigure( ( uint8_t )np, pins, mask, err, sizeof( err ) ) )
    {
        OutVal( "AT+RELECFG: %s", err );
        return R_PARAM;
    }
    COMMIT( );
}

static AtRes_t RelayResult( NodeRelayStatus_t st )
{
    if( st == NODE_RELAY_OK )
    {
        NodeRelayRequestReport( );          /* telemetria com o estado novo */
        return R_OK;
    }
    return ( st == NODE_RELAY_ABSENT ) ? R_ERROR : R_PARAM;
}

static AtRes_t C_Rele( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        OutVal( "%u:%04x", NodeRelayCount( ), NodeRelayMap( ) );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    char *f[2];
    uint32_t ch, act;
    if( ( Split( arg, f, 2 ) != 2 ) || !ParseU32( f[0], &ch, 0, 255 ) || !ParseU32( f[1], &act, 0, 2 ) )
    {
        return R_PARAM;
    }
    return RelayResult( NodeRelaySet( ( uint8_t )ch, ( uint8_t )act ) );
}

static AtRes_t C_RelePulso( AtOp_t op, char *arg )
{
    if( op != OP_SET )
    {
        return R_PARAM;
    }
    char *f[3];
    uint32_t ch, st, ms;
    if( ( Split( arg, f, 3 ) != 3 ) || !ParseU32( f[0], &ch, 0, 255 ) || !ParseU32( f[1], &st, 0, 1 ) ||
        !ParseU32( f[2], &ms, NODE_RELAY_PULSE_MIN_MS, NODE_RELAY_PULSE_MAX_MS ) )
    {
        return R_PARAM;
    }
    return RelayResult( NodeRelayPulse( ( uint8_t )ch, ( uint8_t )st, ms ) );
}

static AtRes_t C_PzemCfg( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        char t[16] = "0";
        uint8_t n = 0;
        for( uint8_t i = 0; i < NodeCfg.pzemCount; i++ )
        {
            n += ( uint8_t )snprintf( t + n, sizeof( t ) - n, "%s%02x", i ? "," : "", NodeCfg.pzemAddr[i] );
        }
        OutLine( t );
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint8_t addr[NODE_PZEM_MAX];
    int n = 0;
    if( strcmp( arg, "0" ) )
    {
        n = ParseList( arg, addr, NODE_PZEM_MAX, true, 1, NODE_PZEM_ADDR_GENERIC );
        if( n <= 0 )
        {
            return R_PARAM;
        }
    }
    char err[64];
    NodeConfig_t c = NodeCfg;
    c.pzemCount = ( uint8_t )n;
    memcpy( c.pzemAddr, addr, ( size_t )n );
    if( n )
    {
        /* PZEM assume GPIO0/GPIO1: a serial transparente (ou outro modo) sai */
        c.gpioMode[0] = c.gpioMode[1] = NODE_GPIO_UNUSED;
        c.gpioPull[0] = c.gpioPull[1] = 0;
    }
    if( !NodeConfigValidate( &c, err, sizeof( err ) ) )
    {
        OutVal( "AT+PZEMCFG: %s", err );
        return R_PARAM;
    }
    BEGIN_SET( );
    if( n )
    {
        NodeIoReleaseSerialPins( );
        NodeCfg.pzemCount = ( uint8_t )n;
        memcpy( NodeCfg.pzemAddr, addr, ( size_t )n );
        NodePzemInit( );
    }
    else
    {
        NodePzemDisable( );
    }
    COMMIT( );
}

static AtRes_t PzemLocal( NodePzemOp_t pop, uint32_t ch, uint32_t arg )
{
    if( ( ch >= NodeCfg.pzemCount ) || !NodePzemActive( ) )
    {
        return R_ERROR;
    }
    return NodePzemRequestLocal( pop, ( uint8_t )ch, ( uint16_t )arg ) ? R_OK : R_BUSY;
}

static AtRes_t C_Pzem( AtOp_t op, char *arg )
{
    if( op == OP_GET )
    {
        if( NodeCfg.pzemCount == 0 )
        {
            OutLine( "0" );
        }
        for( uint8_t ch = 0; ch < NodeCfg.pzemCount; ch++ )
        {
            char s[120];
            NodePzemFormat( ch, s, sizeof( s ) );
            OutVal( "%u:%s", ch, s );
        }
        return R_NONE;
    }
    if( op != OP_SET )
    {
        return R_ERROR;
    }
    uint32_t ch;
    if( !ParseU32( arg, &ch, 0, NODE_PZEM_MAX - 1 ) )
    {
        return R_PARAM;
    }
    return PzemLocal( NODE_PZEM_OP_READ, ch, 0 );     /* AT OK e, depois, "PZEMn: ..." */
}

static AtRes_t C_PzemZera( AtOp_t op, char *arg )
{
    uint32_t ch;
    if( ( op != OP_SET ) || !ParseU32( arg, &ch, 0, NODE_PZEM_MAX - 1 ) )
    {
        return R_PARAM;
    }
    return PzemLocal( NODE_PZEM_OP_RESET_ENERGY, ch, 0 );
}

static AtRes_t C_PzemLim( AtOp_t op, char *arg )
{
    char *f[2];
    uint32_t ch, w;
    if( ( op != OP_SET ) || ( Split( arg, f, 2 ) != 2 ) || !ParseU32( f[0], &ch, 0, NODE_PZEM_MAX - 1 ) ||
        !ParseU32( f[1], &w, 0, NODE_PZEM_POWER_MAX ) )
    {
        return R_PARAM;
    }
    return PzemLocal( NODE_PZEM_OP_POWER_LIMIT, ch, w );
}

static AtRes_t C_PzemEnd( AtOp_t op, char *arg )
{
    uint8_t a;
    if( ( op != OP_SET ) || ( ParseList( arg, &a, 1, true, 1, 0xF7 ) != 1 ) )
    {
        return R_PARAM;
    }
    if( NodeCfg.pzemCount != 1 )
    {
        return R_ERROR;                     /* troque o endereço com um medidor só ligado */
    }
    return PzemLocal( NODE_PZEM_OP_SET_ADDR, 0, a );
}

static const AtCmd_t Cmds[] =
{
    { "DEUI",      C_Deui,      "DevEUI (MSB, como no ChirpStack). =? lê; =<8 bytes>|auto grava" },
    { "DADDR",     C_Daddr,     "DevAddr: =? (OTAA sem JOIN = 00:00:00:00); =c1:ff:60:12" },
    { "APPKEY",    C_AppKey,    "AppKey (16 bytes, OTAA)" },
    { "APPSKEY",   C_AppSKey,   "AppSKey (16 bytes, ABP; no OTAA =? mostra a da sessão)" },
    { "NWKSKEY",   C_NwkSKey,   "NwkSKey (16 bytes, ABP; no OTAA =? mostra a da sessão)" },
    { "APPEUI",    C_AppEui,    "AppEUI/JoinEUI (8 bytes)" },
    { "ADR",       C_Adr,       "ADR: 0 desligado, 1 ligado" },
    { "TXP",       C_Txp,       "potência: 0 = EIRP máx., 1..14 = máx. - 2*n dB, 15 = mín." },
    { "DR",        C_Dr,        "datarate do uplink sem ADR: 2..6 (AU915)" },
    { "DCS",       C_Dcs,       "duty cycle ETSI: 0 desligado, 1 ligado" },
    { "PNM",       C_Pnm,       "rede pública: 0 privada, 1 pública" },
    { "RX2FQ",     C_Rx2Fq,     "frequência da RX2 em Hz (923300000)" },
    { "RX2DR",     C_Rx2Dr,     "DR da RX2: 8..13 (8)" },
    { "RX1DL",     C_Rx1Dl,     "atraso da RX1 em ms (1000)" },
    { "RX2DL",     C_Rx2Dl,     "atraso da RX2 em ms (2000)" },
    { "JN1DL",     C_Jn1Dl,     "atraso da janela 1 do JOIN em ms (5000)" },
    { "JN2DL",     C_Jn2Dl,     "atraso da janela 2 do JOIN em ms (6000)" },
    { "NJM",       C_Njm,       "ativação: 0 ABP, 1 OTAA (reinicia)" },
    { "NWKID",     C_NwkId,     "Network ID (4 bytes)" },
    { "CLASS",     C_Class,     "classe: A, B ou C" },
    { "JOIN",      C_Join,      "faz o JOIN (OTAA): AT JOIN OK / AT JOIN ERROR / AT ALREADY JOINED" },
    { "AJOIN",     C_AJoin,     "JOIN automático no boot: 0 ou 1" },
    { "NJS",       C_Njs,       "status do JOIN: 0 sem sessão, 1 com sessão" },
    { "SENDB",     C_SendB,     "envia hex: =porta:payload (ex. 5:00aabbcc)" },
    { "SEND",      C_Send,      "envia texto: =porta:payload (ex. 5:teste)" },
    { "VER",       C_Ver,       "versão do firmware" },
    { "CFM",       C_Cfm,       "uplink confirmado: 0 não, 1 sim" },
    { "SNR",       C_Snr,       "SNR do último pacote recebido (dB)" },
    { "RSSI",      C_Rssi,      "RSSI do último pacote recebido (dBm)" },
    { "BAT",       C_Bat,       "bateria: 1 = 1,8 V ... 254 = 3,6 V, 255 sem medida" },
    { "BAUDRATE",  C_Baud,      "serial: 9600, 19200, 43000 ou 115200 (ou índice 0..3)" },
    { "NBTRIALS",  C_NbTrials,  "tentativas do confirmado e do AT+JOIN: 0..79" },
    { "KEEPALIVE", C_KeepAlive, "keepalive: =habilitado:porta:confirmado:periodo_ms" },
    { "TXCFM",     C_TxCfm,     "envia texto: =porta:confirmado:tentativas:payload" },
    { "TXBCFM",    C_TxBCfm,    "envia hex: =porta:confirmado:tentativas:payload" },
    { "CHMASK",    C_ChMask,    "máscara de canais: =00ff:0000:0000:0000:0001:0000" },
    { "ADC",       C_Adc,       "lê a entrada analógica: =pino (0, 1, 7, 8) -> 0..4095" },
    { "GPIOC",     C_Gpioc,     "configura pino: =pino:modo:pull (modo 0..8, pull 0..2)" },
    { "WPIN",      C_Wpin,      "escreve saída: =pino:nível" },
    { "RPIN",      C_Rpin,      "lê pino: =pino (digital 0/1, analógico 0..4095)" },
    { "TELEM",     C_Telem,     "uplink periódico: 0 payload fixo, 1 telemetria ADAILSILVA_V1 (porta 20)" },
    { "RELECFG",   C_ReleCfg,   "relés: =pinos:ativo_baixo (ex. 4,5:1 ou 4,5:1,0) · =0 nenhum" },
    { "RELE",      C_Rele,      "relés: =? qtd:mapa · =canal|255:0 desliga, 1 liga, 2 alterna" },
    { "RELEPULSO", C_RelePulso, "pulso: =canal|255:estado:ms (100..86400000)" },
    { "PZEMCFG",   C_PzemCfg,   "PZEM-004T em GPIO0/1: =f8 (um) ou =01,02 (até 4) · =0 desliga" },
    { "PZEM",      C_Pzem,      "PZEM: =? últimas leituras · =canal lê agora" },
    { "PZEMZERA",  C_PzemZera,  "zera a energia do PZEM: =canal" },
    { "PZEMLIM",   C_PzemLim,   "limite de potência do PZEM: =canal:watts (0..23000)" },
    { "PZEMEND",   C_PzemEnd,   "endereço Modbus novo (um PZEM só): =01..f7" },
};
#define CMD_COUNT   ( sizeof( Cmds ) / sizeof( Cmds[0] ) )

void AtPrintHelp( void )
{
    printf( "Comandos AT (firmware Radioenge): AT+CMD=? lê · AT+CMD=x grava · AT+CMD executa · AT+CMD? ajuda\r\n" );
    printf( "  %-10s %s\r\n", "ATZ", "reinicia o módulo" );
    for( size_t i = 0; i < CMD_COUNT; i++ )
    {
        printf( "  AT+%-7s %s\r\n", Cmds[i].name, Cmds[i].help );
    }
}

/* ------------------------------------------------------------ parser --- */
bool AtIsAtLine( const char *l )
{
    while( *l == ' ' )
    {
        l++;
    }
    if( ( toupper( ( unsigned char )l[0] ) != 'A' ) || ( toupper( ( unsigned char )l[1] ) != 'T' ) )
    {
        return false;
    }
    char c = l[2];
    return ( c == '\0' ) || ( c == '+' ) || ( c == '?' ) || ( c == ' ' ) ||
           ( ( toupper( ( unsigned char )c ) == 'Z' ) && ( ( l[3] == '\0' ) || ( l[3] == ' ' ) ) );
}

static AtRes_t Dispatch( char *line )
{
    while( *line == ' ' )
    {
        line++;
    }
    /* "AT", "AT?", "ATZ" */
    char *p = line + 2;
    while( *p == ' ' )
    {
        p++;
    }
    if( *p == '\0' )
    {
        return R_OK;
    }
    if( !strcmp( p, "?" ) )
    {
        if( CapBuf == NULL )
        {
            AtPrintHelp( );
        }
        return R_NONE;
    }
    if( ( toupper( ( unsigned char )*p ) == 'Z' ) && ( p[1] == '\0' || p[1] == ' ' ) )
    {
        OutLine( ResText[R_OK] );
        AppReboot( 200 );
        return R_NONE;
    }
    if( *p++ != '+' )
    {
        return R_ERROR;
    }

    /* nome do comando */
    char name[12];
    uint8_t n = 0;
    while( isalnum( ( unsigned char )*p ) && ( n < sizeof( name ) - 1 ) )
    {
        name[n++] = ( char )toupper( ( unsigned char )*p++ );
    }
    name[n] = '\0';
    while( *p == ' ' )
    {
        p++;
    }

    AtOp_t op;
    char *arg = NULL;
    if( *p == '\0' )
    {
        op = OP_RUN;
    }
    else if( !strcmp( p, "?" ) )
    {
        op = OP_HELP;
    }
    else if( *p == '=' )
    {
        p++;
        while( *p == ' ' )
        {
            p++;
        }
        if( !strcmp( p, "?" ) )
        {
            op = OP_GET;
        }
        else
        {
            op = OP_SET;
            arg = p;
            /* tira espaços do fim */
            size_t l = strlen( arg );
            while( l && ( arg[l - 1] == ' ' ) )
            {
                arg[--l] = '\0';
            }
            if( *arg == '\0' )
            {
                return R_PARAM;
            }
        }
    }
    else
    {
        return R_ERROR;
    }

    if( !strcmp( name, "HELP" ) )
    {
        if( CapBuf == NULL )
        {
            AtPrintHelp( );
        }
        return R_NONE;
    }

    for( size_t i = 0; i < CMD_COUNT; i++ )
    {
        if( !strcmp( name, Cmds[i].name ) )
        {
            if( op == OP_HELP )
            {
                OutVal( "AT+%s: %s", Cmds[i].name, Cmds[i].help );
                return R_NONE;
            }
            /* comandos "de execução" que o original chama como AT+CMD=? (NJS, VER...) */
            if( ( op == OP_RUN ) && ( Cmds[i].fn != C_Join ) )
            {
                op = OP_GET;
            }
            return Cmds[i].fn( op, arg );
        }
    }
    return R_ERROR;
}

static bool IsErrorRes( AtRes_t r )
{
    return ( r == R_ERROR ) || ( r == R_PARAM ) || ( r == R_BUSY ) || ( r == R_TX_LENGTH ) || ( r == R_NOT_JOINED );
}

bool AtExecute( char *line )
{
    SavedCfg = false;
    AtRes_t r = Dispatch( line );
    if( r != R_NONE )
    {
        OutLine( ResText[r] );
    }
    return !IsErrorRes( r );
}

bool AtSavedConfig( void )
{
    return SavedCfg;
}

uint16_t AtExecuteRemote( const uint8_t *cmds, uint8_t len, char *out, uint16_t max )
{
    char line[96];
    uint8_t i = 0;

    CapBuf = out;
    CapMax = max;
    CapLen = 0;
    while( i < len )
    {
        uint8_t n = 0;
        while( ( i < len ) && ( cmds[i] != ';' ) && ( cmds[i] != '\r' ) && ( cmds[i] != '\n' ) )
        {
            if( n < sizeof( line ) - 1 )
            {
                line[n++] = ( char )cmds[i];
            }
            i++;
        }
        i++;
        line[n] = '\0';
        if( n == 0 )
        {
            continue;
        }
        printf( "[AT] por downlink: %s\r\n", line );
        if( !AtIsAtLine( line ) )
        {
            OutLine( ResText[R_ERROR] );
            continue;
        }
        AtRes_t r = Dispatch( line );
        if( r != R_NONE )
        {
            OutLine( ResText[r] );
        }
    }
    CapBuf = NULL;
    return CapLen;
}
