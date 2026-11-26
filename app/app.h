/*
 * app.h — estado do nó e ações que o console pode pedir.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool     joined;
    bool     joining;
    uint32_t devAddr;
    uint32_t joinAttempts;
    uint32_t uplinks;
    uint32_t uplinkErrors;
    uint32_t acks;
    uint32_t downlinks;
    uint32_t fcntUp;
    uint32_t fcntDown;
    int16_t  lastRssi;
    int8_t   lastSnr;
    int8_t   lastDr;
    uint8_t  lastPort;
    uint8_t  lastLen;
    uint8_t  lastData[32];
    const char *lastSlot;
    char     activeClass;      /* classe em uso agora */
    char     beacon;           /* '-' sem beacon, 'A' adquirindo, 'R' recebido, 'N' perdido 1x, 'L' perdido */
    uint32_t beaconsRx;
    uint32_t beaconsMissed;
    uint32_t beaconTime;       /* hora GPS do último beacon (s) */
    bool     timeSynced;
    uint32_t nextTxMs;         /* uptime em que sai o próximo uplink */
    uint32_t sessionAtMs;      /* uptime em que a sessão ficou ativa (JOIN aceito / ABP / restaurada) */
    bool     sessionRestored;  /* true: sessão restaurada da EEPROM no boot */
    uint32_t keepalives;       /* keepalives enviados (AT+KEEPALIVE) */
    uint32_t irqUplinks;       /* uplinks de interrupção (porta 2) */
    uint32_t transpUplinks;    /* uplinks da serial transparente (porta 4) */
    uint32_t atDownlinks;      /* comandos AT recebidos por downlink */
    uint32_t v1Commands;       /* comandos do protocolo ADAILSILVA_V1 (porta 10) */
    uint32_t v1Uplinks;        /* respostas (porta 11) e telemetrias (porta 20) enviadas */
}AppStatus_t;

/* Resultado de um envio pedido pelos comandos AT (respostas do firmware original) */
typedef enum
{
    APP_TX_OK = 0,              /* AT TX OK */
    APP_TX_NOT_JOINED,          /* AT NO NETWORK JOINED */
    APP_TX_BUSY,                /* AT BUSY ERROR */
    APP_TX_LENGTH,              /* AT TX LENGTH ERROR */
    APP_TX_ERROR,               /* AT ERROR */
}AppTxResult_t;

typedef enum
{
    APP_JOIN_STARTED = 0,       /* resposta final virá depois: AT JOIN OK / AT JOIN ERROR */
    APP_JOIN_ALREADY,           /* AT ALREADY JOINED */
    APP_JOIN_RESTART,           /* reinício pendente: reinicia e faz o JOIN no boot */
    APP_JOIN_ERROR,             /* AT ERROR (ABP não faz JOIN) */
}AppJoinResult_t;

extern AppStatus_t AppStatus;

/* Ações (chamadas pelo console no laço principal) */
void     AppSendNow( const uint8_t *data, uint8_t len, bool useCfgPayload );
void     AppRejoin( void );
bool     AppRequestClass( char cls );          /* troca agora, sem salvar */
char     AppCurrentClass( void );               /* classe em uso na LoRaMac-node: 'A', 'B' ou 'C' */
void     AppApplyRuntimeConfig( void );         /* ADR, DR, potência, intervalo, classe */
void     AppRequestDeviceTime( void );
void     AppReboot( uint32_t delayMs );
void     AppFactoryReset( bool keepDevNonce );
uint32_t AppUptimeMs( void );
uint16_t AppGetDevNonce( void );
const char* AppClockSourceName( void );

/*! Chaves da sessão LoRaWAN em uso (LoRaWAN 1.0.x: NwkSKey = FNwkSIntKey).
 *  Retorna false se não houver sessão ativa. */
bool     AppGetSessionKeys( uint32_t *devAddr, uint8_t nwkSKey[16], uint8_t appSKey[16] );

/* Autoteste (comandos diag/jdiag) */
const char* AppResetCauseText( void );    /*!< causa do último reinício, em texto */
const char* AppResetCauseKey( void );     /*!< "por", "pin", "sw", "wdg", "lpwr", "other", "?" */
bool     AppConfigFromEeprom( void );     /*!< false = padrão de fábrica no boot */
bool     AppMacBusy( void );
uint8_t  AppRadioVersion( void );         /*!< registrador 0x42 do SX1272 (0x22) */
uint32_t AppRadioRecoveries( uint32_t *agoS, const char **why );  /*!< recuperações do rádio desde o boot */
void     AppStackInfo( uint32_t *peak, uint32_t *total );   /*!< pico de uso da pilha e área livre */
uint32_t AppHeapUsed( void );

/* ---- comandos AT (at_cmd.c) ---- */
/*! Envia agora (sem fila). trials = tentativas do confirmado (NBTRIALS). */
AppTxResult_t AppSendFrame( uint8_t port, const uint8_t *data, uint8_t len, bool confirmed, uint8_t trials, bool fromAt );
AppJoinResult_t AppAtJoin( void );
/*! Credenciais, canais ou NJM mudaram por AT: reinicia ~5 s depois do último comando */
void     AppScheduleRestart( void );
bool     AppRestartPending( void );
/*! A cada linha recebida no console (LED amarelo e adia o reinício pendente) */
void     AppConsoleActivity( void );
/*! RX2, atrasos das janelas, rede pública e duty cycle (AT+RX2FQ, AT+RX1DL...) */
void     AppApplyMacParams( bool rxWindows );
void     AppApplyKeepalive( void );
void     AppSetConsoleBaudrate( uint32_t baud );
/*! Máximo de bytes de aplicação no DR atual */
uint8_t  AppMaxPayload( void );

#endif /* APP_H */
