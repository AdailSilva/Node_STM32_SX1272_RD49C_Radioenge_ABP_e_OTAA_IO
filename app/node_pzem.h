/*
 * node_pzem.h — medidores de energia PZEM-004T v3 (Modbus RTU, 9600 8N1) na USART2,
 * pinos GPIO0 (RX, pino 6) e GPIO1 (TX, pino 7) do módulo.
 *
 * O modo PZEM e a serial transparente (AT+GPIOC=0:3) usam os mesmos pinos e a mesma
 * USART: são exclusivos. Ligar um desliga o outro (a configuração mais recente vence).
 * Com CONSOLE_UART = 2 o console ocupa a USART2 e não há PZEM.
 *
 * Tudo é assíncrono: um pedido entra numa fila curta, o quadro sai por interrupção,
 * a resposta chega por interrupção e um timer limita a espera (NODE_PZEM_TIMEOUT_MS).
 * O laço principal chama NodePzemProcess( ), que fecha o pedido e chama o "done".
 * Nada bloqueia o rádio nem o modo de baixo consumo (durante a troca de quadros o
 * MCU fica em SLEEP, ~0,5 s no máximo por medidor).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_PZEM_H
#define NODE_PZEM_H

#include <stdbool.h>
#include <stdint.h>
#include "node_config.h"

#define NODE_PZEM_BLOCK_LEN     18      /* bloco PZEM do protocolo ADAILSILVA_V1 (5.2) */
#define NODE_PZEM_TIMEOUT_MS    500     /* espera pela resposta de um medidor */
#define NODE_PZEM_POWER_MAX     23000   /* limite de potência (W), registrador 0x0001 */

typedef enum
{
    NODE_PZEM_OP_READ = 0,      /* 10 registradores de entrada (função 0x04) */
    NODE_PZEM_OP_RESET_ENERGY,  /* zera a energia acumulada (função 0x42) */
    NODE_PZEM_OP_POWER_LIMIT,   /* limite do alarme de potência (função 0x06, registrador 0x0001) */
    NODE_PZEM_OP_SET_ADDR,      /* endereço Modbus do medidor (função 0x06, registrador 0x0002) */
}NodePzemOp_t;

/* Quem pediu (devolvido no "done") */
#define NODE_PZEM_TAG_TELEMETRY 1
#define NODE_PZEM_TAG_V1        2
#define NODE_PZEM_TAG_LOCAL     3   /* console / comandos AT */

typedef void ( *NodePzemDone_t )( uint8_t tag, uint8_t ch, NodePzemOp_t op, bool ok );

/* Liga a USART2 nos pinos GPIO0/GPIO1 se NodeCfg.pzemCount > 0 (boot e depois de AT+PZEMCFG) */
void NodePzemInit( void );
/* Desliga o modo PZEM (pzemCount = 0) e solta a USART2 e os pinos; não grava */
void NodePzemDisable( void );
bool NodePzemActive( void );

/* Enfileira um pedido para o medidor "ch" (0..pzemCount-1). false = fila cheia ou canal
 * inexistente. arg: watts (POWER_LIMIT) ou endereço novo (SET_ADDR). */
bool NodePzemRequest( NodePzemOp_t op, uint8_t ch, uint16_t arg, uint8_t tag, NodePzemDone_t done );
bool NodePzemBusy( void );          /* há pedido em andamento ou na fila */
bool NodePzemHasEvent( void );      /* resposta/timeout esperando o NodePzemProcess( ) */
void NodePzemProcess( void );       /* laço principal */

/* Última leitura do medidor ch: bloco de 18 bytes do protocolo (big-endian).
 * Retorna false se nunca leu ou se a última tentativa falhou. ageS = idade em segundos. */
bool NodePzemLast( uint8_t ch, uint8_t block[NODE_PZEM_BLOCK_LEN], uint32_t *ageS );
/* true se a última tentativa de leitura de ch falhou (sem resposta / CRC) */
bool NodePzemLastFailed( uint8_t ch );

/* Pedido do console / AT: o resultado sai no console ("PZEM0: ...") quando chegar */
bool NodePzemRequestLocal( NodePzemOp_t op, uint8_t ch, uint16_t arg );

/* Texto da última leitura ("230,0 V 0,500 A ...") para o console e o AT */
void NodePzemFormat( uint8_t ch, char *out, uint16_t max );

/* Chamado pela USART2_IRQHandler (node_io.c) quando a serial transparente está desligada */
void NodePzemUsartIrq( void );

/* Modbus CRC-16 (polinômio 0xA001, início 0xFFFF) */
uint16_t NodePzemCrc16( const uint8_t *data, uint8_t len );

#endif /* NODE_PZEM_H */
