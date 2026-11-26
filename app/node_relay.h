/*
 * node_relay.h — relés (0..8 canais) em GPIOs do módulo Radioenge.
 *
 * Canal n = GPIO NodeCfg.relayPin[n] (AT+RELECFG). Qualquer GPIO0..GPIO9 serve
 * (os pinos do rádio SX1272 não são GPIOs do módulo e nunca aparecem aqui); GPIO0 e
 * GPIO1 só quando não estão com a serial (PZEM ou transparente). O pino de um relé
 * fica em saída push-pull e é só dele: AT+GPIOC / AT+WPIN nele respondem AT ERROR.
 *
 * "Ativo em nível baixo" (bit n de NodeCfg.relayActiveLow): o relé n liga com o pino
 * em 0, como nos módulos de relé com optoacoplador mais comuns.
 *
 * Os estados ficam na EEPROM (NODE_RELAY_OFFSET) e voltam no boot. Para poupar a
 * EEPROM, só se grava quando o estado muda, cada vez num registro diferente de um anel
 * de NODE_RELAY_SLOTS (≈ 3 milhões de mudanças até a EEPROM se gastar). Um PULSO em
 * andamento não é gravado: no boot o relé volta ao estado anterior a ele.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_RELAY_H
#define NODE_RELAY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define NODE_RELAY_SLOTS        32
#define NODE_RELAY_PULSE_MIN_MS 100UL
#define NODE_RELAY_PULSE_MAX_MS 86400000UL
#define NODE_RELAY_ALL          0xFF

#define NODE_RELAY_OFF          0
#define NODE_RELAY_ON           1
#define NODE_RELAY_TOGGLE       2

typedef enum
{
    NODE_RELAY_OK = 0,
    NODE_RELAY_ABSENT,          /* canal não configurado */
    NODE_RELAY_PARAM,           /* ação/estado/duração fora da faixa */
}NodeRelayStatus_t;

/* Boot: ANTES do NodeIoInit( ). Põe os pinos dos relés em saída e restaura os estados
 * gravados sem pulso falso na partida (o nível é definido antes do pino virar saída). */
void NodeRelayInit( void );

uint8_t  NodeRelayCount( void );
uint16_t NodeRelayMap( void );          /* bit n = relé n ligado (inclui os pulsos) */
uint16_t NodeRelayPulsing( void );      /* bit n = relé n em pulso */

NodeRelayStatus_t NodeRelaySet( uint8_t ch, uint8_t action );           /* ch 0xFF = todos */
NodeRelayStatus_t NodeRelayMask( uint16_t mask, uint16_t values );
NodeRelayStatus_t NodeRelayPulse( uint8_t ch, uint8_t state, uint32_t ms );   /* ch 0xFF = todos */

/* Reconfigura (AT+RELECFG): muda NodeCfg e os pinos, mas não grava a configuração
 * (quem chama grava). false = recusado (err explica). */
bool NodeRelayConfigure( uint8_t count, const uint8_t *pins, uint8_t activeLow, char *err, size_t errLen );

/* Mudança feita aqui no nó (console, AT): manda uma telemetria com os relés */
void NodeRelayRequestReport( void );
/* true (uma vez) quando um pulso terminou ou houve mudança local: telemetria com RELES */
bool NodeRelayTakeReport( void );

bool NodeRelayHasEvent( void );          /* fim de pulso ou gravação pendente */
void NodeRelayProcess( void );           /* laço principal: fim dos pulsos e gravação na EEPROM */

/* Texto "4,5:1,1" (pinos:ativo-baixo) ou "0" para o AT+RELECFG=? e o jconfig */
void NodeRelayDescribe( char *out, size_t max );

#endif /* NODE_RELAY_H */
