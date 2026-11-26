/*!
 * \file      board-config.h
 *
 * \brief     Pinagem do módulo LoRaWAN EndDevice da Radioenge
 *            (STM32L071CZT6 + SX1272), regravado com firmware próprio.
 *
 * Pinos levantados por engenharia reversa (projeto "Radioenge-Module",
 * ricaun/loranow) e confirmados com a LMiC rodando no módulo.
 *
 *   Rádio SX1272 (SPI1)      MCU
 *   ----------------------   -----
 *   NSS                      PA4
 *   SCLK / MISO / MOSI       PA5 / PA6 / PA7
 *   RESET                    PB14
 *   DIO0 / DIO1 / DIO2 / DIO3  PB10 / PB2 / PB1 / PB0
 *   Chave de antena RXTX     PB13  (1 = recepção, 0 = transmissão)
 *   Chave de antena RXTX2    PB12  (sempre 0)
 *
 *   Pinos do módulo          MCU
 *   ----------------------   -----
 *   2 (RX) / 3 (TX)          PB7 (USART1_RX) / PB6 (USART1_TX)  <- console padrão
 *   6 GPIO0 / 7 GPIO1        PA3 (USART2_RX) / PA2 (USART2_TX)  <- console alternativo
 *   LEDs vermelho/verde/amarelo  PB5 / PB8 / PB9 (acendem em nível 0)
 *   SWDIO / SWCLK / RESET    PA13 / PA14 / NRST (pads da borda inferior)
 *
 * SPDX-License-Identifier: BSD-3-Clause (mesma licença da LoRaMac-node)
 */
#ifndef __BOARD_CONFIG_H__
#define __BOARD_CONFIG_H__

#ifdef __cplusplus
extern "C"
{
#endif

/*!
 * O SX1272 do módulo usa cristal, não TCXO.
 */
#define BOARD_TCXO_WAKEUP_TIME                      0

/*!
 * Rádio
 */
#define RADIO_RESET                                 PB_14

#define RADIO_MOSI                                  PA_7
#define RADIO_MISO                                  PA_6
#define RADIO_SCLK                                  PA_5
#define RADIO_NSS                                   PA_4

#define RADIO_DIO_0                                 PB_10
#define RADIO_DIO_1                                 PB_2
#define RADIO_DIO_2                                 PB_1
#define RADIO_DIO_3                                 PB_0
#define RADIO_DIO_4                                 NC
#define RADIO_DIO_5                                 NC

#define RADIO_ANT_SWITCH                            PB_13   /* RXTX:  1 = RX, 0 = TX */
#define RADIO_ANT_SWITCH_2                          PB_12   /* RXTX2: mantido em 0   */
#define RADIO_ANT_SWITCH_RX_LEVEL                   1

/*!
 * LEDs (ativos em nível baixo)
 */
#define LED_RED                                     PB_5
#define LED_GREEN                                   PB_8
#define LED_YELLOW                                  PB_9
#define LED_ON_LEVEL                                0

/*!
 * GPIOs do módulo (numeração do manual da Radioenge)
 */
#define MODULE_GPIO0                                PA_3
#define MODULE_GPIO1                                PA_2
#define MODULE_GPIO2                                PA_15
#define MODULE_GPIO3                                PA_10
#define MODULE_GPIO4                                PA_9
#define MODULE_GPIO5                                PA_8
#define MODULE_GPIO6                                PB_15
#define MODULE_GPIO7                                PA_0
#define MODULE_GPIO8                                PA_1
#define MODULE_GPIO9                                PB_11

/*!
 * Console: CONSOLE_UART = 1 -> USART1 nos pinos 2/3 do módulo (padrão);
 *          CONSOLE_UART = 2 -> USART2 em GPIO1 (TX) / GPIO0 (RX).
 */
#ifndef CONSOLE_UART
#define CONSOLE_UART                                1
#endif

#if ( CONSOLE_UART == 2 )
#define UART_TX                                     PA_2
#define UART_RX                                     PA_3
#else
#define UART_TX                                     PB_6
#define UART_RX                                     PB_7
#endif

#ifndef CONSOLE_BAUDRATE
#define CONSOLE_BAUDRATE                            115200
#endif

#define OSC_LSE_IN                                  PC_14
#define OSC_LSE_OUT                                 PC_15

#define OSC_HSE_IN                                  PH_0
#define OSC_HSE_OUT                                 PH_1

#define SWCLK                                       PA_14
#define SWDAT                                       PA_13

/* Pinos de depuração do rádio: não há pinos livres dedicados. */
#define RADIO_DBG_PIN_TX                            NC
#define RADIO_DBG_PIN_RX                            NC

#ifdef __cplusplus
}
#endif

#endif // __BOARD_CONFIG_H__
