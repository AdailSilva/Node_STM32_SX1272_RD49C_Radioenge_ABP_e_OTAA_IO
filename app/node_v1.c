/*
 * node_v1.c — protocolo de aplicação ADAILSILVA_V1 (veja node_v1.h e
 * doc/PROTOCOLO_ADAILSILVA_V1.md). Inteiros big-endian.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "timer.h"
#include "node_build.h"
#include "node_config.h"
#include "node_relay.h"
#include "node_pzem.h"
#include "app.h"
#include "node_v1.h"

#define REPLY_MAX       ( 3 + 19 )      /* STATUS: 19 bytes de dados; PZEM_LER: 1 + 18 */
#define REPLY_QUEUE     4
#define TEL_MAX         ( 6 + NODE_PZEM_MAX * ( 3 + NODE_PZEM_BLOCK_LEN ) + 5 + 4 )
#define RETRY_MS        3000

typedef struct
{
    uint8_t  len;
    uint8_t  data[REPLY_MAX];
    bool     reboot;            /* REINICIAR: reinicia depois que esta resposta sair */
}V1Reply_t;

typedef enum { TEL_IDLE = 0, TEL_ACQUIRING, TEL_SENDING } TelState_t;

static uint8_t BootReason = V1_BOOT_DESCONHECIDO;

/* deduplicação */
static uint8_t LastSeq = 0;             /* 0 = nenhum */
static V1Reply_t LastReply;
static bool LastReplyReady = false;     /* false: comando assíncrono ainda em andamento */

/* comando assíncrono (PZEM) em andamento */
static bool AsyncActive = false;
static uint8_t AsyncSeq, AsyncOp, AsyncCh;

/* respostas a enviar */
static V1Reply_t Replies[REPLY_QUEUE];
static uint8_t RHead = 0, RCount = 0;

/* REINICIAR */
static bool RebootArmed = false;
static uint16_t RebootDelayMs = 0;

/* telemetria */
static bool TelWantFull = false, TelWantRelay = false;
static TelState_t TelState = TEL_IDLE;
static bool TelFull = false;
static uint8_t TelReadsLeft = 0;
static uint8_t TelOkMask = 0;
static uint8_t TelBuf[TEL_MAX];
static uint8_t TelLen = 0, TelOff = 0;
static bool BootPending = true;         /* BOOT vai só na primeira telemetria depois de ligar */
static int16_t BootAt = -1;             /* posição do bloco BOOT no TelBuf */

/* adiamento quando a MAC recusa (janela de beacon, duty cycle...) */
static TimerEvent_t RetryTimer;
static volatile bool Backoff = false;

static void OnRetryTimer( void *context )
{
    ( void )context;
    TimerStop( &RetryTimer );
    Backoff = false;
}

static void StartBackoff( void )
{
    Backoff = true;
    TimerStop( &RetryTimer );
    TimerSetValue( &RetryTimer, RETRY_MS );
    TimerStart( &RetryTimer );
}

static const char* OpName( uint8_t op )
{
    switch( op )
    {
    case V1_OP_STATUS:        return "STATUS";
    case V1_OP_INTERVALO:     return "INTERVALO";
    case V1_OP_ENVIAR_AGORA:  return "ENVIAR_AGORA";
    case V1_OP_CONFIRMADO:    return "CONFIRMADO";
    case V1_OP_CLASSE:        return "CLASSE";
    case V1_OP_REINICIAR:     return "REINICIAR";
    case V1_OP_RELE_DEFINIR:  return "RELE_DEFINIR";
    case V1_OP_RELE_PULSO:    return "RELE_PULSO";
    case V1_OP_RELE_MASCARA:  return "RELE_MASCARA";
    case V1_OP_RELE_LER:      return "RELE_LER";
    case V1_OP_PZEM_LER:      return "PZEM_LER";
    case V1_OP_PZEM_ZERAR:    return "PZEM_ZERAR_ENERGIA";
    case V1_OP_PZEM_LIMITE:   return "PZEM_LIMITE_POTENCIA";
    default:                  return "desconhecido";
    }
}

static uint16_t Get16( const uint8_t *p ) { return ( uint16_t )( ( p[0] << 8 ) | p[1] ); }
static uint32_t Get32( const uint8_t *p )
{
    return ( ( uint32_t )p[0] << 24 ) | ( ( uint32_t )p[1] << 16 ) | ( ( uint32_t )p[2] << 8 ) | p[3];
}
static void Put16( uint8_t *p, uint16_t v ) { p[0] = ( uint8_t )( v >> 8 ); p[1] = ( uint8_t )v; }
static void Put32( uint8_t *p, uint32_t v )
{
    p[0] = ( uint8_t )( v >> 24 ); p[1] = ( uint8_t )( v >> 16 ); p[2] = ( uint8_t )( v >> 8 ); p[3] = ( uint8_t )v;
}

