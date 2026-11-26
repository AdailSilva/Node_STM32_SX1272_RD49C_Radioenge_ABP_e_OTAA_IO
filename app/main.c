/*
 * main.c — nó LoRaWAN no módulo Radioenge (STM32L071CZ + SX1272) com a
 * LoRaMac-node v4.7.1: um firmware só para as ativações ABP e OTAA (padrão de
 * fábrica em node_build.h, trocada em tempo de execução por AT+NJM ou "set njm"),
 * Classes A, B e C e os comandos AT do firmware original da Radioenge (at_cmd.c,
 * node_io.c), mais relés (node_relay.c), medidores PZEM-004T v3 (node_pzem.c) e o
 * protocolo de aplicação ADAILSILVA_V1 (node_v1.c: comandos na porta 10, respostas
 * na 11 e telemetria TLV na 20).
 *
 * Laço principal:
 *   - console (UART) para ver o estado e trocar a configuração;
 *   - LmHandlerProcess( ): eventos da LoRaMac-node;
 *   - uplink periódico: telemetria ADAILSILVA_V1 ou o payload configurado (AT+TELEM);
 *   - fim dos pulsos dos relés e leituras Modbus dos PZEM (assíncronas);
 *   - recalibração do LSI (quando o módulo não tem cristal de 32,768 kHz);
 *   - modo STOP entre eventos (o RTC acorda o MCU para as janelas RX, os
 *     beacons e os ping slots; a UART acorda para o console).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "utilities.h"
#include "board.h"
#include "board-ext.h"
#include "gpio.h"
#include "uart.h"
#include "timer.h"
#include "systime.h"
#include "RegionCommon.h"
#include "Commissioning.h"
#include "LmHandler.h"
#include "LmhpCompliance.h"
#include "NvmDataMgmt.h"
#include "node_build.h"
#include "node_config.h"
#include "console.h"
#include "node_log.h"
#include "sx1272.h"
#include "sx1272-board.h"
#include "delay.h"
#include "stm32l0xx.h"
#include "app.h"
#include "LoRaMacTest.h"
#include "node_io.h"
#include "at_cmd.h"
#include "node_relay.h"
#include "node_pzem.h"
#include "node_v1.h"

#define APP_DATA_BUFFER_MAX_SIZE    242
#define LSI_RECAL_PERIOD_MS         ( 10UL * 60UL * 1000UL )
#define CLASS_B_RETRY_MS            ( 60UL * 1000UL )
#define LED_FLASH_MS                60

AppStatus_t AppStatus;

/* O contexto da LoRaMac-node precisa caber antes da configuração na EEPROM */
_Static_assert( sizeof( LoRaMacNvmData_t ) <= NODE_NVM_LIMIT, "contexto LoRaMac maior que NODE_NVM_LIMIT" );

static uint8_t AppDataBuffer[APP_DATA_BUFFER_MAX_SIZE];
static LmHandlerAppData_t AppData = { .Buffer = AppDataBuffer, .BufferSize = 0, .Port = 0 };

static TimerEvent_t TxTimer;
static TimerEvent_t LedTxTimer;
static TimerEvent_t LedRxTimer;
static TimerEvent_t LedSerTimer;
static TimerEvent_t RebootTimer;
static TimerEvent_t ClassBRetryTimer;
static TimerEvent_t TxRetryTimer;
static TimerEvent_t KeepAliveTimer;
static TimerEvent_t RestartTimer;
static TimerEvent_t TranspTimer;

static volatile uint8_t IsMacProcessPending = 0;
static volatile uint8_t IsTxFramePending = 0;
static volatile uint8_t IsClassBRetryPending = 0;
static bool PendingUseCfgPayload = true;
static uint8_t PendingData[NODE_PAYLOAD_MAX];
static uint8_t PendingLen = 0;
static TimerTime_t LastRecal = 0;
static NodeMeta_t Meta;
static bool PendingApply = false;      /* configuração a aplicar quando a MAC ficar livre */
static bool DrSyncPending = false;     /* DR da MAC a acertar (depois do JOIN) quando ela ficar livre */
static bool LastTxDropped = false;     /* último uplink saiu vazio: o payload não coube no DR */
static uint8_t LastTxWanted = 0;       /* bytes de aplicação pedidos nesse uplink */
static char PendingClass = 0;          /* classe a pedir quando a MAC ficar livre */
static volatile LoRaMacStatus_t LastMcpsStatus = LORAMAC_STATUS_OK;
static volatile bool JoinRetryPending = false;   /* JOIN a pedir quando a MAC ficar livre */
static bool NvmRestored = false;
static TimerEvent_t JoinRetryTimer;
static void OnJoinRetryTimerEvent( void *context );

/* Comandos AT */
static volatile uint8_t IsKeepAlivePending = 0;
static volatile uint8_t RxBlinkLeft = 0;
static bool RestartPending = false;
static bool AtJoinActive = false;           /* JOIN pedido por AT+JOIN (limite NBTRIALS) */
static uint8_t AtJoinLeft = 0;
static bool AtTxActive = false;             /* último uplink saiu de um comando AT */
static bool AtTxConfirmed = false;
static uint8_t AtReply[NODE_IO_TRANSP_MAX]; /* resposta de AT recebido por downlink */
static uint8_t AtReplyLen = 0;
static uint8_t TranspBuf[NODE_IO_TRANSP_MAX];
static uint8_t TranspLen = 0;
static void OnKeepAliveTimerEvent( void *context );
static void OnRestartTimerEvent( void *context );
static void OnTranspTimerEvent( void *context );

/* LoRaMacIsBusy( ) sem o efeito colateral do LmHandlerIsBusy( ) (que dispara um JOIN) */
static bool MacBusy( void )
{
    return LoRaMacIsBusy( );
}

/* ------------------------------------------------------------------ */
static void OnMacProcessNotify( void );
static void OnNvmDataChange( LmHandlerNvmContextStates_t state, uint16_t size );
static void OnNetworkParametersChange( CommissioningParams_t *params );
static void OnMacMcpsRequest( LoRaMacStatus_t status, McpsReq_t *mcpsReq, TimerTime_t nextTxIn );
static void OnMacMlmeRequest( LoRaMacStatus_t status, MlmeReq_t *mlmeReq, TimerTime_t nextTxIn );
static void OnJoinRequest( LmHandlerJoinParams_t *params );
static void OnTxData( LmHandlerTxParams_t *params );
static void OnRxData( LmHandlerAppData_t *appData, LmHandlerRxParams_t *params );
static void OnClassChange( DeviceClass_t deviceClass );
static void OnBeaconStatusChange( LoRaMacHandlerBeaconParams_t *params );
static void OnSysTimeUpdate( bool isSynchronized, int32_t timeCorrection );

static void OnTxPeriodicityChanged( uint32_t periodicity );
static void OnTxFrameCtrlChanged( LmHandlerMsgTypes_t isTxConfirmed );
static void OnPingSlotPeriodicityChanged( uint8_t pingSlotPeriodicity );

static void OnTxTimerEvent( void *context );
static void OnLedTxTimerEvent( void *context );
static void OnLedRxTimerEvent( void *context );
static void OnLedSerTimerEvent( void *context );
static void RadioWatchdogBeforeTx( void );
static void OnRebootTimerEvent( void *context );
static void OnClassBRetryTimerEvent( void *context );
static void OnTxRetryTimerEvent( void *context );

static const char* MacStatusText( LoRaMacStatus_t st )
{
    switch( st )
    {
    case LORAMAC_STATUS_OK:                         return "ok";
    case LORAMAC_STATUS_BUSY:                       return "MAC ocupada";
    case LORAMAC_STATUS_PARAMETER_INVALID:          return "parâmetro inválido";
    case LORAMAC_STATUS_DATARATE_INVALID:           return "DR inválido";
    case LORAMAC_STATUS_NO_NETWORK_JOINED:          return "sem sessão";
    case LORAMAC_STATUS_LENGTH_ERROR:               return "payload grande demais para o DR";
    case LORAMAC_STATUS_DUTYCYCLE_RESTRICTED:       return "duty cycle";
    case LORAMAC_STATUS_NO_CHANNEL_FOUND:           return "nenhum canal habilitado";
    case LORAMAC_STATUS_NO_FREE_CHANNEL_FOUND:      return "nenhum canal livre";
    case LORAMAC_STATUS_BUSY_BEACON_RESERVED_TIME:  return "janela reservada do beacon";
    case LORAMAC_STATUS_BUSY_PING_SLOT_WINDOW_TIME: return "janela de ping slot";
    case LORAMAC_STATUS_BUSY_UPLINK_COLLISION:      return "colisão com ping slot";
    case LORAMAC_STATUS_CRYPTO_ERROR:               return "erro de criptografia";
    case LORAMAC_STATUS_FCNT_HANDLER_ERROR:         return "erro de FCnt";
    case LORAMAC_STATUS_CLASS_B_ERROR:              return "erro da Classe B";
    default:                                        return "erro";
    }
}

static bool MacStatusRetryable( LoRaMacStatus_t st )
{
    return ( st == LORAMAC_STATUS_BUSY ) || ( st == LORAMAC_STATUS_BUSY_BEACON_RESERVED_TIME ) ||
           ( st == LORAMAC_STATUS_BUSY_PING_SLOT_WINDOW_TIME ) || ( st == LORAMAC_STATUS_BUSY_UPLINK_COLLISION ) ||
           ( st == LORAMAC_STATUS_DUTYCYCLE_RESTRICTED ) || ( st == LORAMAC_STATUS_NO_FREE_CHANNEL_FOUND );
}

static LmHandlerCallbacks_t LmHandlerCallbacks =
{
    .GetBatteryLevel = BoardGetBatteryLevel,
    .GetTemperature = NULL,
    .GetRandomSeed = BoardGetRandomSeed,
    .OnMacProcess = OnMacProcessNotify,
    .OnNvmDataChange = OnNvmDataChange,
    .OnNetworkParametersChange = OnNetworkParametersChange,
    .OnMacMcpsRequest = OnMacMcpsRequest,
    .OnMacMlmeRequest = OnMacMlmeRequest,
    .OnJoinRequest = OnJoinRequest,
    .OnTxData = OnTxData,
    .OnRxData = OnRxData,
    .OnClassChange = OnClassChange,
    .OnBeaconStatusChange = OnBeaconStatusChange,
    .OnSysTimeUpdate = OnSysTimeUpdate,
};

static LmHandlerParams_t LmHandlerParams =
{
    .Region = LORAMAC_REGION_AU915,
    .AdrEnable = false,
    .IsTxConfirmed = LORAMAC_HANDLER_UNCONFIRMED_MSG,
    .TxDatarate = DR_5,
    .PublicNetworkEnable = LORAWAN_PUBLIC_NETWORK,
    .DutyCycleEnabled = false,           /* AU915 não tem limite de duty cycle */
    .DataBufferMaxSize = APP_DATA_BUFFER_MAX_SIZE,
    .DataBuffer = AppDataBuffer,
    .PingSlotPeriodicity = DEF_PING_PERIODICITY,
};

static LmhpComplianceParams_t LmhpComplianceParams =
{
    .FwVersion.Fields = { .Major = 0, .Minor = 0, .Patch = 1, .Revision = 0 },
    .OnTxPeriodicityChanged = OnTxPeriodicityChanged,
    .OnTxFrameCtrlChanged = OnTxFrameCtrlChanged,
    .OnPingSlotPeriodicityChanged = OnPingSlotPeriodicityChanged,
};

/* =================================================================== */
/*                         configuração -> MAC                         */
/* =================================================================== */
static void MibSet( MibRequestConfirm_t *m )
{
    LoRaMacStatus_t st = LoRaMacMibSetRequestConfirm( m );
    if( st != LORAMAC_STATUS_OK )
    {
        printf( "[MAC] MIB %d recusado (status %d)\r\n", ( int )m->Type, ( int )st );
    }
}

static void ApplyChannelMask( void )
{
    uint16_t mask[6];

    /* Sub-banda 0..7 (ChirpStack au915_<n>), 8 = todos, ou a máscara do AT+CHMASK */
    NodeConfigChannelMask( &NodeCfg, mask );

    MibRequestConfirm_t m;
    m.Type = MIB_CHANNELS_DEFAULT_MASK;
    m.Param.ChannelsDefaultMask = mask;
    MibSet( &m );
    m.Type = MIB_CHANNELS_MASK;
    m.Param.ChannelsMask = mask;
    MibSet( &m );
}

static int8_t TxPowerIndex( int8_t dbm )
{
    if( NodeCfg.txPowIdx != NODE_TXP_FROM_DBM )
    {
        /* AT+TXP: índice direto (15 = RFU no manual: vai para o menor, 14) */
        return ( int8_t )( ( NodeCfg.txPowIdx > 14 ) ? 14 : NodeCfg.txPowIdx );
    }
    /* AU915: EIRP = 30 dBm - 2 * índice; conduzida = EIRP - 2,15 dBi */
    int idx = ( int )( ( 27.85f - ( float )dbm ) / 2.0f + 0.5f );
    if( idx < 0 )
    {
        idx = 0;
    }
    if( idx > 14 )
    {
        idx = 14;
    }
    return ( int8_t )idx;
}

