/*
 * node_config.c — configuração do nó na EEPROM (ver node_config.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "board.h"
#include "eeprom-board.h"
#include "node_build.h"
#include "node_config.h"

NodeConfig_t NodeCfg;
volatile uint16_t NodeCfgRev = 0;

#define META_MAGIC              0x4D455441UL   /* "META" */

_Static_assert( sizeof( NodeConfig_t ) + 4 <= ( NODE_META_OFFSET - NODE_CFG_OFFSET ), "NodeConfig_t grande demais" );
_Static_assert( NODE_META_OFFSET + sizeof( NodeMeta_t ) <= NODE_EEPROM_SIZE, "NodeMeta_t fora da EEPROM" );

uint32_t NodeCrc32( const void *data, size_t len )
{
    const uint8_t *p = ( const uint8_t* )data;
    uint32_t crc = 0xFFFFFFFFUL;

    while( len-- )
    {
        crc ^= *p++;
        for( int i = 0; i < 8; i++ )
        {
            crc = ( crc >> 1 ) ^ ( 0xEDB88320UL & ( uint32_t )( -( int32_t )( crc & 1 ) ) );
        }
    }
    return ~crc;
}

void NodeDefaultDevEui( uint8_t eui[8] )
{
    /* BoardGetUniqueId devolve o ID único do STM32 em 8 bytes (LSB em [0]);
     * aqui vira MSB primeiro, como o ChirpStack mostra. */
    uint8_t id[8];
    BoardGetUniqueId( id );
    for( int i = 0; i < 8; i++ )
    {
        eui[i] = id[7 - i];
    }
}

static void NodeConfigDefaultsAt( NodeConfig_t *c );
static void NodeConfigDefaultsV3( NodeConfig_t *c );

void NodeConfigDefaults( NodeConfig_t *c )
{
    static const uint8_t devEui[8] = DEF_DEVEUI;
    static const uint8_t joinEui[8] = DEF_JOINEUI;
    static const uint8_t appKey[16] = DEF_APPKEY;
    static const uint8_t nwkSKey[16] = DEF_NWKSKEY;
    static const uint8_t appSKey[16] = DEF_APPSKEY;
    static const uint8_t rstCmd[] = DEF_RST_CMD;

    memset( c, 0, sizeof( *c ) );
    c->magic = NODE_CFG_MAGIC;
    c->version = NODE_CFG_VERSION;

    c->devEuiAuto = DEF_DEVEUI_AUTO;
    if( c->devEuiAuto )
    {
        NodeDefaultDevEui( c->devEui );
    }
    else
    {
        memcpy( c->devEui, devEui, 8 );
    }
    memcpy( c->joinEui, joinEui, 8 );
    memcpy( c->appKey, appKey, 16 );
    memcpy( c->nwkKey, appKey, 16 );
    c->devAddr = DEF_DEVADDR;
    memcpy( c->nwkSKey, nwkSKey, 16 );
    memcpy( c->appSKey, appSKey, 16 );
    c->netId = DEF_NETID;

    c->subBand = DEF_SUBBAND;
    c->ch500 = DEF_CH500;
    c->adr = DEF_ADR;
    c->dr = DEF_DR;
    c->txPowDbm = DEF_TXPOW_DBM;

    c->devClass = DEF_CLASS;
    c->pingPeriodicity = DEF_PING_PERIODICITY;

    c->txIntervalS = DEF_TX_INTERVAL_S;
    c->fport = DEF_FPORT;
    c->confirmed = DEF_CONFIRMED;
    c->payloadLen = ( uint8_t )strlen( DEF_PAYLOAD );
    memcpy( c->payload, DEF_PAYLOAD, c->payloadLen );

    c->rstEnable = DEF_RST_ENABLE;
    c->rstPort = DEF_RST_PORT;
    c->rstLen = sizeof( rstCmd );
    memcpy( c->rstCmd, rstCmd, sizeof( rstCmd ) );
    c->rstDelayMs = DEF_RST_DELAY_MS;

    c->cmdPort = DEF_CMD_PORT;

    c->lowPower = DEF_LOWPOWER;
    c->extPower = DEF_EXTPOWER;
    c->leds = DEF_LEDS;

    NodeConfigDefaultsAt( c );
}

