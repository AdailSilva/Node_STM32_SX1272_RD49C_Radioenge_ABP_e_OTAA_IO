/*!
 * \file      board.c
 *
 * \brief     Funções gerais da placa: módulo LoRaWAN EndDevice da Radioenge
 *            (STM32L071CZT6 + SX1272) com a LoRaMac-node.
 *
 * Baseado em src/boards/NucleoL073/board.c da LoRaMac-node v4.7.1
 * (Miguel Luis e Gregory Cristian, Semtech). Mudanças marcadas com RADIOENGE.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include "stm32l0xx.h"
#include "utilities.h"
#include "gpio.h"
#include "adc.h"
#include "spi.h"
#include "uart.h"
#include "timer.h"
#include "sysIrqHandlers.h"
#include "board-config.h"
#include "board-ext.h"
#include "lpm-board.h"
#include "rtc-board.h"
#include "sx1272-board.h"
#include "board.h"

/*!
 * Unique Devices IDs register set ( STM32L0xxx )
 */
#define ID1                                         ( 0x1FF80050 )
#define ID2                                         ( 0x1FF80054 )
#define ID3                                         ( 0x1FF80064 )

/*
 * RADIOENGE: os três LEDs do módulo (acendem em nível 0)
 */
Gpio_t LedRed;
Gpio_t LedGreen;
Gpio_t LedYellow;

/*
 * MCU objects
 */
Adc_t  Adc;
Uart_t ConsoleUart;

static void BoardUnusedIoInit( void );
static void SystemClockConfig( void );
static void SystemClockReConfig( void );

static bool McuInitialized = false;

#define CONSOLE_FIFO_TX_SIZE                        1024
#define CONSOLE_FIFO_RX_SIZE                        512   /* AT+SENDB com 222 bytes */

static uint8_t ConsoleTxBuffer[CONSOLE_FIFO_TX_SIZE];
static uint8_t ConsoleRxBuffer[CONSOLE_FIFO_RX_SIZE];

/*!
 * RADIOENGE: STOP pode ser proibido pela aplicação (ex.: "lowpower off").
 */
static volatile bool StopAllowed = true;

/*!
 * RADIOENGE: depois de receber um byte no console, o MCU fica fora do STOP por
 * este tempo. Ao acordar do STOP, o relógio (PLL) é reconfigurado com as
 * interrupções desligadas; o USART só guarda 1 byte, então os seguintes de um
 * comando enviado de uma vez (página WEB, app, módulo BLE) se perdiam por
 * overrun. No picocom funcionava porque cada tecla chega separada.
 */
#define BOARD_CONSOLE_AWAKE_MS                      3000
static volatile bool ConsoleRxActive = false;
static volatile TimerTime_t ConsoleRxTime = 0;

void BoardConsoleRxActivity( void )
{
    ConsoleRxTime = TimerGetCurrentTime( );
    ConsoleRxActive = true;
}

void BoardCriticalSectionBegin( uint32_t *mask )
{
    *mask = __get_PRIMASK( );
    __disable_irq( );
}

void BoardCriticalSectionEnd( uint32_t *mask )
{
    __set_PRIMASK( *mask );
}

void BoardInitPeriph( void )
{
}

void BoardInitMcu( void )
{
    if( McuInitialized == false )
    {
        HAL_Init( );

        GpioInit( &LedRed, LED_RED, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, !LED_ON_LEVEL );
        GpioInit( &LedGreen, LED_GREEN, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, !LED_ON_LEVEL );
        GpioInit( &LedYellow, LED_YELLOW, PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, !LED_ON_LEVEL );

        SystemClockConfig( );

        FifoInit( &ConsoleUart.FifoTx, ConsoleTxBuffer, CONSOLE_FIFO_TX_SIZE );
        FifoInit( &ConsoleUart.FifoRx, ConsoleRxBuffer, CONSOLE_FIFO_RX_SIZE );
        UartInit( &ConsoleUart, ( CONSOLE_UART == 2 ) ? UART_2 : UART_1, UART_TX, UART_RX );
        UartConfig( &ConsoleUart, RX_TX, CONSOLE_BAUDRATE, UART_8_BIT, UART_1_STOP_BIT, NO_PARITY, NO_FLOW_CTRL );

        /* RADIOENGE: escolhe e calibra o relógio do RTC (LSE ou LSI) antes de iniciar o RTC */
        RtcClockProbe( );
        RtcInit( );

        BoardUnusedIoInit( );

        /* O STM32L0 não tem um modo "OFF" útil aqui (perderia RAM e RTC de contexto):
         * com o OFF desabilitado, LpmEnterLowPower( ) usa o STOP. */
        LpmSetOffMode( LPM_APPLI_ID, LPM_DISABLE );
    }
    else
    {
        SystemClockReConfig( );
    }

    AdcInit( &Adc, NC );

    SpiInit( &SX1272.Spi, SPI_1, RADIO_MOSI, RADIO_MISO, RADIO_SCLK, NC );
    SX1272IoInit( );

    if( McuInitialized == false )
    {
        McuInitialized = true;
        SX1272IoDbgInit( );
        SX1272IoTcxoInit( );
    }
}

