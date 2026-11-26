/*!
 * \file      uart-board.c
 *
 * \brief     UART do console (USART1 nos pinos 2/3 do módulo, ou USART2 em
 *            GPIO1/GPIO0), escrita direto nos registradores.
 *
 * Diferenças em relação ao uart-board.c da NucleoL073:
 *  - USART1 ou USART2, escolhido por CONSOLE_UART (board-config.h);
 *  - relógio do USART = HSI16, para a UART acordar o MCU do modo STOP
 *    pelo start bit (UESM + WUS = start bit). Assim o console responde
 *    mesmo com o nó dormindo entre um evento LoRaWAN e outro;
 *  - TX e RX por interrupção (TXE / RXNE) com as FIFOs da LoRaMac-node.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "stm32l0xx.h"
#include "utilities.h"
#include "board.h"
#include "board-ext.h"
#include "board-config.h"
#include "sysIrqHandlers.h"
#include "uart-board.h"

#if ( CONSOLE_UART == 2 )
#define CON_USART                 USART2
#define CON_IRQn                  USART2_IRQn
#define CON_AF                    GPIO_AF4_USART2
#define CON_CLK_ENABLE( )         __HAL_RCC_USART2_CLK_ENABLE( )
#define CON_CLK_DISABLE( )        __HAL_RCC_USART2_CLK_DISABLE( )
#define CON_FORCE_RESET( )        __HAL_RCC_USART2_FORCE_RESET( )
#define CON_RELEASE_RESET( )      __HAL_RCC_USART2_RELEASE_RESET( )
#define CON_SEL_HSI( )            __HAL_RCC_USART2_CONFIG( RCC_USART2CLKSOURCE_HSI )
#define CON_IRQHandler            USART2_IRQHandler
#else
#define CON_USART                 USART1
#define CON_IRQn                  USART1_IRQn
#define CON_AF                    GPIO_AF0_USART1
#define CON_CLK_ENABLE( )         __HAL_RCC_USART1_CLK_ENABLE( )
#define CON_CLK_DISABLE( )        __HAL_RCC_USART1_CLK_DISABLE( )
#define CON_FORCE_RESET( )        __HAL_RCC_USART1_FORCE_RESET( )
#define CON_RELEASE_RESET( )      __HAL_RCC_USART1_RELEASE_RESET( )
#define CON_SEL_HSI( )            __HAL_RCC_USART1_CONFIG( RCC_USART1CLKSOURCE_HSI )
#define CON_IRQHandler            USART1_IRQHandler
#endif

static Uart_t *ConUart = NULL;

void UartMcuInit( Uart_t *obj, UartId_t uartId, PinNames tx, PinNames rx )
{
    obj->UartId = uartId;
    ConUart = obj;

    CON_FORCE_RESET( );
    CON_RELEASE_RESET( );
    CON_SEL_HSI( );
    CON_CLK_ENABLE( );

    GpioInit( &obj->Tx, tx, PIN_ALTERNATE_FCT, PIN_PUSH_PULL, PIN_PULL_UP, CON_AF );
    GpioInit( &obj->Rx, rx, PIN_ALTERNATE_FCT, PIN_PUSH_PULL, PIN_PULL_UP, CON_AF );
}

void UartMcuConfig( Uart_t *obj, UartMode_t mode, uint32_t baudrate, WordLength_t wordLength, StopBits_t stopBits,
                    Parity_t parity, FlowCtrl_t flowCtrl )
{
    ( void )wordLength;
    ( void )parity;
    ( void )flowCtrl;

    CON_USART->CR1 = 0;
    CON_USART->CR2 = ( stopBits == UART_2_STOP_BIT ) ? USART_CR2_STOP_1 : 0;
    /* Wake-up do STOP pelo start bit (WUS = 10) */
    CON_USART->CR3 = USART_CR3_WUS_1;
    /* HSI16 = 16 MHz, oversampling 16 */
    CON_USART->BRR = ( 16000000UL + baudrate / 2 ) / baudrate;

    uint32_t cr1 = USART_CR1_UE | USART_CR1_UESM | USART_CR1_RXNEIE;
    if( mode != RX_ONLY )
    {
        cr1 |= USART_CR1_TE;
    }
    if( mode != TX_ONLY )
    {
        cr1 |= USART_CR1_RE;
    }
    CON_USART->CR1 = cr1;

    /* Linhas diretas do EXTI do USART1 (25) e USART2 (26): já vêm ligadas no reset
     * do STM32L0, mas deixamos explícito (acordar do STOP depende delas). */
    EXTI->IMR |= ( 1UL << 25 ) | ( 1UL << 26 );

    HAL_NVIC_SetPriority( CON_IRQn, 1, 0 );
    HAL_NVIC_EnableIRQ( CON_IRQn );
}

