/*!
 * \file      rtc-board.c
 *
 * \brief     Target board RTC timer and low power modes management
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
 *              (C)2013-2017 Semtech - STMicroelectronics
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 *
 * \author    MCD Application Team (C)( STMicroelectronics International )
 */
#include <math.h>
#include <time.h>
#include "stm32l0xx.h"
#include "utilities.h"
#include "delay.h"
#include "board.h"
#include "timer.h"
#include "systime.h"
#include "gpio.h"
#include "sysIrqHandlers.h"
#include "lpm-board.h"
#include "rtc-board.h"
#include "board-ext.h"

// MCU Wake Up Time
#define MIN_ALARM_DELAY                             3 // in ticks

// sub-second number of bits
#define N_PREDIV_S                                  10

// Synchronous prediv
#define PREDIV_S                                    ( ( 1 << N_PREDIV_S ) - 1 )

// Asynchronous prediv
// RADIOENGE: deixa de ser constante. Com LSE (32768 Hz) vale 31, igual ao
// original; com LSI (~37 kHz) é escolhido em RtcClockProbe( ) para que
// RTCCLK / ( PREDIV_A + 1 ) fique o mais perto possível de 1024 Hz.
static uint32_t RtcPredivA = ( 1 << ( 15 - N_PREDIV_S ) ) - 1;
#define PREDIV_A                                    RtcPredivA

// Sub-second mask definition
#define ALARM_SUBSECOND_MASK                        ( N_PREDIV_S << RTC_ALRMASSR_MASKSS_Pos )

// RTC Time base in us
#define USEC_NUMBER                                 1000000
#define MSEC_NUMBER                                 ( USEC_NUMBER / 1000 )

#define COMMON_FACTOR                               3
#define CONV_NUMER                                  ( MSEC_NUMBER >> COMMON_FACTOR )
#define CONV_DENOM                                  ( 1 << ( N_PREDIV_S - COMMON_FACTOR ) )

/*!
 * \brief Days, Hours, Minutes and seconds
 */
#define DAYS_IN_LEAP_YEAR                           ( ( uint32_t )  366U )
#define DAYS_IN_YEAR                                ( ( uint32_t )  365U )
#define SECONDS_IN_1DAY                             ( ( uint32_t )86400U )
#define SECONDS_IN_1HOUR                            ( ( uint32_t ) 3600U )
#define SECONDS_IN_1MINUTE                          ( ( uint32_t )   60U )
#define MINUTES_IN_1HOUR                            ( ( uint32_t )   60U )
#define HOURS_IN_1DAY                               ( ( uint32_t )   24U )

/*!
 * \brief Correction factors
 */
#define  DAYS_IN_MONTH_CORRECTION_NORM              ( ( uint32_t )0x99AAA0 )
#define  DAYS_IN_MONTH_CORRECTION_LEAP              ( ( uint32_t )0x445550 )

/*!
 * \brief Calculates ceiling( X / N )
 */
#define DIVC( X, N )                                ( ( ( X ) + ( N ) -1 ) / ( N ) )

/*!
 * RTC timer context 
 */
typedef struct
{
    uint32_t        Time;         // Reference time
    RTC_TimeTypeDef CalendarTime; // Reference time in calendar format
    RTC_DateTypeDef CalendarDate; // Reference date in calendar format
}RtcTimerContext_t;

/*!
 * \brief Indicates if the RTC is already Initialized or not
 */
static bool RtcInitialized = false;

/*
 * RADIOENGE: base de tempo do RTC.
 *
 * O módulo não tem cristal de 32,768 kHz ligado em PC14/PC15 (pelo menos no
 * levantamento de pinos disponível). Sem LSE, o RTC roda no LSI (~37 kHz,
 * impreciso). O LSI é medido contra o HSI16 (e, se houver cristal HSE em
 * PH0/PH1, contra o HSE) e a diferença entre a frequência real do tick e os
 * 1024 Hz nominais é corrigida em software: as conversões ms <-> ticks e o
 * relógio do sistema (SysTime) usam RtcTickRateQ16 (ticks por segundo, Q16).
 */
static RtcClockInfo_t RtcClock = { .Source = RTC_CLOCK_LSE, .RtcClkHz = 32768.0f, .HseHz = 0, .PpmEstimate = 50 };
static volatile uint32_t RtcTickRateQ16 = ( 1024UL << 16 );
/* Âncora do relógio corrigido: em AnchorTicks o relógio valia AnchorMs */
static uint64_t RtcAnchorTicks = 0;
static uint64_t RtcAnchorMs = 0;