static void ApplyCredentials( void )
{
    MibRequestConfirm_t m;

    if( NodeIsOtaa( ) )
    {
        m.Type = MIB_DEV_EUI;     m.Param.DevEui = NodeCfg.devEui;   MibSet( &m );
        m.Type = MIB_JOIN_EUI;    m.Param.JoinEui = NodeCfg.joinEui; MibSet( &m );
        m.Type = MIB_APP_KEY;     m.Param.AppKey = NodeCfg.appKey;   MibSet( &m );
        m.Type = MIB_NWK_KEY;     m.Param.NwkKey = NodeCfg.nwkKey;   MibSet( &m );
        return;
    }
    /* ABP (AT+NJM=0): LoRaWAN 1.0.4 */
    m.Type = MIB_ABP_LORAWAN_VERSION; m.Param.AbpLrWanVersion.Value = ABP_ACTIVATION_LRWAN_VERSION; MibSet( &m );
    m.Type = MIB_DEV_EUI;     m.Param.DevEui = NodeCfg.devEui;   MibSet( &m );
    m.Type = MIB_JOIN_EUI;    m.Param.JoinEui = NodeCfg.joinEui; MibSet( &m );
    m.Type = MIB_NET_ID;      m.Param.NetID = NodeCfg.netId;     MibSet( &m );
    m.Type = MIB_DEV_ADDR;    m.Param.DevAddr = NodeCfg.devAddr; MibSet( &m );
    /* LoRaWAN 1.0.x: a NwkSKey vale para as três chaves de rede */
    m.Type = MIB_F_NWK_S_INT_KEY; m.Param.FNwkSIntKey = NodeCfg.nwkSKey; MibSet( &m );
    m.Type = MIB_S_NWK_S_INT_KEY; m.Param.SNwkSIntKey = NodeCfg.nwkSKey; MibSet( &m );
    m.Type = MIB_NWK_S_ENC_KEY;   m.Param.NwkSEncKey = NodeCfg.nwkSKey;  MibSet( &m );
    m.Type = MIB_APP_S_KEY;       m.Param.AppSKey = NodeCfg.appSKey;     MibSet( &m );
}

/* AT+PNM, AT+DCS, AT+JN1DL/JN2DL e, com rxWindows, AT+RX1DL/RX2DL e AT+RX2FQ/RX2DR.
 * As janelas RX só entram numa sessão nova ou em ABP: no OTAA o Join-accept
 * manda o RxDelay e o DR da RX2 da rede, que valem mais que a configuração. */
void AppApplyMacParams( bool rxWindows )
{
    MibRequestConfirm_t m;

    m.Type = MIB_PUBLIC_NETWORK;      m.Param.EnablePublicNetwork = NodeCfg.publicNet != 0; MibSet( &m );
    LoRaMacTestSetDutyCycleOn( NodeCfg.dutyCycle != 0 );
    m.Type = MIB_JOIN_ACCEPT_DELAY_1; m.Param.JoinAcceptDelay1 = NodeCfg.join1DelayMs; MibSet( &m );
    m.Type = MIB_JOIN_ACCEPT_DELAY_2; m.Param.JoinAcceptDelay2 = NodeCfg.join2DelayMs; MibSet( &m );
    if( rxWindows )
    {
        m.Type = MIB_RECEIVE_DELAY_1; m.Param.ReceiveDelay1 = NodeCfg.rx1DelayMs; MibSet( &m );
        m.Type = MIB_RECEIVE_DELAY_2; m.Param.ReceiveDelay2 = NodeCfg.rx2DelayMs; MibSet( &m );
        RxChannelParams_t rx2 = { .Frequency = NodeCfg.rx2Freq, .Datarate = NodeCfg.rx2Dr };
        m.Type = MIB_RX2_CHANNEL;         m.Param.Rx2Channel = rx2;        MibSet( &m );
        m.Type = MIB_RX2_DEFAULT_CHANNEL; m.Param.Rx2DefaultChannel = rx2; MibSet( &m );
        /* Classe C escuta na frequência/DR da RX2 */
        m.Type = MIB_RXC_CHANNEL;         m.Param.RxCChannel = rx2;        MibSet( &m );
    }
}

void AppApplyKeepalive( void )
{
    TimerStop( &KeepAliveTimer );
    IsKeepAlivePending = 0;
    if( NodeCfg.kaEnable && NodeCfg.kaPeriodMs )
    {
        TimerSetValue( &KeepAliveTimer, NodeCfg.kaPeriodMs );
        TimerStart( &KeepAliveTimer );
    }
}

void AppApplyRuntimeConfig( void )
{
    MibRequestConfirm_t m;

    if( MacBusy( ) )
    {
        /* a LoRaMac-node recusa MIBs com TX/RX em andamento: aplica na próxima folga */
        PendingApply = true;
        return;
    }
    PendingApply = false;

    LmHandlerParams.AdrEnable = NodeCfg.adr;
    LmHandlerParams.TxDatarate = ( int8_t )NodeCfg.dr;
    LmHandlerParams.IsTxConfirmed = NodeCfg.confirmed ? LORAMAC_HANDLER_CONFIRMED_MSG : LORAMAC_HANDLER_UNCONFIRMED_MSG;
    LmHandlerParams.PingSlotPeriodicity = NodeCfg.pingPeriodicity;

    m.Type = MIB_ADR;               m.Param.AdrEnable = NodeCfg.adr;                  MibSet( &m );
    if( NodeCfg.adr == 0 )
    {
        m.Type = MIB_CHANNELS_DATARATE; m.Param.ChannelsDatarate = ( int8_t )NodeCfg.dr; MibSet( &m );
        m.Type = MIB_CHANNELS_TX_POWER; m.Param.ChannelsTxPower = TxPowerIndex( NodeCfg.txPowDbm ); MibSet( &m );
    }
    m.Type = MIB_CHANNELS_DEFAULT_DATARATE; m.Param.ChannelsDefaultDatarate = ( int8_t )NodeCfg.dr; MibSet( &m );

    BoardSetStopAllowed( NodeCfg.lowPower != 0 );
    BoardSetConsoleAwakeInStop( NodeCfg.lowPower == 1 );
    BoardSetExternalPower( NodeCfg.extPower != 0 );

    TimerStop( &TxTimer );
    if( NodeCfg.txIntervalS )
    {
        TimerSetValue( &TxTimer, NodeCfg.txIntervalS * 1000UL );
        TimerStart( &TxTimer );
    }
    AppStatus.nextTxMs = TimerGetCurrentTime( ) + NodeCfg.txIntervalS * 1000UL;
    AppApplyKeepalive( );

    if( AppStatus.joined && ( AppStatus.activeClass != NodeCfg.devClass ) )
    {
        AppRequestClass( NodeCfg.devClass );
    }
}

static void SetMaxRxErrorForClock( void )
{
    /* erro do relógio ao longo da maior espera (RX2 do JOIN, 6 s) + 10 ms */
    const RtcClockInfo_t *ci = RtcGetClockInfo( );
    uint32_t err = 10 + ( uint32_t )( ( ( uint64_t )ci->PpmEstimate * 6000ULL ) / 1000000ULL );
    if( err > 300 )
    {
        err = 300;
    }
    LmHandlerSetSystemMaxRxError( err );
}

const char* AppClockSourceName( void )
{
    switch( RtcGetClockInfo( )->Source )
    {
    case RTC_CLOCK_LSE:         return "LSE 32,768 kHz (cristal)";
    case RTC_CLOCK_LSI_HSE_REF: return "LSI calibrado pelo cristal HSE";
    default:                    return "LSI calibrado pelo HSI16";
    }
}

/* ------------------------------------------------- autoteste (diag) --- */
static uint32_t ResetFlags = 0;        /* RCC->CSR no boot (bits 24..31) */
static bool CfgFromEeprom = false;

#define STACK_PAINT     0xA5A5A5A5UL
extern uint32_t _estack;               /* topo da RAM (linker) */
extern char end;                       /* fim do .bss = começo do heap (linker) */
extern void *_sbrk( ptrdiff_t incr );
static uint32_t *StackPaintFrom = NULL;

/* Pinta a RAM livre entre o heap e a pilha para medir depois o pico de uso da
 * pilha (o primeiro byte que deixou de ter o padrão). Chamada no início do main. */
static void StackPaint( void )
{
    uint32_t *from = ( uint32_t* )( ( ( uintptr_t )_sbrk( 0 ) + 512u + 3u ) & ~3u );   /* folga para o heap */
    uint32_t *sp = ( uint32_t* )__get_MSP( );
    StackPaintFrom = from;
    for( uint32_t *p = from; p < sp - 16; p++ )
    {
        *p = STACK_PAINT;
    }
}

void AppStackInfo( uint32_t *peak, uint32_t *total )
{
    uint32_t *top = &_estack, *p = StackPaintFrom;
    if( p == NULL )
    {
        *peak = *total = 0;
        return;
    }
    /* se o heap cresceu para dentro da área pintada, começa acima dele */
    uint32_t *brk = ( uint32_t* )( ( ( uintptr_t )_sbrk( 0 ) + 3u ) & ~3u );
    if( brk > p )
    {
        p = brk;
    }
    while( ( p < top ) && ( *p == STACK_PAINT ) )
    {
        p++;
    }
    *peak = ( uint32_t )( ( uintptr_t )top - ( uintptr_t )p );
    *total = ( uint32_t )( ( uintptr_t )top - ( uintptr_t )StackPaintFrom );
}

uint32_t AppHeapUsed( void )
{
    return ( uint32_t )( ( uintptr_t )_sbrk( 0 ) - ( uintptr_t )&end );
}

const char* AppResetCauseText( void )
{
    if( ResetFlags & RCC_CSR_IWDGRSTF ) return "watchdog (IWDG)";
    if( ResetFlags & RCC_CSR_WWDGRSTF ) return "watchdog (WWDG)";
    if( ResetFlags & RCC_CSR_LPWRRSTF ) return "baixo consumo (LPWR)";
    if( ResetFlags & RCC_CSR_SFTRSTF )  return "software (reboot, save, reset remoto)";
    if( ResetFlags & RCC_CSR_PORRSTF )  return "alimentação (power-on / brown-out)";
    if( ResetFlags & RCC_CSR_OBLRSTF )  return "option bytes";
    if( ResetFlags & RCC_CSR_FWRSTF )   return "firewall";
    if( ResetFlags & RCC_CSR_PINRSTF )  return "pino NRST (ST-LINK ou botão)";
    return "desconhecida";
}

/* Motivo do boot na tabela 5.4 do protocolo ADAILSILVA_V1. No STM32L0 a queda de
 * tensão (BOR) marca o mesmo PORRSTF da energização: as duas viram 1 (energização). */
static uint8_t BootReasonV1( void )
{
    if( ResetFlags & ( RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF ) ) return V1_BOOT_WATCHDOG;
    if( ResetFlags & RCC_CSR_SFTRSTF )  return V1_BOOT_SOFTWARE;
    if( ResetFlags & RCC_CSR_PORRSTF )  return V1_BOOT_ENERGIZACAO;
    if( ResetFlags & ( RCC_CSR_LPWRRSTF | RCC_CSR_OBLRSTF | RCC_CSR_FWRSTF ) ) return V1_BOOT_DESCONHECIDO;
    if( ResetFlags & RCC_CSR_PINRSTF )  return V1_BOOT_PINO_RESET;
    return V1_BOOT_DESCONHECIDO;
}

const char* AppResetCauseKey( void )
{
    if( ResetFlags & ( RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF ) ) return "wdg";
    if( ResetFlags & RCC_CSR_LPWRRSTF ) return "lpwr";
    if( ResetFlags & RCC_CSR_SFTRSTF )  return "sw";
    if( ResetFlags & RCC_CSR_PORRSTF )  return "por";
    if( ResetFlags & ( RCC_CSR_OBLRSTF | RCC_CSR_FWRSTF ) ) return "other";
    if( ResetFlags & RCC_CSR_PINRSTF )  return "pin";
    return "?";
}

bool AppConfigFromEeprom( void )
{
    return CfgFromEeprom;
}

bool AppMacBusy( void )
{
    return MacBusy( );
}

/* Registrador de versão do SX1272 (0x42): 0x22 no SX1272. 0x00 ou 0xFF = sem SPI. */
uint8_t AppRadioVersion( void )
{
    return SX1272Read( 0x42 );
}

uint16_t AppGetDevNonce( void )
{
    MibRequestConfirm_t m;
    m.Type = MIB_NVM_CTXS;
    if( LoRaMacMibGetRequestConfirm( &m ) != LORAMAC_STATUS_OK )
    {
        return 0;
    }
    return m.Param.Contexts->Crypto.DevNonce;
}

static void SaveDevNonce( void )
{
    if( !NodeIsOtaa( ) )
    {
        return;
    }
    uint16_t dn = AppGetDevNonce( );
    if( dn != Meta.lastDevNonce )
    {
        Meta.lastDevNonce = dn;
        NodeMetaSave( &Meta );
    }
}

/* =================================================================== */
/*                               ações                                 */
/* =================================================================== */
static const uint8_t *FindSeKey( LoRaMacNvmData_t *nvm, KeyIdentifier_t id )
{
    for( uint8_t i = 0; i < NUM_OF_KEYS; i++ )
    {
        if( nvm->SecureElement.KeyList[i].KeyID == id )
        {
            return nvm->SecureElement.KeyList[i].KeyValue;
        }
    }
    return NULL;
}

