/*
 * node_v1.h — protocolo de aplicação ADAILSILVA_V1 (doc/PROTOCOLO_ADAILSILVA_V1.md).
 *
 *   FPort 10 (downlink)  comando   seq | opcode | parâmetros
 *   FPort 11 (uplink)    resposta  seq | opcode | status | dados
 *   FPort 20 (uplink)    telemetria em blocos TLV: tipo | canal | tamanho | valor
 *
 * Deduplicação: o último seq (≠ 0) e a resposta dada ficam em RAM; o mesmo seq de
 * novo só reenvia a resposta guardada, sem executar outra vez.
 *
 * Os uplinks saem pela fila de prioridades do main.c (ServiceExtraUplinks): a resposta
 * logo depois da resposta de AT por downlink; a telemetria depois da serial
 * transparente e antes do keepalive. Resposta que não cabe no DR atual sai só com
 * seq | opcode | status (3 bytes); telemetria que não cabe é dividida em vários
 * uplinks, sempre com blocos inteiros.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_V1_H
#define NODE_V1_H

#include <stdbool.h>
#include <stdint.h>

#define NODE_V1_PORT_CMD        10
#define NODE_V1_PORT_REPLY      11
#define NODE_V1_PORT_TELEMETRY  20
#define NODE_V1_VERSION         1

/* status da resposta */
#define V1_OK                   0x00
#define V1_OPCODE_DESCONHECIDO  0x01
#define V1_PARAMETRO_INVALIDO   0x02
#define V1_PERIFERICO_AUSENTE   0x03
#define V1_FALHA_PERIFERICO     0x04
#define V1_OCUPADO              0x05

/* opcodes */
#define V1_OP_STATUS            0x01
#define V1_OP_INTERVALO         0x02
#define V1_OP_ENVIAR_AGORA      0x03
#define V1_OP_CONFIRMADO        0x04
#define V1_OP_CLASSE            0x05
#define V1_OP_REINICIAR         0x06
#define V1_OP_RELE_DEFINIR      0x20
#define V1_OP_RELE_PULSO        0x21
#define V1_OP_RELE_MASCARA      0x22
#define V1_OP_RELE_LER          0x23
#define V1_OP_PZEM_LER          0x30
#define V1_OP_PZEM_ZERAR        0x31
#define V1_OP_PZEM_LIMITE       0x32

/* tipos TLV da telemetria */
#define V1_TLV_RELES            0x01
#define V1_TLV_ENTRADAS         0x02
#define V1_TLV_PZEM             0x10
#define V1_TLV_PZEM_FALHA       0x11
#define V1_TLV_ALIMENTACAO      0x30
#define V1_TLV_BOOT             0x31

/* motivo do boot (5.4) */
#define V1_BOOT_DESCONHECIDO    0
#define V1_BOOT_ENERGIZACAO     1
#define V1_BOOT_SOFTWARE        2
#define V1_BOOT_WATCHDOG        3
#define V1_BOOT_BROWNOUT        4
#define V1_BOOT_PINO_RESET      5

void NodeV1Init( uint8_t bootReason );
uint8_t NodeV1BootReason( void );

/* Downlink na porta 10 */
void NodeV1OnCommand( const uint8_t *data, uint8_t len );

/* Telemetria: full = relés + PZEM (lidos agora) + alimentação; senão só os relés */
void NodeV1RequestTelemetry( bool full );

/* Laço principal: fim de pulso, leitura dos PZEM e montagem da telemetria */
void NodeV1Process( void );
bool NodeV1HasEvent( void );        /* o NodeV1Process( ) tem algo a fazer */
bool NodeV1HasUplink( void );       /* resposta ou telemetria esperando a MAC livre */

/* Fila de uplinks do main.c (MAC livre e sessão ativa). true = tratou (enviou ou adiou). */
bool NodeV1ServiceReply( void );
bool NodeV1ServiceTelemetry( void );

/* Fim de um uplink (OnTxData): a resposta do REINICIAR já saiu -> reinicia */
void NodeV1OnTxDone( uint8_t port );

#endif /* NODE_V1_H */
