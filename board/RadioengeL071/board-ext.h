/*!
 * \file      board-ext.h
 *
 * \brief     Extensões da placa Radioenge (LEDs, console, relógio do RTC).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __BOARD_EXT_H__
#define __BOARD_EXT_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define BOARD_LED_RED                               0
#define BOARD_LED_GREEN                             1
#define BOARD_LED_YELLOW                            2

void BoardLedSet( uint8_t led, bool on );
void BoardLedToggle( uint8_t led );

/*! true: alimentação externa/regulada (nível de bateria = 0 no DevStatusAns) */
void BoardSetExternalPower( bool external );

/*! Permite ou proíbe o modo STOP (com STOP proibido o MCU só usa SLEEP). */
void BoardSetStopAllowed( bool allowed );
bool BoardGetStopAllowed( void );

/*! true: HSI16 fica ligado no STOP para o console não perder o 1º caractere */
void BoardSetConsoleAwakeInStop( bool awake );

/*! Rádio: frequência (Hz) e potência (dBm no SX1272) da última transmissão; 0 = nenhuma ainda */
uint32_t BoardGetLastTxFreq( void );
int8_t BoardGetLastTxPower( void );

/*! Rádio: frequência (Hz), DR (AU915), RSSI e SNR do último quadro recebido; 0 = nenhum ainda */
void BoardGetLastRx( uint32_t *freq, uint8_t *dr, int16_t *rssi, int8_t *snr );

/*! Diagnóstico de sessão: copia (e consome) o último quadro que foi ao ar / chegou
 *  pelo rádio, exatamente como está no ar (cifrado, com MHDR e MIC). buf >= 256. */
bool BoardRadioTakeTxFrame( uint8_t *buf, uint8_t *len );
bool BoardRadioTakeRxFrame( uint8_t *buf, uint8_t *len );

/*! Console */
/*! Chamado pela interrupção da UART a cada byte recebido: mantém o MCU fora do
 *  STOP por BOARD_CONSOLE_AWAKE_MS, para um comando inteiro (vários bytes
 *  seguidos, como os da página WEB e do app) não perder caracteres. */
void BoardConsoleRxActivity( void );

/*! Chamado a cada printf com o que vai sair no console (fraco: a aplicação pode
 *  sobrescrever para guardar um log em RAM). */
void BoardConsoleTap( const uint8_t *buf, size_t count );
bool UartMcuTxBusy( void );
/*! AT+BAUDRATE: nova velocidade do console (9600, 19200, 43000, 115200) */
void BoardSetConsoleBaudrate( uint32_t baudrate );
void UartMcuFlush( void );

/*! Relógio do RTC */
typedef enum
{
    RTC_CLOCK_LSE = 0,          /*!< cristal de 32,768 kHz */
    RTC_CLOCK_LSI_HSE_REF,      /*!< LSI calibrado contra o cristal HSE */
    RTC_CLOCK_LSI_HSI_REF,      /*!< LSI calibrado contra o HSI16 (~1 %) */
}RtcClockSource_t;

typedef struct
{
    RtcClockSource_t Source;
    float    RtcClkHz;          /*!< frequência medida do relógio do RTC */
    uint32_t HseHz;             /*!< cristal HSE reconhecido (0 = sem HSE) */
    uint32_t PpmEstimate;       /*!< precisão estimada, em ppm */
    uint32_t TickRateQ16;       /*!< ticks por segundo, Q16 (1024 << 16 = nominal) */
    uint32_t PredivA;
}RtcClockInfo_t;

void RtcClockProbe( void );
uint32_t RtcGetTimerValueMs( void );   /*!< ms desde o boot, contínuo entre recalibrações */
bool RtcRecalibrate( void );
const RtcClockInfo_t* RtcGetClockInfo( void );

#ifdef __cplusplus
}
#endif

#endif // __BOARD_EXT_H__