void UartMcuDeInit( Uart_t *obj )
{
    HAL_NVIC_DisableIRQ( CON_IRQn );
    CON_USART->CR1 = 0;
    CON_FORCE_RESET( );
    CON_RELEASE_RESET( );
    CON_CLK_DISABLE( );

    GpioInit( &obj->Tx, obj->Tx.pin, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
    GpioInit( &obj->Rx, obj->Rx.pin, PIN_ANALOGIC, PIN_PUSH_PULL, PIN_NO_PULL, 0 );
}

uint8_t UartMcuPutChar( Uart_t *obj, uint8_t data )
{
    uint8_t ret = 1;

    CRITICAL_SECTION_BEGIN( );
    if( IsFifoFull( &obj->FifoTx ) == false )
    {
        FifoPush( &obj->FifoTx, data );
        CON_USART->CR1 |= USART_CR1_TXEIE;
        ret = 0;
    }
    CRITICAL_SECTION_END( );
    return ret;
}

uint8_t UartMcuGetChar( Uart_t *obj, uint8_t *data )
{
    uint8_t ret = 1;

    CRITICAL_SECTION_BEGIN( );
    if( IsFifoEmpty( &obj->FifoRx ) == false )
    {
        *data = FifoPop( &obj->FifoRx );
        ret = 0;
    }
    CRITICAL_SECTION_END( );
    return ret;
}

uint8_t UartMcuPutBuffer( Uart_t *obj, uint8_t *buffer, uint16_t size )
{
    for( uint16_t i = 0; i < size; i++ )
    {
        /* Espera haver lugar na FIFO (a interrupção de TX vai esvaziando). */
        uint32_t guard = 0;
        while( UartMcuPutChar( obj, buffer[i] ) != 0 )
        {
            if( ( __get_PRIMASK( ) != 0 ) || ( ++guard > 2000000UL ) )
            {
                return 1; /* com interrupções desligadas a FIFO não esvazia */
            }
        }
    }
    return 0;
}

uint8_t UartMcuGetBuffer( Uart_t *obj, uint8_t *buffer, uint16_t size, uint16_t *nbReadBytes )
{
    uint16_t n = 0;

    while( ( n < size ) && ( UartMcuGetChar( obj, buffer + n ) == 0 ) )
    {
        n++;
    }
    *nbReadBytes = n;
    return ( n == 0 ) ? 1 : 0;
}

/* AT+BAUDRATE: troca a velocidade do console sem reiniciar o USART */
void BoardSetConsoleBaudrate( uint32_t baudrate )
{
    if( baudrate == 0 )
    {
        return;
    }
    UartMcuFlush( );
    CON_USART->CR1 &= ~USART_CR1_UE;
    CON_USART->BRR = ( 16000000UL + baudrate / 2 ) / baudrate;
    CON_USART->CR1 |= USART_CR1_UE;
}

bool UartMcuTxBusy( void )
{
    if( ConUart == NULL )
    {
        return false;
    }
    return ( IsFifoEmpty( &ConUart->FifoTx ) == false ) || ( ( CON_USART->ISR & USART_ISR_TC ) == 0 );
}

void UartMcuFlush( void )
{
    uint32_t guard = 0;
    while( UartMcuTxBusy( ) && ( ++guard < 4000000UL ) )
    {
    }
}

void CON_IRQHandler( void )
{
    uint32_t isr = CON_USART->ISR;

    if( isr & ( USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE ) )
    {
        CON_USART->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
    }
    if( isr & USART_ISR_WUF )
    {
        CON_USART->ICR = USART_ICR_WUCF;
        BoardConsoleRxActivity( );      /* acordou por início de byte (STOP econômico) */
    }
    if( isr & USART_ISR_RXNE )
    {
        uint8_t c = ( uint8_t )CON_USART->RDR;
        if( ( ConUart != NULL ) && ( IsFifoFull( &ConUart->FifoRx ) == false ) )
        {
            FifoPush( &ConUart->FifoRx, c );
        }
        BoardConsoleRxActivity( );
        if( ( ConUart != NULL ) && ( ConUart->IrqNotify != NULL ) )
        {
            ConUart->IrqNotify( UART_NOTIFY_RX );
        }
    }
    if( ( CON_USART->CR1 & USART_CR1_TXEIE ) && ( isr & USART_ISR_TXE ) )
    {
        if( ( ConUart != NULL ) && ( IsFifoEmpty( &ConUart->FifoTx ) == false ) )
        {
            CON_USART->TDR = FifoPop( &ConUart->FifoTx );
        }
        else
        {
            CON_USART->CR1 &= ~USART_CR1_TXEIE;
        }
    }
}
