/*
 * node_config.h — configuração do nó gravada na EEPROM do STM32L071.
 *
 * Mapa da EEPROM de dados (6 KB, 0x08080000–0x080817FF):
 *   0x0000 .. NODE_NVM_LIMIT-1   contexto da LoRaMac-node (sessão, FCnt, DevNonce,
 *                                canais, Classe B...) gravado pelo NvmDataMgmt
 *   NODE_CFG_OFFSET ..           esta configuração (NodeConfig_t + CRC)
 *   NODE_META_OFFSET ..          metadados (impressão das credenciais, último DevNonce)
 *   NODE_RELAY_OFFSET ..         estado dos relés (anel de NODE_RELAY_SLOTS registros, veja
 *                                node_relay.c: cada mudança grava num registro diferente)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define NODE_NVM_LIMIT          0x1000      /* 4 KB para o contexto da LoRaMac-node */
#define NODE_CFG_OFFSET         0x1000
#define NODE_META_OFFSET        0x1600
#define NODE_RELAY_OFFSET       0x1700      /* estado dos relés (256 bytes) */
#define NODE_RELAY_AREA         0x0100
#define NODE_EEPROM_SIZE        0x1800

#define NODE_CFG_MAGIC          0x524E      /* "RN" */
#define NODE_CFG_VERSION        3       /* 2: campos dos comandos AT da Radioenge; 3: relés, PZEM e
                                         * conteúdo do uplink periódico (v1 e v2 são migradas) */
#define NODE_PAYLOAD_MAX        51          /* AU915 com dwell time: DR2 aceita 11 bytes, DR3 53 */
#define NODE_RST_CMD_MAX        16

#define NODE_SUBBAND_ALL        8       /* "subband 8": os 64 canais de 125 kHz */
#define NODE_SUBBAND_CUSTOM     9       /* máscara livre gravada por AT+CHMASK (chMask[]) */

#define NODE_GPIO_COUNT         10      /* GPIO0..GPIO9 do módulo */
#define NODE_GPIO_UNUSED        0xFF    /* pino não configurado (alta impedância, menor consumo) */
#define NODE_TXP_FROM_DBM       0xFF    /* txPowIdx: usar txPowDbm */

#define NODE_RELAY_MAX          8       /* relés (canais 0..7) em GPIOs do módulo */
#define NODE_PZEM_MAX           4       /* medidores PZEM-004T v3 no barramento Modbus */
#define NODE_PZEM_ADDR_GENERIC  0xF8    /* endereço Modbus "genérico" (um medidor só no barramento) */

#define NODE_UPLINK_LEGACY      0       /* uplink periódico: payload fixo na FPort configurada */
#define NODE_UPLINK_V1          1       /* uplink periódico: telemetria ADAILSILVA_V1 (TLV, porta 20) */

typedef struct __attribute__( ( packed ) )
{
    uint16_t magic;
    uint16_t version;

    /* credenciais (MSB primeiro, como no ChirpStack) */
    uint8_t  devEuiAuto;
    uint8_t  devEui[8];
    uint8_t  joinEui[8];
    uint8_t  appKey[16];
    uint8_t  nwkKey[16];        /* LoRaWAN 1.1; em 1.0.x é igual à AppKey */
    uint32_t devAddr;
    uint8_t  nwkSKey[16];
    uint8_t  appSKey[16];
    uint32_t netId;

    /* rádio / MAC */
    uint8_t  subBand;           /* 0..7 (SB n = canais 8n..8n+7 + 64+n); 8 = todos */
    uint8_t  ch500;             /* canal de 500 kHz da sub-banda */
    uint8_t  adr;
    uint8_t  dr;                /* 2..6 (AU915 com dwell time de 400 ms: DR0/DR1 proibidos) */
    int8_t   txPowDbm;

    /* classe */
    char     devClass;          /* 'A', 'B' ou 'C' */
    uint8_t  pingPeriodicity;   /* 0..7: ping slot a cada 2^n s */

    /* uplink */
    uint32_t txIntervalS;
    uint8_t  fport;
    uint8_t  confirmed;
    uint8_t  payloadLen;
    uint8_t  payload[NODE_PAYLOAD_MAX];

    /* reset remoto */
    uint8_t  rstEnable;
    uint8_t  rstPort;
    uint8_t  rstLen;
    uint8_t  rstCmd[NODE_RST_CMD_MAX];
    uint16_t rstDelayMs;

    /* comandos por downlink */
    uint8_t  cmdPort;

    /* energia / LEDs */
    uint8_t  lowPower;          /* 0 = sem STOP, 1 = STOP + console, 2 = STOP econômico */
    uint8_t  extPower;
    uint8_t  leds;

    /* ---- v2: comandos AT do firmware original da Radioenge ---------- */
    uint8_t  activation;        /* AT+NJM: 0 = ABP, 1 = OTAA (padrão: NODE_ACTIVATION do node_build.h) */
    uint8_t  autoJoin;          /* AT+AJOIN: JOIN automático no boot (OTAA) */
    uint8_t  txPowIdx;          /* AT+TXP: 0..15 (EIRP máx. - 2*n); 0xFF = usar txPowDbm */
    uint8_t  dutyCycle;         /* AT+DCS */
    uint8_t  publicNet;         /* AT+PNM */
    uint8_t  rx2Dr;             /* AT+RX2DR */
    uint32_t rx2Freq;           /* AT+RX2FQ (Hz) */
    uint16_t rx1DelayMs;        /* AT+RX1DL */
    uint16_t rx2DelayMs;        /* AT+RX2DL */
    uint16_t join1DelayMs;      /* AT+JN1DL */
    uint16_t join2DelayMs;      /* AT+JN2DL */
    uint8_t  nbTrials;          /* AT+NBTRIALS: tentativas (confirmados e JOIN pedido por AT) */
    uint8_t  baudIdx;           /* AT+BAUDRATE: 0 = 9600, 1 = 19200, 2 = 43000, 3 = 115200 */
    uint8_t  kaEnable;          /* AT+KEEPALIVE=enable:port:confirmed:period */
    uint8_t  kaPort;
    uint8_t  kaConfirmed;
    uint32_t kaPeriodMs;
    uint16_t chMask[6];         /* AT+CHMASK (vale com subBand = NODE_SUBBAND_CUSTOM) */
    uint8_t  gpioMode[NODE_GPIO_COUNT];   /* AT+GPIOC: modo 0..8 ou NODE_GPIO_UNUSED */
    uint8_t  gpioPull[NODE_GPIO_COUNT];   /* 0 = sem, 1 = pull-up, 2 = pull-down */
    uint8_t  atPort;            /* FPort dos comandos AT por downlink (0 = desligado) */

    /* Diagnóstico de sessão a cada envio (DevAddr, NwkSKey, AppSKey, Frame raw,
     * FCnt e MIC). Gravado invertido para que as
     * configurações antigas (reservado = 0) já venham com ele ligado. */
    uint8_t  sessDiagOff;       /* 0 = ligado (padrão), 1 = desligado */

    /* ---- v3: atuadores, sensores e protocolo ADAILSILVA_V1 ----------- */
    uint8_t  uplinkMode;        /* NODE_UPLINK_LEGACY ou NODE_UPLINK_V1 (AT+TELEM) */
    uint8_t  relayCount;        /* relés configurados (0..NODE_RELAY_MAX); canal n = relayPin[n] */
    uint8_t  relayPin[NODE_RELAY_MAX];   /* GPIO do módulo de cada relé (AT+RELECFG) */
    uint8_t  relayActiveLow;    /* bit n = relé n liga com nível 0 (módulos de relé comuns) */
    uint8_t  pzemCount;         /* medidores PZEM-004T v3 (0 = modo PZEM desligado) */
    uint8_t  pzemAddr[NODE_PZEM_MAX];    /* endereço Modbus de cada medidor (AT+PZEMCFG) */

    uint8_t  reserved[23];
}NodeConfig_t;

