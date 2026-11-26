/*
 * node_pzem.c — medidores PZEM-004T v3 por Modbus RTU na USART2 (veja node_pzem.h).
 *
 * Quadros (endereço a = NodeCfg.pzemAddr[canal]; F8 = genérico, um medidor só):
 *   ler 10 registradores de entrada   a 04 00 00 00 0A crc  -> a 04 14 <20 bytes> crc (25)
 *   zerar energia                     a 42 crc              -> a 42 crc (4)
 *   limite de potência (reg. 0x0001)  a 06 00 01 WW WW crc  -> eco (8)
 *   endereço novo (reg. 0x0002)       a 06 00 02 00 AA crc  -> eco (8)
 *   erro                                                    -> a (f|80) código crc (5)
 * Registradores de entrada: 0 tensão (0,1 V); 1-2 corrente (0,001 A, palavra baixa
 * primeiro); 3-4 potência (0,1 W); 5-6 energia (Wh); 7 frequência (0,1 Hz);
 * 8 fator de potência (0,01); 9 alarme (FFFF = potência acima do limite).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "stm32l0xx.h"
#include "utilities.h"
#include "board.h"
#include "board-ext.h"
#include "board-config.h"
#include "gpio.h"
#include "timer.h"
#include "node_config.h"
#include "node_io.h"
#include "node_pzem.h"

uint16_t NodePzemCrc16( const uint8_t *data, uint8_t len )
{
    uint16_t crc = 0xFFFF;
    while( len-- )
    {
        crc ^= *data++;
        for( uint8_t i = 0; i < 8; i++ )
        {
            crc = ( crc & 1 ) ? ( uint16_t )( ( crc >> 1 ) ^ 0xA001 ) : ( uint16_t )( crc >> 1 );
        }
    }
    return crc;
}

#if ( CONSOLE_UART != 2 )

#define PZEM_BAUD           9600UL
#define PZEM_QUEUE          6

typedef struct
{
    uint8_t        op;
    uint8_t        ch;
    uint8_t        tag;
    uint16_t       arg;
    NodePzemDone_t done;
}PzemJob_t;

typedef struct
{
    uint8_t  block[NODE_PZEM_BLOCK_LEN];
    uint32_t atMs;
    uint8_t  state;             /* 0 = nunca leu, 1 = ok, 2 = última tentativa falhou */
}PzemReading_t;

static bool On = false;
static Gpio_t PinTx, PinRx;
static TimerEvent_t PzemTimer;
static bool TimerReady = false;

static PzemJob_t Queue[PZEM_QUEUE];
static uint8_t QHead = 0, QCount = 0;
static PzemJob_t Cur;
static volatile bool CurActive = false;

static uint8_t Tx[8];
static volatile uint8_t TxLen = 0, TxPos = 0;
static uint8_t Rx[25];
static volatile uint8_t RxLen = 0, RxExpect = 0;
static volatile bool RxDone = false, TimedOut = false;

static PzemReading_t Readings[NODE_PZEM_MAX];

static void OnPzemTimer( void *context )
{
    ( void )context;
    TimerStop( &PzemTimer );
    TimedOut = true;                        /* o laço principal fecha o pedido */
}

static void UsartStart( void )
{
    __HAL_RCC_USART2_FORCE_RESET( );
    __HAL_RCC_USART2_RELEASE_RESET( );
    __HAL_RCC_USART2_CONFIG( RCC_USART2CLKSOURCE_HSI );
    __HAL_RCC_USART2_CLK_ENABLE( );
    /* GPIO1 = PA2 = TX, GPIO0 = PA3 = RX (pull-up: linha parada em 1 sem o PZEM) */
    GpioInit( &PinTx, MODULE_GPIO1, PIN_ALTERNATE_FCT, PIN_PUSH_PULL, PIN_NO_PULL, GPIO_AF4_USART2 );
    GpioInit( &PinRx, MODULE_GPIO0, PIN_ALTERNATE_FCT, PIN_PUSH_PULL, PIN_PULL_UP, GPIO_AF4_USART2 );
    USART2->CR1 = 0;
    USART2->CR2 = 0;
    USART2->CR3 = USART_CR3_WUS_1;          /* acorda do STOP pelo start bit */
    USART2->BRR = ( 16000000UL + PZEM_BAUD / 2 ) / PZEM_BAUD;
    USART2->CR1 = USART_CR1_UE | USART_CR1_UESM | USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE;
    EXTI->IMR |= ( 1UL << 26 );
    HAL_NVIC_SetPriority( USART2_IRQn, 1, 0 );
    HAL_NVIC_EnableIRQ( USART2_IRQn );
}

static void UsartStop( void )
{
    HAL_NVIC_DisableIRQ( USART2_IRQn );
    USART2->CR1 = 0;
    __HAL_RCC_USART2_CLK_DISABLE( );
}

