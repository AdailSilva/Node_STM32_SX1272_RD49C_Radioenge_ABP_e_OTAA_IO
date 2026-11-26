/*!
 * \file      eeprom-board.c
 *
 * \brief     Target board EEPROM driver implementation
 *
 * \copyright Revised BSD License, see section \ref LICENSE.
 *
 * \code
 *                ______                              _
 *               / _____)             _              | |
 *              ( (____  _____ ____ _| |_ _____  ____| |__
 *               \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 *               _____) ) ____| | | || |_| ____( (___| | | |
 *              (______/|_____)_|_|_| \__)_____)\____)_| |_|
 *              (C)2013-2017 Semtech
 *
 * \endcode
 *
 * \author    Miguel Luis ( Semtech )
 *
 * \author    Gregory Cristian ( Semtech )
 */
#include "stm32l0xx.h"
#include "utilities.h"
#include "eeprom-board.h"

LmnStatus_t EepromMcuWriteBuffer( uint16_t addr, uint8_t *buffer, uint16_t size )
{
    /* RADIOENGE:
     *  - só grava os bytes que mudaram (a EEPROM de dados do STM32L0 aguenta
     *    ~100 mil ciclos por byte; a maior parte do contexto não muda);
     *  - interrupções desligadas só durante cada byte (~3 ms), não no buffer
     *    inteiro, para não atrasar o RTC, o rádio e a UART;
     *  - devolve erro se algum byte não gravar. */
    LmnStatus_t status = LMN_STATUS_OK;
    volatile uint8_t *ee = ( volatile uint8_t* )( DATA_EEPROM_BASE + addr );

    assert_param( buffer != NULL );
    if( ( ( uint32_t )addr + size ) > ( DATA_EEPROM_BANK2_END - DATA_EEPROM_BASE + 1 ) )
    {
        return LMN_STATUS_ERROR;
    }

    if( HAL_FLASHEx_DATAEEPROM_Unlock( ) != HAL_OK )
    {
        return LMN_STATUS_ERROR;
    }
    for( uint16_t i = 0; i < size; i++ )
    {
        if( ee[i] == buffer[i] )
        {
            continue;
        }
        CRITICAL_SECTION_BEGIN( );
        HAL_StatusTypeDef st = HAL_FLASHEx_DATAEEPROM_Program( FLASH_TYPEPROGRAMDATA_BYTE,
                                                               ( DATA_EEPROM_BASE + addr + i ), buffer[i] );
        CRITICAL_SECTION_END( );
        if( ( st != HAL_OK ) || ( ee[i] != buffer[i] ) )
        {
            status = LMN_STATUS_ERROR;
            break;
        }
    }
    HAL_FLASHEx_DATAEEPROM_Lock( );
    return status;
}

LmnStatus_t EepromMcuReadBuffer( uint16_t addr, uint8_t *buffer, uint16_t size )
{
    assert_param( ( DATA_EEPROM_BASE + addr ) >= DATA_EEPROM_BASE );
    assert_param( buffer != NULL );
    assert_param( size < ( DATA_EEPROM_BANK2_END - DATA_EEPROM_BASE ) );

    memcpy1( buffer, ( uint8_t* )( DATA_EEPROM_BASE + addr ), size );
    return LMN_STATUS_OK;
}

void EepromMcuSetDeviceAddr( uint8_t addr )
{
    assert_param( LMN_STATUS_ERROR );
}

LmnStatus_t EepromMcuGetDeviceAddr( void )
{
    assert_param( LMN_STATUS_ERROR );
    return 0;
}