/* Tamanho da configuração v1 (antes dos campos AT), para migrar sem perder nada */
#define NODE_CFG_V1_SIZE        ( offsetof( NodeConfig_t, activation ) + 32 )
/* Tamanho da configuração v2 (antes dos relés/PZEM): os 23 bytes reservados vinham logo
 * depois do sessDiagOff */
#define NODE_CFG_V2_SIZE        ( offsetof( NodeConfig_t, uplinkMode ) + 23 )

typedef struct __attribute__( ( packed ) )
{
    uint32_t magic;
    uint32_t credFingerprint;   /* credenciais/canais com que o contexto LoRaMac foi criado */
    uint16_t lastDevNonce;      /* sobrevive ao "factory" e à troca de chaves */
    uint16_t flags;             /* NODE_META_JOIN_ON_BOOT... */
    uint32_t crc;
}NodeMeta_t;

#define NODE_META_JOIN_ON_BOOT  0x0001  /* AT+JOIN com reinício pendente: fazer o JOIN no boot */
#define NODE_META_FP_UNIFIED    0x0002  /* impressão já no formato do firmware único ABP e OTAA */

extern NodeConfig_t NodeCfg;

/* Revisão da configuração: muda a cada alteração do NodeCfg (save, "set",
 * AT, downlink de comando). Vai no jstatus ("cfgrev") para a página e o app
 * relerem o jconfig quando a configuração muda por fora. */
extern volatile uint16_t NodeCfgRev;

static const uint32_t NodeBaudrates[4] = { 9600, 19200, 43000, 115200 };

void     NodeConfigDefaults( NodeConfig_t *c );
bool     NodeConfigLoad( void );                 /* false = usou o padrão de fábrica */
bool     NodeConfigSave( void );
uint32_t NodeConfigCredFingerprint( const NodeConfig_t *c );
#define NODE_FP_NJM_XOR         0x4E4A4D31UL   /* "NJM1": parcela da ativação na impressão */
bool     NodeConfigValidate( const NodeConfig_t *c, char *err, size_t errLen );

bool     NodeMetaLoad( NodeMeta_t *m );
bool     NodeMetaSave( NodeMeta_t *m );

void     NodeDefaultDevEui( uint8_t eui[8] );

/* true = OTAA (AT+NJM=1), false = ABP */
bool     NodeIsOtaa( void );
/* Máscara de canais AU915 (6 palavras de 16 bits, como no AT+CHMASK) da configuração */
void     NodeConfigChannelMask( const NodeConfig_t *c, uint16_t *mask );   /* 6 palavras; aceita destino desalinhado */
/* Grava a máscara: vira sub-banda (0..8 + ch500) quando bate, senão máscara livre.
 * false = máscara inválida (nenhum canal, ou bits fora dos 72 canais) */
bool     NodeConfigSetChannelMask( NodeConfig_t *c, const uint16_t mask[6] );

/* Máscara (bit n = GPIOn) dos pinos usados pelos relés */
uint16_t NodeConfigRelayPins( const NodeConfig_t *c );

/* utilitários */
uint32_t NodeCrc32( const void *data, size_t len );
int      HexToBytes( const char *hex, uint8_t *out, size_t max );   /* -1 = inválido */
void     BytesToHex( const uint8_t *in, size_t len, char *out );    /* out >= 2*len+1 */

#endif /* NODE_CONFIG_H */