bool AppGetSessionKeys( uint32_t *devAddr, uint8_t nwkSKey[16], uint8_t appSKey[16] )
{
    if( !AppStatus.joined )
    {
        return false;
    }
    MibRequestConfirm_t m;
    m.Type = MIB_NVM_CTXS;
    if( LoRaMacMibGetRequestConfirm( &m ) != LORAMAC_STATUS_OK )
    {
        return false;
    }
    LoRaMacNvmData_t *nvm = m.Param.Contexts;
    /* LoRaWAN 1.0.x: FNwkSIntKey = SNwkSIntKey = NwkSEncKey = a NwkSKey do ChirpStack */
    const uint8_t *nwk = FindSeKey( nvm, F_NWK_S_INT_KEY );
    const uint8_t *app = FindSeKey( nvm, APP_S_KEY );
    if( ( nwk == NULL ) || ( app == NULL ) )
    {
        return false;
    }
    memcpy( nwkSKey, nwk, 16 );
    memcpy( appSKey, app, 16 );
    *devAddr = AppStatus.devAddr;
    return true;
}

/* =================================================================== */
/*        diagnóstico de sessão                                        */
/* =================================================================== */
/* A cada quadro que vai ao ar: DevAddr, NwkSKey, AppSKey, o quadro cifrado
 * ("Frame raw"), os contadores e o MIC (4 últimos bytes). O quadro vem do
 * gancho do rádio (sx1272-board.c) e é impresso aqui, no laço principal, fora
 * do tempo crítico da MAC. Vai só para o console (página/app ao vivo e
 * "Baixar sessão"), não para o log em RAM do nó: as chaves não ficam
 * guardadas lá (como no "> keys") e os 3 KB não somem em poucos envios.
 * Liga/desliga: "verbose 0|1" (vale na hora; padrão ligado). */
static void PrintRaw( const char *label, const uint8_t *f, uint8_t n )
{
    printf( "%s", label );
    for( uint8_t i = 0; i < n; i++ )
    {
        printf( "%02X ", f[i] );
    }
    printf( "\r\n" );
}

static void PrintMic( const uint8_t *f, uint8_t n )
{
    if( n >= 4 )
    {
        printf( "  MIC: %02X-%02X-%02X-%02X\r\n", f[n - 4], f[n - 3], f[n - 2], f[n - 1] );
    }
}

/* EUI do quadro (little-endian no ar) como é mostrado no ChirpStack */
static void PrintEuiLe( const char *label, const uint8_t *p )
{
    printf( "%s", label );
    for( int i = 7; i >= 0; i-- )
    {
        printf( "%02X", p[i] );
    }
    printf( "\r\n" );
}

static void PrintTxDiag( const uint8_t *f, uint8_t n )
{
    uint8_t mtype = f[0] >> 5;
    if( ( mtype == 0 ) && ( n == 23 ) )
    {
        /* Join-request: MHDR | JoinEUI | DevEUI | DevNonce | MIC (sem sessão ainda) */
        printf( "\r\nEV_TXSTART (Join-request)\r\n" );
        PrintEuiLe( "LoRaMac JoinEUI: ", f + 1 );
        PrintEuiLe( "LoRaMac DevEUI : ", f + 9 );
        printf( "LoRaMac DevNonce: %u\r\n\r\n", ( unsigned )( f[17] | ( f[18] << 8 ) ) );
        PrintRaw( "Frame raw: ", f, n );
        PrintMic( f, n );
        return;
    }
    if( ( mtype != 2 ) && ( mtype != 4 ) )
    {
        printf( "\r\nEV_TXSTART (MType %u)\r\n", mtype );
        PrintRaw( "Frame raw: ", f, n );
        PrintMic( f, n );
        return;
    }

    /* Uplink de dados: a ordem é a mesma de todos os logs (DevAddr, NwkSKey, AppSKey) */
    printf( "\r\nEV_TXSTART\r\n" );
    uint32_t da = 0;
    uint8_t nwk[16], app[16];
    char hex[33];
    if( AppGetSessionKeys( &da, nwk, app ) )
    {
        printf( "LoRaMac DevAddr: 0x%08lX\r\n", ( unsigned long )da );
        BytesToHex( nwk, 16, hex );
        printf( "LoRaMac NwkSKey: %s\r\n", hex );
        BytesToHex( app, 16, hex );
        printf( "LoRaMac AppSKey: %s\r\n", hex );
    }
    else
    {
        printf( "LoRaMac DevAddr: 0x%02X%02X%02X%02X (chaves da sessão indisponíveis)\r\n", f[4], f[3], f[2], f[1] );
    }
    printf( "\r\n" );
    PrintRaw( "Frame raw: ", f, n );

    /* Contadores de 32 bits da MAC. FCntUp é o deste quadro (a LoRaMac-node já
     * incrementou antes do Send); o seqnoUp da LMiC mostra o próximo (+1). */
    uint32_t up = ( uint32_t )( f[6] | ( f[7] << 8 ) );
    uint32_t dn = 0;
    bool dnValid = false;
    MibRequestConfirm_t m;
    m.Type = MIB_NVM_CTXS;
    if( LoRaMacMibGetRequestConfirm( &m ) == LORAMAC_STATUS_OK )
    {
        LoRaMacNvmData_t *nvm = m.Param.Contexts;
        up = nvm->Crypto.FCntList.FCntUp;
        dn = nvm->Crypto.FCntList.FCntDown;
        if( dn == 0xFFFFFFFFUL )
        {
            /* LoRaWAN 1.1: contadores separados de rede e de aplicação */
            uint32_t nf = nvm->Crypto.FCntList.NFCntDown, af = nvm->Crypto.FCntList.AFCntDown;
            dn = ( nf != 0xFFFFFFFFUL ) ? nf : af;
            if( ( af != 0xFFFFFFFFUL ) && ( nf != 0xFFFFFFFFUL ) && ( af > nf ) )
            {
                dn = af;
            }
        }
        dnValid = ( dn != 0xFFFFFFFFUL );
    }
    printf( "FCnt/up: %lu\r\n", ( unsigned long )up );
    if( dnValid )
    {
        printf( "FCnt/down: %lu\r\n", ( unsigned long )dn );
    }
    else
    {
        printf( "FCnt/down: - (nenhum downlink nesta sessão)\r\n" );
    }
    PrintMic( f, n );
}

/* Join-accept cifrado (o rádio o entrega antes do OnJoinRequest( )) */
static uint8_t JoinAcceptRaw[64];
static uint8_t JoinAcceptRawLen = 0;

static void PrintRxDiag( const uint8_t *f, uint8_t n )
{
    if( n == 0 )
    {
        return;
    }
    uint8_t mtype = f[0] >> 5;
    if( mtype != 1 )
    {
        printf( "\r\n" );   /* separa o quadro recebido do envio anterior */
    }
    if( mtype == 1 )
    {
        /* Join-accept: guardado para o OnJoinRequest( ), que o imprime na
         * mesma posição dos nós ESP32 (depois do [RX] JoinAccept, antes das
         * chaves da sessão). */
        JoinAcceptRawLen = ( n > sizeof( JoinAcceptRaw ) ) ? sizeof( JoinAcceptRaw ) : n;
        memcpy( JoinAcceptRaw, f, JoinAcceptRawLen );
        return;
    }
    PrintRaw( "Frame raw (RX): ", f, n );
    if( ( mtype == 3 ) || ( mtype == 5 ) )
    {
        printf( "FCnt/down (quadro): %u\r\n", ( unsigned )( f[6] | ( f[7] << 8 ) ) );
    }
    PrintMic( f, n );
}

static void SessionDiagFlush( void )
{
    static uint8_t f[256];
    uint8_t n = 0;
    bool on = ( NodeCfg.sessDiagOff == 0 );

    if( BoardRadioTakeTxFrame( f, &n ) && ( n > 0 ) )
    {
        NodeLogPause( true );
        if( on )
        {
            PrintTxDiag( f, n );
        }
        else
        {
            /* sem diagnóstico: só o cabeçalho (como os nós ESP32 com verbose 0) */
            uint8_t mt = f[0] >> 5;
            if( ( mt == 0 ) && ( n == 23 ) )
            {
                printf( "\r\nEV_TXSTART (Join-request)\r\n" );
            }
            else if( ( mt != 2 ) && ( mt != 4 ) )
            {
                printf( "\r\nEV_TXSTART (MType %u)\r\n", mt );
            }
            else
            {
                printf( "\r\nEV_TXSTART\r\n" );
            }
        }
        if( ( ( f[0] >> 5 ) == 2 ) || ( ( f[0] >> 5 ) == 4 ) )
        {
            printf( "Packet queued.\r\n" );
        }
        NodeLogPause( false );
    }
    if( BoardRadioTakeRxFrame( f, &n ) && on )
    {
        NodeLogPause( true );
        PrintRxDiag( f, n );
        NodeLogPause( false );
    }
}

uint32_t AppUptimeMs( void )
{
    return ( uint32_t )TimerGetCurrentTime( );
}

void AppSendNow( const uint8_t *data, uint8_t len, bool useCfgPayload )
{
    PendingUseCfgPayload = useCfgPayload;
    if( !useCfgPayload )
    {
        PendingLen = ( len > NODE_PAYLOAD_MAX ) ? NODE_PAYLOAD_MAX : len;
        memcpy( PendingData, data, PendingLen );
    }
    IsTxFramePending = 1;
}

void AppRejoin( void )
{
    AppStatus.joined = false;
    AppStatus.joining = true;
    AppStatus.activeClass = 'A';
    PendingClass = 0;
    printf( "[LoRaWAN] %s\r\n", NodeIsOtaa( ) ? "Refazendo o JOIN (OTAA)..." : "Reativando a sessão ABP..." );
    /* o JOIN só é aceito com a MAC livre: o laço principal chama na hora certa */
    JoinRetryPending = true;
}

char AppCurrentClass( void )
{
    /* A classe que a LoRaMac-node está usando de fato (vem do contexto da EEPROM
     * quando a sessão é restaurada), não a que o firmware acha que pediu */
    DeviceClass_t c = LmHandlerGetCurrentClass( );
    return ( c == CLASS_B ) ? 'B' : ( c == CLASS_C ) ? 'C' : 'A';
}

/* Confere, com a MAC livre, se a classe em uso é a gravada. Garante que um contexto
 * antigo da EEPROM (sessão restaurada depois de regravar o firmware) não deixe o nó
 * em Classe C com a configuração em A, nem o contrário. A Classe B tem a própria
 * retentativa (beacon): aqui só se corrige a C. */
static void ClassSync( void )
{
    static uint32_t lastMs;
    uint32_t now = AppUptimeMs( );

    if( !AppStatus.joined || PendingClass || ( ( now - lastMs ) < 5000 ) )
    {
        return;
    }
    lastMs = now;
    char mac = AppCurrentClass( );
    if( mac != AppStatus.activeClass )
    {
        printf( "[Classe] A MAC está em Classe %c (o estado dizia %c): corrigido.\r\n", mac, AppStatus.activeClass );
        AppStatus.activeClass = mac;
    }
    if( ( mac == 'C' ) != ( NodeCfg.devClass == 'C' ) )
    {
        printf( "[Classe] Em uso: %c, gravada: %c. Voltando para a gravada.\r\n", mac, NodeCfg.devClass );
        AppRequestClass( NodeCfg.devClass );
    }
}

bool AppRequestClass( char cls )
{
    DeviceClass_t target = ( cls == 'B' ) ? CLASS_B : ( cls == 'C' ) ? CLASS_C : CLASS_A;

    if( !AppStatus.joined )
    {
        printf( "[Classe] Ainda sem sessão: a Classe %c entra depois do JOIN.\r\n", cls );
        return false;
    }

    if( MacBusy( ) )
    {
        PendingClass = cls;
        printf( "[Classe] MAC ocupada (TX/RX em andamento): a Classe %c entra em seguida.\r\n", cls );
        return true;
    }
    PendingClass = 0;

    DeviceClass_t cur = LmHandlerGetCurrentClass( );
    if( cur == target )
    {
        AppStatus.activeClass = cls;    /* já está nela: só acerta o estado mostrado */
        return true;
    }
    /* B <-> C passa pela A */
    if( ( cur != CLASS_A ) && ( target != CLASS_A ) )
    {
        LmHandlerRequestClass( CLASS_A );
    }

    if( target == CLASS_B )
    {
        const RtcClockInfo_t *ci = RtcGetClockInfo( );
        if( ci->Source != RTC_CLOCK_LSE )
        {
            printf( "[Classe] AVISO: sem cristal de 32,768 kHz o relógio tem ~%lu ppm; "
                    "os ping slots podem ser perdidos.\r\n", ( unsigned long )ci->PpmEstimate );
        }
        printf( "[Classe] Pedindo Classe B: DeviceTimeReq -> beacon -> PingSlotInfoReq (2^%u = %u s)...\r\n",
                NodeCfg.pingPeriodicity, 1U << NodeCfg.pingPeriodicity );
        AppStatus.beacon = 'A';
    }

    LmHandlerErrorStatus_t st = LmHandlerRequestClass( target );
    if( st != LORAMAC_HANDLER_SUCCESS )
    {
        printf( "[Classe] Pedido da Classe %c recusado pela LoRaMac-node.\r\n", cls );
        return false;
    }
    if( target == CLASS_B )
    {
        /* o DeviceTimeReq vai junto do próximo uplink: manda um agora */
        AppSendNow( NULL, 0, true );
    }
    return true;
}