/* Fecha o pedido em andamento e a fila (modo PZEM desligado): todos recebem "falhou" */
static void AbortAll( void )
{
    if( TimerReady )
    {
        TimerStop( &PzemTimer );
    }
    PzemJob_t pending[PZEM_QUEUE + 1];
    uint8_t n = 0;
    if( CurActive )
    {
        pending[n++] = Cur;
        CurActive = false;
    }
    while( QCount )
    {
        pending[n++] = Queue[QHead];
        QHead = ( uint8_t )( ( QHead + 1 ) % PZEM_QUEUE );
        QCount--;
    }
    for( uint8_t i = 0; i < n; i++ )
    {
        if( pending[i].done != NULL )
        {
            pending[i].done( pending[i].tag, pending[i].ch, ( NodePzemOp_t )pending[i].op, false );
        }
    }
}

void NodePzemInit( void )
{
    if( !TimerReady )
    {
        TimerInit( &PzemTimer, OnPzemTimer );
        TimerReady = true;
    }
    if( NodeCfg.pzemCount == 0 )
    {
        if( On )
        {
            AbortAll( );
            UsartStop( );
            On = false;
        }
        return;
    }
    AbortAll( );
    UsartStart( );
    On = true;
    memset( Readings, 0, sizeof( Readings ) );
}

void NodePzemDisable( void )
{
    NodeCfg.pzemCount = 0;
    if( On )
    {
        AbortAll( );
        UsartStop( );
        On = false;
        NodeIoApplyPin( 0 );
        NodeIoApplyPin( 1 );
    }
}

bool NodePzemActive( void )
{
    return On;
}

bool NodePzemRequest( NodePzemOp_t op, uint8_t ch, uint16_t arg, uint8_t tag, NodePzemDone_t done )
{
    if( !On || ( ch >= NodeCfg.pzemCount ) || ( QCount >= PZEM_QUEUE ) )
    {
        return false;
    }
    PzemJob_t *j = &Queue[( QHead + QCount ) % PZEM_QUEUE];
    j->op = ( uint8_t )op;
    j->ch = ch;
    j->arg = arg;
    j->tag = tag;
    j->done = done;
    QCount++;
    return true;
}

bool NodePzemBusy( void )
{
    return On && ( CurActive || ( QCount > 0 ) );
}

bool NodePzemHasEvent( void )
{
    return On && ( ( CurActive && ( RxDone || TimedOut ) ) || ( !CurActive && ( QCount > 0 ) ) );
}

static void StartJob( void )
{
    Cur = Queue[QHead];
    QHead = ( uint8_t )( ( QHead + 1 ) % PZEM_QUEUE );
    QCount--;

    uint8_t a = NodeCfg.pzemAddr[Cur.ch];
    uint8_t n = 0;
    Tx[n++] = a;
    switch( Cur.op )
    {
    case NODE_PZEM_OP_READ:
        Tx[n++] = 0x04; Tx[n++] = 0x00; Tx[n++] = 0x00; Tx[n++] = 0x00; Tx[n++] = 0x0A;
        RxExpect = 25;
        break;
    case NODE_PZEM_OP_RESET_ENERGY:
        Tx[n++] = 0x42;
        RxExpect = 4;
        break;
    default:            /* POWER_LIMIT / SET_ADDR: escreve um registrador */
        Tx[n++] = 0x06;
        Tx[n++] = 0x00;
        Tx[n++] = ( Cur.op == NODE_PZEM_OP_POWER_LIMIT ) ? 0x01 : 0x02;
        Tx[n++] = ( uint8_t )( Cur.arg >> 8 );
        Tx[n++] = ( uint8_t )Cur.arg;
        RxExpect = 8;
        break;
    }
    uint16_t crc = NodePzemCrc16( Tx, n );
    Tx[n++] = ( uint8_t )crc;
    Tx[n++] = ( uint8_t )( crc >> 8 );

    CRITICAL_SECTION_BEGIN( );
    USART2->RQR = USART_RQR_RXFRQ;          /* descarta lixo da linha */
    RxLen = 0;
    RxDone = false;
    TimedOut = false;
    TxLen = n;
    TxPos = 0;
    CurActive = true;
    CRITICAL_SECTION_END( );
    BoardConsoleRxActivity( );              /* SLEEP (não STOP) durante a troca de quadros */
    TimerSetValue( &PzemTimer, NODE_PZEM_TIMEOUT_MS );
    TimerStart( &PzemTimer );
    USART2->CR1 |= USART_CR1_TXEIE;         /* o quadro sai pela interrupção */
}

static uint16_t Reg( uint8_t i )
{
    return ( uint16_t )( ( Rx[3 + 2 * i] << 8 ) | Rx[4 + 2 * i] );
}