static uint64_t RtcTicksToMsAnchored( uint64_t ticks );

/*!
 * Number of days in each month on a normal year
 */
static const uint8_t DaysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/*!
 * Number of days in each month on a leap year
 */
static const uint8_t DaysInMonthLeapYear[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/*!
 * \brief RTC Handle
 */
static RTC_HandleTypeDef RtcHandle = 
{
    .Instance = NULL,
    .Init = 
    { 
        .HourFormat = 0,
        .AsynchPrediv = 0,
        .SynchPrediv = 0,
        .OutPut = 0,
        .OutPutRemap = 0,
        .OutPutPolarity = 0,
        .OutPutType = 0
    },
    .Lock = HAL_UNLOCKED,
    .State = HAL_RTC_STATE_RESET
};

/*!
 * \brief RTC Alarm
 */
static RTC_AlarmTypeDef RtcAlarm;

/*!
 * Keep the value of the RTC timer when the RTC alarm is set
 * Set with the \ref RtcSetTimerContext function
 * Value is kept as a Reference to calculate alarm
 */
static RtcTimerContext_t RtcTimerContext;

/*!
 * \brief Get the current time from calendar in ticks
 *
 * \param [IN] date           Pointer to RTC_DateStruct
 * \param [IN] time           Pointer to RTC_TimeStruct
 * \retval calendarValue Time in ticks
 */
static uint64_t RtcGetCalendarValue( RTC_DateTypeDef* date, RTC_TimeTypeDef* time );

void RtcInit( void )
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;

    if( RtcInitialized == false )
    {
        __HAL_RCC_RTC_ENABLE( );

        RtcHandle.Instance            = RTC;
        RtcHandle.Init.HourFormat     = RTC_HOURFORMAT_24;
        RtcHandle.Init.AsynchPrediv   = PREDIV_A;  // RTC_ASYNCH_PREDIV;
        RtcHandle.Init.SynchPrediv    = PREDIV_S;  // RTC_SYNCH_PREDIV;
        RtcHandle.Init.OutPut         = RTC_OUTPUT_DISABLE;
        RtcHandle.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
        RtcHandle.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
        HAL_RTC_Init( &RtcHandle );

        date.Year                     = 0;
        date.Month                    = RTC_MONTH_JANUARY;
        date.Date                     = 1;
        date.WeekDay                  = RTC_WEEKDAY_MONDAY;
        HAL_RTC_SetDate( &RtcHandle, &date, RTC_FORMAT_BIN );

        /*at 0:0:0*/
        time.Hours                    = 0;
        time.Minutes                  = 0;
        time.Seconds                  = 0;
        time.SubSeconds               = 0;
        time.TimeFormat               = 0;
        time.StoreOperation           = RTC_STOREOPERATION_RESET;
        time.DayLightSaving           = RTC_DAYLIGHTSAVING_NONE;
        HAL_RTC_SetTime( &RtcHandle, &time, RTC_FORMAT_BIN );

        // Enable Direct Read of the calendar registers (not through Shadow registers)
        HAL_RTCEx_EnableBypassShadow( &RtcHandle );

        /* RADIOENGE: âncora do relógio corrigido no zero do calendário */
        RtcAnchorTicks = 0;
        RtcAnchorMs = 0;

        HAL_NVIC_SetPriority( RTC_IRQn, 1, 0 );
        HAL_NVIC_EnableIRQ( RTC_IRQn );

        // Init alarm.
        HAL_RTC_DeactivateAlarm( &RtcHandle, RTC_ALARM_A );

        RtcSetTimerContext( );
        RtcInitialized = true;
    }
}

/*!
 * \brief Sets the RTC timer reference, sets also the RTC_DateStruct and RTC_TimeStruct
 *
 * \param none
 * \retval timerValue In ticks
 */
uint32_t RtcSetTimerContext( void )
{
    RtcTimerContext.Time = ( uint32_t )RtcGetCalendarValue( &RtcTimerContext.CalendarDate, &RtcTimerContext.CalendarTime );
    return ( uint32_t )RtcTimerContext.Time;
}

/*!
 * \brief Gets the RTC timer reference
 *
 * \param none
 * \retval timerValue In ticks
 */
uint32_t RtcGetTimerContext( void )
{
    return RtcTimerContext.Time;
}

/*!
 * \brief returns the wake up time in ticks
 *
 * \retval wake up time in ticks
 */