void AppRequestDeviceTime( void )
{
    if( LmHandlerDeviceTimeReq( ) == LORAMAC_HANDLER_SUCCESS )
    {
        printf( "[MAC] DeviceTimeReq na fila (vai no próximo uplink).\r\n" );
        AppSendNow( NULL, 0, true );
    }
}

/* ------------------------------------------------- envio (todas as origens) --- */
/* Deixa o DR da MAC (o que o LoRaMacQueryTxPossible( ) e o LmHandlerSend( ) usam
 * para ver se o payload cabe) igual ao DR configurado.
 *
 * O JOIN troca o DR da MAC pelo DR do Join-request: no AU915, com o dwell time de
 * 400 ms, a RegionAU915AlternateDr( ) devolve DR2 (ou DR6 a cada 9 tentativas),
 * qualquer que seja o DR configurado. Depois do Join-accept a MAC continua em DR2
 * (máx. 11 bytes) e o LmHandlerSend( ) consulta o tamanho nesse DR: 14 bytes não
 * cabem, ele troca o quadro por um vazio (só cabeçalho/comandos MAC) e o manda no
 * DR configurado (DR5). O payload do 1º uplink depois de cada JOIN se perdia.
 *  - ADR desligado: o DR é sempre o configurado (acerta antes de cada envio);
 *  - ADR ligado: só depois do JOIN, como DR inicial; depois quem manda é o
 *    LinkADRReq da rede. */
static void SyncTxDatarate( void )
{
    if( MacBusy( ) || !AppStatus.joined || ( !DrSyncPending && NodeCfg.adr ) )
    {
        return;
    }
    MibRequestConfirm_t m;
    m.Type = MIB_CHANNELS_DATARATE;
    m.Param.ChannelsDatarate = ( int8_t )NodeCfg.dr;
    if( ( LoRaMacMibSetRequestConfirm( &m ) != LORAMAC_STATUS_OK ) && DrSyncPending )
    {
        printf( "[LoRaWAN] DR%u recusado pela região: a MAC segue no DR do JOIN.\r\n", ( unsigned )NodeCfg.dr );
    }
    DrSyncPending = false;
}

uint8_t AppMaxPayload( void )
{
    LoRaMacTxInfo_t txInfo;
    SyncTxDatarate( );
    LoRaMacQueryTxPossible( 0, &txInfo );
    return txInfo.MaxPossibleApplicationDataSize;
}

static void LedFlash( uint8_t led, TimerEvent_t *t )
{
    if( NodeCfg.leds )
    {
        BoardLedSet( led, true );
        TimerStart( t );
    }
}

/* Envia um quadro já, com a porta e a confirmação pedidas. Usada pelo uplink
 * periódico, pelo console, pelos comandos AT, pelo keepalive, pelas
 * interrupções e pela serial transparente. */
static AppTxResult_t SendFrameEx( uint8_t port, const uint8_t *data, uint8_t len, bool confirmed, uint8_t trials,
                                  bool fromAt, LoRaMacStatus_t *macStatus )
{
    if( !AppStatus.joined )
    {
        return APP_TX_NOT_JOINED;
    }
    if( MacBusy( ) )
    {
        return APP_TX_BUSY;
    }
    SyncTxDatarate( );                      /* DR da MAC = DR configurado (o JOIN deixa em DR2) */
    LoRaMacTxInfo_t txInfo;
    LastTxDropped = false;
    if( LoRaMacQueryTxPossible( len, &txInfo ) != LORAMAC_STATUS_OK )
    {
        if( fromAt )
        {
            return APP_TX_LENGTH;           /* AT TX LENGTH ERROR (o original não manda nada) */
        }
        LastTxDropped = true;
        LastTxWanted = len;
        printf( "[TX] AVISO: %u bytes não cabem no DR atual (máx. %u): a LoRaMac-node manda só o "
                "cabeçalho/comandos MAC. Use um DR maior ou um payload menor.\r\n", len, txInfo.MaxPossibleApplicationDataSize );
    }
    if( !NodeCfg.adr )
    {
        /* NBTRIALS: tentativas do confirmado; o não confirmado vai uma vez */
        MibRequestConfirm_t m;
        m.Type = MIB_CHANNELS_NB_TRANS;
        m.Param.ChannelsNbTrans = confirmed ? ( ( trials == 0 ) ? 1 : ( trials > 15 ) ? 15 : trials ) : 1;
        MibSet( &m );
    }
    memcpy( AppData.Buffer, data, len );
    AppData.BufferSize = len;
    AppData.Port = port;
    RadioWatchdogBeforeTx( );               /* rádio resetado desde o último uso? arruma antes */
    LastMcpsStatus = LORAMAC_STATUS_OK;
    LmHandlerErrorStatus_t st = LmHandlerSend( &AppData, confirmed ? LORAMAC_HANDLER_CONFIRMED_MSG : LORAMAC_HANDLER_UNCONFIRMED_MSG );
    if( macStatus != NULL )
    {
        *macStatus = LastMcpsStatus;
    }
    if( st != LORAMAC_HANDLER_SUCCESS )
    {
        if( MacStatusRetryable( LastMcpsStatus ) )
        {
            return APP_TX_BUSY;
        }
        return ( LastMcpsStatus == LORAMAC_STATUS_LENGTH_ERROR ) ? APP_TX_LENGTH : APP_TX_ERROR;
    }
    AtTxActive = fromAt;
    AtTxConfirmed = confirmed;
    LedFlash( BOARD_LED_RED, &LedTxTimer );     /* vermelho: transmissão (como no original) */
    return APP_TX_OK;
}

AppTxResult_t AppSendFrame( uint8_t port, const uint8_t *data, uint8_t len, bool confirmed, uint8_t trials, bool fromAt )
{
    return SendFrameEx( port, data, len, confirmed, trials, fromAt, NULL );
}

static void StartJoin( void )
{
    AppStatus.joined = false;
    AppStatus.joining = true;
    AppStatus.activeClass = 'A';
    PendingClass = 0;
    JoinRetryPending = true;                /* o laço principal pede com a MAC livre */
}

AppJoinResult_t AppAtJoin( void )
{
    if( !NodeIsOtaa( ) )
    {
        return APP_JOIN_ERROR;              /* "Em ABP, não é possível enviar pacotes de JOIN" */
    }
    if( RestartPending )
    {
        /* credenciais novas ainda não aplicadas: reinicia e faz o JOIN no boot */
        Meta.flags |= NODE_META_JOIN_ON_BOOT;
        NodeMetaSave( &Meta );
        AppReboot( 200 );
        return APP_JOIN_RESTART;
    }
    if( AppStatus.joined )
    {
        return APP_JOIN_ALREADY;
    }
    AtJoinActive = true;
    AtJoinLeft = ( NodeCfg.nbTrials == 0 ) ? 1 : NodeCfg.nbTrials;
    TimerStop( &JoinRetryTimer );
    StartJoin( );
    return APP_JOIN_STARTED;
}

void AppScheduleRestart( void )
{
    RestartPending = true;
    TimerStop( &RestartTimer );
    TimerSetValue( &RestartTimer, 5000 );
    TimerStart( &RestartTimer );
}

bool AppRestartPending( void )
{
    return RestartPending;
}

void AppConsoleActivity( void )
{
    LedFlash( BOARD_LED_YELLOW, &LedSerTimer );  /* amarelo: algo chegou na serial de comandos */
    if( RestartPending )
    {
        TimerStop( &RestartTimer );
        TimerStart( &RestartTimer );         /* espera o fim de uma sequência de AT+... */
    }
}

void AppSetConsoleBaudrate( uint32_t baud )
{
    BoardSetConsoleBaudrate( baud );
    NodeIoTransparentSetBaudrate( baud );
}

void AppReboot( uint32_t delayMs )
{
    printf( "[Sistema] Reiniciando em %lu ms...\r\n", ( unsigned long )delayMs );
    TimerStop( &RebootTimer );
    TimerSetValue( &RebootTimer, delayMs ? delayMs : 1 );
    TimerStart( &RebootTimer );
}

void AppFactoryReset( bool keepDevNonce )
{
    uint16_t dn = keepDevNonce ? AppGetDevNonce( ) : 0;
    NodeConfigDefaults( &NodeCfg );
    NodeConfigSave( );
    NvmDataMgmtFactoryReset( );
    Meta.credFingerprint = 0;    /* força a recriação do contexto no boot */
    Meta.lastDevNonce = keepDevNonce ? ( ( dn > Meta.lastDevNonce ) ? dn : Meta.lastDevNonce ) : 0;
    NodeMetaSave( &Meta );
    AppReboot( 500 );
}

/* =================================================================== */
/*            watchdog do rádio (SX1272) e do MCU (IWDG)               */
/* =================================================================== */
/* O rádio volta a funcionar sozinho, sem perder a sessão:
 *   - a cada RADIO_CHECK_MS (o RTC acorda o MCU do STOP) e logo antes de cada
 *     uplink, confere se o SX1272 responde no SPI (versão 0x22), se continua no
 *     modo LoRa com a sync word da rede (um reset do rádio por queda de tensão
 *     ou pico no RESET volta ao FSK com a sync word 0x12: o nó "transmite" e o
 *     gateway não ouve) e, na Classe C, se a recepção contínua segue ligada.
 *     Se não: reset do rádio com a configuração da LoRaMac-node (sync word
 *     incluída) e, na Classe C, a janela contínua é reaberta;
 *   - TX ou janela RX que não termina (interrupção DIO e timer perdidos): a
 *     LoRaMac-node recebe o timeout que faltou e fecha a operação;
 *   - MCU travado (laço sem fim no driver, SPI preso): o IWDG reinicia em ~20 a
 *     40 s e a sessão volta da EEPROM (sem novo JOIN).
 * DevAddr, chaves, FCnt, canais e ADR ficam na LoRaMac-node: nada se perde.
 * Rádio que não responde de jeito nenhum é tentado de novo a cada verificação:
 * aí é fiação ou alimentação para corrigir. */
#define RADIO_CHECK_MS          10000UL     /* verificação periódica; também alimenta o IWDG */
#define RADIO_OP_STUCK_MS       30000UL     /* TX (até ~3 s) e janelas RX (< 3 s) terminam bem antes */
#define RADIO_WARN_PER_H        3           /* recuperações por hora que indicam problema de hardware */

static TimerEvent_t RadioWdgTimer;
static volatile uint8_t IsRadioCheckPending = 0;
static uint32_t RadioRecoveries = 0;
static const char *RadioLastWhy = "-";
static uint32_t RadioLastMs = 0;

static void OnRadioWdgTimerEvent( void *context )
{
    ( void )context;
    IsRadioCheckPending = 1;            /* acorda o laço principal (IWDG + rádio) */
    TimerStart( &RadioWdgTimer );
}

/* IWDG: LSI (26..56 kHz) / 256, recarga 4095 -> 19..40 s. Não para no STOP:
 * o RadioWdgTimer acorda o MCU a cada 10 s para alimentá-lo. Com o núcleo
 * parado pelo depurador o IWDG também para (DBGMCU). */
static void IwdgStart( void )
{
    uint32_t dbgen = RCC->APB2ENR & RCC_APB2ENR_DBGEN;
    RCC->APB2ENR |= RCC_APB2ENR_DBGEN;
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
    if( dbgen == 0 )
    {
        RCC->APB2ENR &= ~RCC_APB2ENR_DBGEN;
    }

    IWDG->KR = 0x0000CCCCUL;            /* liga (liga o LSI também) */
    IWDG->KR = 0x00005555UL;            /* libera PR e RLR */
    IWDG->PR = 6;                       /* /256 */
    IWDG->RLR = 0x0FFF;
    while( IWDG->SR != 0 )
    {
    }
    IWDG->KR = 0x0000AAAAUL;
}

static inline void IwdgKick( void )
{
    IWDG->KR = 0x0000AAAAUL;
}

static const char *RadioCheck( void )
{
    uint8_t ver, op, sync = 0;
    RadioModems_t modem;
    RadioState_t st;
    bool rxc, pub;

    CRITICAL_SECTION_BEGIN( );          /* o driver também usa o SPI dentro das interrupções DIO */
    ver = SX1272Read( REG_LR_VERSION );
    op = SX1272Read( REG_LR_OPMODE );
    if( ( op & RFLR_OPMODE_LONGRANGEMODE_ON ) != 0 )
    {
        sync = SX1272Read( REG_LR_SYNCWORD );
    }
    modem = SX1272.Settings.Modem;
    st = SX1272.Settings.State;
    rxc = SX1272.Settings.LoRa.RxContinuous;
    pub = SX1272.Settings.LoRa.PublicNetwork;
    CRITICAL_SECTION_END( );

    if( ver != 0x22 )
    {
        return "rádio não responde no SPI";
    }
    if( modem != MODEM_LORA )
    {
        return NULL;                    /* FSK: nada de LoRa para conferir */
    }
    if( ( op & RFLR_OPMODE_LONGRANGEMODE_ON ) == 0 )
    {
        return "rádio voltou ao FSK (foi resetado)";
    }
    if( sync != ( pub ? LORA_MAC_PUBLIC_SYNCWORD : LORA_MAC_PRIVATE_SYNCWORD ) )
    {
        return "sync word errada (rádio foi resetado)";
    }
    if( ( st == RF_RX_RUNNING ) && rxc && ( ( op & ~RFLR_OPMODE_MASK ) != RFLR_OPMODE_RECEIVER ) )
    {
        return "recepção contínua (Classe C) parada";
    }
    return NULL;
}