static void PutU32( uint8_t *p, uint32_t v )
{
    p[0] = ( uint8_t )( v >> 24 ); p[1] = ( uint8_t )( v >> 16 ); p[2] = ( uint8_t )( v >> 8 ); p[3] = ( uint8_t )v;
}

static bool CheckReply( void )
{
    uint8_t n = RxLen;
    if( !RxDone || ( n < 4 ) || ( n != RxExpect ) )
    {
        return false;
    }
    uint16_t crc = NodePzemCrc16( Rx, ( uint8_t )( n - 2 ) );
    if( ( Rx[n - 2] != ( uint8_t )crc ) || ( Rx[n - 1] != ( uint8_t )( crc >> 8 ) ) )
    {
        return false;
    }
    if( ( Tx[0] != NODE_PZEM_ADDR_GENERIC ) && ( Rx[0] != Tx[0] ) )
    {
        return false;
    }
    if( Rx[1] != Tx[1] )
    {
        return false;                       /* resposta de erro (função | 0x80) */
    }
    if( ( Cur.op == NODE_PZEM_OP_READ ) && ( Rx[2] != 20 ) )
    {
        return false;
    }
    return true;
}

void NodePzemProcess( void )
{
    if( !On )
    {
        return;
    }
    if( CurActive && ( RxDone || TimedOut ) )
    {
        TimerStop( &PzemTimer );
        USART2->CR1 &= ~USART_CR1_TXEIE;
        bool ok = CheckReply( );
        CurActive = false;
        uint8_t ch = Cur.ch;
        if( Cur.op == NODE_PZEM_OP_READ )
        {
            PzemReading_t *r = &Readings[ch];
            r->atMs = ( uint32_t )TimerGetCurrentTime( );
            if( ok )
            {
                uint8_t *b = r->block;
                b[0] = Rx[3];                                   /* tensão (registrador 0) */
                b[1] = Rx[4];
                PutU32( b + 2, ( ( uint32_t )Reg( 2 ) << 16 ) | Reg( 1 ) );    /* corrente */
                PutU32( b + 6, ( ( uint32_t )Reg( 4 ) << 16 ) | Reg( 3 ) );    /* potência */
                PutU32( b + 10, ( ( uint32_t )Reg( 6 ) << 16 ) | Reg( 5 ) );   /* energia */
                b[14] = Rx[17];                                 /* frequência (registrador 7) */
                b[15] = Rx[18];
                uint16_t pf = Reg( 8 );
                b[16] = ( uint8_t )( ( pf > 255 ) ? 255 : pf );
                b[17] = Reg( 9 ) ? 1 : 0;
                r->state = 1;
            }
            else
            {
                r->state = 2;
            }
        }
        else if( ok && ( Cur.op == NODE_PZEM_OP_SET_ADDR ) )
        {
            NodeCfg.pzemAddr[ch] = ( uint8_t )Cur.arg;      /* quem pediu grava */
        }
        if( !ok )
        {
            printf( "[PZEM] Medidor %u (endereço %02X): %s.\r\n", ch, Tx[0],
                    TimedOut && !RxDone ? ( RxLen ? "resposta incompleta" : "sem resposta" ) : "resposta inválida" );
        }
        if( Cur.done != NULL )
        {
            Cur.done( Cur.tag, ch, ( NodePzemOp_t )Cur.op, ok );
        }
    }
    if( !CurActive && QCount )
    {
        StartJob( );
    }
}

bool NodePzemLast( uint8_t ch, uint8_t block[NODE_PZEM_BLOCK_LEN], uint32_t *ageS )
{
    if( ( ch >= NODE_PZEM_MAX ) || ( Readings[ch].state != 1 ) )
    {
        return false;
    }
    if( block != NULL )
    {
        memcpy( block, Readings[ch].block, NODE_PZEM_BLOCK_LEN );
    }
    if( ageS != NULL )
    {
        *ageS = TimerGetElapsedTime( Readings[ch].atMs ) / 1000;
    }
    return true;
}

bool NodePzemLastFailed( uint8_t ch )
{
    return ( ch < NODE_PZEM_MAX ) && ( Readings[ch].state == 2 );
}

