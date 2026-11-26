/*
 * at_cmd.h — comandos AT do firmware original do Módulo LoRaWAN EndDevice
 * Radioenge (Lora_AT_v1r3s35), no mesmo formato e com as mesmas respostas:
 *
 *   AT+CMD=?   lê (devolve só o valor)
 *   AT+CMD=x   grava (AT OK / AT PARAM ERROR ...)
 *   AT+CMD     executa (AT+JOIN, ATZ)
 *   AT+CMD?    ajuda do comando
 *
 * Respostas: AT OK · AT ERROR · AT PARAM ERROR · AT BUSY ERROR · AT TX OK ·
 * AT TX LENGTH ERROR · AT NO NETWORK JOINED · AT ACK OK · AT ACK ERROR ·
 * AT JOIN OK · AT JOIN ERROR · AT ALREADY JOINED. Downlinks: RX:<hex>:<RSSI>:<SNR>.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AT_CMD_H
#define AT_CMD_H

#include <stdbool.h>
#include <stdint.h>

/* true se a linha é um comando AT ("AT", "ATZ", "AT+...", "AT?") */
bool AtIsAtLine( const char *line );

/* Executa uma linha AT no console. false = a resposta foi um erro. */
bool AtExecute( char *line );

/* true se o último AtExecute( ) gravou a configuração na EEPROM */
bool AtSavedConfig( void );

/* AT por downlink: executa um ou mais comandos (separados por ';', CR ou LF)
 * e devolve em out as respostas, separadas por '\n'. Retorna o tamanho. */
uint16_t AtExecuteRemote( const uint8_t *cmds, uint8_t len, char *out, uint16_t max );

/* Respostas assíncronas (AT JOIN OK, AT ACK OK...) */
void AtPrintAsync( const char *msg );

/* Downlink no formato do original: RX:<payload hex>:<RSSI>:<SNR> */
void AtPrintRx( const uint8_t *data, uint8_t len, int16_t rssi, int8_t snr );

/* Lista dos comandos AT (AT? / AT+HELP) */
void AtPrintHelp( void );

#endif /* AT_CMD_H */