uint32_t RtcGetMinimumTimeout( void )
{
    return( MIN_ALARM_DELAY );
}

/*!
 * \brief converts time in ms to time in ticks
 *
 * \param[IN] milliseconds Time in milliseconds
 * \retval returns time in timer ticks
 */
uint32_t RtcMs2Tick( uint32_t milliseconds )
{
    /* RADIOENGE: usa a taxa real do tick (LSE: exatamente 1024/s) */
    return ( uint32_t )( ( ( uint64_t )milliseconds * RtcTickRateQ16 + 500ULL * 65536ULL ) / ( 1000ULL * 65536ULL ) );
}

/*!
 * \brief converts time in ticks to time in ms
 *
 * \param[IN] time in timer ticks
 * \retval returns time in milliseconds
 */
uint32_t RtcTick2Ms( uint32_t tick )
{
    /* RADIOENGE: usa a taxa real do tick (LSE: exatamente 1024/s) */
    return ( uint32_t )( ( ( uint64_t )tick * 1000ULL * 65536ULL ) / RtcTickRateQ16 );
}

/*!
 * \brief a delay of delay ms by polling RTC
 *
 * \param[IN] delay in ms
 */
void RtcDelayMs( uint32_t delay )
{
    uint64_t delayTicks = 0;
    uint64_t refTicks = RtcGetTimerValue( );

    delayTicks = RtcMs2Tick( delay );

    // Wait delay ms
    while( ( ( RtcGetTimerValue( ) - refTicks ) ) < delayTicks )
    {
        __NOP( );
    }
}

/*!
 * \brief Sets the alarm
 *
 * \note The alarm is set at now (read in this function) + timeout
 *
 * \param timeout Duration of the Timer ticks
 */
void RtcSetAlarm( uint32_t timeout )
{
    // We don't go in Low Power mode for timeout below MIN_ALARM_DELAY
    if( ( int64_t )MIN_ALARM_DELAY < ( int64_t )( timeout - RtcGetTimerElapsedTime( ) ) )
    {
        LpmSetStopMode( LPM_RTC_ID, LPM_ENABLE );
    }
    else
    {
        LpmSetStopMode( LPM_RTC_ID, LPM_DISABLE );
    }

    RtcStartAlarm( timeout );
}

void RtcStopAlarm( void )
{
    // Disable the Alarm A interrupt
    HAL_RTC_DeactivateAlarm( &RtcHandle, RTC_ALARM_A );

    // Clear RTC Alarm Flag
    __HAL_RTC_ALARM_CLEAR_FLAG( &RtcHandle, RTC_FLAG_ALRAF );

    // Clear the EXTI's line Flag for RTC Alarm
    __HAL_RTC_ALARM_EXTI_CLEAR_FLAG( );
}