void BoardResetMcu( void )
{
    UartMcuFlush( );
    CRITICAL_SECTION_BEGIN( );
    NVIC_SystemReset( );
}

void BoardDeInitMcu( void )
{
    AdcDeInit( &Adc );
    SpiDeInit( &SX1272.Spi );
    SX1272IoDeInit( );
}

uint32_t BoardGetRandomSeed( void )
{
    return ( ( *( uint32_t* )ID1 ) ^ ( *( uint32_t* )ID2 ) ^ ( *( uint32_t* )ID3 ) );
}

void BoardGetUniqueId( uint8_t *id )
{
    id[7] = ( ( *( uint32_t* )ID1 ) + ( *( uint32_t* )ID3 ) ) >> 24;
    id[6] = ( ( *( uint32_t* )ID1 ) + ( *( uint32_t* )ID3 ) ) >> 16;
    id[5] = ( ( *( uint32_t* )ID1 ) + ( *( uint32_t* )ID3 ) ) >> 8;
    id[4] = ( ( *( uint32_t* )ID1 ) + ( *( uint32_t* )ID3 ) );
    id[3] = ( ( *( uint32_t* )ID2 ) ) >> 24;
    id[2] = ( ( *( uint32_t* )ID2 ) ) >> 16;
    id[1] = ( ( *( uint32_t* )ID2 ) ) >> 8;
    id[0] = ( ( *( uint32_t* )ID2 ) );
}

#define VDDA_VREFINT_CAL                            ( ( uint32_t ) 3000 )  // mV
#define VREFINT_CAL                                 ( *( uint16_t* ) ( ( uint32_t ) 0x1FF80078 ) )
#define TEMP30_CAL_ADDR                             ( *( uint16_t* ) ( ( uint32_t ) 0x1FF8007A ) )
#define TEMP110_CAL_ADDR                            ( *( uint16_t* ) ( ( uint32_t ) 0x1FF8007E ) )
#define VDDA_TEMP_CAL                               ( ( uint32_t ) 3000 )

#define BATTERY_MAX_LEVEL                           3000  // mV
#define BATTERY_MIN_LEVEL                           2400  // mV
#define BATTERY_SHUTDOWN_LEVEL                      2300  // mV

#define BATTERY_LORAWAN_UNKNOWN_LEVEL               255
#define BATTERY_LORAWAN_MAX_LEVEL                   254
#define BATTERY_LORAWAN_EXT_PWR                     0

#define COMPUTE_TEMPERATURE( TS_ADC_DATA, VDDA_APPLI )                                                          \
    ( ( ( ( ( ( ( int32_t )( ( TS_ADC_DATA * VDDA_APPLI ) / VDDA_TEMP_CAL ) - ( int32_t ) TEMP30_CAL_ADDR ) ) * \
            ( int32_t )( 110 - 30 ) )                                                                           \
          << 8 ) /                                                                                              \
        ( int32_t )( TEMP110_CAL_ADDR - TEMP30_CAL_ADDR ) ) +                                                   \
      ( 30 << 8 ) )

static uint16_t BatteryVoltage = BATTERY_MAX_LEVEL;
static bool ExternalPower = true;

void BoardSetExternalPower( bool external )
{
    ExternalPower = external;
}

uint16_t BoardBatteryMeasureVoltage( void )
{
    uint16_t vref = AdcReadChannel( &Adc, ADC_CHANNEL_VREFINT );

    if( vref == 0 )
    {
        return 0;
    }
    return ( ( ( uint32_t ) VDDA_VREFINT_CAL * VREFINT_CAL ) / vref );
}

uint32_t BoardGetBatteryVoltage( void )
{
    return BatteryVoltage;
}

