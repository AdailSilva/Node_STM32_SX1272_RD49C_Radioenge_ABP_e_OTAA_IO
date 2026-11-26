/*!
 * \file      sx1272-board.c
 *
 * \brief     Target board SX1272MB2DAS shield driver implementation
 *
 * \copyright Revised BSD License, see section \ref LICENSE.
 *
 * \code
 *                ______                              _
 *               / _____)             _              | |
 *              ( (____  _____ ____ _| |_ _____  ____| |__
 *               \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 *               _____) ) ____| | | || |_| ____( (___| | | |
 *              (______/|_____)_|_|_| \__)_____)\____)_| |_|
 *              (C)2013-2017 Semtech
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 */
#include <stdlib.h>
#include <string.h>
#include "utilities.h"
#include "board-config.h"
#include "delay.h"
#include "radio.h"
#include "sx1272-board.h"
#include "board-ext.h"

extern SX1272_t SX1272;

/*!
 * RADIOENGE: frequência e potência da última transmissão (uplink ou
 * Join-request), gravadas no instante em que o rádio entra em TX. As janelas
 * RX1/RX2 mudam o canal do rádio depois, por isso o valor é guardado aqui.
 */
static volatile uint32_t LastTxFreq = 0;
static volatile int8_t LastTxPower = 0;
static volatile int8_t AppliedTxPower = 0;   /* potência que o SX1272 usa de fato (depois dos limites) */

uint32_t BoardGetLastTxFreq( void )
{
    return LastTxFreq;
}

int8_t BoardGetLastTxPower( void )
{
    return LastTxPower;
}

/*!
 * RADIOENGE: diagnóstico de sessão ("Frame raw").
 * A LoRaMac-node não expõe o quadro cifrado que vai ao ar, então o Send( ) e o
 * RxDone( ) do rádio passam por aqui: só copiam o quadro (nada de printf no
 * tempo crítico da MAC) e a aplicação imprime depois, no laço principal.
 */
static uint8_t RawTxBuf[256];
static volatile uint8_t RawTxLen = 0;
static volatile bool RawTxReady = false;
static uint8_t RawRxBuf[256];
static volatile uint8_t RawRxLen = 0;
static volatile bool RawRxReady = false;
static RadioEvents_t *MacRadioEvents = NULL;   /* eventos da LoRaMac-node */
static RadioEvents_t HookRadioEvents;          /* cópia com o RxDone interceptado */

static void HookRadioSend( uint8_t *buffer, uint8_t size )
{
    memcpy( RawTxBuf, buffer, size );
    RawTxLen = size;
    RawTxReady = true;
    SX1272Send( buffer, size );
}

/*!
 * RADIOENGE: frequência, DR, RSSI e SNR do último quadro recebido (downlink ou
 * Join-accept). O rádio ainda está no canal da janela quando o RxDone chega.
 */
static volatile uint32_t LastRxFreq = 0;
static volatile uint8_t LastRxDr = 0;
static volatile int16_t LastRxRssi = 0;
static volatile int8_t LastRxSnr = 0;

void BoardGetLastRx( uint32_t *freq, uint8_t *dr, int16_t *rssi, int8_t *snr )
{
    CRITICAL_SECTION_BEGIN( );
    if( freq != NULL ) *freq = LastRxFreq;
    if( dr != NULL )   *dr = LastRxDr;
    if( rssi != NULL ) *rssi = LastRxRssi;
    if( snr != NULL )  *snr = LastRxSnr;
    CRITICAL_SECTION_END( );
}

static void HookRadioRxDone( uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr )
{
    /* AU915: 125 kHz SF12..SF7 = DR0..DR5, 500 kHz SF12..SF7 = DR8..DR13
     * (SX1272: Bandwidth 0 = 125, 1 = 250, 2 = 500 kHz; Datarate = SF) */
    uint32_t sf = SX1272.Settings.LoRa.Datarate;
    LastRxFreq = SX1272.Settings.Channel;
    LastRxDr = ( sf >= 7 && sf <= 12 ) ? ( uint8_t )( ( SX1272.Settings.LoRa.Bandwidth == 2 ) ? ( 20 - sf ) : ( 12 - sf ) ) : 0;
    LastRxRssi = rssi;
    LastRxSnr = snr;
    uint16_t n = ( size > sizeof( RawRxBuf ) ) ? sizeof( RawRxBuf ) : size;
    memcpy( RawRxBuf, payload, n );
    RawRxLen = ( uint8_t )n;
    RawRxReady = true;
    if( ( MacRadioEvents != NULL ) && ( MacRadioEvents->RxDone != NULL ) )
    {
        MacRadioEvents->RxDone( payload, size, rssi, snr );
    }
}