/* "0.1.0" -> 0, 1, 0 */
static void FwVersion( uint8_t v[3] )
{
    const char *s = NODE_FW_VERSION;
    for( uint8_t i = 0; i < 3; i++ )
    {
        uint16_t n = 0;
        while( ( *s >= '0' ) && ( *s <= '9' ) )
        {
            n = ( uint16_t )( n * 10 + ( *s++ - '0' ) );
        }
        v[i] = ( uint8_t )( ( n > 255 ) ? 255 : n );
        while( *s && ( ( *s < '0' ) || ( *s > '9' ) ) )
        {
            s++;
        }
    }
}

/* ------------------------------------------------------------ respostas */
static void Enqueue( const V1Reply_t *r )
{
    if( RCount >= REPLY_QUEUE )
    {
        /* fila cheia (Classe C com muitos comandos seguidos): descarta a mais antiga;
         * o servidor reenvia com o mesmo seq e recebe a resposta guardada */
        RHead = ( uint8_t )( ( RHead + 1 ) % REPLY_QUEUE );
        RCount--;
    }
    Replies[( RHead + RCount ) % REPLY_QUEUE] = *r;
    RCount++;
}

static void Finish( uint8_t seq, V1Reply_t *r )
{
    /* guarda só se ainda for o último seq (um comando assíncrono pode terminar depois
     * de outro comando ter chegado) */
    if( ( seq != 0 ) && ( seq == LastSeq ) )
    {
        LastSeq = seq;
        LastReply = *r;
        LastReply.reboot = false;           /* reenvio de um REINICIAR não reinicia de novo */
        LastReplyReady = true;
    }
    Enqueue( r );
    printf( "[V1] Resposta seq %u · %s · status %u · %u bytes na porta %u.\r\n", seq, OpName( r->data[1] ), r->data[2],
            r->len, NODE_V1_PORT_REPLY );
}

static void RelayData( V1Reply_t *r )
{
    r->data[r->len++] = NodeRelayCount( );
    Put16( &r->data[r->len], NodeRelayMap( ) );
    r->len += 2;
}

static uint8_t RelayStatus( NodeRelayStatus_t s )
{
    return ( s == NODE_RELAY_OK ) ? V1_OK : ( s == NODE_RELAY_ABSENT ) ? V1_PERIFERICO_AUSENTE : V1_PARAMETRO_INVALIDO;
}

static void OnPzemDone( uint8_t tag, uint8_t ch, NodePzemOp_t op, bool ok );