/* Campos da v2 (comandos AT). Também usada para completar uma configuração v1. */
static void NodeConfigDefaultsAt( NodeConfig_t *c )
{
    c->activation = ( NODE_ACTIVATION == NODE_ACTIVATION_OTAA ) ? 1 : 0;
    c->autoJoin = DEF_AUTOJOIN;
    c->txPowIdx = DEF_TXP_INDEX;
    c->dutyCycle = DEF_DUTYCYCLE;
    c->publicNet = DEF_PUBLIC_NETWORK;
    c->rx2Dr = DEF_RX2_DR;
    c->rx2Freq = DEF_RX2_FREQ;
    c->rx1DelayMs = DEF_RX1_DELAY_MS;
    c->rx2DelayMs = DEF_RX2_DELAY_MS;
    c->join1DelayMs = DEF_JOIN1_DELAY_MS;
    c->join2DelayMs = DEF_JOIN2_DELAY_MS;
    c->nbTrials = DEF_NBTRIALS;
    c->baudIdx = DEF_BAUD_INDEX;
    c->kaEnable = DEF_KEEPALIVE_ENABLE;
    c->kaPort = DEF_KEEPALIVE_PORT;
    c->kaConfirmed = DEF_KEEPALIVE_CONFIRMED;
    c->kaPeriodMs = DEF_KEEPALIVE_PERIOD_MS;
    NodeConfigChannelMask( c, ( uint16_t* )( ( uint8_t* )c + offsetof( NodeConfig_t, chMask ) ) );
    memset( c->gpioMode, NODE_GPIO_UNUSED, sizeof( c->gpioMode ) );
    memset( c->gpioPull, 0, sizeof( c->gpioPull ) );
    c->atPort = DEF_AT_PORT;
    c->sessDiagOff = 0;
    NodeConfigDefaultsV3( c );
}

/* Campos da v3 (relés, PZEM e conteúdo do uplink periódico). Também usada para
 * completar uma configuração v1 ou v2: credenciais, sessão e GPIOs ficam como estavam. */
static void NodeConfigDefaultsV3( NodeConfig_t *c )
{
    c->uplinkMode = DEF_UPLINK_MODE;
    c->relayCount = 0;
    memset( c->relayPin, NODE_GPIO_UNUSED, sizeof( c->relayPin ) );
    c->relayActiveLow = 0;
    c->pzemCount = 0;
    memset( c->pzemAddr, NODE_PZEM_ADDR_GENERIC, sizeof( c->pzemAddr ) );
#if ( CONSOLE_UART != 2 )
    /* GPIO0/GPIO1 livres (nem serial transparente nem outro modo): PZEM de fábrica */
    if( ( c->gpioMode[0] == NODE_GPIO_UNUSED ) && ( c->gpioMode[1] == NODE_GPIO_UNUSED ) )
    {
        c->pzemCount = DEF_PZEM_COUNT;      /* um medidor no endereço genérico F8 */
    }
#endif
    memset( c->reserved, 0, sizeof( c->reserved ) );
}

uint16_t NodeConfigRelayPins( const NodeConfig_t *c )
{
    uint16_t m = 0;
    for( uint8_t i = 0; ( i < c->relayCount ) && ( i < NODE_RELAY_MAX ); i++ )
    {
        if( c->relayPin[i] < NODE_GPIO_COUNT )
        {
            m |= ( uint16_t )( 1U << c->relayPin[i] );
        }
    }
    return m;
}

bool NodeIsOtaa( void )
{
    return NodeCfg.activation != 0;
}