uint8_t BoardGetBatteryLevel( void )
{
    BatteryVoltage = BoardBatteryMeasureVoltage( );

    /* RADIOENGE: com regulador (4–12 V) o VDD do MCU fica fixo em 3,3 V e não
     * diz nada da bateria. Por isso o padrão é "alimentação externa" (0). */
    if( ExternalPower == true )
    {
        return BATTERY_LORAWAN_EXT_PWR;
    }
    if( BatteryVoltage >= BATTERY_MAX_LEVEL )
    {
        return BATTERY_LORAWAN_MAX_LEVEL;
    }
    if( BatteryVoltage > BATTERY_MIN_LEVEL )
    {
        return ( ( 253 * ( BatteryVoltage - BATTERY_MIN_LEVEL ) ) / ( BATTERY_MAX_LEVEL - BATTERY_MIN_LEVEL ) ) + 1;
    }
    if( BatteryVoltage > BATTERY_SHUTDOWN_LEVEL )
    {
        return 1;
    }
    return BATTERY_LORAWAN_UNKNOWN_LEVEL;
}

int16_t BoardGetTemperature( void )
{
    BatteryVoltage = BoardBatteryMeasureVoltage( );
    uint16_t tempRaw = AdcReadChannel( &Adc, ADC_CHANNEL_TEMPSENSOR );

    return ( int16_t ) COMPUTE_TEMPERATURE( tempRaw, BatteryVoltage );
}

static void BoardUnusedIoInit( void )
{
#if defined( USE_DEBUGGER )
    HAL_DBGMCU_EnableDBGSleepMode( );
    HAL_DBGMCU_EnableDBGStopMode( );
    HAL_DBGMCU_EnableDBGStandbyMode( );
#else
    HAL_DBGMCU_DisableDBGSleepMode( );
    HAL_DBGMCU_DisableDBGStopMode( );
    HAL_DBGMCU_DisableDBGStandbyMode( );
#endif
}

/*!
 * RADIOENGE: SYSCLK = HSI16 x 4 / 2 = 32 MHz. O relógio do RTC (LSE ou LSI)
 * é escolhido depois, em RtcClockProbe( ) (rtc-board.c).
 */
static void SystemClockConfig( void )
{
    RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
    RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

    __HAL_RCC_PWR_CLK_ENABLE( );
    __HAL_PWR_VOLTAGESCALING_CONFIG( PWR_REGULATOR_VOLTAGE_SCALE1 );

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLLMUL_4;
    RCC_OscInitStruct.PLL.PLLDIV          = RCC_PLLDIV_2;
    if( HAL_RCC_OscConfig( &RCC_OscInitStruct ) != HAL_OK )
    {
        assert_param( LMN_STATUS_ERROR );
    }

    RCC_ClkInitStruct.ClockType      = ( RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 );
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if( HAL_RCC_ClockConfig( &RCC_ClkInitStruct, FLASH_LATENCY_1 ) != HAL_OK )
    {
        assert_param( LMN_STATUS_ERROR );
    }

    /* Ao sair do STOP o MCU volta pelo HSI16 (a UART do console também usa o HSI16) */
    __HAL_RCC_WAKEUPSTOP_CLK_CONFIG( RCC_STOP_WAKEUPCLOCK_HSI );

    HAL_SYSTICK_Config( HAL_RCC_GetHCLKFreq( ) / 1000 );
    HAL_SYSTICK_CLKSourceConfig( SYSTICK_CLKSOURCE_HCLK );
    HAL_NVIC_SetPriority( SysTick_IRQn, 0, 0 );
}

static void SystemClockReConfig( void )
{
    __HAL_RCC_PWR_CLK_ENABLE( );
    __HAL_PWR_VOLTAGESCALING_CONFIG( PWR_REGULATOR_VOLTAGE_SCALE1 );

    __HAL_RCC_HSI_CONFIG( RCC_HSI_ON );
    while( __HAL_RCC_GET_FLAG( RCC_FLAG_HSIRDY ) == RESET )
    {
    }

    __HAL_RCC_PLL_ENABLE( );
    while( __HAL_RCC_GET_FLAG( RCC_FLAG_PLLRDY ) == RESET )
    {
    }

    __HAL_RCC_SYSCLK_CONFIG( RCC_SYSCLKSOURCE_PLLCLK );
    while( __HAL_RCC_GET_SYSCLK_SOURCE( ) != RCC_SYSCLKSOURCE_STATUS_PLLCLK )
    {
    }
}

void SysTick_Handler( void )
{
    HAL_IncTick( );
    HAL_SYSTICK_IRQHandler( );
}