/* Executa o comando. Retorna false se ficou assíncrono (a resposta sai depois). */
static bool Execute( uint8_t seq, uint8_t op, const uint8_t *p, uint8_t n, V1Reply_t *r )
{
    uint8_t *st = &r->data[2];
    *st = V1_OK;
    switch( op )
    {
    case V1_OP_STATUS:
    {
        uint8_t *d = &r->data[3];
        uint8_t v[3];
        FwVersion( v );
        d[0] = NODE_V1_VERSION;
        d[1] = v[0]; d[2] = v[1]; d[3] = v[2];
        Put32( d + 4, AppUptimeMs( ) / 1000 );
        Put32( d + 8, NodeCfg.txIntervalS );
        d[12] = ( uint8_t )AppCurrentClass( );      /* 'A' ou 'C' ('B' só neste firmware) */
        d[13] = NodeCfg.confirmed ? 1 : 0;
        d[14] = NodeRelayCount( );
        Put16( d + 15, NodeRelayMap( ) );
        d[17] = NodeCfg.pzemCount;
        d[18] = BootReason;
        r->len = 3 + 19;
        break;
    }
    case V1_OP_INTERVALO:
    {
        if( n < 4 )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        uint32_t s = Get32( p );
        if( ( s < 5 ) || ( s > 86400 ) )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        if( NodeCfg.txIntervalS != s )
        {
            NodeCfg.txIntervalS = s;
            NodeConfigSave( );
            AppApplyRuntimeConfig( );
        }
        printf( "[V1] Intervalo de telemetria: %lu s.\r\n", ( unsigned long )s );
        Put32( &r->data[3], s );
        r->len = 7;
        break;
    }
    case V1_OP_ENVIAR_AGORA:
        NodeV1RequestTelemetry( true );
        break;
    case V1_OP_CONFIRMADO:
        if( ( n < 1 ) || ( p[0] > 1 ) )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        if( NodeCfg.confirmed != p[0] )
        {
            NodeCfg.confirmed = p[0];
            NodeConfigSave( );
            AppApplyRuntimeConfig( );
        }
        break;
    case V1_OP_CLASSE:
        if( ( n < 1 ) || ( ( p[0] != 'A' ) && ( p[0] != 'C' ) ) )
        {
            *st = V1_PARAMETRO_INVALIDO;    /* o V1 define A e C ('B' fica no AT+CLASS) */
            break;
        }
        if( NodeCfg.devClass != ( char )p[0] )
        {
            NodeCfg.devClass = ( char )p[0];
            NodeConfigSave( );
        }
        printf( "[V1] Classe %c.\r\n", p[0] );
        AppRequestClass( ( char )p[0] );
        break;
    case V1_OP_REINICIAR:
    {
        if( n < 2 )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        uint16_t ms = Get16( p );
        if( ms > 60000 )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        RebootDelayMs = ms;
        r->reboot = true;
        printf( "[V1] Reinício pedido: sai a resposta e reinicia %u ms depois.\r\n", ms );
        break;
    }
    case V1_OP_RELE_DEFINIR:
        if( n < 2 )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        *st = RelayStatus( NodeRelaySet( p[0], p[1] ) );
        if( *st == V1_OK )
        {
            RelayData( r );
        }
        break;
    case V1_OP_RELE_PULSO:
        if( n < 6 )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        *st = RelayStatus( NodeRelayPulse( p[0], p[1], Get32( p + 2 ) ) );
        if( *st == V1_OK )
        {
            RelayData( r );
        }
        break;
    case V1_OP_RELE_MASCARA:
        if( n < 4 )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        *st = RelayStatus( NodeRelayMask( Get16( p ), Get16( p + 2 ) ) );
        if( *st == V1_OK )
        {
            RelayData( r );
        }
        break;
    case V1_OP_RELE_LER:
        RelayData( r );
        break;
    case V1_OP_PZEM_LER:
    case V1_OP_PZEM_ZERAR:
    case V1_OP_PZEM_LIMITE:
    {
        if( ( n < 1 ) || ( ( op == V1_OP_PZEM_LIMITE ) && ( n < 3 ) ) )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        uint16_t w = ( op == V1_OP_PZEM_LIMITE ) ? Get16( p + 1 ) : 0;
        if( w > NODE_PZEM_POWER_MAX )
        {
            *st = V1_PARAMETRO_INVALIDO;
            break;
        }
        if( ( p[0] >= NodeCfg.pzemCount ) || !NodePzemActive( ) )
        {
            *st = V1_PERIFERICO_AUSENTE;
            break;
        }
        NodePzemOp_t pop = ( op == V1_OP_PZEM_LER ) ? NODE_PZEM_OP_READ
                         : ( op == V1_OP_PZEM_ZERAR ) ? NODE_PZEM_OP_RESET_ENERGY : NODE_PZEM_OP_POWER_LIMIT;
        if( AsyncActive || !NodePzemRequest( pop, p[0], w, NODE_PZEM_TAG_V1, OnPzemDone ) )
        {
            *st = V1_OCUPADO;
            break;
        }
        AsyncActive = true;
        AsyncSeq = seq;
        AsyncOp = op;
        AsyncCh = p[0];
        return false;
    }
    default:
        *st = V1_OPCODE_DESCONHECIDO;
        break;
    }
    return true;
}

void NodeV1OnCommand( const uint8_t *d, uint8_t n )
{
    if( n == 0 )
    {
        return;
    }
    uint8_t seq = d[0];
    AppStatus.v1Commands++;
    if( ( seq != 0 ) && ( seq == LastSeq ) )
    {
        if( LastReplyReady )
        {
            printf( "[V1] seq %u repetido: reenviando a resposta guardada (sem executar de novo).\r\n", seq );
            Enqueue( &LastReply );
        }
        else
        {
            printf( "[V1] seq %u repetido: o comando ainda está em andamento.\r\n", seq );
        }
        return;
    }

    V1Reply_t r;
    memset( &r, 0, sizeof( r ) );
    r.data[0] = seq;
    r.data[1] = ( n >= 2 ) ? d[1] : 0;
    r.len = 3;
    if( n < 2 )
    {
        r.data[2] = V1_PARAMETRO_INVALIDO;  /* sem opcode */
        Finish( seq, &r );
        return;
    }
    printf( "[V1] Comando seq %u · %s (0x%02X) · %u byte(s) de parâmetros.\r\n", seq, OpName( d[1] ), d[1], n - 2 );
    if( seq != 0 )
    {
        LastSeq = seq;                      /* a partir daqui, o mesmo seq não executa de novo */
        LastReplyReady = false;
    }
    if( Execute( seq, d[1], d + 2, ( uint8_t )( n - 2 ), &r ) )
    {
        Finish( seq, &r );
    }
}

