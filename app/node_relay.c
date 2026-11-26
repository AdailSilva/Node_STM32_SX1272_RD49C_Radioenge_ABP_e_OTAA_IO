/*
 * node_relay.c — relés em GPIOs do módulo (veja node_relay.h).
 *
 * Registro do anel na EEPROM (8 bytes, NODE_RELAY_SLOTS registros a partir de
 * NODE_RELAY_OFFSET): contador u32, mapa u16, verificação u16. Vale o registro
 * íntegro de maior contador; o próximo vai no registro seguinte do anel.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "utilities.h"
#include "timer.h"
#include "eeprom-board.h"
#include "node_config.h"
#include "node_io.h"
#include "node_relay.h"

typedef struct __attribute__( ( packed ) )
{
    uint32_t counter;
    uint16_t map;
    uint16_t check;
}RelaySlot_t;

_Static_assert( sizeof( RelaySlot_t ) * NODE_RELAY_SLOTS <= NODE_RELAY_AREA, "anel dos relés maior que a área" );
_Static_assert( NODE_META_OFFSET + 16 <= NODE_RELAY_OFFSET, "anel dos relés em cima dos metadados" );
_Static_assert( NODE_RELAY_OFFSET + NODE_RELAY_AREA <= NODE_EEPROM_SIZE, "anel dos relés fora da EEPROM" );

static uint16_t Map = 0;                /* estado atual (com os pulsos) */
static uint16_t PulseMask = 0;          /* canais em pulso */
static uint16_t PulseRestore = 0;       /* estado de cada canal antes do pulso */
static uint32_t PulseEnd[NODE_RELAY_MAX];
static uint16_t Saved = 0;              /* mapa gravado na EEPROM */
static uint32_t SavedCounter = 0;
static uint8_t SavedSlot = NODE_RELAY_SLOTS - 1;
static bool PersistPending = false;
static volatile bool PulseDue = false;
static bool Report = false;
static TimerEvent_t RelayTimer;
static bool TimerReady = false;

static uint16_t Check( uint32_t cnt, uint16_t map )
{
    return ( uint16_t )( ~( map ^ ( uint16_t )cnt ^ ( uint16_t )( cnt >> 16 ) ) ^ 0x5AA5 );
}

static uint16_t AllMask( void )
{
    return ( uint16_t )( ( 1U << NodeCfg.relayCount ) - 1U );
}

/* Estado "estável": o que volta depois de um reinício (pulsos no estado anterior) */
static uint16_t Stable( void )
{
    return ( uint16_t )( ( ( Map & ~PulseMask ) | ( PulseRestore & PulseMask ) ) & AllMask( ) );
}

static uint8_t LevelOf( uint8_t ch, bool on )
{
    bool al = ( NodeCfg.relayActiveLow >> ch ) & 1;
    return ( uint8_t )( on != al );
}

static void Drive( uint8_t ch )
{
    NodeIoRelayDrive( NodeCfg.relayPin[ch], LevelOf( ch, ( Map >> ch ) & 1 ) );
}

static void LoadSaved( void )
{
    RelaySlot_t s;
    bool found = false;
    for( uint8_t i = 0; i < NODE_RELAY_SLOTS; i++ )
    {
        EepromMcuReadBuffer( ( uint16_t )( NODE_RELAY_OFFSET + i * sizeof( s ) ), ( uint8_t* )&s, sizeof( s ) );
        if( ( s.check == Check( s.counter, s.map ) ) && ( !found || ( s.counter > SavedCounter ) ) )
        {
            found = true;
            SavedCounter = s.counter;
            SavedSlot = i;
            Saved = s.map;
        }
    }
    if( !found )
    {
        Saved = 0;
        SavedCounter = 0;
        SavedSlot = NODE_RELAY_SLOTS - 1;
    }
}

static void Persist( void )
{
    uint16_t st = Stable( );
    PersistPending = false;
    if( st == Saved )
    {
        return;                             /* nada mudou: não gasta a EEPROM */
    }
    RelaySlot_t s;
    s.counter = SavedCounter + 1;
    s.map = st;
    s.check = Check( s.counter, s.map );
    uint8_t slot = ( uint8_t )( ( SavedSlot + 1 ) % NODE_RELAY_SLOTS );
    if( EepromMcuWriteBuffer( ( uint16_t )( NODE_RELAY_OFFSET + slot * sizeof( s ) ), ( uint8_t* )&s, sizeof( s ) ) == LMN_STATUS_OK )
    {
        SavedSlot = slot;
        SavedCounter = s.counter;
        Saved = st;
    }
    else
    {
        printf( "[Relés] Falha ao gravar o estado na EEPROM.\r\n" );
    }
}

static void OnRelayTimer( void *context )
{
    ( void )context;
    TimerStop( &RelayTimer );
    PulseDue = true;                        /* o laço principal termina os pulsos */
}

