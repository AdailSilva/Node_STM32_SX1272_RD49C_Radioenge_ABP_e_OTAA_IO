/*
 * node_io.c — GPIOs, ADC, interrupções, keepalive e serial transparente
 * (comportamento do firmware AT original da Radioenge; veja node_io.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "stm32l0xx.h"
#include "utilities.h"
#include "board.h"
#include "board-ext.h"
#include "board-config.h"
#include "gpio.h"
#include "adc.h"
#include "timer.h"
#include "node_config.h"
#include "node_io.h"
#include "node_pzem.h"

extern Adc_t Adc;                      /* board.c */

static const PinNames GpioPin[NODE_GPIO_COUNT] =
{
    MODULE_GPIO0, MODULE_GPIO1, MODULE_GPIO2, MODULE_GPIO3, MODULE_GPIO4,
    MODULE_GPIO5, MODULE_GPIO6, MODULE_GPIO7, MODULE_GPIO8, MODULE_GPIO9,
};

/* canal do ADC de cada GPIO (0xFF = sem entrada analógica) */
static const uint8_t GpioAdcCh[NODE_GPIO_COUNT] = { 3, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 0xFF };

/* GPIOs aceitos como interrupção (veja node_io.h): 2, 4, 5, 6, 9 */
static const uint16_t IrqAllowed = ( 1U << 2 ) | ( 1U << 4 ) | ( 1U << 5 ) | ( 1U << 6 ) | ( 1U << 9 );

static Gpio_t Io[NODE_GPIO_COUNT];
static volatile uint16_t IrqPendingMask = 0;
static uint16_t IrqArmed = 0;           /* GPIOs com interrupção registrada */
static uint16_t OutLevels = 0;          /* nível das saídas (não vai para a EEPROM) */

/* ------------------------------------------------------------------ */
static bool IsInputMode( uint8_t m )   { return ( m == 0 ) || ( m >= 6 && m <= 8 ); }
static bool IsOutputMode( uint8_t m )  { return ( m == 1 ) || ( m == 2 ); }
static bool IsAfMode( uint8_t m )      { return ( m == 3 ) || ( m == 4 ); }

const char* NodeIoModeName( uint8_t mode )
{
    switch( mode )
    {
    case 0: return "entrada";
    case 1: return "saída push-pull";
    case 2: return "saída open-drain";
    case 3: return "serial transparente (push-pull)";
    case 4: return "serial transparente (open-drain)";
    case 5: return "analógica";
    case 6: return "interrupção subida";
    case 7: return "interrupção descida";
    case 8: return "interrupção subida e descida";
    default: return "não configurado";
    }
}

static PinTypes PullOf( uint8_t pull )
{
    return ( pull == 1 ) ? PIN_PULL_UP : ( pull == 2 ) ? PIN_PULL_DOWN : PIN_NO_PULL;
}

static void OnGpioIrq( void *context )
{
    uint8_t g = ( uint8_t )( uintptr_t )context;
    IrqPendingMask |= ( uint16_t )( 1U << g );
    BoardConsoleRxActivity( );          /* fica acordado um instante para enviar o uplink */
}

/* ------------------------------------------------- serial transparente */
#if ( CONSOLE_UART != 2 )
#define TRANSP_RX_SIZE      128
static uint8_t TranspRx[TRANSP_RX_SIZE];
static volatile uint16_t TranspHead = 0, TranspTail = 0;
static volatile uint32_t TranspLastRx = 0;
static bool TranspOn = false;
static uint32_t TranspBaud = 115200;

static void TranspStart( uint8_t mode )
{
    PinConfigs cfg = ( mode == 4 ) ? PIN_OPEN_DRAIN : PIN_PUSH_PULL;
    __HAL_RCC_USART2_FORCE_RESET( );
    __HAL_RCC_USART2_RELEASE_RESET( );
    __HAL_RCC_USART2_CONFIG( RCC_USART2CLKSOURCE_HSI );
    __HAL_RCC_USART2_CLK_ENABLE( );
    /* GPIO1 = PA2 = TX, GPIO0 = PA3 = RX */
    GpioInit( &Io[1], MODULE_GPIO1, PIN_ALTERNATE_FCT, cfg, PullOf( NodeCfg.gpioPull[1] ), GPIO_AF4_USART2 );
    GpioInit( &Io[0], MODULE_GPIO0, PIN_ALTERNATE_FCT, cfg, PIN_PULL_UP, GPIO_AF4_USART2 );
    USART2->CR1 = 0;
    USART2->CR2 = 0;
    USART2->CR3 = USART_CR3_WUS_1;     /* acorda do STOP pelo start bit */
    USART2->BRR = ( 16000000UL + TranspBaud / 2 ) / TranspBaud;
    USART2->CR1 = USART_CR1_UE | USART_CR1_UESM | USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE;
    EXTI->IMR |= ( 1UL << 26 );
    HAL_NVIC_SetPriority( USART2_IRQn, 1, 0 );
    HAL_NVIC_EnableIRQ( USART2_IRQn );
    TranspHead = TranspTail = 0;
    TranspOn = true;
}