static void OnPzemDone( uint8_t tag, uint8_t ch, NodePzemOp_t op, bool ok )
{
    if( tag == NODE_PZEM_TAG_TELEMETRY )
    {
        if( ok )
        {
            TelOkMask |= ( uint8_t )( 1U << ch );
        }
        if( TelReadsLeft )
        {
            TelReadsLeft--;
        }
        return;
    }
    if( ( tag != NODE_PZEM_TAG_V1 ) || !AsyncActive )
    {
        return;
    }
    AsyncActive = false;
    V1Reply_t r;
    memset( &r, 0, sizeof( r ) );
    r.data[0] = AsyncSeq;
    r.data[1] = AsyncOp;
    r.data[2] = ok ? V1_OK : V1_FALHA_PERIFERICO;
    r.len = 3;
    if( ok && ( op == NODE_PZEM_OP_READ ) )
    {
        r.data[3] = AsyncCh;
        NodePzemLast( ch, &r.data[4], NULL );
        r.len = 4 + NODE_PZEM_BLOCK_LEN;
    }
    Finish( AsyncSeq, &r );
}

bool NodeV1ServiceReply( void )
{
    if( ( RCount == 0 ) || Backoff )
    {
        return false;
    }
    V1Reply_t *r = &Replies[RHead];
    uint8_t max = AppMaxPayload( );
    uint8_t len = ( r->len > max ) ? 3 : r->len;    /* não cabe: só seq | opcode | status */
    AppTxResult_t res = AppSendFrame( NODE_V1_PORT_REPLY, r->data, len, false, 1, false );
    if( res == APP_TX_BUSY )
    {
        StartBackoff( );
        return true;
    }
    if( res == APP_TX_OK )
    {
        AppStatus.v1Uplinks++;
        if( len < r->len )
        {
            printf( "[V1] Resposta seq %u reduzida a 3 bytes (máx. %u no DR atual).\r\n", r->data[0], max );
        }
    }
    if( r->reboot )
    {
        RebootArmed = true;
        if( res != APP_TX_OK )
        {
            AppReboot( RebootDelayMs );     /* a resposta não pôde sair: reinicia assim mesmo */
        }
        else
        {
            AppReboot( 30000 );             /* garantia, se o fim do envio não for avisado */
        }
    }
    RHead = ( uint8_t )( ( RHead + 1 ) % REPLY_QUEUE );
    RCount--;
    return true;
}

void NodeV1OnTxDone( uint8_t port )
{
    if( RebootArmed && ( port == NODE_V1_PORT_REPLY ) )
    {
        RebootArmed = false;
        AppReboot( RebootDelayMs );
    }
}

/* ------------------------------------------------------------ telemetria */
void NodeV1RequestTelemetry( bool full )
{
    if( full )
    {
        TelWantFull = true;
    }
    else
    {
        TelWantRelay = true;
    }
}

static uint8_t Tlv( uint8_t pos, uint8_t type, uint8_t ch, uint8_t len )
{
    TelBuf[pos] = type;
    TelBuf[pos + 1] = ch;
    TelBuf[pos + 2] = len;
    return ( uint8_t )( pos + 3 );
}

static void Build( void )
{
    uint8_t n = 0;
    BootAt = -1;
    if( NodeRelayCount( ) )
    {
        n = Tlv( n, V1_TLV_RELES, 0, 3 );
        TelBuf[n++] = NodeRelayCount( );
        Put16( &TelBuf[n], NodeRelayMap( ) );
        n += 2;
    }
    if( TelFull )
    {
        for( uint8_t ch = 0; ch < NodeCfg.pzemCount; ch++ )
        {
            if( ( TelOkMask & ( 1U << ch ) ) && NodePzemLast( ch, &TelBuf[n + 3], NULL ) )
            {
                n = Tlv( n, V1_TLV_PZEM, ch, NODE_PZEM_BLOCK_LEN );
                n += NODE_PZEM_BLOCK_LEN;
            }
            else
            {
                n = Tlv( n, V1_TLV_PZEM_FALHA, ch, 0 );
            }
        }
        BoardGetBatteryLevel( );            /* mede o VDD de novo */
        uint32_t mv = BoardGetBatteryVoltage( );
        n = Tlv( n, V1_TLV_ALIMENTACAO, 0, 2 );
        Put16( &TelBuf[n], ( uint16_t )( ( mv > 0xFFFF ) ? 0xFFFF : mv ) );
        n += 2;
    }
    if( BootPending )
    {
        BootAt = n;
        n = Tlv( n, V1_TLV_BOOT, 0, 1 );
        TelBuf[n++] = BootReason;
    }
    TelLen = n;
    TelOff = 0;
    TelState = n ? TEL_SENDING : TEL_IDLE;
}