void RtcStartAlarm( uint32_t timeout )
{
    uint16_t rtcAlarmSubSeconds = 0;
    uint16_t rtcAlarmSeconds = 0;
    uint16_t rtcAlarmMinutes = 0;
    uint16_t rtcAlarmHours = 0;
    uint16_t rtcAlarmDays = 0;
    RTC_TimeTypeDef time = RtcTimerContext.CalendarTime;
    RTC_DateTypeDef date = RtcTimerContext.CalendarDate;

    RtcStopAlarm( );

    /*reverse counter */
    rtcAlarmSubSeconds =  PREDIV_S - time.SubSeconds;
    rtcAlarmSubSeconds += ( timeout & PREDIV_S );
    // convert timeout  to seconds
    timeout >>= N_PREDIV_S;

    // Convert microsecs to RTC format and add to 'Now'
    rtcAlarmDays =  date.Date;
    while( timeout >= TM_SECONDS_IN_1DAY )
    {
        timeout -= TM_SECONDS_IN_1DAY;
        rtcAlarmDays++;
    }

    // Calc hours
    rtcAlarmHours = time.Hours;
    while( timeout >= TM_SECONDS_IN_1HOUR )
    {
        timeout -= TM_SECONDS_IN_1HOUR;
        rtcAlarmHours++;
    }

    // Calc minutes
    rtcAlarmMinutes = time.Minutes;
    while( timeout >= TM_SECONDS_IN_1MINUTE )
    {
        timeout -= TM_SECONDS_IN_1MINUTE;
        rtcAlarmMinutes++;
    }

    // Calc seconds
    rtcAlarmSeconds =  time.Seconds + timeout;

    //***** Correct for modulo********
    while( rtcAlarmSubSeconds >= ( PREDIV_S + 1 ) )
    {
        rtcAlarmSubSeconds -= ( PREDIV_S + 1 );
        rtcAlarmSeconds++;
    }

    while( rtcAlarmSeconds >= TM_SECONDS_IN_1MINUTE )
    { 
        rtcAlarmSeconds -= TM_SECONDS_IN_1MINUTE;
        rtcAlarmMinutes++;
    }

    while( rtcAlarmMinutes >= TM_MINUTES_IN_1HOUR )
    {
        rtcAlarmMinutes -= TM_MINUTES_IN_1HOUR;
        rtcAlarmHours++;
    }

    while( rtcAlarmHours >= TM_HOURS_IN_1DAY )
    {
        rtcAlarmHours -= TM_HOURS_IN_1DAY;
        rtcAlarmDays++;
    }

    if( date.Year % 4 == 0 ) 
    {
        if( rtcAlarmDays > DaysInMonthLeapYear[date.Month - 1] )
        {
            rtcAlarmDays = rtcAlarmDays % DaysInMonthLeapYear[date.Month - 1];
        }
    }
    else
    {
        if( rtcAlarmDays > DaysInMonth[date.Month - 1] )
        {   
            rtcAlarmDays = rtcAlarmDays % DaysInMonth[date.Month - 1];
        }
    }

    /* Set RTC_AlarmStructure with calculated values*/
    RtcAlarm.AlarmTime.SubSeconds     = PREDIV_S - rtcAlarmSubSeconds;
    RtcAlarm.AlarmSubSecondMask       = ALARM_SUBSECOND_MASK; 
    RtcAlarm.AlarmTime.Seconds        = rtcAlarmSeconds;
    RtcAlarm.AlarmTime.Minutes        = rtcAlarmMinutes;
    RtcAlarm.AlarmTime.Hours          = rtcAlarmHours;
    RtcAlarm.AlarmDateWeekDay         = ( uint8_t )rtcAlarmDays;
    RtcAlarm.AlarmTime.TimeFormat     = time.TimeFormat;
    RtcAlarm.AlarmDateWeekDaySel      = RTC_ALARMDATEWEEKDAYSEL_DATE; 
    RtcAlarm.AlarmMask                = RTC_ALARMMASK_NONE;
    RtcAlarm.Alarm                    = RTC_ALARM_A;
    RtcAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    RtcAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;

    // Set RTC_Alarm
    HAL_RTC_SetAlarm_IT( &RtcHandle, &RtcAlarm, RTC_FORMAT_BIN );
}

uint32_t RtcGetTimerValue( void )
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;

    uint32_t calendarValue = ( uint32_t )RtcGetCalendarValue( &date, &time );

    return( calendarValue );
}

uint32_t RtcGetTimerElapsedTime( void )
{
  RTC_TimeTypeDef time;
  RTC_DateTypeDef date;
  
  uint32_t calendarValue = ( uint32_t )RtcGetCalendarValue( &date, &time );

  return( ( uint32_t )( calendarValue - RtcTimerContext.Time ) );
}

static uint64_t RtcGetCalendarValue( RTC_DateTypeDef* date, RTC_TimeTypeDef* time )
{
    uint64_t calendarValue = 0;
    uint32_t firstRead;
    uint32_t correction;
    uint32_t seconds;

    // Make sure it is correct due to asynchronus nature of RTC
    do
    {
        firstRead = RTC->SSR;
        HAL_RTC_GetDate( &RtcHandle, date, RTC_FORMAT_BIN );
        HAL_RTC_GetTime( &RtcHandle, time, RTC_FORMAT_BIN );
    }while( firstRead != RTC->SSR );

    // Calculte amount of elapsed days since 01/01/2000
    seconds = DIVC( ( DAYS_IN_YEAR * 3 + DAYS_IN_LEAP_YEAR ) * date->Year , 4 );

    correction = ( ( date->Year % 4 ) == 0 ) ? DAYS_IN_MONTH_CORRECTION_LEAP : DAYS_IN_MONTH_CORRECTION_NORM;

    seconds += ( DIVC( ( date->Month-1 ) * ( 30 + 31 ), 2 ) - ( ( ( correction >> ( ( date->Month - 1 ) * 2 ) ) & 0x03 ) ) );

    seconds += ( date->Date -1 );

    // Convert from days to seconds
    seconds *= SECONDS_IN_1DAY;

    seconds += ( ( uint32_t )time->Seconds + 
                 ( ( uint32_t )time->Minutes * SECONDS_IN_1MINUTE ) +
                 ( ( uint32_t )time->Hours * SECONDS_IN_1HOUR ) ) ;

    calendarValue = ( ( ( uint64_t )seconds ) << N_PREDIV_S ) + ( PREDIV_S - time->SubSeconds );

    return( calendarValue );
}

