/*
 * node_log.c — log do nó em RAM (buffer circular), veja node_log.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "board-ext.h"
#include "node_log.h"

static char     Log[NODE_LOG_SIZE];
static uint32_t Head = 0;          /* total de bytes escritos desde que ligou */
static bool     Paused = false;
static bool     LineStart = true;

void NodeLogInit( void )
{
    Head = 0;
    LineStart = true;
    Paused = false;
}

void NodeLogPause( bool pause )
{
    Paused = pause;
}

static inline void Put( char c )
{
    Log[Head % NODE_LOG_SIZE] = c;
    Head++;
}

static void Store( const uint8_t *buf, size_t n )
{
    for( size_t i = 0; i < n; i++ )
    {
        char c = ( char )buf[i];
        if( c == '\r' )
        {
            continue;
        }
        if( LineStart && ( c != '\n' ) )
        {
            /* "[hh:mm:ss] " = tempo desde que ligou, no começo de cada linha */
            char ts[16];
            uint32_t s = AppUptimeMs( ) / 1000;
            int k = snprintf( ts, sizeof( ts ), "[%02lu:%02lu:%02lu] ", ( unsigned long )( s / 3600 ),
                              ( unsigned long )( ( s / 60 ) % 60 ), ( unsigned long )( s % 60 ) );
            for( int j = 0; j < k; j++ )
            {
                Put( ts[j] );
            }
            LineStart = false;
        }
        Put( c );
        if( c == '\n' )
        {
            LineStart = true;
        }
    }
}

void NodeLogWrite( const uint8_t *buf, size_t n )
{
    if( !Paused )
    {
        Store( buf, n );
    }
}

void NodeLogLine( const char *fmt, ... )
{
    char line[120];
    va_list ap;
    va_start( ap, fmt );
    int k = vsnprintf( line, sizeof( line ) - 2, fmt, ap );
    va_end( ap );
    if( k < 0 )
    {
        return;
    }
    if( k > ( int )sizeof( line ) - 3 )
    {
        k = sizeof( line ) - 3;
    }
    if( !LineStart )
    {
        Store( ( const uint8_t* )"\n", 1 );    /* fecha uma linha que ficou pela metade */
    }
    line[k++] = '\n';
    Store( ( const uint8_t* )line, ( size_t )k );
}

void NodeLogClear( void )
{
    Head = 0;
    LineStart = true;
}

uint32_t NodeLogHead( void )
{
    return Head;
}

static uint32_t Start( void )
{
    return ( Head > NODE_LOG_SIZE ) ? Head - NODE_LOG_SIZE : 0;
}

uint32_t NodeLogUsed( void )
{
    return Head - Start( );
}

static inline char At( uint32_t p )
{
    return Log[p % NODE_LOG_SIZE];
}

/* Primeiro byte de linha inteira a partir de p (o começo do buffer pode ter
 * metade de uma linha que já foi sobrescrita). */
static uint32_t AlignLine( uint32_t p )
{
    if( p == 0 )
    {
        return 0;
    }
    if( At( p - 1 ) == '\n' )
    {
        return p;
    }
    while( ( p < Head ) && ( At( p ) != '\n' ) )
    {
        p++;
    }
    return ( p < Head ) ? p + 1 : Head;
}

/* Fim das linhas completas (a última linha pode estar sendo escrita) */
static uint32_t EndOfLines( void )
{
    uint32_t s = Start( ), e = Head;
    while( ( e > s ) && ( At( e - 1 ) != '\n' ) )
    {
        e--;
    }
    return e;
}

void NodeLogDump( void )
{
    uint32_t p = AlignLine( Start( ) );
    printf( "Log do nó em RAM: %lu de %u bytes (o mais antigo primeiro)\r\n", ( unsigned long )( Head - p ),
            ( unsigned )NODE_LOG_SIZE );
    printf( "--------------------------------------------------\r\n" );
    for( ; p < Head; p++ )
    {
        char c = At( p );
        if( c == '\n' )
        {
            printf( "\r\n" );
        }
        else
        {
            putchar( c );
        }
    }
    if( !LineStart )
    {
        printf( "\r\n" );
    }
    printf( "--------------------------------------------------\r\n" );
}

void NodeLogDumpApp( uint32_t from )
{
    uint32_t s = Start( ), end = EndOfLines( );
    uint32_t p = from;
    if( ( p > Head ) || ( p < s ) )
    {
        p = AlignLine( s );                  /* o nó reiniciou ou os bytes já foram sobrescritos */
    }
    else
    {
        p = AlignLine( p );
    }
    uint32_t sent = 0;
    while( ( p < end ) && ( sent < NODE_LOG_CHUNK ) )
    {
        printf( "@L " );
        uint32_t n = 0;
        while( ( p < end ) && ( At( p ) != '\n' ) )
        {
            if( n < 200 )                    /* linhas enormes ficam cortadas */
            {
                putchar( At( p ) );
            }
            p++;
            n++;
        }
        p++;                                 /* o '\n' */
        printf( "\r\n" );
        sent += n + 1;
    }
    if( p > end )
    {
        p = end;
    }
    printf( "@LN {\"start\":%lu,\"next\":%lu,\"end\":%lu,\"size\":%u}\r\n", ( unsigned long )AlignLine( s ),
            ( unsigned long )p, ( unsigned long )end, ( unsigned )NODE_LOG_SIZE );
}

/* Gancho do _write (board.c): tudo o que sai no console passa por aqui */
void BoardConsoleTap( const uint8_t *buf, size_t count )
{
    NodeLogWrite( buf, count );
}