void NodePzemUsartIrq( void )
{
    uint32_t isr = USART2->ISR;
    if( isr & ( USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE ) )
    {
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
    }
    if( isr & USART_ISR_WUF )
    {
        USART2->ICR = USART_ICR_WUCF;
        BoardConsoleRxActivity( );
    }
    if( isr & USART_ISR_RXNE )
    {
        uint8_t c = ( uint8_t )USART2->RDR;
        if( CurActive && !RxDone && ( RxLen < sizeof( Rx ) ) )
        {
            Rx[RxLen++] = c;
            if( ( RxLen == 2 ) && ( c & 0x80 ) )
            {
                RxExpect = 5;               /* resposta de erro do Modbus */
            }
            if( RxLen >= RxExpect )
            {
                RxDone = true;
            }
        }
        BoardConsoleRxActivity( );
    }
    if( ( USART2->CR1 & USART_CR1_TXEIE ) && ( isr & USART_ISR_TXE ) )
    {
        if( TxPos < TxLen )
        {
            USART2->TDR = Tx[TxPos++];
        }
        else
        {
            USART2->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}

#else   /* CONSOLE_UART == 2: a USART2 é o console, não há PZEM */

void NodePzemInit( void ) { }
void NodePzemDisable( void ) { NodeCfg.pzemCount = 0; }
bool NodePzemActive( void ) { return false; }
bool NodePzemRequest( NodePzemOp_t op, uint8_t ch, uint16_t arg, uint8_t tag, NodePzemDone_t done )
{
    ( void )op; ( void )ch; ( void )arg; ( void )tag; ( void )done;
    return false;
}
bool NodePzemBusy( void ) { return false; }
bool NodePzemHasEvent( void ) { return false; }
void NodePzemProcess( void ) { }
bool NodePzemLast( uint8_t ch, uint8_t block[NODE_PZEM_BLOCK_LEN], uint32_t *ageS )
{
    ( void )ch; ( void )block; ( void )ageS;
    return false;
}
bool NodePzemLastFailed( uint8_t ch ) { ( void )ch; return false; }
void NodePzemUsartIrq( void ) { }

#endif

static void OnLocalDone( uint8_t tag, uint8_t ch, NodePzemOp_t op, bool ok )
{
    char s[120];
    ( void )tag;
    if( !ok )
    {
        printf( "PZEM%u: sem resposta (FALHA_PERIFERICO)\r\n", ch );
        return;
    }
    switch( op )
    {
    case NODE_PZEM_OP_READ:
        NodePzemFormat( ch, s, sizeof( s ) );
        printf( "PZEM%u: %s\r\n", ch, s );
        break;
    case NODE_PZEM_OP_RESET_ENERGY:
        printf( "PZEM%u: energia zerada\r\n", ch );
        break;
    case NODE_PZEM_OP_POWER_LIMIT:
        printf( "PZEM%u: limite de potência gravado no medidor\r\n", ch );
        break;
    case NODE_PZEM_OP_SET_ADDR:
        NodeConfigSave( );                  /* NodePzemProcess( ) já trocou o endereço */
        printf( "PZEM%u: endereço Modbus agora %02X (gravado)\r\n", ch, NodeCfg.pzemAddr[ch] );
        break;
    }
}

bool NodePzemRequestLocal( NodePzemOp_t op, uint8_t ch, uint16_t arg )
{
    return NodePzemRequest( op, ch, arg, NODE_PZEM_TAG_LOCAL, OnLocalDone );
}

void NodePzemFormat( uint8_t ch, char *out, uint16_t max )
{
    uint8_t b[NODE_PZEM_BLOCK_LEN];
    uint32_t age = 0;
    if( !NodePzemLast( ch, b, &age ) )
    {
        snprintf( out, max, "%s", NodePzemLastFailed( ch ) ? "sem resposta na última leitura" : "sem leitura" );
        return;
    }
    uint32_t v = ( ( uint32_t )b[0] << 8 ) | b[1];
    uint32_t i = ( ( uint32_t )b[2] << 24 ) | ( ( uint32_t )b[3] << 16 ) | ( ( uint32_t )b[4] << 8 ) | b[5];
    uint32_t p = ( ( uint32_t )b[6] << 24 ) | ( ( uint32_t )b[7] << 16 ) | ( ( uint32_t )b[8] << 8 ) | b[9];
    uint32_t e = ( ( uint32_t )b[10] << 24 ) | ( ( uint32_t )b[11] << 16 ) | ( ( uint32_t )b[12] << 8 ) | b[13];
    uint32_t f = ( ( uint32_t )b[14] << 8 ) | b[15];
    snprintf( out, max, "%lu,%lu V · %lu,%03lu A · %lu,%lu W · %lu Wh · %lu,%lu Hz · FP %u,%02u · alarme %s · há %lu s",
              ( unsigned long )( v / 10 ), ( unsigned long )( v % 10 ), ( unsigned long )( i / 1000 ), ( unsigned long )( i % 1000 ),
              ( unsigned long )( p / 10 ), ( unsigned long )( p % 10 ), ( unsigned long )e,
              ( unsigned long )( f / 10 ), ( unsigned long )( f % 10 ), b[16] / 100, b[16] % 100, b[17] ? "sim" : "não",
              ( unsigned long )age );
}