uint32_t RtcGetCalendarTime( uint16_t *milliseconds )
{
    RTC_TimeTypeDef time ;
    RTC_DateTypeDef date;

    uint64_t calendarValue = RtcGetCalendarValue( &date, &time );

    /* RADIOENGE: segundos reais, corrigidos pela taxa medida do tick */
    uint64_t ms = RtcTicksToMsAnchored( calendarValue );

    *milliseconds = ( uint16_t )( ms % 1000 );
    return ( uint32_t )( ms / 1000 );
}

/*
 * RADIOENGE: ticks do calendário -> ms reais, com âncora. A âncora avança a
 * cada 2^24 ticks (~4,5 h) para a conta em 64 bits nunca estourar, e é
 * refeita a cada recalibração para o relógio não dar saltos.
 */
static uint64_t RtcTicksToMsAnchored( uint64_t ticks )
{
    uint64_t ms;

    CRITICAL_SECTION_BEGIN( );
    if( ticks < RtcAnchorTicks )
    {
        /* calendário reiniciado: recomeça a âncora */
        RtcAnchorTicks = ticks;
        RtcAnchorMs = ( ticks * 1000ULL ) >> N_PREDIV_S;
    }
    while( ( ticks - RtcAnchorTicks ) > ( 1ULL << 24 ) )
    {
        RtcAnchorMs += ( ( 1ULL << 24 ) * 1000ULL * 65536ULL ) / RtcTickRateQ16;
        RtcAnchorTicks += ( 1ULL << 24 );
    }
    ms = RtcAnchorMs + ( ( ticks - RtcAnchorTicks ) * 1000ULL * 65536ULL ) / RtcTickRateQ16;
    CRITICAL_SECTION_END( );
    return ms;
}

/*!
 * \brief RTC IRQ Handler of the RTC Alarm
 */
void RTC_IRQHandler( void )
{
    RTC_HandleTypeDef* hrtc = &RtcHandle;

    // Enable low power at irq
    LpmSetStopMode( LPM_RTC_ID, LPM_ENABLE );

    // Clear the EXTI's line Flag for RTC Alarm
    __HAL_RTC_ALARM_EXTI_CLEAR_FLAG( );

    // Gets the AlarmA interrupt source enable status
    if( __HAL_RTC_ALARM_GET_IT_SOURCE( hrtc, RTC_IT_ALRA ) != RESET )
    {
        // Gets the pending status of the AlarmA interrupt
        if( __HAL_RTC_ALARM_GET_FLAG( hrtc, RTC_FLAG_ALRAF ) != RESET )
        {
            // Clear the AlarmA interrupt pending bit
            __HAL_RTC_ALARM_CLEAR_FLAG( hrtc, RTC_FLAG_ALRAF ); 
            // AlarmA callback
            HAL_RTC_AlarmAEventCallback( hrtc );
        }
    }
}

/*!
 * \brief  Alarm A callback.
 *
 * \param [IN] hrtc RTC handle
 */
void HAL_RTC_AlarmAEventCallback( RTC_HandleTypeDef *hrtc )
{
    TimerIrqHandler( );
}

void RtcBkupWrite( uint32_t data0, uint32_t data1 )
{
    HAL_RTCEx_BKUPWrite( &RtcHandle, RTC_BKP_DR0, data0 );
    HAL_RTCEx_BKUPWrite( &RtcHandle, RTC_BKP_DR1, data1 );
}

void RtcBkupRead( uint32_t *data0, uint32_t *data1 )
{
  *data0 = HAL_RTCEx_BKUPRead( &RtcHandle, RTC_BKP_DR0 );
  *data1 = HAL_RTCEx_BKUPRead( &RtcHandle, RTC_BKP_DR1 );
}

void RtcProcess( void )
{
    // Not used on this platform.
}