static void RadioRecovered( const char *why )
{
    static TimerTime_t winStart = 0;
    static uint8_t inWin = 0;

    RadioRecoveries++;
    RadioLastWhy = why;
    RadioLastMs = AppUptimeMs( );
    printf( "[Rádio] Pronto de novo (recuperação nº %lu desde o boot).\r\n", ( unsigned long )RadioRecoveries );

    if( ( inWin == 0 ) || ( TimerGetElapsedTime( winStart ) >= 3600000UL ) )
    {
        winStart = TimerGetCurrentTime( );
        inWin = 0;
    }
    if( ++inWin > RADIO_WARN_PER_H )
    {
        printf( "[Rádio] %u recuperações na última hora: o nó segue funcionando, mas é problema de hardware "
                "a corrigir (alimentação de 3,3 V e desacoplamento, antena, fios).\r\n", inWin );
    }
}

/* force = true: verificação antes de um uplink (sem esperar o período) */
static void RadioWatchdog( bool force )
{
    static uint32_t stuckOp = 0;
    static TimerTime_t stuckSince = 0;
    RadioState_t st;
    bool rxc;
    uint32_t ops;
    const char *why;

    if( !force && !IsRadioCheckPending )
    {
        return;
    }
    IsRadioCheckPending = 0;

    CRITICAL_SECTION_BEGIN( );
    st = SX1272.Settings.State;
    rxc = ( SX1272.Settings.Modem == MODEM_LORA ) ? SX1272.Settings.LoRa.RxContinuous : SX1272.Settings.Fsk.RxContinuous;
    ops = SX1272OpCount;
    CRITICAL_SECTION_END( );

    /* 1. TX ou janela RX que não termina: a mesma operação há RADIO_OP_STUCK_MS */
    if( ( st == RF_TX_RUNNING ) || ( ( st == RF_RX_RUNNING ) && !rxc ) )
    {
        if( ( stuckSince == 0 ) || ( ops != stuckOp ) )
        {
            stuckOp = ops;
            stuckSince = TimerGetCurrentTime( ) | 1;
        }
        else if( TimerGetElapsedTime( stuckSince ) >= RADIO_OP_STUCK_MS )
        {
            why = ( st == RF_TX_RUNNING ) ? "TX sem fim (interrupção perdida)" : "janela RX sem fim (interrupção perdida)";
            printf( "[Rádio] %s: entregando o timeout à LoRaMac-node.\r\n", why );
            stuckSince = 0;
            SX1272ForceTimeout( );
            if( st == RF_RX_RUNNING )
            {
                SX1272Recover( );       /* o timeout de RX LoRa não mexe no rádio */
            }
            RadioRecovered( why );
        }
        return;
    }
    stuckSince = 0;

    /* 2. rádio parado ou em RX contínuo: registradores */
    why = RadioCheck( );
    if( why == NULL )
    {
        return;
    }
    DelayMs( 1 );
    why = RadioCheck( );                /* confirma (ruído no SPI não conta) */
    if( why == NULL )
    {
        return;
    }
    printf( "[Rádio] %s: reiniciando o rádio (sessão LoRaWAN mantida)...\r\n", why );
    SX1272Recover( );
    if( RadioCheck( ) != NULL )
    {
        printf( "[Rádio] O rádio não voltou: confira a alimentação de 3,3 V e os fios do SPI/RESET. "
                "Nova tentativa em %lu s.\r\n", RADIO_CHECK_MS / 1000 );
        return;
    }
    if( AppStatus.joined && ( LmHandlerGetCurrentClass( ) == CLASS_C ) )
    {
        /* reabre a janela contínua: C -> A -> C, direto na MAC (sem uplink) */
        MibRequestConfirm_t m;
        m.Type = MIB_DEVICE_CLASS;
        m.Param.Class = CLASS_A;
        MibSet( &m );
        m.Param.Class = CLASS_C;
        MibSet( &m );
    }
    RadioRecovered( why );
}

static void RadioWatchdogBeforeTx( void )
{
    RadioWatchdog( true );
}

uint32_t AppRadioRecoveries( uint32_t *agoS, const char **why )
{
    if( agoS != NULL )
    {
        *agoS = RadioRecoveries ? ( AppUptimeMs( ) - RadioLastMs ) / 1000 : 0;
    }
    if( why != NULL )
    {
        *why = RadioLastWhy;
    }
    return RadioRecoveries;
}

/* =================================================================== */
/*                                main                                 */
/* =================================================================== */
static void PrintBanner( bool cfgLoaded )
{
    const RtcClockInfo_t *ci = RtcGetClockInfo( );
    char eui[17];

    printf( "\r\n\r\n==================================================\r\n" );
    printf( " %s  v%s\r\n", NODE_FW_NAME, NODE_FW_VERSION );
    printf( " %s · STM32L071CZ + SX1272 · AU915\r\n", NODE_STACK_VERSION );
    printf( " Comandos AT do firmware Radioenge (AT+...) e console de texto\r\n" );
    printf( "==================================================\r\n" );
    printf( "Relógio do RTC : %s, %lu Hz, ~%lu ppm",
            AppClockSourceName( ), ( unsigned long )( ci->RtcClkHz + 0.5f ), ( unsigned long )ci->PpmEstimate );
    if( ci->HseHz )
    {
        printf( " (HSE %lu MHz)", ( unsigned long )( ci->HseHz / 1000000UL ) );
    }
    printf( "\r\n" );
    if( ci->Source != RTC_CLOCK_LSE )
    {
        printf( "                 Sem cristal de 32,768 kHz: Classes A e C ok; Classe B instável.\r\n" );
    }
    printf( "Configuração   : %s\r\n", cfgLoaded ? "EEPROM" : "padrão de fábrica (EEPROM vazia ou inválida)" );
    printf( "Reinício       : %s\r\n", AppResetCauseText( ) );
    printf( "Ativação       : %s%s\r\n", NodeIsOtaa( ) ? "OTAA" : "ABP",
            ( NodeIsOtaa( ) != ( NODE_ACTIVATION == NODE_ACTIVATION_OTAA ) )
            ? ( ( NODE_ACTIVATION == NODE_ACTIVATION_OTAA ) ? " (padrão de fábrica: OTAA)" : " (padrão de fábrica: ABP)" ) : "" );
    BytesToHex( NodeCfg.devEui, 8, eui );
    printf( "DevEUI         : %s%s\r\n", eui, NodeCfg.devEuiAuto ? " (do ID único do STM32)" : "" );
    if( NodeIsOtaa( ) )
    {
        BytesToHex( NodeCfg.joinEui, 8, eui );
        printf( "JoinEUI        : %s\r\n", eui );
    }
    else
    {
        printf( "DevAddr        : %08lX\r\n", ( unsigned long )NodeCfg.devAddr );
    }
    printf( "Classe         : %c%s · uplink ", NodeCfg.devClass, ( NodeCfg.devClass == 'B' ) ? " (ping slot 2^n s)" : "" );
    if( NodeCfg.txIntervalS )
    {
        printf( "a cada %lu s na FPort %u\r\n", ( unsigned long )NodeCfg.txIntervalS, NodeCfg.fport );
    }
    else
    {
        printf( "periódico desligado (só AT+SEND, keepalive, eventos)\r\n" );
    }
    printf( "Serial         : %lu bps (AT+BAUDRATE)\r\n", ( unsigned long )NodeBaudrates[NodeCfg.baudIdx & 3] );
    printf( "Uplink         : %s\r\n", ( NodeCfg.uplinkMode == NODE_UPLINK_V1 )
            ? "telemetria ADAILSILVA_V1 (porta 20) · comandos na porta 10, respostas na 11" : "payload fixo (AT+TELEM=1 liga a telemetria V1)" );
    if( NodeCfg.relayCount )
    {
        char rc[40];
        NodeRelayDescribe( rc, sizeof( rc ) );
        printf( "Relés          : %u (GPIOs:ativo-baixo %s) · estado restaurado %04X\r\n", NodeCfg.relayCount, rc, NodeRelayMap( ) );
    }
    else
    {
        printf( "Relés          : nenhum (AT+RELECFG)\r\n" );
    }
    if( NodeCfg.pzemCount )
    {
        printf( "PZEM-004T      : %u medidor(es) em GPIO0/GPIO1 (Modbus 9600 8N1), endereço(s)", NodeCfg.pzemCount );
        for( uint8_t i = 0; i < NodeCfg.pzemCount; i++ )
        {
            printf( " %02X", NodeCfg.pzemAddr[i] );
        }
        printf( "\r\n" );
    }
    else
    {
        printf( "PZEM-004T      : desligado (AT+PZEMCFG)\r\n" );
    }
    printf( "Digite 'help' (console) ou 'AT?' (comandos AT).\r\n\r\n" );
}

/* Pisca na inicialização como o firmware original: vermelho = Classe A,
 * verde = Classe C (B: os dois) */
static void BootBlink( void )
{
    if( !NodeCfg.leds )
    {
        return;
    }
    bool red = ( NodeCfg.devClass != 'C' ), green = ( NodeCfg.devClass != 'A' );
    for( int i = 0; i < 2; i++ )
    {
        BoardLedSet( BOARD_LED_RED, red );
        BoardLedSet( BOARD_LED_GREEN, green );
        DelayMs( 120 );
        BoardLedSet( BOARD_LED_RED, false );
        BoardLedSet( BOARD_LED_GREEN, false );
        DelayMs( 120 );
    }
}

/* Uplinks além do periódico/console, por prioridade, com a MAC livre */
static void ServiceExtraUplinks( void )
{
    if( !AppStatus.joined || MacBusy( ) || IsTxFramePending )
    {
        return;
    }
    uint8_t buf[NODE_IO_KEEPALIVE_LEN > NODE_IO_IRQ_LEN ? NODE_IO_KEEPALIVE_LEN : NODE_IO_IRQ_LEN];
    AppTxResult_t r;
    uint8_t g;

    /* 1. resposta de um AT recebido por downlink */
    if( AtReplyLen )
    {
        uint8_t max = AppMaxPayload( );
        r = AppSendFrame( NodeCfg.atPort, AtReply, ( AtReplyLen > max ) ? max : AtReplyLen, false, 1, false );
        if( r != APP_TX_BUSY )
        {
            AtReplyLen = 0;
        }
        else
        {
            TimerStart( &TxRetryTimer );
        }
        return;
    }
    /* 1b. resposta de um comando ADAILSILVA_V1 (porta 11) */
    if( NodeV1ServiceReply( ) )
    {
        return;
    }
    /* 2. interrupção de GPIO (porta 2, Tabela 13) */
    if( NodeIoTakeIrq( &g ) )
    {
        uint8_t n = NodeIoBuildIrqPacket( buf, g, AppStatus.lastSnr, AppStatus.lastRssi );
        printf( "[GPIO] Interrupção no GPIO%u: uplink na porta %u.\r\n", g, NODE_IO_PORT_IRQ );
        r = AppSendFrame( NODE_IO_PORT_IRQ, buf, n, NodeCfg.confirmed, NodeCfg.nbTrials, false );
        if( r == APP_TX_OK )
        {
            AppStatus.irqUplinks++;
        }
        else if( r == APP_TX_BUSY )
        {
            NodeIoRequeueIrq( g );          /* manda na próxima folga */
            TimerStart( &TxRetryTimer );
        }
        return;
    }
    /* 3. serial transparente (porta 4) */
    if( ( TranspLen == 0 ) && NodeIoTransparentReady( ) )
    {
        TranspLen = NodeIoTransparentTake( TranspBuf, AppMaxPayload( ) ? AppMaxPayload( ) : 1 );
    }
    if( TranspLen )
    {
        r = AppSendFrame( NODE_IO_PORT_TRANSPARENT, TranspBuf, TranspLen, NodeCfg.confirmed, NodeCfg.nbTrials, false );
        if( r == APP_TX_BUSY )
        {
            TimerStart( &TxRetryTimer );
            return;
        }
        if( r == APP_TX_OK )
        {
            AppStatus.transpUplinks++;
        }
        TranspLen = 0;
        return;
    }
    /* 4. telemetria ADAILSILVA_V1 (porta 20): periódica, ENVIAR_AGORA, fim de pulso */
    if( NodeV1ServiceTelemetry( ) )
    {
        return;
    }
    /* 5. keepalive (Tabela 9) */
    if( IsKeepAlivePending )
    {
        uint8_t n = NodeIoBuildKeepalive( buf, AppStatus.lastSnr, AppStatus.lastRssi );
        r = AppSendFrame( NodeCfg.kaPort, buf, n, NodeCfg.kaConfirmed, NodeCfg.nbTrials, false );
        if( r == APP_TX_BUSY )
        {
            TimerStart( &TxRetryTimer );
            return;
        }
        IsKeepAlivePending = 0;
        if( r == APP_TX_OK )
        {
            AppStatus.keepalives++;
        }
    }
}

