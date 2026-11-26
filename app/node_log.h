/*
 * node_log.h — log do nó em RAM (em escala de STM32L0).
 *
 * Tudo o que o firmware imprime no console por conta própria (banner, [TX], [RX],
 * [JOIN], [Classe], [Beacon]...) também vai para um buffer circular em RAM, com o
 * tempo desde que ligou no começo de cada linha. As respostas dos comandos do
 * console não entram (o "jstatus" a cada 5 s apagaria o resto); os comandos que
 * mudam alguma coisa entram como uma linha "> comando".
 *
 * Console:
 *   log                  mostra o log guardado (texto)
 *   log clear            apaga o log
 *   jlog [de]            modo app: linhas "@L <texto>" a partir do byte <de>, em
 *                        blocos de ~NODE_LOG_CHUNK bytes, e no fim
 *                        "@LN {"start":s,"next":n,"end":e,"size":t}"
 *                        (repita "jlog <next>" até next == end)
 *
 * O buffer fica em RAM: um reinício apaga o log.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_LOG_H
#define NODE_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NODE_LOG_SIZE
#define NODE_LOG_SIZE   3072u      /* bytes em RAM (o STM32L071 tem 20 KB) */
#endif
#define NODE_LOG_CHUNK  360u       /* bytes de texto por resposta do "jlog" */

void     NodeLogInit( void );
void     NodeLogPause( bool pause );                     /* respostas de comandos não entram */
void     NodeLogWrite( const uint8_t *buf, size_t n );    /* chamado pelo _write */
void     NodeLogLine( const char *fmt, ... );             /* linha avulsa só no log */
void     NodeLogClear( void );
uint32_t NodeLogHead( void );                             /* total de bytes já escritos */
uint32_t NodeLogUsed( void );                             /* bytes guardados agora */

/* Imprime o log no console: humano (texto) ou app ("@L"/"@LN" a partir de "from") */
void     NodeLogDump( void );
void     NodeLogDumpApp( uint32_t from );

#endif /* NODE_LOG_H */