TimerTime_t RtcTempCompensation( TimerTime_t period, float temperature )
{
    float k = RTC_TEMP_COEFFICIENT;
    float kDev = RTC_TEMP_DEV_COEFFICIENT;
    float t = RTC_TEMP_TURNOVER;
    float tDev = RTC_TEMP_DEV_TURNOVER;
    float interim = 0.0f;
    float ppm = 0.0f;

    if( k < 0.0f )
    {
        ppm = ( k - kDev );
    }
    else
    {
        ppm = ( k + kDev );
    }
    interim = ( temperature - ( t - tDev ) );
    ppm *=  interim * interim;

    // Calculate the drift in time
    interim = ( ( float ) period * ppm ) / 1000000.0f;
    // Calculate the resulting time period
    interim += period;
    interim = floor( interim );

    if( interim < 0.0f )
    {
        interim = ( float )period;
    }

    // Calculate the resulting period
    return ( TimerTime_t ) interim;
}

/* ===================================================================== */
/*  RADIOENGE: escolha e calibração do relógio do RTC                    */
/* ===================================================================== */

#define TIM_CLK_HZ                                  32000000.0f   /* PCLK2 nominal (HSI16 x 4 / 2) */
#define TI1_RMP_HSE_RTC                             ( TIM21_OR_TI1_RMP_1 )
#define TI1_RMP_LSI                                 ( TIM21_OR_TI1_RMP_2 | TIM21_OR_TI1_RMP_0 )
#define TI1_RMP_LSE                                 ( TIM21_OR_TI1_RMP_2 )
#define LSE_PROBE_TIMEOUT_MS                        2000
#define HSE_PROBE_LOOPS                             400000UL      /* ~50 ms a 32 MHz */

/*!
 * Mede, com o TIM21 a 32 MHz, a soma dos períodos de 'captures' grupos de 8
 * ciclos do sinal ligado ao TI1 ('remap': LSI ou HSE_RTC). Retorna 0 em
 * caso de timeout ou de captura perdida.
 */
static uint32_t Tim21Measure( uint32_t remap, uint32_t captures, bool irqOff )
{
    uint32_t total = 0;
    uint16_t last = 0;
    uint32_t got = 0;
    uint32_t guard = 0;
    uint32_t primask = __get_PRIMASK( );

    __HAL_RCC_TIM21_CLK_ENABLE( );
    TIM21->CR1   = 0;
    TIM21->SMCR  = 0;
    TIM21->DIER  = 0;
    TIM21->PSC   = 0;
    TIM21->ARR   = 0xFFFF;
    TIM21->OR    = ( TIM21->OR & ~TIM21_OR_TI1_RMP ) | remap;
    TIM21->CCMR1 = TIM_CCMR1_CC1S_0 | TIM_CCMR1_IC1PSC_0 | TIM_CCMR1_IC1PSC_1;  /* IC1 <- TI1, /8 */
    TIM21->CCER  = TIM_CCER_CC1E;
    TIM21->EGR   = TIM_EGR_UG;
    TIM21->SR    = 0;

    if( irqOff )
    {
        __disable_irq( );
    }
    TIM21->CR1 = TIM_CR1_CEN;

    while( got <= captures )
    {
        uint32_t sr = TIM21->SR;
        if( sr & TIM_SR_CC1OF )
        {
            total = 0;   /* perdeu uma captura: medida inválida */
            break;
        }
        if( sr & TIM_SR_CC1IF )
        {
            uint16_t c = ( uint16_t )TIM21->CCR1;   /* a leitura limpa o CC1IF */
            if( got > 0 )
            {
                total += ( uint16_t )( c - last );
            }
            last = c;
            got++;
            guard = 0;
        }
        else if( ++guard > 2000000UL )
        {
            total = 0;   /* sinal não chegou */
            break;
        }
    }

    TIM21->CR1  = 0;
    TIM21->CCER = 0;
    __set_PRIMASK( primask );
    __HAL_RCC_TIM21_CLK_DISABLE( );
    return total;
}

/*!
 * Liga o HSE (cristal em PH0/PH1), mede a frequência dele contra o HSI16 e
 * aproxima para um valor comercial. Retorna o valor nominal em Hz e, em
 * *ratio, nominal / medido (fator que corrige o erro do HSI16). 0 sem HSE.
 */