int main( void )
{
    ResetFlags = RCC->CSR & 0xFF000000UL;   /* causa do reinício (antes de limpar) */
    RCC->CSR |= RCC_CSR_RMVF;
    StackPaint( );
    NodeLogInit( );

    BoardInitMcu( );
    BoardInitPeriph( );

    bool cfgLoaded = NodeConfigLoad( );
    CfgFromEeprom = cfgLoaded;
    if( NodeBaudrates[NodeCfg.baudIdx & 3] != CONSOLE_BAUDRATE )
    {
        BoardSetConsoleBaudrate( NodeBaudrates[NodeCfg.baudIdx & 3] );   /* AT+BAUDRATE */
    }
    BoardSetExternalPower( NodeCfg.extPower != 0 );
    BoardSetStopAllowed( NodeCfg.lowPower != 0 );
    BoardSetConsoleAwakeInStop( NodeCfg.lowPower == 1 );
    NodeRelayInit( );                       /* relés: nível restaurado antes de o pino virar saída */
    NodeIoInit( );                          /* AT+GPIOC gravados */
    NodePzemInit( );                        /* PZEM-004T em GPIO0/GPIO1 (AT+PZEMCFG) */

    memset( &AppStatus, 0, sizeof( AppStatus ) );
    AppStatus.activeClass = 'A';
    AppStatus.beacon = '-';
    AppStatus.lastSlot = "-";

    TimerInit( &TxTimer, OnTxTimerEvent );
    TimerInit( &LedTxTimer, OnLedTxTimerEvent );
    TimerSetValue( &LedTxTimer, LED_FLASH_MS );
    TimerInit( &LedRxTimer, OnLedRxTimerEvent );
    TimerSetValue( &LedRxTimer, LED_FLASH_MS );
    TimerInit( &LedSerTimer, OnLedSerTimerEvent );
    TimerSetValue( &LedSerTimer, LED_FLASH_MS );
    TimerInit( &RebootTimer, OnRebootTimerEvent );
    TimerInit( &ClassBRetryTimer, OnClassBRetryTimerEvent );
    TimerInit( &TxRetryTimer, OnTxRetryTimerEvent );
    TimerInit( &JoinRetryTimer, OnJoinRetryTimerEvent );
    TimerInit( &KeepAliveTimer, OnKeepAliveTimerEvent );
    TimerInit( &RestartTimer, OnRestartTimerEvent );
    TimerInit( &TranspTimer, OnTranspTimerEvent );
    TimerSetValue( &TranspTimer, 60 );
    TimerSetValue( &JoinRetryTimer, 10000 );
    TimerSetValue( &TxRetryTimer, 3000 );
    TimerSetValue( &ClassBRetryTimer, CLASS_B_RETRY_MS );
    NodeV1Init( BootReasonV1( ) );

    PrintBanner( cfgLoaded );
    BootBlink( );

    /* Credenciais ou canais mudaram desde que o contexto LoRaMac foi gravado? */
    NodeMetaLoad( &Meta );
    bool joinOnBoot = ( Meta.flags & NODE_META_JOIN_ON_BOOT ) != 0;
    if( joinOnBoot )
    {
        Meta.flags &= ( uint16_t )~NODE_META_JOIN_ON_BOOT;
        NodeMetaSave( &Meta );
    }
    uint32_t fp = NodeConfigCredFingerprint( &NodeCfg );
    if( ( Meta.flags & NODE_META_FP_UNIFIED ) == 0 )
    {
        /* 1º boot deste firmware único: um módulo que veio do antigo firmware ABP
         * (que tinha ABP como padrão) tem a parcela da ativação invertida na impressão.
         * Só a ativação difere: a sessão LoRaWAN e o FCnt/DevNonce continuam valendo. */
        if( Meta.credFingerprint == ( fp ^ NODE_FP_NJM_XOR ) )
        {
            printf( "[NVM] Configuração do firmware anterior (ABP ou OTAA) migrada; sessão mantida.\r\n" );
            Meta.credFingerprint = fp;
        }
        Meta.flags |= NODE_META_FP_UNIFIED;
        NodeMetaSave( &Meta );
    }
    bool freshContext = ( Meta.credFingerprint != fp );
    if( freshContext )
    {
        printf( "[NVM] Credenciais/canais novos: apagando a sessão LoRaWAN guardada.\r\n" );
        NvmDataMgmtFactoryReset( );
    }

    LmHandlerParams.AdrEnable = NodeCfg.adr;
    LmHandlerParams.TxDatarate = ( int8_t )NodeCfg.dr;
    LmHandlerParams.IsTxConfirmed = NodeCfg.confirmed ? LORAMAC_HANDLER_CONFIRMED_MSG : LORAMAC_HANDLER_UNCONFIRMED_MSG;
    LmHandlerParams.PingSlotPeriodicity = NodeCfg.pingPeriodicity;
    LmHandlerParams.PublicNetworkEnable = NodeCfg.publicNet != 0;
    LmHandlerParams.DutyCycleEnabled = NodeCfg.dutyCycle != 0;

    if( LmHandlerInit( &LmHandlerCallbacks, &LmHandlerParams ) != LORAMAC_HANDLER_SUCCESS )
    {
        printf( "[MAC] Falha ao iniciar a LoRaMac-node.\r\n" );
        BoardLedSet( BOARD_LED_RED, true );
        while( 1 )
        {
        }
    }
    LmHandlerSetActivationType( NodeIsOtaa( ) );   /* AT+NJM */

    SetMaxRxErrorForClock( );
    LmHandlerPackageRegister( PACKAGE_ID_COMPLIANCE, &LmhpComplianceParams );

    ApplyCredentials( );
    if( !freshContext && !NvmRestored )
    {
        /* CRC ruim ou EEPROM gravada pela metade: a MAC voltou ao padrão */
        printf( "[NVM] Contexto LoRaWAN ausente ou inválido: recriando.\r\n" );
        freshContext = true;
    }

    MibRequestConfirm_t nvmReq;
    nvmReq.Type = MIB_NVM_CTXS;
    LoRaMacNvmData_t *nvm = ( LoRaMacMibGetRequestConfirm( &nvmReq ) == LORAMAC_STATUS_OK ) ? nvmReq.Param.Contexts : NULL;

    if( freshContext )
    {
        ApplyChannelMask( );
        Meta.credFingerprint = fp;
        NodeMetaSave( &Meta );
    }
    else if( nvm != NULL )
    {
        /* O FCnt só é gravado a cada NVM_FCNT_STORE_PERIOD uplinks: pula esse tanto
         * para nunca repetir um contador já usado (a LoRaMac recalcula o CRC). */
        nvm->Crypto.FCntList.FCntUp += NVM_FCNT_STORE_PERIOD;
    }
    if( NodeIsOtaa( ) && ( nvm != NULL ) )
    {
        /* O DevNonce (LoRaWAN 1.0.4) nunca pode repetir: o maior entre o do contexto e
         * o guardado à parte, mais 1 de folga (o guardado pode estar um atrás). */
        uint16_t dn = nvm->Crypto.DevNonce;
        if( Meta.lastDevNonce > dn )
        {
            dn = Meta.lastDevNonce;
        }
        if( dn > 0 )
        {
            nvm->Crypto.DevNonce = dn + 1;
        }
    }

    /* RX1/RX2: numa sessão nova, ou sempre no ABP (no OTAA vêm do Join-accept) */
    AppApplyMacParams( freshContext || !NodeIsOtaa( ) );
    AppApplyRuntimeConfig( );

    if( LmHandlerJoinStatus( ) == LORAMAC_HANDLER_SET )
    {
        MibRequestConfirm_t m;
        m.Type = MIB_DEV_ADDR;
        LoRaMacMibGetRequestConfirm( &m );
        AppStatus.joined = true;
        AppStatus.devAddr = m.Param.DevAddr;
        AppStatus.sessionAtMs = AppUptimeMs( );
        AppStatus.sessionRestored = true;
        printf( "[LoRaWAN] Sessão restaurada da EEPROM (DevAddr %08lX).\r\n", ( unsigned long )AppStatus.devAddr );
        /* a classe também vem do contexto restaurado: parte dela, não de 'A' */
        AppStatus.activeClass = AppCurrentClass( );
        if( AppStatus.activeClass != NodeCfg.devClass )
        {
            printf( "[Classe] O contexto da EEPROM estava em Classe %c; a gravada é a %c.\r\n",
                    AppStatus.activeClass, NodeCfg.devClass );
        }
        if( joinOnBoot )
        {
            printf( "AT ALREADY JOINED\r\n" );
        }
        AppRequestClass( NodeCfg.devClass );
    }
    else if( !NodeIsOtaa( ) || NodeCfg.autoJoin || joinOnBoot )
    {
        if( joinOnBoot )
        {
            AtJoinActive = true;            /* AT+JOIN antes do reinício: responde no fim */
            AtJoinLeft = ( NodeCfg.nbTrials == 0 ) ? 1 : NodeCfg.nbTrials;
        }
        AppStatus.joining = true;
        printf( "[LoRaWAN] %s\r\n", NodeIsOtaa( ) ? "Iniciando o JOIN (OTAA)..." : "Ativando a sessão ABP..." );
        LmHandlerJoin( );
    }
    else
    {
        printf( "[LoRaWAN] AT+AJOIN=0: esperando o AT+JOIN.\r\n" );
    }

    LastRecal = TimerGetCurrentTime( );
    ConsoleInit( );

    /* watchdog do rádio + IWDG (veja RadioWatchdog) */
    TimerInit( &RadioWdgTimer, OnRadioWdgTimerEvent );
    TimerSetValue( &RadioWdgTimer, RADIO_CHECK_MS );
    TimerStart( &RadioWdgTimer );
    IwdgStart( );

    while( 1 )
    {
        IwdgKick( );
        RadioWatchdog( false );

        ConsoleProcess( );

        LmHandlerProcess( );

        NodeRelayProcess( );                /* fim de pulso; estado na EEPROM só quando muda */
        NodePzemProcess( );                 /* respostas Modbus / timeouts */
        NodeV1Process( );                   /* monta a telemetria (depois das leituras) */

        if( IsTxFramePending )
        {
            IsTxFramePending = 0;
            if( !AppStatus.joined )
            {
                if( !AppStatus.joining && NodeIsOtaa( ) && NodeCfg.autoJoin )
                {
                    AppRejoin( );
                }
            }
            else if( MacBusy( ) )
            {
                IsTxFramePending = 1;    /* sai quando a MAC terminar (o fim acorda o MCU) */
            }
            else if( PendingUseCfgPayload && ( NodeCfg.uplinkMode == NODE_UPLINK_V1 ) )
            {
                /* uplink periódico = telemetria ADAILSILVA_V1 (lê os PZEM e sai pela fila) */
                NodeV1RequestTelemetry( true );
            }
            else
            {
                const uint8_t *d = PendingUseCfgPayload ? NodeCfg.payload : PendingData;
                uint8_t n = PendingUseCfgPayload ? NodeCfg.payloadLen : PendingLen;
                LoRaMacStatus_t ms = LORAMAC_STATUS_OK;
                AppTxResult_t r = SendFrameEx( NodeCfg.fport, d, n, NodeCfg.confirmed, NodeCfg.nbTrials, false, &ms );
                if( r == APP_TX_BUSY )
                {
                    /* janela do beacon/ping slot ou MAC ocupada: tenta de novo em 3 s */
                    printf( "[TX] Adiado (%s): nova tentativa em 3 s.\r\n", MacStatusText( ms ) );
                    TimerStart( &TxRetryTimer );
                }
                else
                {
                    PendingUseCfgPayload = true;
                    if( r != APP_TX_OK )
                    {
                        AppStatus.uplinkErrors++;
                        printf( "[TX] Envio recusado: %s.\r\n", MacStatusText( ms ) );
                    }
                }
            }
        }

        ServiceExtraUplinks( );

        /* serial transparente: espera o silêncio de 50 ms (o timer acorda o MCU) */
        if( NodeIoTransparentPending( ) && !NodeIoTransparentReady( ) && !TimerIsStarted( &TranspTimer ) )
        {
            TimerStart( &TranspTimer );
        }

        if( JoinRetryPending && !MacBusy( ) )
        {
            JoinRetryPending = false;
            AppStatus.joining = true;
            LmHandlerJoin( );
        }

        if( !MacBusy( ) )
        {
            if( PendingApply )
            {
                AppApplyRuntimeConfig( );
            }
            if( DrSyncPending )
            {
                SyncTxDatarate( );
            }
            if( PendingClass && AppStatus.joined )
            {
                AppRequestClass( PendingClass );
            }
            ClassSync( );
        }

        if( IsClassBRetryPending )
        {
            IsClassBRetryPending = 0;
            if( AppStatus.joined && ( NodeCfg.devClass == 'B' ) && ( AppStatus.activeClass != 'B' ) )
            {
                printf( "[Classe] Tentando a Classe B de novo...\r\n" );
                AppRequestClass( 'B' );
            }
        }

        /* Sem LSE: recalibra o LSI de tempos em tempos, com a MAC parada */
        if( ( RtcGetClockInfo( )->Source != RTC_CLOCK_LSE ) &&
            ( TimerGetElapsedTime( LastRecal ) > LSI_RECAL_PERIOD_MS ) && !MacBusy( ) )
        {
            LastRecal = TimerGetCurrentTime( );
            RtcRecalibrate( );
        }

        /* Frame raw / chaves / MIC do que foi ao ar (ou chegou) nesta volta */
        SessionDiagFlush( );

        bool extra = AppStatus.joined && ( AtReplyLen || NodeIoIrqPending( ) || TranspLen ||
                                           NodeIoTransparentReady( ) || IsKeepAlivePending || NodeV1HasUplink( ) );
        CRITICAL_SECTION_BEGIN( );
        if( IsMacProcessPending == 1 )
        {
            IsMacProcessPending = 0;
        }
        else if( !ConsoleHasInput( ) && !IsRadioCheckPending &&
                 !NodeRelayHasEvent( ) && !NodePzemHasEvent( ) && !NodeV1HasEvent( ) &&
                 !( ( IsTxFramePending || PendingApply || JoinRetryPending || extra || ( PendingClass && AppStatus.joined ) ) && !MacBusy( ) ) )
        {
            BoardLowPowerHandler( );
        }
        CRITICAL_SECTION_END( );
    }
}