static void HookRadioInit( RadioEvents_t *events )
{
    if( events == NULL )
    {
        SX1272Init( events );
        return;
    }
    MacRadioEvents = events;
    HookRadioEvents = *events;
    HookRadioEvents.RxDone = ( events->RxDone != NULL ) ? HookRadioRxDone : NULL;
    SX1272Init( &HookRadioEvents );
}

static bool TakeFrame( uint8_t *src, volatile uint8_t *len, volatile bool *ready, uint8_t *out, uint8_t *outLen )
{
    bool ok = false;
    CRITICAL_SECTION_BEGIN( );
    if( *ready )
    {
        memcpy( out, src, *len );
        *outLen = *len;
        *ready = false;
        ok = true;
    }
    CRITICAL_SECTION_END( );
    return ok;
}

bool BoardRadioTakeTxFrame( uint8_t *buf, uint8_t *len )
{
    return TakeFrame( RawTxBuf, &RawTxLen, &RawTxReady, buf, len );
}

bool BoardRadioTakeRxFrame( uint8_t *buf, uint8_t *len )
{
    return TakeFrame( RawRxBuf, &RawRxLen, &RawRxReady, buf, len );
}

/*!
 * \brief Gets the board PA selection configuration
 *
 * \param [IN] channel Channel frequency in Hz
 * \retval PaSelect RegPaConfig PaSelect value
 */
static uint8_t SX1272GetPaSelect( uint32_t channel );

/*!
 * Flag used to set the RF switch control pins in low power mode when the radio is not active.
 */
static bool RadioIsActive = false;

/*!
 * Radio driver structure initialization
 */
const struct Radio_s Radio =
{
    HookRadioInit,          /* RADIOENGE: intercepta o RxDone (Frame raw do RX) */
    SX1272GetStatus,
    SX1272SetModem,
    SX1272SetChannel,
    SX1272IsChannelFree,
    SX1272Random,
    SX1272SetRxConfig,
    SX1272SetTxConfig,
    SX1272CheckRfFrequency,
    SX1272GetTimeOnAir,
    HookRadioSend,          /* RADIOENGE: copia o quadro enviado (Frame raw / MIC) */
    SX1272SetSleep,
    SX1272SetStby,
    SX1272SetRx,
    SX1272StartCad,
    SX1272SetTxContinuousWave,
    SX1272ReadRssi,
    SX1272Write,
    SX1272Read,
    SX1272WriteBuffer,
    SX1272ReadBuffer,
    SX1272SetMaxPayloadLength,
    SX1272SetPublicNetwork,
    SX1272GetWakeupTime,
    NULL, // void ( *IrqProcess )( void )
    NULL, // void ( *RxBoosted )( uint32_t timeout ) - SX126x Only
    NULL, // void ( *SetRxDutyCycle )( uint32_t rxTime, uint32_t sleepTime ) - SX126x Only
};

/*!
 * Antenna switch GPIO pins objects
 */
Gpio_t AntSwitch;
Gpio_t AntSwitch2;

/*!
 * Debug GPIO pins objects
 */
#if defined( USE_RADIO_DEBUG )
Gpio_t DbgPinTx;
Gpio_t DbgPinRx;
#endif

void SX1272IoInit( void )
{
    GpioInit( &SX1272.Spi.Nss, RADIO_NSS, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1 );

    GpioInit( &SX1272.DIO0, RADIO_DIO_0, PIN_INPUT, PIN_PUSH_PULL, PIN_PULL_UP, 0 );
    GpioInit( &SX1272.DIO1, RADIO_DIO_1, PIN_INPUT, PIN_PUSH_PULL, PIN_PULL_UP, 0 );
    GpioInit( &SX1272.DIO2, RADIO_DIO_2, PIN_INPUT, PIN_PUSH_PULL, PIN_PULL_UP, 0 );
    GpioInit( &SX1272.DIO3, RADIO_DIO_3, PIN_INPUT, PIN_PUSH_PULL, PIN_PULL_UP, 0 );
    // DIO4 and DIO5 aren't connected.
    // Initialize Gpio_t port to NULL.
    SX1272.DIO4.port = NULL;
    SX1272.DIO5.port = NULL;
}

