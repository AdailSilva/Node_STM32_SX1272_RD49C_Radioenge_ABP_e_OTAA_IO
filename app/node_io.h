/*
 * node_io.h — GPIOs, entradas analógicas, interrupções, keepalive e serial
 * transparente do módulo Radioenge, com o mesmo comportamento do firmware AT
 * original (comandos AT+GPIOC, AT+WPIN, AT+RPIN, AT+ADC, AT+KEEPALIVE, AT+BAT).
 *
 *   GPIO   pino   MCU     analógica   interrupção   outra função
 *   GPIO0   6     PA3     ADC_IN3     -             RX da serial transparente (USART2)
 *   GPIO1   7     PA2     ADC_IN2     (1)           TX da serial transparente (USART2)
 *   GPIO2   9     PA15    -           EXTI15 (2)
 *   GPIO3  10     PA10    -           -  (3)
 *   GPIO4  11     PA9     -           EXTI9
 *   GPIO5  12     PA8     -           EXTI8
 *   GPIO6  13     PB15    -           EXTI15 (2)
 *   GPIO7  14     PA0     ADC_IN0     -
 *   GPIO8  15     PA1     ADC_IN1     -
 *   GPIO9  16     PB11    -           EXTI11
 *
 *   (1) o manual permite, mas a linha EXTI2 é a do DIO1 do SX1272 (PB2): recusado.
 *   (2) PA15 e PB15 dividem a linha EXTI15: só um dos dois por vez.
 *   (3) fora da lista do manual (EXTI10 é a do DIO0 do SX1272).
 *
 * Modos (AT+GPIOC=pino:modo:pull), iguais aos do manual da Radioenge:
 *   0 entrada digital · 1 saída push-pull · 2 saída open-drain
 *   3/4 função alternativa (GPIO0/GPIO1: serial transparente, porta 4)
 *   5 entrada analógica (GPIO0, 1, 7, 8)
 *   6/7/8 interrupção na subida / descida / nas duas (GPIO2, 4, 5, 6, 9)
 *   pull: 0 sem, 1 pull-up, 2 pull-down
 *
 * Relés (AT+RELECFG, node_relay.c): o pino de um relé fica em saída push-pull e é
 * só dele: AT+GPIOC e AT+WPIN nesse pino respondem AT ERROR até o relé sair da lista.
 * PZEM-004T v3 (AT+PZEMCFG, node_pzem.c): usa GPIO0/GPIO1 (USART2), como a serial
 * transparente; os dois são exclusivos e a configuração mais recente vence.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_IO_H
#define NODE_IO_H

#include <stdbool.h>
#include <stdint.h>

#define NODE_IO_PORT_IRQ            2      /* uplink de interrupção (Tabela 13 do manual)   */
#define NODE_IO_PORT_TRANSPARENT    4      /* serial transparente (uplink e downlink)       */
#define NODE_IO_TRANSP_MAX          64     /* bytes por uplink da serial transparente       */
#define NODE_IO_KEEPALIVE_LEN       16
#define NODE_IO_IRQ_LEN             8

typedef enum
{
    NODE_IO_OK = 0,
    NODE_IO_PARAM_ERROR,            /* pino, modo ou pull fora do permitido */
    NODE_IO_ERROR,                  /* pino não está no modo certo para a operação */
}NodeIoStatus_t;

/* Aplica os modos gravados em NodeCfg (boot e depois de "factory"). */
void NodeIoInit( void );

NodeIoStatus_t NodeIoConfigure( uint8_t gpio, uint8_t mode, uint8_t pull );
NodeIoStatus_t NodeIoWrite( uint8_t gpio, uint8_t level );
/* Digital: 0/1. Analógico: 0..4095. */
NodeIoStatus_t NodeIoRead( uint8_t gpio, uint16_t *value );
NodeIoStatus_t NodeIoReadAdc( uint8_t gpio, uint16_t *value );

/* Bit n = nível do GPIOn configurado como entrada digital ou interrupção */
uint16_t NodeIoInputsBitmap( void );

/* Nível de bateria na escala do AT+BAT: 1 = 1,8 V ... 254 = 3,6 V; 255 = sem medida */
uint8_t NodeIoBatteryLevel( void );

/* Pacotes no formato do manual (Tabelas 9 e 13) */
uint8_t NodeIoBuildKeepalive( uint8_t *buf, int8_t snr, int16_t rssi );
uint8_t NodeIoBuildIrqPacket( uint8_t *buf, uint8_t gpio, int8_t snr, int16_t rssi );

/* Interrupção pendente (true + número do GPIO); limpa o pendente */
bool NodeIoTakeIrq( uint8_t *gpio );
bool NodeIoIrqPending( void );
void NodeIoRequeueIrq( uint8_t gpio );

/* Serial transparente (GPIO0/GPIO1 em função alternativa) */
bool    NodeIoTransparentActive( void );
bool    NodeIoTransparentPending( void );        /* há bytes esperando */
bool    NodeIoTransparentReady( void );          /* bytes + 50 ms de silêncio (ou buffer cheio) */
uint8_t NodeIoTransparentTake( uint8_t *buf, uint8_t max );
void    NodeIoTransparentWrite( const uint8_t *buf, uint16_t len );
void    NodeIoTransparentSetBaudrate( uint32_t baud );

/* Relés e PZEM (node_relay.c / node_pzem.c) */
void NodeIoApplyPin( uint8_t gpio );                 /* reaplica o modo gravado do pino */
void NodeIoPresetLevel( uint8_t gpio, uint8_t level ); /* nível inicial da saída (antes do NodeIoInit) */
void NodeIoRelayDrive( uint8_t gpio, uint8_t level ); /* escreve a saída de um relé */
void NodeIoReleaseSerialPins( void );                /* GPIO0/GPIO1 livres (desliga a serial transparente) */

/* Texto "modo:pull" dos 10 GPIOs para o @C / config ("-" = não configurado) */
void NodeIoDescribe( char *out, uint16_t max );
const char* NodeIoModeName( uint8_t mode );

#endif /* NODE_IO_H */