/* =================================================================== */
/*                        callbacks da LmHandler                       */
/* =================================================================== */
static void OnMacProcessNotify( void )
{
    IsMacProcessPending = 1;
}

static void OnNvmDataChange( LmHandlerNvmContextStates_t state, uint16_t size )
{
    if( state == LORAMAC_HANDLER_NVM_RESTORE )
    {
        NvmRestored = true;
        printf( "[NVM] Contexto LoRaWAN restaurado (%u bytes).\r\n", size );
    }
    else
    {
        /* Gravar o contexto para a MAC (LoRaMacStop/Start). Na Classe C a janela RXC
         * só reabre no próximo LoRaMacProcess( ): garante que ele rode já. */
        IsMacProcessPending = 1;
    }
}

static void OnNetworkParametersChange( CommissioningParams_t *params )
{
    ( void )params;
}

static void OnMacMcpsRequest( LoRaMacStatus_t status, McpsReq_t *mcpsReq, TimerTime_t nextTxIn )
{
    ( void )mcpsReq;
    ( void )nextTxIn;
    LastMcpsStatus = status;
}

static void OnMacMlmeRequest( LoRaMacStatus_t status, MlmeReq_t *mlmeReq, TimerTime_t nextTxIn )
{
    ( void )nextTxIn;
    if( mlmeReq->Type == MLME_JOIN )
    {
        AppStatus.joinAttempts++;
        if( status == LORAMAC_STATUS_OK )
        {
            if( NodeIsOtaa( ) )
            {
                printf( "[JOIN] Join-request enviado (tentativa %lu, DevNonce %u).\r\n",
                        ( unsigned long )AppStatus.joinAttempts, AppGetDevNonce( ) );
                SaveDevNonce( );
            }
        }
        else
        {
            printf( "[JOIN] Pedido recusado pela MAC: %s. Nova tentativa em 10 s.\r\n", MacStatusText( status ) );
            TimerStart( &JoinRetryTimer );
        }
    }
}

/* "917,8 MHz" (resolução de 100 kHz, como em todos os logs dos nós) */
static void PrintMHz( uint32_t hz )
{
    printf( "%lu,%lu MHz", ( unsigned long )( hz / 1000000UL ), ( unsigned long )( ( hz / 100000UL ) % 10UL ) );
}

/* Canal AU915 de uma frequência de uplink (0..63 = 125 kHz, 64..71 = 500 kHz) */
static int Au915UpChannel( uint32_t f )
{
    if( ( f >= 915200000UL ) && ( f <= 927800000UL ) && ( ( ( f - 915200000UL ) % 200000UL ) == 0 ) )
    {
        return ( int )( ( f - 915200000UL ) / 200000UL );
    }
    if( ( f >= 915900000UL ) && ( f <= 927100000UL ) && ( ( ( f - 915900000UL ) % 1600000UL ) == 0 ) )
    {
        return 64 + ( int )( ( f - 915900000UL ) / 1600000UL );
    }
    return -1;
}

/* [TX] JoinRequest · DR · MHz · dBm · canal — mesma linha dos nós ESP32 */
static void PrintJoinTx( int8_t dr )
{
    uint32_t f = BoardGetLastTxFreq( );
    printf( "[TX] JoinRequest · DR%d · ", dr );
    PrintMHz( f );
    printf( " · %d dBm · canal %d\r\n", BoardGetLastTxPower( ), Au915UpChannel( f ) );
}

/* [RX] JoinAccept · RX1/RX2 · DR · MHz · RSSI · SNR — mesma linha dos nós ESP32.
 * A LoRaMac-node não informa a janela do Join-accept: RX1 tem a frequência
 * 923,3 + 0,6 x (canal mod 8) MHz e o DR do Join-request + 8 (offset 0). */
static void PrintJoinAccept( int8_t joinDr )
{
    uint32_t rf = 0;
    uint8_t rdr = 0;
    int16_t rssi = 0;
    int8_t snr = 0;
    BoardGetLastRx( &rf, &rdr, &rssi, &snr );
    int ch = Au915UpChannel( BoardGetLastTxFreq( ) );
    uint32_t rx1f = ( ch >= 0 ) ? ( 923300000UL + 600000UL * ( uint32_t )( ch & 7 ) ) : 0;
    uint8_t rx1dr = ( joinDr <= 5 ) ? ( uint8_t )( joinDr + 8 ) : 13;
    printf( "[RX] JoinAccept · %s · DR%u · ", ( ( rf == rx1f ) && ( rdr == rx1dr ) ) ? "RX1" : "RX2", rdr );
    PrintMHz( rf );
    printf( " · RSSI %d dBm · SNR %d dB\r\n", rssi, snr );
}

/* Chave como os nós ESP32 mostram depois do JOIN: "40-D8-48-…" */
static void PrintKeyDashed( const char *label, const uint8_t *k )
{
    printf( "%s", label );
    for( int i = 0; i < 16; i++ )
    {
        printf( i ? "-%02X" : "%02X", k[i] );
    }
    printf( "\r\n" );
}

/* Depois do [RX] JoinAccept — igual aos nós ESP32:
 *   (linha em branco)
 *   Frame raw (RX, Join-accept cifrado): …     (diagnóstico de sessão)
 *     DevAddr : 0x01034B99
 *     NwkSKey : 40-D8-48-…
 *     AppSKey : 5F-9D-E0-…
 *     NetID   : 0
 * Vai só para o console (página/app ao vivo e "Baixar sessão"), como o resto
 * do diagnóstico: o log em RAM do nó não guarda as chaves. */
static void PrintJoinSession( void )
{
    static uint8_t f[256];
    uint8_t n = 0;

    /* o Join-accept ainda pode estar no gancho do rádio (o laço principal não
     * passou por ele): pega daqui, senão usa o que o laço guardou */
    if( BoardRadioTakeRxFrame( f, &n ) && ( n > 0 ) && ( ( f[0] >> 5 ) == 1 ) )
    {
        JoinAcceptRawLen = ( n > sizeof( JoinAcceptRaw ) ) ? sizeof( JoinAcceptRaw ) : n;
        memcpy( JoinAcceptRaw, f, JoinAcceptRawLen );
    }

    NodeLogPause( true );
    if( ( NodeCfg.sessDiagOff == 0 ) && ( JoinAcceptRawLen > 0 ) )
    {
        printf( "\r\n" );
        PrintRaw( "Frame raw (RX, Join-accept cifrado): ", JoinAcceptRaw, JoinAcceptRawLen );
    }
    JoinAcceptRawLen = 0;

    uint32_t da = 0;
    uint8_t nwk[16], app[16];
    if( AppGetSessionKeys( &da, nwk, app ) )
    {
        printf( "  DevAddr : 0x%08lX\r\n", ( unsigned long )da );
        PrintKeyDashed( "  NwkSKey : ", nwk );
        PrintKeyDashed( "  AppSKey : ", app );
    }
    else
    {
        printf( "  DevAddr : 0x%08lX (chaves da sessão indisponíveis)\r\n", ( unsigned long )AppStatus.devAddr );
    }
    MibRequestConfirm_t m;
    m.Type = MIB_NET_ID;
    if( LoRaMacMibGetRequestConfirm( &m ) == LORAMAC_STATUS_OK )
    {
        printf( "  NetID   : %lu\r\n", ( unsigned long )m.Param.NetID );
    }
    NodeLogPause( false );
}

static void OnJoinRequest( LmHandlerJoinParams_t *params )
{
    AppStatus.joining = false;
    if( params->Status == LORAMAC_HANDLER_ERROR )
    {
        printf( "EV_JOIN_TXCOMPLETE: no JoinAccept\r\n" );
        JoinAcceptRawLen = 0;
        PrintJoinTx( params->Datarate );
        uint32_t f = BoardGetLastTxFreq( );
        printf( "[JOIN] Sem Join-accept (Join-request em %lu,%lu MHz, DR%d, %d dBm). Tentando de novo...\r\n",
                ( unsigned long )( f / 1000000UL ), ( unsigned long )( ( f / 100000UL ) % 10UL ),
                params->Datarate, BoardGetLastTxPower( ) );
        if( NodeCfg.leds )
        {
            BoardLedSet( BOARD_LED_RED, true );
            TimerStart( &LedRxTimer );
        }
        SaveDevNonce( );
        if( AtJoinActive && ( --AtJoinLeft == 0 ) )
        {
            /* AT+JOIN: acabaram as NBTRIALS tentativas */
            AtJoinActive = false;
            AppStatus.joining = false;
            AtPrintAsync( "AT JOIN ERROR" );
            return;
        }
        AppStatus.joining = true;
        /* aqui dentro a MAC ainda está ocupada: o laço principal refaz o pedido */
        JoinRetryPending = true;
        return;
    }
    SaveDevNonce( );
    if( AtJoinActive )
    {
        AtJoinActive = false;
        AtPrintAsync( "AT JOIN OK" );
    }

    MibRequestConfirm_t m;
    m.Type = MIB_DEV_ADDR;
    LoRaMacMibGetRequestConfirm( &m );
    AppStatus.joined = true;
    AppStatus.devAddr = m.Param.DevAddr;
    DrSyncPending = true;                   /* a MAC ficou no DR do Join-request: volta ao configurado */
    AppStatus.sessionAtMs = AppUptimeMs( );
    AppStatus.sessionRestored = false;
    if( params->CommissioningParams->IsOtaaActivation )
    {
        printf( "EV_JOINED\r\n" );
        PrintJoinTx( params->Datarate );
        PrintJoinAccept( params->Datarate );
        PrintJoinSession( );
        uint32_t f = BoardGetLastTxFreq( );
        printf( "[JOIN] Conectado! DevAddr %08lX, DR%d (Join-request em %lu,%lu MHz).\r\n", ( unsigned long )AppStatus.devAddr,
                params->Datarate, ( unsigned long )( f / 1000000UL ), ( unsigned long )( ( f / 100000UL ) % 10UL ) );
    }
    else
    {
        printf( "[ABP] Sessão ativa. DevAddr %08lX.\r\n", ( unsigned long )AppStatus.devAddr );
        static const uint8_t zero[16] = { 0 };
        if( ( NodeCfg.devAddr == 0 ) || ( memcmp( NodeCfg.nwkSKey, zero, 16 ) == 0 ) || ( memcmp( NodeCfg.appSKey, zero, 16 ) == 0 ) )
        {
            printf( "[ABP] ATENÇÃO: DevAddr/NwkSKey/AppSKey ainda zerados. Configure com 'set devaddr', "
                    "'set nwkskey', 'set appskey' e 'save'.\r\n" );
        }
    }

    AppRequestClass( NodeCfg.devClass );

    /* primeiro uplink logo depois do JOIN */
    AppSendNow( NULL, 0, true );
}

static const char* SlotName( int8_t slot )
{
    switch( slot )
    {
    case RX_SLOT_WIN_1:                     return "RX1";
    case RX_SLOT_WIN_2:                     return "RX2";
    case RX_SLOT_WIN_CLASS_C:               return "Classe C (RXC)";
    case RX_SLOT_WIN_CLASS_C_MULTICAST:     return "Classe C multicast";
    case RX_SLOT_WIN_CLASS_B_PING_SLOT:     return "Classe B (ping slot)";
    case RX_SLOT_WIN_CLASS_B_MULTICAST_SLOT:return "Classe B multicast";
    default:                                return "?";
    }
}

static void OnTxData( LmHandlerTxParams_t *params )
{
    if( params->IsMcpsConfirm == 0 )
    {
        return;   /* confirmação de MLME (JOIN, DeviceTime...) */
    }
    /* mesma sequência dos nós ESP32: EV_TXCOMPLETE e, logo depois, o [TX] */
    printf( "EV_TXCOMPLETE (includes waiting for RX windows)\r\n" );
    NodeV1OnTxDone( params->AppData.Port );     /* REINICIAR: a resposta já saiu */
    AppStatus.fcntUp = params->UplinkCounter;
    if( params->Status == LORAMAC_EVENT_INFO_STATUS_OK )
    {
        AppStatus.uplinks++;
        if( params->AckReceived )
        {
            AppStatus.acks++;
        }
    }
    else
    {
        AppStatus.uplinkErrors++;
    }
    if( AtTxActive && AtTxConfirmed )
    {
        AtPrintAsync( params->AckReceived ? "AT ACK OK" : "AT ACK ERROR" );
    }
    AtTxActive = false;
    uint32_t f = BoardGetLastTxFreq( );
    if( LastTxDropped )
    {
        /* o LmHandlerSend( ) mandou o quadro vazio: o TxParams ainda traz o payload pedido */
        LastTxDropped = false;
        printf( "[TX] FCnt %lu · %s · DR%d · %lu,%lu MHz · %d dBm · canal %u · sem payload (%u bytes não couberam: "
                "só cabeçalho/comandos MAC)%s\r\n",
                ( unsigned long )params->UplinkCounter, "não confirmado",
                params->Datarate, ( unsigned long )( f / 1000000UL ), ( unsigned long )( ( f / 100000UL ) % 10UL ),
                BoardGetLastTxPower( ), params->Channel, ( unsigned )LastTxWanted,
                ( params->Status == LORAMAC_EVENT_INFO_STATUS_OK ) ? "" : " · ERRO" );
        return;
    }
    printf( "[TX] FCnt %lu · %s · DR%d · %lu,%lu MHz · %d dBm · canal %u · porta %u · %u bytes%s%s\r\n",
            ( unsigned long )params->UplinkCounter,
            ( params->MsgType == LORAMAC_HANDLER_CONFIRMED_MSG ) ? "confirmado" : "não confirmado",
            params->Datarate, ( unsigned long )( f / 1000000UL ), ( unsigned long )( ( f / 100000UL ) % 10UL ),
            BoardGetLastTxPower( ), params->Channel, params->AppData.Port, params->AppData.BufferSize,
            ( params->MsgType == LORAMAC_HANDLER_CONFIRMED_MSG ) ? ( params->AckReceived ? " · ACK" : " · sem ACK" ) : "",
            ( params->Status == LORAMAC_EVENT_INFO_STATUS_OK ) ? "" : " · ERRO" );
}