void SX1272IoIrqInit( DioIrqHandler **irqHandlers )
{
    GpioSetInterrupt( &SX1272.DIO0, IRQ_RISING_EDGE, IRQ_HIGH_PRIORITY, irqHandlers[0] );
    GpioSetInterrupt( &SX1272.DIO1, IRQ_RISING_FALLING_EDGE, IRQ_HIGH_PRIORITY, irqHandlers[1] );
    GpioSetInterrupt( &SX1272.DIO2, IRQ_RISING_EDGE, IRQ_HIGH_PRIORITY, irqHandlers[2] );
    GpioSetInterrupt( &SX1272.DIO3, IRQ_RISING_EDGE, IRQ_HIGH_PRIORITY, irqHandlers[3] );
}

void SX1272IoDeInit( void )
{
    GpioInit( &SX1272.Spi.Nss, RADIO_NSS, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1 );

    GpioInit( &SX1272.DIO0, RADIO_DIO_0, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &SX1272.DIO1, RADIO_DIO_1, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &SX1272.DIO2, RADIO_DIO_2, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &SX1272.DIO3, RADIO_DIO_3, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
}

void SX1272IoDbgInit( void )
{
#if defined( USE_RADIO_DEBUG )
    GpioInit( &DbgPinTx, RADIO_DBG_PIN_TX, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &DbgPinRx, RADIO_DBG_PIN_RX, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
#endif
}

void SX1272IoTcxoInit( void )
{
    // No TCXO component available on this board design.
}

void SX1272SetBoardTcxo( uint8_t state )
{
    // No TCXO component available on this board design.
#if 0
    if( state == true )
    {
        TCXO_ON( );
        DelayMs( BOARD_TCXO_WAKEUP_TIME );
    }
    else
    {
        TCXO_OFF( );
    }
#endif
}

uint32_t SX1272GetBoardTcxoWakeupTime( void )
{
    return BOARD_TCXO_WAKEUP_TIME;
}

void SX1272Reset( void )
{
    // Enables the TCXO if available on the board design
    SX1272SetBoardTcxo( true );

    // Set RESET pin to 1
    GpioInit( &SX1272.Reset, RADIO_RESET, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1 );

    // Wait 1 ms
    DelayMs( 1 );

    // Configure RESET as input
    GpioInit( &SX1272.Reset, RADIO_RESET, PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1 );

    // Wait 6 ms
    DelayMs( 6 );
}

void SX1272SetRfTxPower( int8_t power )
{
    uint8_t paConfig = 0;
    uint8_t paDac = 0;

    paConfig = SX1272Read( REG_PACONFIG );
    paDac = SX1272Read( REG_PADAC );

    paConfig = ( paConfig & RF_PACONFIG_PASELECT_MASK ) | SX1272GetPaSelect( SX1272.Settings.Channel );

    if( ( paConfig & RF_PACONFIG_PASELECT_PABOOST ) == RF_PACONFIG_PASELECT_PABOOST )
    {
        if( power > 17 )
        {
            paDac = ( paDac & RF_PADAC_20DBM_MASK ) | RF_PADAC_20DBM_ON;
        }
        else
        {
            paDac = ( paDac & RF_PADAC_20DBM_MASK ) | RF_PADAC_20DBM_OFF;
        }
        if( ( paDac & RF_PADAC_20DBM_ON ) == RF_PADAC_20DBM_ON )
        {
            if( power < 5 )
            {
                power = 5;
            }
            if( power > 20 )
            {
                power = 20;
            }
            paConfig = ( paConfig & RFLR_PACONFIG_OUTPUTPOWER_MASK ) | ( uint8_t )( ( uint16_t )( power - 5 ) & 0x0F );
        }
        else
        {
            if( power < 2 )
            {
                power = 2;
            }
            if( power > 17 )
            {
                power = 17;
            }
            paConfig = ( paConfig & RFLR_PACONFIG_OUTPUTPOWER_MASK ) | ( uint8_t )( ( uint16_t )( power - 2 ) & 0x0F );
        }
    }
    else
    {
        if( power < -1 )
        {
            power = -1;
        }
        if( power > 14 )
        {
            power = 14;
        }
        paConfig = ( paConfig & RFLR_PACONFIG_OUTPUTPOWER_MASK ) | ( uint8_t )( ( uint16_t )( power + 1 ) & 0x0F );
    }
    SX1272Write( REG_PACONFIG, paConfig );
    SX1272Write( REG_PADAC, paDac );
    AppliedTxPower = power;
    /* RADIOENGE: +20 dBm (PA_DAC 20 dBm) consome ~125 mA: sobe o limite do OCP para
     * 130 mA (trim 16). Nos demais casos, o padrão de 100 mA (trim 11). */
    SX1272Write( REG_OCP, ( ( paDac & RF_PADAC_20DBM_ON ) == RF_PADAC_20DBM_ON ) ? ( RF_OCP_ON | 0x10 ) : ( RF_OCP_ON | 0x0B ) );
}

static uint8_t SX1272GetPaSelect( uint32_t channel )
{
    /* RADIOENGE: a saída de RF do módulo (até +20 dBm) sai pelo PA_BOOST.
     * É o mesmo caminho que a LMiC usa no módulo (política 20 dBm / PA_BOOST). */
    return RF_PACONFIG_PASELECT_PABOOST;
}

void SX1272SetAntSwLowPower( bool status )
{
    if( RadioIsActive != status )
    {
        RadioIsActive = status;

        if( status == false )
        {
            SX1272AntSwInit( );
        }
        else
        {
            SX1272AntSwDeInit( );
        }
    }
}

/*
 * RADIOENGE: chave de antena com duas linhas, como no pinmap da LMiC que
 * funciona no módulo (.rxtx = PB13, .rxtx_rx_active = 1; PB12 em 0):
 *   PB13 (RXTX)  = 1 em recepção, 0 em transmissão
 *   PB12 (RXTX2) = sempre 0
 */
void SX1272AntSwInit( void )
{
    GpioInit( &AntSwitch, RADIO_ANT_SWITCH, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, RADIO_ANT_SWITCH_RX_LEVEL );
    GpioInit( &AntSwitch2, RADIO_ANT_SWITCH_2, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
}

void SX1272AntSwDeInit( void )
{
    GpioInit( &AntSwitch, RADIO_ANT_SWITCH, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &AntSwitch2, RADIO_ANT_SWITCH_2, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
}

void SX1272SetAntSw( uint8_t opMode )
{
    switch( opMode )
    {
    case RFLR_OPMODE_TRANSMITTER:
        GpioWrite( &AntSwitch, !RADIO_ANT_SWITCH_RX_LEVEL );
        LastTxFreq = SX1272.Settings.Channel;
        LastTxPower = AppliedTxPower;   /* a MAC pode pedir mais que o SX1272 entrega (+20 dBm no PA_BOOST) */
        break;
    case RFLR_OPMODE_RECEIVER:
    case RFLR_OPMODE_RECEIVER_SINGLE:
    case RFLR_OPMODE_CAD:
    default:
        GpioWrite( &AntSwitch, RADIO_ANT_SWITCH_RX_LEVEL );
        break;
    }
    GpioWrite( &AntSwitch2, 0 );
}

bool SX1272CheckRfFrequency( uint32_t frequency )
{
    /* RADIOENGE: módulo homologado para 915–928 MHz (AU915). O SX1272 cobre
     * 860–1020 MHz; aceita a faixa de 902–928 MHz (AU915 e US915). */
    return ( frequency >= 902000000UL ) && ( frequency <= 928000000UL );
}

uint32_t SX1272GetDio1PinState( void )
{
    return GpioRead( &SX1272.DIO1 );
}

#if defined( USE_RADIO_DEBUG )
void SX1272DbgPinTxWrite( uint8_t state )
{
    GpioWrite( &DbgPinTx, state );
}

void SX1272DbgPinRxWrite( uint8_t state )
{
    GpioWrite( &DbgPinRx, state );
}
#endif