bool NodeV1HasEvent( void )
{
    return ( ( TelState == TEL_IDLE ) && ( TelWantFull || TelWantRelay ) ) ||
           ( ( TelState == TEL_ACQUIRING ) && ( TelReadsLeft == 0 ) );
}

bool NodeV1HasUplink( void )
{
    return !Backoff && ( ( RCount > 0 ) || ( TelState == TEL_SENDING ) );
}

void NodeV1Process( void )
{
    if( NodeRelayTakeReport( ) )
    {
        TelWantRelay = true;                /* fim de pulso ou mudança local */
    }
    if( ( TelState == TEL_IDLE ) && ( TelWantFull || TelWantRelay ) )
    {
        if( !AppStatus.joined )
        {
            TelWantFull = TelWantRelay = false;   /* sem sessão: a próxima periódica leva tudo */
            return;
        }
        TelFull = TelWantFull;
        TelWantFull = TelWantRelay = false;
        TelOkMask = 0;
        TelReadsLeft = 0;
        if( TelFull && NodePzemActive( ) )
        {
            for( uint8_t ch = 0; ch < NodeCfg.pzemCount; ch++ )
            {
                if( NodePzemRequest( NODE_PZEM_OP_READ, ch, 0, NODE_PZEM_TAG_TELEMETRY, OnPzemDone ) )
                {
                    TelReadsLeft++;
                }
            }
        }
        TelState = TelReadsLeft ? TEL_ACQUIRING : TEL_IDLE;
        if( TelState == TEL_IDLE )
        {
            Build( );
        }
    }
    if( ( TelState == TEL_ACQUIRING ) && ( TelReadsLeft == 0 ) )
    {
        Build( );
    }
}

bool NodeV1ServiceTelemetry( void )
{
    if( ( TelState != TEL_SENDING ) || Backoff )
    {
        return false;
    }
    uint8_t max = AppMaxPayload( );
    if( max == 0 )
    {
        StartBackoff( );                    /* comandos MAC ocupando o quadro: tenta depois */
        return true;
    }
    uint8_t end = TelOff;
    while( end < TelLen )
    {
        uint8_t blen = ( uint8_t )( 3 + TelBuf[end + 2] );
        if( ( uint16_t )( end + blen - TelOff ) > max )
        {
            break;
        }
        end = ( uint8_t )( end + blen );
    }
    if( end == TelOff )
    {
        /* um bloco sozinho maior que o DR permite (PZEM = 21 bytes não cabe em DR2) */
        uint8_t blen = ( uint8_t )( 3 + TelBuf[TelOff + 2] );
        printf( "[V1] AVISO: bloco 0x%02X de %u bytes não cabe no DR atual (máx. %u): descartado. "
                "Use DR3 ou maior com PZEM.\r\n", TelBuf[TelOff], blen, max );
        TelOff = ( uint8_t )( TelOff + blen );
        if( TelOff >= TelLen )
        {
            TelState = TEL_IDLE;
        }
        return true;
    }
    AppTxResult_t res = AppSendFrame( NODE_V1_PORT_TELEMETRY, &TelBuf[TelOff], ( uint8_t )( end - TelOff ),
                                      NodeCfg.confirmed, NodeCfg.nbTrials, false );
    if( res == APP_TX_BUSY )
    {
        StartBackoff( );
        return true;
    }
    if( res == APP_TX_OK )
    {
        AppStatus.v1Uplinks++;
        if( ( BootAt >= TelOff ) && ( BootAt < end ) )
        {
            BootPending = false;
        }
        if( ( TelOff != 0 ) || ( end < TelLen ) )
        {
            printf( "[V1] Telemetria dividida: bytes %u..%u de %u.\r\n", TelOff, end - 1, TelLen );
        }
    }
    TelOff = end;
    if( TelOff >= TelLen )
    {
        TelState = TEL_IDLE;
    }
    return true;
}

void NodeV1Init( uint8_t bootReason )
{
    BootReason = bootReason;
    BootPending = true;
    TimerInit( &RetryTimer, OnRetryTimer );
}

uint8_t NodeV1BootReason( void )
{
    return BootReason;
}