static bool BytesEqual( const uint8_t *a, const uint8_t *b, uint8_t n )
{
    for( uint8_t i = 0; i < n; i++ )
    {
        if( a[i] != b[i] )
        {
            return false;
        }
    }
    return true;
}

static void HandleCommand( const uint8_t *d, uint8_t n )
{
    /* Comandos na FPort NodeCfg.cmdPort:
     *   01 cc        classe: 00 = A, 01 = B, 02 = C (grava)
     *   02 hh ll     intervalo de uplink em segundos (grava)
     *   03 pp        periodicidade do ping slot 0..7 (grava)
     *   04 cc        uplink confirmado: 00 = não, 01 = sim (grava)
     *   05           manda um uplink agora                              */
    if( n == 0 )
    {
        return;
    }
    switch( d[0] )
    {
    case 0x01:
        if( ( n >= 2 ) && ( d[1] <= 2 ) )
        {
            NodeCfg.devClass = "ABC"[d[1]];
            NodeConfigSave( );
            printf( "[CMD] Classe %c por downlink.\r\n", NodeCfg.devClass );
            AppRequestClass( NodeCfg.devClass );
        }
        break;
    case 0x02:
        if( n >= 3 )
        {
            uint32_t s = ( ( uint32_t )d[1] << 8 ) | d[2];
            if( ( s >= 5 ) && ( s <= 65535 ) )
            {
                NodeCfg.txIntervalS = s;
                NodeConfigSave( );
                printf( "[CMD] Intervalo de uplink: %lu s.\r\n", ( unsigned long )s );
                AppApplyRuntimeConfig( );
            }
        }
        break;
    case 0x03:
        if( ( n >= 2 ) && ( d[1] <= 7 ) )
        {
            NodeCfg.pingPeriodicity = d[1];
            NodeConfigSave( );
            LmHandlerParams.PingSlotPeriodicity = d[1];
            printf( "[CMD] Ping slot a cada %u s.\r\n", 1U << d[1] );
            if( AppStatus.activeClass == 'B' )
            {
                LmHandlerPingSlotReq( d[1] );
            }
        }
        break;
    case 0x04:
        if( n >= 2 )
        {
            NodeCfg.confirmed = d[1] ? 1 : 0;
            NodeConfigSave( );
            AppApplyRuntimeConfig( );
            printf( "[CMD] Uplink %s.\r\n", NodeCfg.confirmed ? "confirmado" : "não confirmado" );
        }
        break;
    case 0x05:
        AppSendNow( NULL, 0, true );
        break;
    default:
        printf( "[CMD] Comando 0x%02X desconhecido.\r\n", d[0] );
        break;
    }
}

static void OnRxData( LmHandlerAppData_t *appData, LmHandlerRxParams_t *params )
{
    if( ( params->IsMcpsIndication == 0 ) || ( appData == NULL ) )
    {
        return;
    }

    AppStatus.downlinks++;
    AppStatus.fcntDown = params->DownlinkCounter;
    AppStatus.lastRssi = params->Rssi;
    AppStatus.lastSnr = params->Snr;
    AppStatus.lastDr = params->Datarate;
    AppStatus.lastSlot = SlotName( params->RxSlot );
    AppStatus.lastPort = appData->Port;
    AppStatus.lastLen = appData->BufferSize;
    memcpy( AppStatus.lastData, appData->Buffer,
            ( appData->BufferSize > sizeof( AppStatus.lastData ) ) ? sizeof( AppStatus.lastData ) : appData->BufferSize );

    if( NodeCfg.leds )
    {
        BoardLedSet( BOARD_LED_GREEN, true );
        TimerStart( &LedRxTimer );
        RxBlinkLeft = 1;                    /* verde pisca duas vezes (como no original) */
    }

    char hex[2 * 32 + 1];
    uint8_t shown = ( appData->BufferSize > 32 ) ? 32 : appData->BufferSize;
    BytesToHex( appData->Buffer, shown, hex );
    uint32_t rxf = 0;
    BoardGetLastRx( &rxf, NULL, NULL, NULL );
    printf( "[RX] %s · DR%d · ", AppStatus.lastSlot, params->Datarate );
    PrintMHz( rxf );
    printf( " · porta %u · %u bytes%s%s%s · RSSI %d dBm · SNR %d dB · FCntDown %lu\r\n",
            appData->Port, appData->BufferSize, ( appData->BufferSize > 0 ) ? " " : "", hex,
            ( appData->BufferSize > 32 ) ? "…" : "",
            params->Rssi, params->Snr, ( unsigned long )params->DownlinkCounter );

    /* Serial transparente: o que chega na porta 4 vai para GPIO1 (TX), sem RSSI/SNR */
    if( ( appData->Port == NODE_IO_PORT_TRANSPARENT ) && NodeIoTransparentActive( ) )
    {
        NodeIoTransparentWrite( appData->Buffer, appData->BufferSize );
        return;
    }

    /* Formato do firmware original: RX:<payload hex>:<RSSI>:<SNR> */
    AtPrintRx( appData->Buffer, appData->BufferSize, params->Rssi, params->Snr );

    if( appData->BufferSize == 0 )
    {
        return;
    }

    /* Protocolo ADAILSILVA_V1: comando seq | opcode | parâmetros na porta 10 */
    if( appData->Port == NODE_V1_PORT_CMD )
    {
        NodeV1OnCommand( appData->Buffer, appData->BufferSize );
        return;
    }

    /* Comandos AT por downlink (texto ASCII; vários separados por ';' ou nova linha) */
    if( ( NodeCfg.atPort != 0 ) && ( appData->Port == NodeCfg.atPort ) )
    {
        AppStatus.atDownlinks++;
        AtReplyLen = ( uint8_t )AtExecuteRemote( appData->Buffer, appData->BufferSize, ( char* )AtReply, sizeof( AtReply ) );
        printf( "[AT] Comando por downlink executado; resposta (%u bytes) vai na porta %u.\r\n", AtReplyLen, NodeCfg.atPort );
        return;
    }

    if( NodeCfg.rstEnable && ( appData->Port == NodeCfg.rstPort ) && ( appData->BufferSize == NodeCfg.rstLen ) &&
        BytesEqual( appData->Buffer, NodeCfg.rstCmd, NodeCfg.rstLen ) )
    {
        printf( "[RESET] Comando de reset remoto recebido.\r\n" );
        AppReboot( NodeCfg.rstDelayMs );
        return;
    }

    if( ( NodeCfg.cmdPort != 0 ) && ( appData->Port == NodeCfg.cmdPort ) )
    {
        HandleCommand( appData->Buffer, appData->BufferSize );
    }
}

static void OnClassChange( DeviceClass_t deviceClass )
{
    char c = ( deviceClass == CLASS_B ) ? 'B' : ( deviceClass == CLASS_C ) ? 'C' : 'A';
    AppStatus.activeClass = c;
    printf( "[Classe] Agora em Classe %c.\r\n", c );

    if( deviceClass == CLASS_B )
    {
        /* Avisa o servidor já: o uplink sai com o bit Class B ligado */
        AppSendNow( NULL, 0, true );
    }
    else if( ( deviceClass == CLASS_A ) && ( NodeCfg.devClass == 'B' ) && AppStatus.joined )
    {
        /* Classe B caiu (beacon perdido): tenta de novo daqui a pouco */
        TimerStart( &ClassBRetryTimer );
    }
}

static void OnBeaconStatusChange( LoRaMacHandlerBeaconParams_t *params )
{
    switch( params->State )
    {
    case LORAMAC_HANDLER_BEACON_RX:
        AppStatus.beacon = 'R';
        AppStatus.beaconsRx++;
        AppStatus.beaconTime = params->Info.Time.Seconds;
        printf( "[Beacon] Recebido · GPS %lu s · %lu kHz · DR%d · RSSI %d dBm · SNR %d dB\r\n",
                ( unsigned long )params->Info.Time.Seconds, ( unsigned long )( params->Info.Frequency / 1000UL ),
                params->Info.Datarate, params->Info.Rssi, params->Info.Snr );
        if( NodeCfg.leds )
        {
            BoardLedSet( BOARD_LED_GREEN, true );
            TimerStart( &LedRxTimer );
        }
        break;
    case LORAMAC_HANDLER_BEACON_NRX:
        AppStatus.beacon = 'N';
        AppStatus.beaconsMissed++;
        printf( "[Beacon] Não recebido nesta janela.\r\n" );
        break;
    case LORAMAC_HANDLER_BEACON_LOST:
        AppStatus.beacon = 'L';
        printf( "[Beacon] Perdido: voltando para a Classe A.\r\n" );
        break;
    case LORAMAC_HANDLER_BEACON_ACQUIRING:
    default:
        AppStatus.beacon = 'A';
        break;
    }
}

static void OnSysTimeUpdate( bool isSynchronized, int32_t timeCorrection )
{
    AppStatus.timeSynced = isSynchronized;
    SysTime_t t = SysTimeGet( );
    printf( "[Tempo] Hora sincronizada pelo servidor (GPS %lu s, correção %ld s).\r\n",
            ( unsigned long )t.Seconds, ( long )timeCorrection );
}

static void OnTxPeriodicityChanged( uint32_t periodicity )
{
    /* pacote de certificação (porta 224) */
    TimerStop( &TxTimer );
    TimerSetValue( &TxTimer, periodicity ? periodicity : NodeCfg.txIntervalS * 1000UL );
    TimerStart( &TxTimer );
}

static void OnTxFrameCtrlChanged( LmHandlerMsgTypes_t isTxConfirmed )
{
    LmHandlerParams.IsTxConfirmed = isTxConfirmed;
}

static void OnPingSlotPeriodicityChanged( uint8_t pingSlotPeriodicity )
{
    LmHandlerParams.PingSlotPeriodicity = pingSlotPeriodicity;
}

/* =================================================================== */
/*                               timers                                */
/* =================================================================== */
static void OnTxTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &TxTimer );
    if( NodeCfg.txIntervalS == 0 )
    {
        return;                             /* uplink periódico desligado */
    }
    IsTxFramePending = 1;
    TimerSetValue( &TxTimer, NodeCfg.txIntervalS * 1000UL );
    TimerStart( &TxTimer );
    AppStatus.nextTxMs = TimerGetCurrentTime( ) + NodeCfg.txIntervalS * 1000UL;
}

static void OnLedTxTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &LedTxTimer );
    BoardLedSet( BOARD_LED_RED, false );
}

static void OnLedRxTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &LedRxTimer );
    if( RxBlinkLeft )
    {
        /* segunda piscada do verde: apaga agora, acende de novo no próximo disparo */
        static bool off = false;
        off = !off;
        BoardLedSet( BOARD_LED_GREEN, !off );
        if( !off )
        {
            RxBlinkLeft--;
        }
        TimerStart( &LedRxTimer );
        return;
    }
    BoardLedSet( BOARD_LED_GREEN, false );
    BoardLedSet( BOARD_LED_RED, false );
}

static void OnLedSerTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &LedSerTimer );
    BoardLedSet( BOARD_LED_YELLOW, false );
}

static void OnKeepAliveTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &KeepAliveTimer );
    IsKeepAlivePending = 1;
    if( NodeCfg.kaEnable && NodeCfg.kaPeriodMs )
    {
        TimerSetValue( &KeepAliveTimer, NodeCfg.kaPeriodMs );
        TimerStart( &KeepAliveTimer );
    }
}

static void OnRestartTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &RestartTimer );
    BoardResetMcu( );                       /* credenciais/canais/NJM novos por AT */
}

static void OnTranspTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &TranspTimer );              /* só acorda o laço principal */
}

static void OnRebootTimerEvent( void *context )
{
    ( void )context;
    BoardResetMcu( );
}

static void OnJoinRetryTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &JoinRetryTimer );
    JoinRetryPending = true;
}

static void OnTxRetryTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &TxRetryTimer );
    IsTxFramePending = 1;
}

static void OnClassBRetryTimerEvent( void *context )
{
    ( void )context;
    TimerStop( &ClassBRetryTimer );
    IsClassBRetryPending = 1;
}