void NodeConfigChannelMask( const NodeConfig_t *c, uint16_t *out )
{
    /* "out" pode ser o chMask da própria NodeConfig_t ("packed", desalinhado):
     * monta numa cópia alinhada e copia byte a byte */
    uint16_t mask[6];
    /* Sub-banda 0..7 como no ChirpStack (au915_<n>) e nos gateways ESP32:
     * SB n = canais 8n..8n+7 de 125 kHz + canal 64+n de 500 kHz. 8 = todos. */
    memset( mask, 0, 6 * sizeof( uint16_t ) );
    if( c->subBand == NODE_SUBBAND_CUSTOM )
    {
        memcpy( mask, c->chMask, 6 * sizeof( uint16_t ) );
    }
    else if( c->subBand >= NODE_SUBBAND_ALL )
    {
        mask[0] = mask[1] = mask[2] = mask[3] = 0xFFFF;
        mask[4] = c->ch500 ? 0x00FF : 0x0000;
    }
    else
    {
        uint8_t sb = c->subBand;
        mask[sb / 2] = ( uint16_t )( 0x00FF << ( 8 * ( sb % 2 ) ) );
        mask[4] = c->ch500 ? ( uint16_t )( 1U << sb ) : 0x0000;
    }
    memcpy( out, mask, sizeof( mask ) );
}

bool NodeConfigSetChannelMask( NodeConfig_t *c, const uint16_t mask[6] )
{
    if( ( mask[4] & 0xFF00 ) || mask[5] )
    {
        return false;   /* AU915 vai só até o canal 71 (72 no manual) */
    }
    if( !( mask[0] | mask[1] | mask[2] | mask[3] | mask[4] ) )
    {
        return false;   /* nenhum canal */
    }
    /* Uma sub-banda (ou "todos") com ou sem o 500 kHz vira subband/ch500 */
    NodeConfig_t t = *c;
    for( uint8_t sb = 0; sb <= NODE_SUBBAND_ALL; sb++ )
    {
        for( uint8_t ch = 0; ch <= 1; ch++ )
        {
            uint16_t m[6];
            t.subBand = sb;
            t.ch500 = ch;
            NodeConfigChannelMask( &t, m );
            if( memcmp( m, mask, sizeof( m ) ) == 0 )
            {
                c->subBand = sb;
                c->ch500 = ch;
                memcpy( c->chMask, mask, sizeof( c->chMask ) );
                return true;
            }
        }
    }
    c->subBand = NODE_SUBBAND_CUSTOM;
    c->ch500 = ( mask[4] != 0 );
    memcpy( c->chMask, mask, sizeof( c->chMask ) );
    return true;
}