uint8_t GetBoardPowerSource( void )
{
    return ExternalPower ? USB_POWER : BATTERY_POWER;
}

void BoardSetStopAllowed( bool allowed )
{
    StopAllowed = allowed;
}

void BoardSetConsoleAwakeInStop( bool awake )
{
    /* HSIKERON: o HSI16 continua ligado para o USART no STOP. O console não perde
     * o primeiro caractere, ao custo de ~0,1 mA a mais com o MCU dormindo. */
    if( awake )
    {
        SET_BIT( RCC->CR, RCC_CR_HSIKERON );
    }
    else
    {
        CLEAR_BIT( RCC->CR, RCC_CR_HSIKERON );
    }
}

bool BoardGetStopAllowed( void )
{
    return StopAllowed;
}

void LpmEnterStopMode( void )
{
    CRITICAL_SECTION_BEGIN( );

    BoardDeInitMcu( );

    HAL_PWR_DisablePVD( );
    SET_BIT( PWR->CR, PWR_CR_CWUF );
    HAL_PWREx_EnableUltraLowPower( );
    HAL_PWREx_EnableFastWakeUp( );

    CRITICAL_SECTION_END( );

    HAL_PWR_EnterSTOPMode( PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI );
}

void LpmExitStopMode( void )
{
    CRITICAL_SECTION_BEGIN( );
    BoardInitMcu( );
    CRITICAL_SECTION_END( );
}

void LpmEnterSleepMode( void )
{
    HAL_PWR_EnterSLEEPMode( PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI );
}

void BoardLowPowerHandler( void )
{
    __disable_irq( );

    /* RADIOENGE: com texto saindo pelo console, com o STOP proibido pela
     * aplicação ou logo depois de receber algo no console, dorme só em SLEEP
     * (o USART para no STOP no meio de um byte; veja BOARD_CONSOLE_AWAKE_MS). */
    bool consoleAwake = false;
    if( ConsoleRxActive )
    {
        consoleAwake = ( TimerGetElapsedTime( ConsoleRxTime ) < BOARD_CONSOLE_AWAKE_MS );
        if( consoleAwake == false )
        {
            ConsoleRxActive = false;
        }
    }

    if( ( StopAllowed == false ) || UartMcuTxBusy( ) || consoleAwake )
    {
        LpmEnterSleepMode( );
    }
    else
    {
        LpmEnterLowPower( );
    }

    __enable_irq( );
}

void BoardLedSet( uint8_t led, bool on )
{
    Gpio_t *g = ( led == BOARD_LED_RED ) ? &LedRed : ( led == BOARD_LED_GREEN ) ? &LedGreen : &LedYellow;
    GpioWrite( g, on ? LED_ON_LEVEL : !LED_ON_LEVEL );
}

void BoardLedToggle( uint8_t led )
{
    Gpio_t *g = ( led == BOARD_LED_RED ) ? &LedRed : ( led == BOARD_LED_GREEN ) ? &LedGreen : &LedYellow;
    GpioToggle( g );
}

/*
 * printf -> console
 */
/* A aplicação pode guardar uma cópia do que sai no console (log em RAM, app/node_log.c) */
__attribute__( ( weak ) ) void BoardConsoleTap( const uint8_t *buf, size_t count )
{
    ( void )buf;
    ( void )count;
}

int _write( int fd, const void *buf, size_t count )
{
    ( void )fd;
    BoardConsoleTap( ( const uint8_t* )buf, count );
    UartPutBuffer( &ConsoleUart, ( uint8_t* )buf, ( uint16_t )count );
    return count;
}

int _read( int fd, const void *buf, size_t count )
{
    ( void )fd;
    size_t bytesRead = 0;
    while( UartGetBuffer( &ConsoleUart, ( uint8_t* )buf, count, ( uint16_t* )&bytesRead ) != 0 )
    {
    }
    return bytesRead;
}

/* stubs do newlib (sem sistema de arquivos) */
#include <sys/stat.h>
int _close( int fd )                       { ( void )fd; return -1; }
int _fstat( int fd, struct stat *st )      { ( void )fd; st->st_mode = S_IFCHR; return 0; }
int _isatty( int fd )                      { ( void )fd; return 1; }
int _lseek( int fd, int ptr, int dir )     { ( void )fd; ( void )ptr; ( void )dir; return 0; }

#ifdef USE_FULL_ASSERT
void assert_failed( uint8_t* file, uint32_t line )
{
    printf( "assert: %s:%lu\r\n", ( const char* )file, line );
    while( 1 )
    {
    }
}
#endif