static void TranspStop( void )
{
    if( !TranspOn )
    {
        return;
    }
    HAL_NVIC_DisableIRQ( USART2_IRQn );
    USART2->CR1 = 0;
    __HAL_RCC_USART2_CLK_DISABLE( );
    TranspOn = false;
}

void USART2_IRQHandler( void )
{
    if( !TranspOn )
    {
        NodePzemUsartIrq( );                /* GPIO0/GPIO1 no modo PZEM (Modbus RTU) */
        return;
    }
    uint32_t isr = USART2->ISR;
    if( isr & ( USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE ) )
    {
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
    }
    if( isr & USART_ISR_WUF )
    {
        USART2->ICR = USART_ICR_WUCF;
        BoardConsoleRxActivity( );
    }
    if( isr & USART_ISR_RXNE )
    {
        uint8_t c = ( uint8_t )USART2->RDR;
        uint16_t next = ( uint16_t )( ( TranspHead + 1 ) % TRANSP_RX_SIZE );
        if( next != TranspTail )
        {
            TranspRx[TranspHead] = c;
            TranspHead = next;
        }
        TranspLastRx = ( uint32_t )TimerGetCurrentTime( );
        BoardConsoleRxActivity( );      /* fora do STOP enquanto chegam bytes */
    }
}

bool NodeIoTransparentActive( void )   { return TranspOn; }
bool NodeIoTransparentPending( void )  { return TranspOn && ( TranspHead != TranspTail ); }

bool NodeIoTransparentReady( void )
{
    if( !NodeIoTransparentPending( ) )
    {
        return false;
    }
    uint16_t n = ( uint16_t )( ( TranspHead + TRANSP_RX_SIZE - TranspTail ) % TRANSP_RX_SIZE );
    return ( n >= NODE_IO_TRANSP_MAX ) || ( TimerGetElapsedTime( TranspLastRx ) >= 50 );
}

uint8_t NodeIoTransparentTake( uint8_t *buf, uint8_t max )
{
    uint8_t n = 0;
    while( ( n < max ) && ( TranspTail != TranspHead ) )
    {
        buf[n++] = TranspRx[TranspTail];
        TranspTail = ( uint16_t )( ( TranspTail + 1 ) % TRANSP_RX_SIZE );
    }
    return n;
}

void NodeIoTransparentWrite( const uint8_t *buf, uint16_t len )
{
    if( !TranspOn )
    {
        return;
    }
    for( uint16_t i = 0; i < len; i++ )
    {
        uint32_t guard = 0;
        while( !( USART2->ISR & USART_ISR_TXE ) && ( ++guard < 200000UL ) )
        {
        }
        USART2->TDR = buf[i];
    }
}

void NodeIoTransparentSetBaudrate( uint32_t baud )
{
    TranspBaud = baud;
    if( TranspOn )
    {
        USART2->CR1 &= ~USART_CR1_UE;
        USART2->BRR = ( 16000000UL + baud / 2 ) / baud;
        USART2->CR1 |= USART_CR1_UE;
    }
}
#else
/* CONSOLE_UART = 2: o USART2 é o console; não há serial transparente */
static void TranspStart( uint8_t mode ) { ( void )mode; }
static void TranspStop( void ) { }
bool NodeIoTransparentActive( void ) { return false; }
bool NodeIoTransparentPending( void ) { return false; }
bool NodeIoTransparentReady( void ) { return false; }
uint8_t NodeIoTransparentTake( uint8_t *buf, uint8_t max ) { ( void )buf; ( void )max; return 0; }
void NodeIoTransparentWrite( const uint8_t *buf, uint16_t len ) { ( void )buf; ( void )len; }
void NodeIoTransparentSetBaudrate( uint32_t baud ) { ( void )baud; }
#endif