bool NodeConfigValidate( const NodeConfig_t *c, char *err, size_t errLen )
{
#define FAIL( ... ) do { snprintf( err, errLen, __VA_ARGS__ ); return false; } while( 0 )
    if( c->subBand > NODE_SUBBAND_CUSTOM )               FAIL( "sub-banda deve ser 0..7 (ou 8 = todos os canais)" );
    if( ( c->dr < 2 ) || ( c->dr > 6 ) )                 FAIL( "DR deve ser 2..6 (AU915 com dwell time)" );
    if( ( c->devClass != 'A' ) && ( c->devClass != 'B' ) && ( c->devClass != 'C' ) )
                                                         FAIL( "classe deve ser A, B ou C" );
    if( c->pingPeriodicity > 7 )                         FAIL( "ping deve ser 0..7 (2^n s)" );
    if( ( c->txIntervalS != 0 ) && ( ( c->txIntervalS < 5 ) || ( c->txIntervalS > 86400 ) ) )
                                                         FAIL( "intervalo deve ser 0 (desligado) ou 5..86400 s" );
    if( ( c->fport < 1 ) || ( c->fport > 223 ) )         FAIL( "FPort deve ser 1..223" );
    if( ( c->rstPort < 1 ) || ( c->rstPort > 223 ) )     FAIL( "FPort do reset deve ser 1..223" );
    if( ( c->rstLen < 1 ) || ( c->rstLen > NODE_RST_CMD_MAX ) )
                                                         FAIL( "comando de reset deve ter 1..16 bytes" );
    if( c->cmdPort > 223 )                               FAIL( "FPort de comandos deve ser 0 (desligado) ou 1..223" );
    if( ( c->cmdPort != 0 ) && ( c->cmdPort == c->rstPort ) )
                                                         FAIL( "FPort de comandos igual ao do reset" );
    if( ( c->txPowDbm < 2 ) || ( c->txPowDbm > 20 ) )    FAIL( "potência deve ser 2..20 dBm" );
    if( c->payloadLen > NODE_PAYLOAD_MAX )               FAIL( "payload até %d bytes", NODE_PAYLOAD_MAX );
    if( c->lowPower > 2 )                                FAIL( "lowpower deve ser 0, 1 ou 2" );
    /* v2: comandos AT */
    if( c->activation > 1 )                              FAIL( "njm deve ser 0 (ABP) ou 1 (OTAA)" );
    if( ( c->txPowIdx > 15 ) && ( c->txPowIdx != NODE_TXP_FROM_DBM ) )
                                                         FAIL( "TXP deve ser 0..15" );
    if( ( c->rx2Dr < 8 ) || ( c->rx2Dr > 13 ) )          FAIL( "RX2DR deve ser 8..13 (AU915)" );
    if( ( c->rx2Freq < 923300000UL ) || ( c->rx2Freq > 927500000UL ) )
                                                         FAIL( "RX2FQ deve ser 923300000..927500000 Hz" );
    if( ( c->rx1DelayMs < 1000 ) || ( c->rx1DelayMs > 15000 ) || ( c->rx2DelayMs != 0 && c->rx2DelayMs <= c->rx1DelayMs ) )
                                                         FAIL( "RX1DL 1000..15000 ms e RX2DL > RX1DL" );
    if( ( c->join1DelayMs < 1000 ) || ( c->join2DelayMs <= c->join1DelayMs ) )
                                                         FAIL( "JN1DL >= 1000 ms e JN2DL > JN1DL" );
    if( c->nbTrials > 79 )                               FAIL( "NBTRIALS deve ser 0..79" );
    if( c->baudIdx > 3 )                                 FAIL( "baudrate inválido" );
    if( c->kaPort > 223 )                                FAIL( "porta do keepalive deve ser 0..223" );
    if( c->kaEnable && ( c->kaPeriodMs < 5000 ) )        FAIL( "periodicidade do keepalive >= 5000 ms" );
    if( ( c->atPort > 223 ) || ( c->atPort == 4 ) || ( c->atPort && ( c->atPort == c->cmdPort ) ) )
                                                         FAIL( "FPort de AT: 0 ou 1..223, diferente de 4 e do cmdport" );
    if( c->sessDiagOff > 1 )                             FAIL( "diagnóstico de sessão: 0 ou 1" );
    for( int i = 0; i < NODE_GPIO_COUNT; i++ )
    {
        if( ( c->gpioMode[i] > 8 ) && ( c->gpioMode[i] != NODE_GPIO_UNUSED ) ) FAIL( "modo de GPIO inválido" );
        if( c->gpioPull[i] > 2 )                         FAIL( "pull de GPIO inválido" );
    }
    /* v3: relés, PZEM e protocolo ADAILSILVA_V1 (comandos na porta 10) */
    if( c->uplinkMode > NODE_UPLINK_V1 )                 FAIL( "conteúdo do uplink: 0 (payload) ou 1 (telemetria V1)" );
    if( ( c->cmdPort == 10 ) || ( c->atPort == 10 ) || ( c->rstPort == 10 ) )
                                                         FAIL( "FPort 10 é dos comandos do protocolo V1" );
    bool serial01 = ( c->gpioMode[0] == 3 ) || ( c->gpioMode[0] == 4 ) || ( c->gpioMode[1] == 3 ) || ( c->gpioMode[1] == 4 );
    if( c->relayCount > NODE_RELAY_MAX )                 FAIL( "até %d relés", NODE_RELAY_MAX );
    uint16_t used = 0;
    for( uint8_t i = 0; i < c->relayCount; i++ )
    {
        uint8_t g = c->relayPin[i];
        if( g >= NODE_GPIO_COUNT )                       FAIL( "relé %u: GPIO inválido", i );
        if( used & ( 1U << g ) )                         FAIL( "GPIO%u em dois relés", g );
        used |= ( uint16_t )( 1U << g );
        if( g <= 1 )
        {
#if ( CONSOLE_UART == 2 )
                                                         FAIL( "GPIO0/GPIO1 são o console" );
#endif
            if( c->pzemCount || serial01 )               FAIL( "GPIO%u está com a serial (PZEM ou transparente)", g );
        }
    }
    if( c->pzemCount > NODE_PZEM_MAX )                   FAIL( "até %d medidores PZEM", NODE_PZEM_MAX );
#if ( CONSOLE_UART == 2 )
    if( c->pzemCount )                                   FAIL( "PZEM usa GPIO0/GPIO1, que são o console" );
#endif
    if( c->pzemCount && serial01 )                       FAIL( "PZEM e serial transparente usam GPIO0/GPIO1: só um" );
    for( uint8_t i = 0; i < c->pzemCount; i++ )
    {
        uint8_t a = c->pzemAddr[i];
        if( ( a == 0 ) || ( ( a > 0xF7 ) && ( a != NODE_PZEM_ADDR_GENERIC ) ) )
                                                         FAIL( "endereço Modbus: 01..F7 (ou F8 com um medidor só)" );
        if( ( a == NODE_PZEM_ADDR_GENERIC ) && ( c->pzemCount > 1 ) )
                                                         FAIL( "F8 (genérico) só com um medidor no barramento" );
        for( uint8_t j = 0; j < i; j++ )
        {
            if( c->pzemAddr[j] == a )                    FAIL( "endereço Modbus repetido" );
        }
    }
#undef FAIL
    return true;
}

