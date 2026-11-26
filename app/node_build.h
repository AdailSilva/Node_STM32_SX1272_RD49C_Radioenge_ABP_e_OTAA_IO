/*
 * node_build.h — valores de fábrica do firmware LoRaWAN Node Radioenge ABP e OTAA
 * com atuadores (relés) e sensores (PZEM-004T v3), protocolo ADAILSILVA_V1.
 *
 * Um firmware só para as duas ativações: ABP e OTAA estão sempre presentes e a
 * escolha é feita em tempo de execução (console "set njm 0|1" ou AT+NJM=0/1,
 * pela página WEB ou pelo app), sem regravar. Tudo o que está aqui é só o
 * padrão de fábrica: o valor em uso fica na EEPROM do STM32 e é trocado pelo
 * console (comando "set") ou pelos comandos AT do firmware original da
 * Radioenge (AT+NJM, AT+DADDR, AT+APPKEY...).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NODE_BUILD_H
#define NODE_BUILD_H

/* ----------------------------------------------------------- ativação */
#define NODE_ACTIVATION_ABP     0
#define NODE_ACTIVATION_OTAA    1
/* Ativação de fábrica (módulo novo, depois do "factory" ou do "Do zero").
 * OTAA, como recomenda a LoRa Alliance; para ABP: "set njm 0" + "save" ou AT+NJM=0. */
#define NODE_ACTIVATION         NODE_ACTIVATION_OTAA

#define NODE_FW_NAME            "LoRaWAN Node Radioenge Atuadores e Sensores"
#ifndef NODE_FW_VERSION                /* o GitHub Actions passa a versão da tag */
#define NODE_FW_VERSION         "0.0.1"
#endif
#define NODE_STACK_VERSION      "LoRaMac-node v4.7.1"

/* ------------------------------------------------- credenciais (OTAA) */
/* Usadas com AT+NJM=1 (OTAA). Em ABP não há JOIN: DevEUI e JoinEUI ficam só
 * como identificação. */
/* DevEUI: com DEF_DEVEUI_AUTO = 1 ele é gerado do ID único do STM32 (cada
 * módulo tem o seu). Com 0, vale DEF_DEVEUI. Bytes na ordem em que o
 * ChirpStack mostra (MSB primeiro). */
#define DEF_DEVEUI_AUTO         1
#define DEF_DEVEUI              { 0, 0, 0, 0, 0, 0, 0, 0 }
#define DEF_JOINEUI             { 0xb2, 0xf6, 0x34, 0xa1, 0xe6, 0x2c, 0x10, 0xc7 }  /* AppEUI */
#define DEF_APPKEY              { 0x6f, 0xc5, 0x7c, 0x4c, 0xda, 0x46, 0xdd, 0xc5, \
                                  0x02, 0xbc, 0xb7, 0x85, 0x5e, 0x4e, 0x94, 0xae }

/* -------------------------------------------------- credenciais (ABP) */
/* Usadas com AT+NJM=0 (ABP). Preencha com os valores do ChirpStack (aba
 * "Activation" do dispositivo) ou troque pelo console: set devaddr /
 * set nwkskey / set appskey (ou AT+DADDR / AT+NWKSKEY / AT+APPSKEY). */
#define DEF_DEVADDR             0x00000000UL
#define DEF_NWKSKEY             { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
#define DEF_APPSKEY             { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
#define DEF_NETID               0x000000UL

/* -------------------------------------------------------- rádio / MAC */
#define DEF_SUBBAND             1      /* AU915 SB1 = canais 8–15 (0..7; 8 = todos) */
#define DEF_CH500               1      /* + canal de 500 kHz 64 + sub-banda (65) */
#define DEF_ADR                 0
#define DEF_DR                  5      /* DR5 = SF7/BW125 (AU915)                */
#define DEF_TXPOW_DBM           14     /* potência conduzida aproximada (dBm)    */

/* ------------------------------------------------------------- classe */
#define DEF_CLASS               'A'    /* 'A', 'B' ou 'C'                        */
#define DEF_PING_PERIODICITY    4      /* Classe B: ping slot a cada 2^4 = 16 s  */