static uint32_t ProbeHse( float *ratio )
{
    static const uint32_t candidates[] = { 8000000, 12000000, 16000000, 24000000, 25000000, 26000000, 32000000 };
    static int8_t hsePresent = -1;   /* -1: ainda não testado */
    uint32_t result = 0;
    uint32_t loops = 0;

    if( hsePresent == 0 )
    {
        return 0;   /* não há cristal HSE: não perde ~50 ms a cada recalibração */
    }

    /* HSE_RTC = HSE / 16 (RTCPRE = 11) no TI1 do TIM21. O RTCPRE só pode ser
     * mudado com o HSE desligado, por isso vem antes do HSEON. */
    MODIFY_REG( RCC->CR, RCC_CR_RTCPRE, RCC_CR_RTCPRE_0 | RCC_CR_RTCPRE_1 );
    SET_BIT( RCC->CR, RCC_CR_HSEON );
    while( ( READ_BIT( RCC->CR, RCC_CR_HSERDY ) == 0 ) && ( ++loops < HSE_PROBE_LOOPS ) )
    {
    }
    if( READ_BIT( RCC->CR, RCC_CR_HSERDY ) != 0 )
    {
        const uint32_t n = 1024;
        uint32_t counts = Tim21Measure( TI1_RMP_HSE_RTC, n, true );
        if( counts > 0 )
        {
            float measured = TIM_CLK_HZ * 8.0f * 16.0f * ( float )n / ( float )counts;
            for( uint32_t i = 0; i < sizeof( candidates ) / sizeof( candidates[0] ); i++ )
            {
                float err = ( measured - ( float )candidates[i] ) / ( float )candidates[i];
                if( ( err > -0.03f ) && ( err < 0.03f ) )
                {
                    result = candidates[i];
                    *ratio = ( float )candidates[i] / measured;
                    break;
                }
            }
        }
    }
    CLEAR_BIT( RCC->CR, RCC_CR_HSEON );
    hsePresent = ( result != 0 ) ? 1 : 0;
    return result;
}

/*!
 * Mede o LSI (Hz). Com HSE, o erro do HSI16 é cancelado; sem HSE, a medida
 * herda a tolerância de fábrica do HSI16 (~1 %).
 */
static float MeasureLsi( uint32_t *hseHz )
{
    const uint32_t n = 64;   /* 64 x 8 ciclos do LSI: ~14 ms */
    float ratio = 1.0f;
    uint32_t hse = ProbeHse( &ratio );

    if( hseHz != NULL )
    {
        *hseHz = hse;
    }
    /* Uma interrupção longa pode fazer perder uma captura (CC1OF): tenta de novo.
     * Retorna 0 se não conseguir uma medida plausível (LSI: 26–56 kHz). */
    for( int tries = 0; tries < 4; tries++ )
    {
        uint32_t counts = Tim21Measure( TI1_RMP_LSI, n, false );
        if( counts != 0 )
        {
            float f = TIM_CLK_HZ * 8.0f * ( float )n / ( float )counts * ratio;
            if( ( f > 26000.0f ) && ( f < 56000.0f ) )
            {
                return f;
            }
        }
    }
    return 0.0f;
}

/*!
 * RADIOENGE: o LSERDY sozinho não prova que existe cristal. Sem cristal, com
 * PC14/PC15 soltos, o oscilador às vezes dá "pronto" captando ruído; aí o RTC
 * fica parado (uptime 0, timers da LoRaMac sem disparar, ABP/JOIN travado e
 * nenhum uplink). Mede o LSE com o TIM21 contra o HSI16 e só aceita perto de
 * 32,768 kHz (o HSI16 de fábrica tem ±1–3 %).
 */
static bool LseLooksReal( void )
{
    const uint32_t n = 64;   /* 64 x 8 ciclos do LSE: ~16 ms */
    for( int tries = 0; tries < 3; tries++ )
    {
        uint32_t counts = Tim21Measure( TI1_RMP_LSE, n, false );
        if( counts != 0 )
        {
            float f = TIM_CLK_HZ * 8.0f * ( float )n / ( float )counts;
            if( ( f > 31130.0f ) && ( f < 34410.0f ) )   /* 32768 Hz ± 5 % */
            {
                return true;
            }
        }
    }
    return false;
}

void RtcClockProbe( void )
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_PeriphCLKInitTypeDef clk = { 0 };

    HAL_PWR_EnableBkUpAccess( );

    /* 1) Cristal de 32,768 kHz (LSE) em PC14/PC15? */
#if defined( RTC_FORCE_LSI )
    bool lseOk = false;     /* teste: ignora o LSE e usa o LSI calibrado */
    if( 0 )
#else
    bool lseOk = ( __HAL_RCC_GET_FLAG( RCC_FLAG_LSERDY ) != RESET );
    if( lseOk == false )