bool NodeConfigLoad( void )
{
    uint8_t buf[sizeof( NodeConfig_t ) + 4];

    EepromMcuReadBuffer( NODE_CFG_OFFSET, buf, sizeof( buf ) );
    NodeConfig_t *c = ( NodeConfig_t* )buf;
    uint32_t crc;
    memcpy( &crc, buf + sizeof( NodeConfig_t ), 4 );

    char err[64];
    if( ( c->magic == NODE_CFG_MAGIC ) && ( c->version == 2 ) )
    {
        /* Configuração v2 (firmware sem relés/PZEM): mantém tudo e completa os campos novos */
        memcpy( &crc, buf + NODE_CFG_V2_SIZE, 4 );
        if( crc == NodeCrc32( buf, NODE_CFG_V2_SIZE ) )
        {
            NodeConfigDefaultsV3( c );
            c->version = NODE_CFG_VERSION;
            crc = NodeCrc32( buf, sizeof( NodeConfig_t ) );
        }
    }
    else if( ( c->magic == NODE_CFG_MAGIC ) && ( c->version == 1 ) )
    {
        /* Configuração v1 (antes dos comandos AT): mantém tudo e completa os campos novos */
        memcpy( &crc, buf + NODE_CFG_V1_SIZE, 4 );
        if( crc == NodeCrc32( buf, NODE_CFG_V1_SIZE ) )
        {
            NodeConfigDefaultsAt( c );
            NodeConfigChannelMask( c, ( uint16_t* )( ( uint8_t* )c + offsetof( NodeConfig_t, chMask ) ) );
            c->version = NODE_CFG_VERSION;
            crc = NodeCrc32( buf, sizeof( NodeConfig_t ) );
        }
    }
    if( ( c->magic == NODE_CFG_MAGIC ) && ( c->version == NODE_CFG_VERSION ) &&
        ( crc == NodeCrc32( buf, sizeof( NodeConfig_t ) ) ) && NodeConfigValidate( c, err, sizeof( err ) ) )
    {
        memcpy( &NodeCfg, buf, sizeof( NodeConfig_t ) );
        if( NodeCfg.devEuiAuto )
        {
            NodeDefaultDevEui( NodeCfg.devEui );
        }
        return true;
    }
    NodeConfigDefaults( &NodeCfg );
    return false;
}

bool NodeConfigSave( void )
{
    uint8_t buf[sizeof( NodeConfig_t ) + 4];
    memcpy( buf, &NodeCfg, sizeof( NodeConfig_t ) );
    uint32_t crc = NodeCrc32( buf, sizeof( NodeConfig_t ) );
    memcpy( buf + sizeof( NodeConfig_t ), &crc, 4 );
    NodeCfgRev++;                      /* a página e o app releem o jconfig */
    return EepromMcuWriteBuffer( NODE_CFG_OFFSET, buf, sizeof( buf ) ) == LMN_STATUS_OK;
}

