# Perfis de dispositivo (Device profiles) no ChirpStack

Este guia mostra como cadastrar no ChirpStack os **perfis de dispositivo de exemplo** usados com este projeto no plano **AU915**. **32 perfis = 2 configurações de região** (`au915_0` / `au915_1`) **× 2 revisões** (A / B) **× 2 ativações** (ABP / OTAA) **× 4 combinações de classe** (A / A+B / A+B+C / A+C).

Cadastre só os que for usar ou os 32 de uma vez: com todos cadastrados, mudar a classe, a ativação ou a sub-banda de um nó é só trocar o perfil do dispositivo, sem editar campos.

## Sumário

- [Qual perfil usar neste projeto](#qual-perfil-usar-neste-projeto)
- [Nome dos perfis](#nome-dos-perfis)
- [Criar um perfil no ChirpStack v4](#criar-um-perfil-no-chirpstack-v4)
- [Os 32 perfis](#os-32-perfis)
- [ChirpStack v3](#chirpstack-v3)

## Qual perfil usar neste projeto

O perfil acompanha a **ativação** (`njm`) e a **classe** gravadas no nó. Com a sub-banda 1 (padrão), revisão B:

| Classe no nó | Nó em ABP (`njm 0`) | Nó em OTAA (`njm 1`) |
|:---:|---|---|
| A | **#25** `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A` | **#29** `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A` |
| B | **#26** `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A+B` | **#30** `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A+B` |
| C | **#28** `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A+C` | **#32** `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A+C` |

Com a sub-banda 0, use o perfil `AU915_0` de mesmo nome (subtraia 16 do número). No ChirpStack de referência o EndDevice roda com os perfis **#28** (ABP) e **#32** (OTAA), em Classe C.

- **Classe B:** os perfis de exemplo usam *ping-slot periodicity* **every 128 seconds**, que corresponde a `set ping 7` no nó (o padrão do firmware é `ping 4` = 16 s). Os dois lados precisam bater: mude o nó ou o perfil.
- **A+B+C:** evite com o nó em Classe C. Com Class-B marcada, o ChirpStack recalcula a classe a cada uplink pelo bit *Class B* do FCtrl (1 → B, 0 → A) e o nó volta a ser tratado como Classe A.
- Depois de trocar o perfil ou a classe, refaça o JOIN (OTAA) ou reative a sessão (ABP): o ChirpStack só lê a Classe C nesse momento.

### Relés, PZEM-004T e o protocolo ADAILSILVA_V1

- **Data rate:** um bloco PZEM da telemetria tem **21 bytes** com o cabeçalho TLV. Em AU915 o DR2 com *dwell time* de 400 ms leva só **11 bytes** de payload: com PZEM use **DR3 ou maior** (`set dr 3` ou `AT+DR=3`; o firmware vem em **DR5**). Com ADR ligado, confira que o servidor não leva o nó abaixo de DR3 (em *Region configuration*, o DR mínimo do ADR). Se a telemetria não couber, o nó divide em vários uplinks com blocos inteiros, e um bloco maior que o DR permite é descartado (aviso no log).
- **Codec:** na aba **Codec**, *JavaScript functions*, cole `doc/codec/codec_adailsilva_v1_chirpstack_v4.js` (no ChirpStack v3, `doc/codec/codec_adailsilva_v1_chirpstack_v3.js`). Detalhes no README, seção *Protocolo ADAILSILVA_V1*.
- **Fila de downlinks:** comandos na **FPort 10**; a resposta chega na porta 11 com o mesmo `seq`. Em **Classe A** o comando sai na janela RX1/RX2 do próximo uplink (use um intervalo curto ou `ENVIAR_AGORA` em seguida); em **Classe C** (perfis A+C) ele chega na hora.
- **Expected uplink interval:** ajuste para o intervalo de telemetria do nó (padrão do firmware: 60 s; o comando `INTERVALO` ou `set interval` no console muda).

A coluna **Region configuration** precisa ser a mesma sub-banda do gateway e do nó: `au915_0` = sub-banda 0 (canais 0–7 + 64) e `au915_1` = sub-banda 1 (canais 8–15 + 65).

## Nome dos perfis

```
Device Profile - <AU915_0 | AU915_1> - 1.0.3 - <A | B> - <ABP | OTAA> - Class <A | A+B | A+B+C | A+C>
                 │                     │       │         │               └ classes que o perfil liga
                 │                     │       │         └ ativação (Supports OTAA desmarcado / marcado)
                 │                     │       └ Regional parameters revision
                 │                     └ MAC version (LoRaWAN 1.0.3)
                 └ Region configuration (au915_0 = sub-banda 0, au915_1 = sub-banda 1)
```

*Description* sugerida, no mesmo padrão: `Device Profile Region AU915 - Region configuration 1 - MAC version LoRaWAN 1.0.3 - Regional parameters revision B - OTAA - Class A`.

O **ID** de cada perfil é gerado pelo ChirpStack ao salvar; não há o que copiar.

## Criar um perfil no ChirpStack v4

*Tenant → Device profiles → Add device profile*. Preencha as abas abaixo e clique em **Submit**. Para criar o próximo, abra um perfil pronto e repita trocando só o que muda (nome, *region configuration*, revisão, OTAA, Class-B e Class-C).

> A *region configuration* (`au915_0`, `au915_1`) só aparece na lista se estiver habilitada no ChirpStack (`[network] enabled_regions` do `chirpstack.toml`, com os arquivos `region_au915_0.toml` / `region_au915_1.toml`).

### Aba General (igual nos 32, menos as três primeiras linhas)

| Campo | Valor |
|---|---|
| Name | o nome da [tabela](#os-32-perfis) |
| Region configuration | `au915_0` ou `au915_1` |
| Regional parameters revision | `A` ou `B` |
| Description | o padrão acima |
| Region | `AU915` |
| MAC version | `LoRaWAN 1.0.3` |
| ADR algorithm | `Default ADR algorithm (LoRa only)` |
| Flush queue on activate | ✅ marcado |
| Allow roaming | desmarcado |
| Expected uplink interval (secs) | `3600` |
| Device-status request frequency (req/day) | `1` |
| RX1 Delay (0 = use system default) | `0` |
| Supported data-rates | vazio |

### Aba Join (OTAA / ABP)

| Campo | Perfis ABP | Perfis OTAA |
|---|---|---|
| Device supports OTAA | desmarcado | ✅ marcado |
| RX1 delay | `1` | — (vem no JoinAccept) |
| RX1 data-rate offset | `0` | — |
| RX2 data-rate | `8` | — |
| RX2 frequency (Hz) | `923300000` | — |

No ABP o nó não recebe esses parâmetros do servidor: os valores do perfil precisam ser os mesmos configurados no nó.

### Aba Class-B (perfis A+B e A+B+C)

| Campo | Valor |
|---|---|
| Device supports Class-B | ✅ marcado (desmarcado nos perfis A e A+C) |
| Class-B confirmed downlink timeout (seconds) | `5` |
| Class-B ping-slot periodicity | `every 128 seconds` |
| Class-B ping-slot data-rate | `8` |
| Class-B ping-slot frequency (Hz) | `0` (frequência padrão da região) |

A *ping-slot periodicity* precisa ser a mesma configurada no nó. Com Class-B marcada, o ChirpStack recalcula a classe a cada uplink pelo bit *Class B* do FCtrl (1 → B, 0 → A); por isso, para um nó em Classe C, prefira os perfis **A+C**.

### Aba Class-C (perfis A+C e A+B+C)

| Campo | Valor |
|---|---|
| Device supports Class-C | ✅ marcado (desmarcado nos perfis A e A+B) |
| Class-C confirmed downlink timeout (seconds) | `0` |

No LoRaWAN 1.0.x o ChirpStack só lê a Classe C do perfil no **JOIN** (OTAA) ou na **(re)ativação** (ABP). Depois de trocar o perfil ou marcar/desmarcar Class-C, refaça o JOIN ou reative a sessão.

### Aba Codec

| Campo | Valor |
|---|---|
| Payload codec | `JavaScript functions` (funções `decodeUplink` / `encodeDownlink`) |

Neste projeto: o conteúdo de `doc/codec/codec_adailsilva_v1_chirpstack_v4.js` (protocolo ADAILSILVA_V1).

## Os 32 perfis

**32 perfis = 2 configurações de região** (`au915_0` / `au915_1`) **× 2 revisões** (A / B) **× 2 ativações** (ABP / OTAA) **× 4 combinações de classe** (A / A+B / A+B+C / A+C).

| # | Nome do perfil (*Name*) | Region configuration | Revisão | Supports OTAA | Class-B | Class-C |
|---:|---|:---:|:---:|:---:|:---:|:---:|
| 1 | `Device Profile - AU915_0 - 1.0.3 - A - ABP - Class A` | `au915_0` | A | — | — | — |
| 2 | `Device Profile - AU915_0 - 1.0.3 - A - ABP - Class A+B` | `au915_0` | A | — | ✅ | — |
| 3 | `Device Profile - AU915_0 - 1.0.3 - A - ABP - Class A+B+C` | `au915_0` | A | — | ✅ | ✅ |
| 4 | `Device Profile - AU915_0 - 1.0.3 - A - ABP - Class A+C` | `au915_0` | A | — | — | ✅ |
| 5 | `Device Profile - AU915_0 - 1.0.3 - A - OTAA - Class A` | `au915_0` | A | ✅ | — | — |
| 6 | `Device Profile - AU915_0 - 1.0.3 - A - OTAA - Class A+B` | `au915_0` | A | ✅ | ✅ | — |
| 7 | `Device Profile - AU915_0 - 1.0.3 - A - OTAA - Class A+B+C` | `au915_0` | A | ✅ | ✅ | ✅ |
| 8 | `Device Profile - AU915_0 - 1.0.3 - A - OTAA - Class A+C` | `au915_0` | A | ✅ | — | ✅ |
| 9 | `Device Profile - AU915_0 - 1.0.3 - B - ABP - Class A` | `au915_0` | B | — | — | — |
| 10 | `Device Profile - AU915_0 - 1.0.3 - B - ABP - Class A+B` | `au915_0` | B | — | ✅ | — |
| 11 | `Device Profile - AU915_0 - 1.0.3 - B - ABP - Class A+B+C` | `au915_0` | B | — | ✅ | ✅ |
| 12 | `Device Profile - AU915_0 - 1.0.3 - B - ABP - Class A+C` | `au915_0` | B | — | — | ✅ |
| 13 | `Device Profile - AU915_0 - 1.0.3 - B - OTAA - Class A` | `au915_0` | B | ✅ | — | — |
| 14 | `Device Profile - AU915_0 - 1.0.3 - B - OTAA - Class A+B` | `au915_0` | B | ✅ | ✅ | — |
| 15 | `Device Profile - AU915_0 - 1.0.3 - B - OTAA - Class A+B+C` | `au915_0` | B | ✅ | ✅ | ✅ |
| 16 | `Device Profile - AU915_0 - 1.0.3 - B - OTAA - Class A+C` | `au915_0` | B | ✅ | — | ✅ |
| 17 | `Device Profile - AU915_1 - 1.0.3 - A - ABP - Class A` | `au915_1` | A | — | — | — |
| 18 | `Device Profile - AU915_1 - 1.0.3 - A - ABP - Class A+B` | `au915_1` | A | — | ✅ | — |
| 19 | `Device Profile - AU915_1 - 1.0.3 - A - ABP - Class A+B+C` | `au915_1` | A | — | ✅ | ✅ |
| 20 | `Device Profile - AU915_1 - 1.0.3 - A - ABP - Class A+C` | `au915_1` | A | — | — | ✅ |
| 21 | `Device Profile - AU915_1 - 1.0.3 - A - OTAA - Class A` | `au915_1` | A | ✅ | — | — |
| 22 | `Device Profile - AU915_1 - 1.0.3 - A - OTAA - Class A+B` | `au915_1` | A | ✅ | ✅ | — |
| 23 | `Device Profile - AU915_1 - 1.0.3 - A - OTAA - Class A+B+C` | `au915_1` | A | ✅ | ✅ | ✅ |
| 24 | `Device Profile - AU915_1 - 1.0.3 - A - OTAA - Class A+C` | `au915_1` | A | ✅ | — | ✅ |
| 25 | `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A` | `au915_1` | B | — | — | — |
| 26 | `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A+B` | `au915_1` | B | — | ✅ | — |
| 27 | `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A+B+C` | `au915_1` | B | — | ✅ | ✅ |
| 28 | `Device Profile - AU915_1 - 1.0.3 - B - ABP - Class A+C` | `au915_1` | B | — | — | ✅ |
| 29 | `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A` | `au915_1` | B | ✅ | — | — |
| 30 | `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A+B` | `au915_1` | B | ✅ | ✅ | — |
| 31 | `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A+B+C` | `au915_1` | B | ✅ | ✅ | ✅ |
| 32 | `Device Profile - AU915_1 - 1.0.3 - B - OTAA - Class A+C` | `au915_1` | B | ✅ | — | ✅ |

Legenda: ✅ marcado · — desmarcado. Todos os 32 usam **Region** `AU915` e **MAC version** `LoRaWAN 1.0.3`.

## ChirpStack v3

Os mesmos 32 nomes servem no ChirpStack v3 (*Device-profiles → Create*). O v3 não tem o campo *Region configuration*: a sub-banda vem do **Gateway-profile** (*Enabled channels* `0–7, 64` para `AU915_0` e `8–15, 65` para `AU915_1`), e o nome do perfil só identifica a sub-banda a que o nó pertence.

| Aba / campo (v3) | Valor |
|---|---|
| General → Network-server | o seu network-server (ex.: `ChirpStack-v3_NS`) |
| General → LoRaWAN MAC version | `1.0.3` |
| General → LoRaWAN Regional Parameters revision | `A` ou `B` |
| General → ADR algorithm | `Default ADR algorithm (LoRa only)` |
| General → Max EIRP | `30` |
| General → Uplink interval (seconds) | `60` |
| Join (OTAA / ABP) → Device supports OTAA | marcado nos perfis OTAA |
| Join → RX1 delay / RX1 data-rate offset / RX2 data-rate / RX2 channel frequency (Hz) | `1` / `0` / `8` / `923300000` (só ABP) |
| Join → Factory-preset frequencies (Hz) | `916800000` (só ABP) |
| Class-B → confirmed downlink timeout / ping-slot periodicity / data-rate / frequency | `5` / `every 128 seconds (ping_slot_period = 4096)` / `8` / `0` |
| Class-C → confirmed downlink timeout | `0` |
| Codec → Payload codec | `Custom JavaScript codec functions` (funções `Decode` / `Encode`) |

No v3, *Device-status request frequency*, *Add gateway meta-data* e os limites de data-rate (`0` a `5`) ficam no **Service-profile**, não no perfil de dispositivo.