static void ArmTimer( void )
{
    TimerStop( &RelayTimer );
    if( !PulseMask )
    {
        return;
    }
    uint32_t now = ( uint32_t )TimerGetCurrentTime( ), next = 0xFFFFFFFFUL;
    for( uint8_t ch = 0; ch < NodeCfg.relayCount; ch++ )
    {
        if( PulseMask & ( 1U << ch ) )
        {
            int32_t left = ( int32_t )( PulseEnd[ch] - now );
            uint32_t l = ( left <= 0 ) ? 1 : ( uint32_t )left;
            if( l < next )
            {
                next = l;
            }
        }
    }
    /* esperas longas em etapas de 1 h (o pulso pode durar até 24 h) */
    TimerSetValue( &RelayTimer, ( next > 3600000UL ) ? 3600000UL : next );
    TimerStart( &RelayTimer );
}

void NodeRelayInit( void )
{
    if( !TimerReady )
    {
        TimerInit( &RelayTimer, OnRelayTimer );
        TimerReady = true;
    }
    LoadSaved( );
    Map = Saved & AllMask( );
    PulseMask = PulseRestore = 0;
    for( uint8_t ch = 0; ch < NodeCfg.relayCount; ch++ )
    {
        uint8_t g = NodeCfg.relayPin[ch];
        NodeCfg.gpioMode[g] = 1;            /* saída push-pull (o NodeIoInit aplica) */
        NodeCfg.gpioPull[g] = 0;
        NodeIoPresetLevel( g, LevelOf( ch, ( Map >> ch ) & 1 ) );
    }
}

uint8_t NodeRelayCount( void )
{
    return NodeCfg.relayCount;
}

uint16_t NodeRelayMap( void )
{
    return Map & AllMask( );
}

uint16_t NodeRelayPulsing( void )
{
    return PulseMask;
}

static void Apply( uint8_t ch, bool on )
{
    if( on )
    {
        Map |= ( uint16_t )( 1U << ch );
    }
    else
    {
        Map &= ( uint16_t )~( 1U << ch );
    }
    Drive( ch );
}

NodeRelayStatus_t NodeRelaySet( uint8_t ch, uint8_t action )
{
    if( action > NODE_RELAY_TOGGLE )
    {
        return NODE_RELAY_PARAM;
    }
    if( ( NodeCfg.relayCount == 0 ) || ( ( ch != NODE_RELAY_ALL ) && ( ch >= NodeCfg.relayCount ) ) )
    {
        return NODE_RELAY_ABSENT;
    }
    for( uint8_t c = 0; c < NodeCfg.relayCount; c++ )
    {
        if( ( ch != NODE_RELAY_ALL ) && ( c != ch ) )
        {
            continue;
        }
        PulseMask &= ( uint16_t )~( 1U << c );     /* um comando explícito encerra o pulso */
        bool cur = ( Map >> c ) & 1;
        Apply( c, ( action == NODE_RELAY_TOGGLE ) ? !cur : ( action == NODE_RELAY_ON ) );
    }
    ArmTimer( );
    PersistPending = true;
    return NODE_RELAY_OK;
}

NodeRelayStatus_t NodeRelayMask( uint16_t mask, uint16_t values )
{
    if( ( NodeCfg.relayCount == 0 ) || ( mask & ~AllMask( ) ) )
    {
        return NODE_RELAY_ABSENT;
    }
    for( uint8_t c = 0; c < NodeCfg.relayCount; c++ )
    {
        if( mask & ( 1U << c ) )
        {
            PulseMask &= ( uint16_t )~( 1U << c );
            Apply( c, ( values >> c ) & 1 );
        }
    }
    ArmTimer( );
    PersistPending = true;
    return NODE_RELAY_OK;
}

NodeRelayStatus_t NodeRelayPulse( uint8_t ch, uint8_t state, uint32_t ms )
{
    if( ( state > 1 ) || ( ms < NODE_RELAY_PULSE_MIN_MS ) || ( ms > NODE_RELAY_PULSE_MAX_MS ) )
    {
        return NODE_RELAY_PARAM;
    }
    if( ( NodeCfg.relayCount == 0 ) || ( ( ch != NODE_RELAY_ALL ) && ( ch >= NodeCfg.relayCount ) ) )
    {
        return NODE_RELAY_ABSENT;
    }
    uint32_t end = ( uint32_t )TimerGetCurrentTime( ) + ms;
    for( uint8_t c = 0; c < NodeCfg.relayCount; c++ )
    {
        if( ( ch != NODE_RELAY_ALL ) && ( c != ch ) )
        {
            continue;
        }
        if( !( PulseMask & ( 1U << c ) ) )
        {
            /* pulso novo: guarda o estado para onde volta (pulso repetido mantém o original) */
            PulseRestore = ( uint16_t )( ( PulseRestore & ~( 1U << c ) ) | ( Map & ( 1U << c ) ) );
            PulseMask |= ( uint16_t )( 1U << c );
        }
        PulseEnd[c] = end;
        Apply( c, state != 0 );
    }
    ArmTimer( );
    PersistPending = true;                  /* o estável não muda; Persist( ) não grava */
    return NODE_RELAY_OK;
}