#endif
    {
        __HAL_RCC_LSEDRIVE_CONFIG( RCC_LSEDRIVE_MEDIUMHIGH );
        __HAL_RCC_LSE_CONFIG( RCC_LSE_ON );
        uint32_t t0 = HAL_GetTick( );
        while( ( __HAL_RCC_GET_FLAG( RCC_FLAG_LSERDY ) == RESET ) && ( ( HAL_GetTick( ) - t0 ) < LSE_PROBE_TIMEOUT_MS ) )
        {
        }
        lseOk = ( __HAL_RCC_GET_FLAG( RCC_FLAG_LSERDY ) != RESET );
    }
#if !defined( RTC_FORCE_LSI )
    if( lseOk && !LseLooksReal( ) )
    {
        lseOk = false;   /* LSERDY falso (sem cristal): usa o LSI calibrado */
    }
#endif

    if( lseOk )
    {
        RtcClock.Source = RTC_CLOCK_LSE;
        RtcClock.RtcClkHz = 32768.0f;
        RtcClock.HseHz = 0;
        RtcClock.PpmEstimate = 50;
        RtcPredivA = 31;
        RtcTickRateQ16 = ( 1024UL << 16 );
        clk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    }
    else
    {
        __HAL_RCC_LSE_CONFIG( RCC_LSE_OFF );

        osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        osc.LSIState = RCC_LSI_ON;
        osc.PLL.PLLState = RCC_PLL_NONE;
        HAL_RCC_OscConfig( &osc );

        uint32_t hse = 0;
        float lsi = MeasureLsi( &hse );
        bool measured = ( lsi > 0.0f );
        if( !measured )
        {
            lsi = ( float )LSI_VALUE;   /* sem medida: valor típico, bem impreciso */
        }

        RtcClock.Source = ( hse != 0 ) ? RTC_CLOCK_LSI_HSE_REF : RTC_CLOCK_LSI_HSI_REF;
        RtcClock.RtcClkHz = lsi;
        RtcClock.HseHz = hse;
        /* HSI16 de fábrica: ±1 % a 25 °C, ±2–3 % na faixa toda; sem medida: LSI ±15 % */
        RtcClock.PpmEstimate = !measured ? 150000 : ( hse != 0 ) ? 500 : 25000;

        uint32_t a = ( uint32_t )( ( lsi / 1024.0f ) + 0.5f );
        if( a < 1 )
        {
            a = 1;
        }
        if( a > 128 )
        {
            a = 128;
        }
        RtcPredivA = a - 1;
        RtcTickRateQ16 = ( uint32_t )( ( lsi / ( float )a ) * 65536.0f + 0.5f );
        clk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
    }

    clk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    HAL_RCCEx_PeriphCLKConfig( &clk );
}

bool RtcRecalibrate( void )
{
    if( ( RtcInitialized == false ) || ( RtcClock.Source == RTC_CLOCK_LSE ) )
    {
        return false;
    }

    uint32_t hse = 0;
    float lsi = MeasureLsi( &hse );
    if( lsi <= 0.0f )
    {
        return false;   /* medida ruim: mantém a taxa atual */
    }
    uint32_t newRate = ( uint32_t )( ( lsi / ( float )( RtcPredivA + 1 ) ) * 65536.0f + 0.5f );

    /* refaz a âncora no instante atual com a taxa antiga, depois troca a taxa */
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    uint64_t now = RtcGetCalendarValue( &date, &time );
    uint64_t nowMs = RtcTicksToMsAnchored( now );

    CRITICAL_SECTION_BEGIN( );
    RtcAnchorTicks = now;
    RtcAnchorMs = nowMs;
    RtcTickRateQ16 = newRate;
    CRITICAL_SECTION_END( );

    RtcClock.RtcClkHz = lsi;
    RtcClock.HseHz = hse;
    RtcClock.Source = ( hse != 0 ) ? RTC_CLOCK_LSI_HSE_REF : RTC_CLOCK_LSI_HSI_REF;
    RtcClock.PpmEstimate = ( hse != 0 ) ? 500 : 25000;
    return true;
}

uint32_t RtcGetTimerValueMs( void )
{
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    return ( uint32_t )RtcTicksToMsAnchored( RtcGetCalendarValue( &date, &time ) );
}

const RtcClockInfo_t* RtcGetClockInfo( void )
{
    RtcClock.TickRateQ16 = RtcTickRateQ16;
    RtcClock.PredivA = RtcPredivA;
    return &RtcClock;
}