/* ------------------------------------------------------------- uplink */
#define DEF_TX_INTERVAL_S       60
#define DEF_FPORT               101
#define DEF_CONFIRMED           0
#define DEF_PAYLOAD             "AdailSilva-IoT"

/* ------------------------------------------------------- reset remoto */
#define DEF_RST_ENABLE          1
#define DEF_RST_PORT            101
#define DEF_RST_CMD             { 0xAD, 0xA1, 0xDE, 0xAD, 0xBE, 0xEF, 0x0D, 0x0A }
#define DEF_RST_DELAY_MS        2000

/* -------------------------------------------- comandos por downlink */
#define DEF_CMD_PORT            102    /* 0 = desligado                          */

/* ------------------------------------------------------------- energia */
#define DEF_LOWPOWER            1      /* 0 = sem STOP (só SLEEP)
                                        * 1 = STOP, console sempre pronto (+~0,1 mA)
                                        * 2 = STOP mais econômico: o 1º caractere
                                        *     digitado só acorda o MCU (tecle Enter antes) */
#define DEF_EXTPOWER            1      /* 1 = alimentação externa (bateria = 0)  */
#define DEF_LEDS                1

/* ------------------------------------- comandos AT (firmware Radioenge) */
/* Valores de fábrica dos parâmetros que só os comandos AT mexem. Os nomes
 * seguem os comandos do firmware original (Lora_AT_v1r3s35). */
#define DEF_AUTOJOIN            1      /* AT+AJOIN: JOIN automático no boot (OTAA)     */
#define DEF_TXP_INDEX           0xFF   /* AT+TXP: 0xFF = usa DEF_TXPOW_DBM             */
#define DEF_DUTYCYCLE           0      /* AT+DCS: AU915 não tem duty cycle            */
#define DEF_PUBLIC_NETWORK      1      /* AT+PNM: sync word pública (0x34)            */
#define DEF_RX2_FREQ            923300000UL  /* AT+RX2FQ: AU915                       */
#define DEF_RX2_DR              8      /* AT+RX2DR: DR8 = SF12/BW500                  */
#define DEF_RX1_DELAY_MS        1000   /* AT+RX1DL                                    */
#define DEF_RX2_DELAY_MS        2000   /* AT+RX2DL                                    */
#define DEF_JOIN1_DELAY_MS      5000   /* AT+JN1DL                                    */
#define DEF_JOIN2_DELAY_MS      6000   /* AT+JN2DL                                    */
#define DEF_NBTRIALS            8      /* AT+NBTRIALS                                 */
#define DEF_BAUD_INDEX          3      /* AT+BAUDRATE: 3 = 115200 (Radioenge: 0 = 9600) */
#define DEF_KEEPALIVE_ENABLE    0      /* AT+KEEPALIVE=0:5:0:60000                    */
#define DEF_KEEPALIVE_PORT      5
#define DEF_KEEPALIVE_CONFIRMED 0
#define DEF_KEEPALIVE_PERIOD_MS 60000UL
#define DEF_AT_PORT             103    /* AT por downlink (0 = desligado)             */

/* --------------------- atuadores, sensores e protocolo ADAILSILVA_V1 */
/* Conteúdo do uplink periódico (AT+TELEM / "set uplinkmode"):
 *   0 = payload fixo (DEF_PAYLOAD) na FPort DEF_FPORT;
 *   1 = telemetria do protocolo ADAILSILVA_V1 (TLV) na FPort 20. */
#define DEF_UPLINK_MODE         1
/* Medidores PZEM-004T v3 de fábrica: 1 no endereço Modbus genérico F8 (GPIO0/GPIO1,
 * USART2 a 9600 8N1). 0 = modo PZEM desligado (GPIO0/GPIO1 livres). Um módulo que já
 * usa a serial transparente (AT+GPIOC=0:3) continua com ela. Relés: nenhum de fábrica
 * (AT+RELECFG). */
#define DEF_PZEM_COUNT          1

#endif /* NODE_BUILD_H */