bool NodeRelayConfigure( uint8_t count, const uint8_t *pins, uint8_t activeLow, char *err, size_t errLen )
{
    if( count > NODE_RELAY_MAX )
    {
        snprintf( err, errLen, "até %d relés", NODE_RELAY_MAX );
        return false;
    }
    NodeConfig_t c = NodeCfg;
    uint16_t oldPins = NodeConfigRelayPins( &NodeCfg ), newPins = 0;
    c.relayCount = count;
    memset( c.relayPin, NODE_GPIO_UNUSED, sizeof( c.relayPin ) );
    for( uint8_t i = 0; i < count; i++ )
    {
        c.relayPin[i] = pins[i];
        if( pins[i] < NODE_GPIO_COUNT )
        {
            newPins |= ( uint16_t )( 1U << pins[i] );
        }
    }
    c.relayActiveLow = ( uint8_t )( activeLow & ( ( 1U << count ) - 1U ) );
    for( uint8_t g = 0; g < NODE_GPIO_COUNT; g++ )
    {
        if( newPins & ( 1U << g ) )
        {
            c.gpioMode[g] = 1;
            c.gpioPull[g] = 0;
        }
        else if( oldPins & ( 1U << g ) )
        {
            c.gpioMode[g] = NODE_GPIO_UNUSED;   /* deixou de ser relé: alta impedância */
            c.gpioPull[g] = 0;
        }
    }
    if( !NodeConfigValidate( &c, err, errLen ) )
    {
        return false;
    }

    /* estados: os canais que continuam existindo mantêm o estado (sem pulsos) */
    uint16_t keep = ( uint16_t )( Stable( ) & ( ( 1U << count ) - 1U ) );
    PulseMask = PulseRestore = 0;
    TimerStop( &RelayTimer );
    NodeCfg = c;
    Map = keep;
    for( uint8_t g = 0; g < NODE_GPIO_COUNT; g++ )
    {
        if( ( oldPins & ( 1U << g ) ) && !( newPins & ( 1U << g ) ) )
        {
            NodeIoApplyPin( g );
        }
    }
    for( uint8_t ch = 0; ch < count; ch++ )
    {
        NodeIoPresetLevel( NodeCfg.relayPin[ch], LevelOf( ch, ( Map >> ch ) & 1 ) );
        NodeIoApplyPin( NodeCfg.relayPin[ch] );
    }
    PersistPending = true;
    Report = true;
    return true;
}

void NodeRelayRequestReport( void )
{
    Report = true;
}

bool NodeRelayTakeReport( void )
{
    bool r = Report;
    Report = false;
    return r && ( NodeCfg.relayCount > 0 );
}

bool NodeRelayHasEvent( void )
{
    return PulseDue || PersistPending;
}

void NodeRelayProcess( void )
{
    if( PulseDue )
    {
        PulseDue = false;
        uint32_t now = ( uint32_t )TimerGetCurrentTime( );
        for( uint8_t ch = 0; ch < NodeCfg.relayCount; ch++ )
        {
            if( ( PulseMask & ( 1U << ch ) ) && ( ( int32_t )( now - PulseEnd[ch] ) >= 0 ) )
            {
                PulseMask &= ( uint16_t )~( 1U << ch );
                Apply( ch, ( PulseRestore >> ch ) & 1 );
                printf( "[Relés] Fim do pulso do relé %u: %s.\r\n", ch, ( ( Map >> ch ) & 1 ) ? "ligado" : "desligado" );
                Report = true;              /* telemetria com o TLV de relés */
            }
        }
        ArmTimer( );
    }
    if( PersistPending )
    {
        Persist( );
    }
}

void NodeRelayDescribe( char *out, size_t max )
{
    size_t n = 0;
    if( NodeCfg.relayCount == 0 )
    {
        snprintf( out, max, "0" );
        return;
    }
    out[0] = '\0';
    for( uint8_t i = 0; ( i < NodeCfg.relayCount ) && ( n + 4 < max ); i++ )
    {
        n += ( size_t )snprintf( out + n, max - n, "%s%u", i ? "," : "", NodeCfg.relayPin[i] );
    }
    for( uint8_t i = 0; ( i < NodeCfg.relayCount ) && ( n + 3 < max ); i++ )
    {
        n += ( size_t )snprintf( out + n, max - n, "%c%u", i ? ',' : ':', ( NodeCfg.relayActiveLow >> i ) & 1 );
    }
}