uint32_t NodeConfigCredFingerprint( const NodeConfig_t *c )
{
    /* Tudo o que, se mudar, invalida a sessão LoRaWAN guardada. */
    uint8_t b[8 + 8 + 16 + 16 + 4 + 16 + 16 + 4 + 2];
    uint8_t *p = b;
    memcpy( p, c->devEui, 8 );   p += 8;
    memcpy( p, c->joinEui, 8 );  p += 8;
    memcpy( p, c->appKey, 16 );  p += 16;
    memcpy( p, c->nwkKey, 16 );  p += 16;
    memcpy( p, &c->devAddr, 4 ); p += 4;
    memcpy( p, c->nwkSKey, 16 ); p += 16;
    memcpy( p, c->appSKey, 16 ); p += 16;
    memcpy( p, &c->netId, 4 );   p += 4;
    *p++ = c->subBand;
    *p++ = c->ch500;
    uint32_t fp = NodeCrc32( b, sizeof( b ) );
    /* v2: só entram quando fogem do padrão, para a impressão de uma configuração
     * v1 migrada continuar a mesma (a sessão LoRaWAN guardada não se perde) */
    if( c->subBand == NODE_SUBBAND_CUSTOM )
    {
        fp ^= NodeCrc32( c->chMask, sizeof( c->chMask ) );
    }
    /* Ativação: entra só no ABP. Não depende da ativação de fábrica (NODE_ACTIVATION),
     * então trocar o padrão no node_build.h não apaga sessões. É a mesma regra do antigo
     * firmware OTAA; os módulos que vieram do antigo firmware ABP são migrados no boot
     * (NODE_META_FP_UNIFIED, em main.c) sem perder a sessão nem o FCnt. */
    if( c->activation == 0 )
    {
        fp ^= NODE_FP_NJM_XOR;
    }
    return fp;
}

bool NodeMetaLoad( NodeMeta_t *m )
{
    EepromMcuReadBuffer( NODE_META_OFFSET, ( uint8_t* )m, sizeof( *m ) );
    if( ( m->magic == META_MAGIC ) && ( m->crc == NodeCrc32( m, offsetof( NodeMeta_t, crc ) ) ) )
    {
        return true;
    }
    memset( m, 0, sizeof( *m ) );
    m->magic = META_MAGIC;
    return false;
}

bool NodeMetaSave( NodeMeta_t *m )
{
    m->magic = META_MAGIC;
    m->crc = NodeCrc32( m, offsetof( NodeMeta_t, crc ) );
    return EepromMcuWriteBuffer( NODE_META_OFFSET, ( uint8_t* )m, sizeof( *m ) ) == LMN_STATUS_OK;
}

int HexToBytes( const char *hex, uint8_t *out, size_t max )
{
    size_t n = 0;
    int hi = -1;

    for( ; *hex; hex++ )
    {
        char ch = *hex;
        if( ( ch == ' ' ) || ( ch == ':' ) || ( ch == '-' ) )
        {
            continue;
        }
        int v;
        if( ( ch >= '0' ) && ( ch <= '9' ) )      v = ch - '0';
        else if( ( ch >= 'a' ) && ( ch <= 'f' ) ) v = ch - 'a' + 10;
        else if( ( ch >= 'A' ) && ( ch <= 'F' ) ) v = ch - 'A' + 10;
        else return -1;

        if( hi < 0 )
        {
            hi = v;
        }
        else
        {
            if( n >= max )
            {
                return -1;
            }
            out[n++] = ( uint8_t )( ( hi << 4 ) | v );
            hi = -1;
        }
    }
    return ( hi < 0 ) ? ( int )n : -1;
}

void BytesToHex( const uint8_t *in, size_t len, char *out )
{
    static const char h[] = "0123456789ABCDEF";
    for( size_t i = 0; i < len; i++ )
    {
        out[2 * i] = h[in[i] >> 4];
        out[2 * i + 1] = h[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}