/* ------------------------------------------------------------ pinos */
static void ApplyPin( uint8_t g )
{
    uint8_t mode = NodeCfg.gpioMode[g];
    PinTypes pull = PullOf( NodeCfg.gpioPull[g] );

    if( IrqArmed & ( 1U << g ) )
    {
        uint32_t line = 1UL << ( GpioPin[g] & 0x0F );
        GpioRemoveInterrupt( &Io[g] );
        EXTI->IMR &= ~line;
        EXTI->RTSR &= ~line;
        EXTI->FTSR &= ~line;
        IrqArmed &= ( uint16_t )~( 1U << g );
    }
    IrqPendingMask &= ( uint16_t )~( 1U << g );

    switch( mode )
    {
    case 0:
        GpioInit( &Io[g], GpioPin[g], PIN_INPUT, PIN_PUSH_PULL, pull, 0 );
        break;
    case 1:
    case 2:
        GpioInit( &Io[g], GpioPin[g], PIN_OUTPUT, ( mode == 2 ) ? PIN_OPEN_DRAIN : PIN_PUSH_PULL, pull,
                  ( OutLevels >> g ) & 1 );
        break;
    case 5:
        GpioInit( &Io[g], GpioPin[g], PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
        break;
    case 6:
    case 7:
    case 8:
        GpioInit( &Io[g], GpioPin[g], PIN_INPUT, PIN_PUSH_PULL, pull, 0 );
        GpioSetContext( &Io[g], ( void* )( uintptr_t )g );
        GpioSetInterrupt( &Io[g], ( mode == 6 ) ? IRQ_RISING_EDGE : ( mode == 7 ) ? IRQ_FALLING_EDGE : IRQ_RISING_FALLING_EDGE,
                          IRQ_LOW_PRIORITY, OnGpioIrq );
        IrqArmed |= ( uint16_t )( 1U << g );
        break;
    case 3:
    case 4:
        break;                              /* tratado em ApplyTransparent( ) */
    default:
        /* não configurado: analógico = alta impedância e menor consumo */
        GpioInit( &Io[g], GpioPin[g], PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
        break;
    }
}

static void ApplyTransparent( void )
{
    uint8_t m0 = NodeCfg.gpioMode[0], m1 = NodeCfg.gpioMode[1];
    if( IsAfMode( m0 ) || IsAfMode( m1 ) )
    {
        TranspStart( IsAfMode( m1 ) ? m1 : m0 );
    }
    else
    {
        TranspStop( );
    }
}

void NodeIoInit( void )
{
#if ( CONSOLE_UART == 2 )
    /* GPIO0/GPIO1 são o console */
    for( uint8_t g = 2; g < NODE_GPIO_COUNT; g++ )
#else
    NodeIoTransparentSetBaudrate( NodeBaudrates[NodeCfg.baudIdx & 3] );
    for( uint8_t g = 0; g < NODE_GPIO_COUNT; g++ )
#endif
    {
        if( ( g <= 1 ) && NodeCfg.pzemCount )
        {
            continue;                       /* GPIO0/GPIO1 são do PZEM (NodePzemInit) */
        }
        if( !IsAfMode( NodeCfg.gpioMode[g] ) )
        {
            ApplyPin( g );
        }
    }
#if ( CONSOLE_UART != 2 )
    ApplyTransparent( );
#endif
}

void NodeIoApplyPin( uint8_t g )
{
    if( g < NODE_GPIO_COUNT )
    {
        ApplyPin( g );
    }
}

void NodeIoPresetLevel( uint8_t g, uint8_t level )
{
    if( g >= NODE_GPIO_COUNT )
    {
        return;
    }
    if( level )
    {
        OutLevels |= ( uint16_t )( 1U << g );
    }
    else
    {
        OutLevels &= ( uint16_t )~( 1U << g );
    }
}

void NodeIoRelayDrive( uint8_t g, uint8_t level )
{
    if( ( g >= NODE_GPIO_COUNT ) || !IsOutputMode( NodeCfg.gpioMode[g] ) )
    {
        return;
    }
    NodeIoPresetLevel( g, level );
    GpioWrite( &Io[g], level );
}

void NodeIoReleaseSerialPins( void )
{
#if ( CONSOLE_UART != 2 )
    bool af = IsAfMode( NodeCfg.gpioMode[0] ) || IsAfMode( NodeCfg.gpioMode[1] );
    for( uint8_t g = 0; g <= 1; g++ )
    {
        NodeCfg.gpioMode[g] = NODE_GPIO_UNUSED;
        NodeCfg.gpioPull[g] = 0;
        if( !af )
        {
            ApplyPin( g );                  /* tira interrupção/saída antes do PZEM assumir */
        }
    }
    if( af )
    {
        TranspStop( );
    }
#endif
}

NodeIoStatus_t NodeIoConfigure( uint8_t g, uint8_t mode, uint8_t pull )
{
    if( ( g >= NODE_GPIO_COUNT ) || ( mode > 8 ) || ( pull > 2 ) )
    {
        return NODE_IO_PARAM_ERROR;
    }
#if ( CONSOLE_UART == 2 )
    if( g <= 1 )
    {
        return NODE_IO_PARAM_ERROR;         /* console nesses pinos */
    }
#endif
    if( NodeConfigRelayPins( &NodeCfg ) & ( 1U << g ) )
    {
        return NODE_IO_ERROR;               /* pino de relé: AT+RELECFG manda nele */
    }
    if( IsAfMode( mode ) && ( g > 1 ) )
    {
        return NODE_IO_PARAM_ERROR;
    }
    if( ( mode == 5 ) && ( GpioAdcCh[g] == 0xFF ) )
    {
        return NODE_IO_PARAM_ERROR;
    }
    if( ( mode >= 6 ) && !( IrqAllowed & ( 1U << g ) ) )
    {
        return NODE_IO_PARAM_ERROR;
    }
    if( mode >= 6 )
    {
        /* PA15 (GPIO2) e PB15 (GPIO6) dividem a linha EXTI15 */
        uint8_t other = ( g == 2 ) ? 6 : ( g == 6 ) ? 2 : 0xFF;
        if( ( other != 0xFF ) && ( NodeCfg.gpioMode[other] >= 6 ) && ( NodeCfg.gpioMode[other] <= 8 ) )
        {
            return NODE_IO_PARAM_ERROR;
        }
    }

    if( IsAfMode( mode ) && ( NodeConfigRelayPins( &NodeCfg ) & 0x3 ) )
    {
        return NODE_IO_ERROR;               /* a serial usa os dois pinos e um deles é relé */
    }
    if( ( g <= 1 ) && NodeCfg.pzemCount )
    {
        /* GPIO0/GPIO1 configurados de novo: a configuração mais recente vence e o
         * modo PZEM é desligado (AT+PZEMCFG liga de novo) */
        NodePzemDisable( );
        if( !IsAfMode( mode ) )
        {
            ApplyPin( g ^ 1 );              /* o outro pino volta ao modo gravado */
        }
    }
    bool wasAf = IsAfMode( NodeCfg.gpioMode[g] );
    NodeCfg.gpioMode[g] = mode;
    NodeCfg.gpioPull[g] = pull;
    if( IsAfMode( mode ) )
    {
        /* a serial usa os dois pinos */
        NodeCfg.gpioMode[g ^ 1] = mode;
        ApplyTransparent( );
    }
    else
    {
        if( wasAf )
        {
            NodeCfg.gpioMode[g ^ 1] = NODE_GPIO_UNUSED;
            ApplyTransparent( );
            ApplyPin( g ^ 1 );
        }
        ApplyPin( g );
    }
    return NODE_IO_OK;
}

NodeIoStatus_t NodeIoWrite( uint8_t g, uint8_t level )
{
    if( ( g >= NODE_GPIO_COUNT ) || ( level > 1 ) )
    {
        return NODE_IO_PARAM_ERROR;
    }
    if( !IsOutputMode( NodeCfg.gpioMode[g] ) || ( NodeConfigRelayPins( &NodeCfg ) & ( 1U << g ) ) )
    {
        return NODE_IO_ERROR;               /* relé: AT+RELE (o estado fica gravado) */
    }
    if( level )
    {
        OutLevels |= ( uint16_t )( 1U << g );
    }
    else
    {
        OutLevels &= ( uint16_t )~( 1U << g );
    }
    GpioWrite( &Io[g], level );
    return NODE_IO_OK;
}

NodeIoStatus_t NodeIoReadAdc( uint8_t g, uint16_t *value )
{
    if( g >= NODE_GPIO_COUNT )
    {
        return NODE_IO_PARAM_ERROR;
    }
    if( ( NodeCfg.gpioMode[g] != 5 ) || ( GpioAdcCh[g] == 0xFF ) )
    {
        return NODE_IO_ERROR;
    }
    static const uint32_t ch[4] = { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3 };
    *value = AdcReadChannel( &Adc, ch[GpioAdcCh[g]] );
    return NODE_IO_OK;
}

NodeIoStatus_t NodeIoRead( uint8_t g, uint16_t *value )
{
    if( g >= NODE_GPIO_COUNT )
    {
        return NODE_IO_PARAM_ERROR;
    }
    uint8_t m = NodeCfg.gpioMode[g];
    if( m == 5 )
    {
        return NodeIoReadAdc( g, value );
    }
    if( IsInputMode( m ) )
    {
        *value = ( uint16_t )GpioRead( &Io[g] );
        return NODE_IO_OK;
    }
    if( IsOutputMode( m ) )
    {
        *value = ( uint16_t )( ( OutLevels >> g ) & 1 );
        return NODE_IO_OK;
    }
    return NODE_IO_ERROR;
}

uint16_t NodeIoInputsBitmap( void )
{
    uint16_t b = 0;
    for( uint8_t g = 0; g < NODE_GPIO_COUNT; g++ )
    {
        if( IsInputMode( NodeCfg.gpioMode[g] ) && GpioRead( &Io[g] ) )
        {
            b |= ( uint16_t )( 1U << g );
        }
    }
    return b;
}

uint8_t NodeIoBatteryLevel( void )
{
    BoardGetBatteryLevel( );                    /* mede o VDD de novo */
    uint32_t mv = BoardGetBatteryVoltage( );
    if( mv == 0 )
    {
        return 255;
    }
    if( mv <= 1800 )
    {
        return 1;
    }
    if( mv >= 3600 )
    {
        return 254;
    }
    return ( uint8_t )( 1 + ( ( mv - 1800 ) * 253UL ) / 1800UL );
}

/* Tabela 9 do manual: keepalive */
uint8_t NodeIoBuildKeepalive( uint8_t *b, int8_t snr, int16_t rssi )
{
    uint16_t in = NodeIoInputsBitmap( );
    static const uint8_t adGpio[4] = { 0, 1, 7, 8 };   /* AD0..AD3 */

    b[0] = NodeIoBatteryLevel( );
    b[1] = 0x00;                                /* reservado */
    b[2] = 0xFF;
    b[3] = ( uint8_t )( in >> 8 );
    b[4] = ( uint8_t )in;
    for( uint8_t i = 0; i < 4; i++ )
    {
        uint16_t v = 0;
        NodeIoReadAdc( adGpio[i], &v );         /* 0 se o pino não for analógico */
        b[5 + 2 * i] = ( uint8_t )( v >> 8 );
        b[6 + 2 * i] = ( uint8_t )v;
    }
    b[13] = ( uint8_t )snr;
    b[14] = ( uint8_t )( ( uint16_t )rssi >> 8 );
    b[15] = ( uint8_t )rssi;
    return NODE_IO_KEEPALIVE_LEN;
}

/* Tabela 13 do manual: interrupção (com o byte LSB dos pinos, que o manual omite) */
uint8_t NodeIoBuildIrqPacket( uint8_t *b, uint8_t gpio, int8_t snr, int16_t rssi )
{
    uint16_t in = NodeIoInputsBitmap( );
    b[0] = NodeIoBatteryLevel( );
    b[1] = 0x00;
    b[2] = gpio;
    b[3] = ( uint8_t )( in >> 8 );
    b[4] = ( uint8_t )in;
    b[5] = ( uint8_t )snr;
    b[6] = ( uint8_t )( ( uint16_t )rssi >> 8 );
    b[7] = ( uint8_t )rssi;
    return NODE_IO_IRQ_LEN;
}

void NodeIoRequeueIrq( uint8_t gpio )
{
    if( gpio < NODE_GPIO_COUNT )
    {
        IrqPendingMask |= ( uint16_t )( 1U << gpio );
    }
}

bool NodeIoIrqPending( void )
{
    return IrqPendingMask != 0;
}

bool NodeIoTakeIrq( uint8_t *gpio )
{
    bool found = false;
    CRITICAL_SECTION_BEGIN( );
    for( uint8_t g = 0; g < NODE_GPIO_COUNT; g++ )
    {
        if( IrqPendingMask & ( 1U << g ) )
        {
            IrqPendingMask &= ( uint16_t )~( 1U << g );
            *gpio = g;
            found = true;
            break;
        }
    }
    CRITICAL_SECTION_END( );
    return found;
}

void NodeIoDescribe( char *out, uint16_t max )
{
    uint16_t n = 0;
    out[0] = '\0';
    for( uint8_t g = 0; ( g < NODE_GPIO_COUNT ) && ( n + 6 < max ); g++ )
    {
        uint8_t m = NodeCfg.gpioMode[g];
        if( m == NODE_GPIO_UNUSED )
        {
            n += ( uint16_t )snprintf( out + n, max - n, "%s-", g ? "," : "" );
        }
        else
        {
            n += ( uint16_t )snprintf( out + n, max - n, "%s%u:%u", g ? "," : "", m, NodeCfg.gpioPull[g] );
        }
    }
}
