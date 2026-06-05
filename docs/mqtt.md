# Documentação MQTT — Estação Argos de Encosta

Todos os tópicos carregam o `id` da estação no caminho, permitindo agregar uma
**frota** de estações no mesmo broker. Troque a constante `ESTACAO_ID` no firmware
para implantar outra estação.

- **Broker:** `c175cd285334458eab19178117bdbc91.s1.eu.hivemq.cloud`
- **Porta:** `8883` (TLS)
- **Autenticação:** usuário + senha (HiveMQ Cloud)
- **Client ID:** `argos-<ESTACAO_ID>`
- **Padrão de tópico:** `argos/estacao/{id}/<sufixo>`

> O SPEC original sugere o broker público `broker.hivemq.com:1883`. Esta
> implementação usa o HiveMQ Cloud com TLS por já estar provisionado e ser mais
> confiável/privado. Para voltar ao broker público, ajuste `MQTT_BROKER`,
> `MQTT_PORT = 1883`, troque `WiFiClientSecure` por `WiFiClient` e remova
> `setInsecure()` / credenciais.

---

## 1. `argos/estacao/{id}/leituras`

Dados brutos dos sensores. **Publicado a cada 5 s.** QoS 0, não retido.

```json
{
  "id": "sp-mboi-mirim-01",
  "umidade": 87.4,
  "inclinacao": 12.7,
  "ts": "2026-06-04T14:05:00Z"
}
```

| Campo | Tipo | Unidade | Descrição |
|---|---|---|---|
| `id` | string | — | Identificador da estação |
| `umidade` | número | % | Umidade relativa do ar (DHT22) |
| `inclinacao` | número | graus | Ângulo de inclinação do terreno (MPU6050) |
| `ts` | string | ISO-8601 UTC | Timestamp (vazio se NTP não sincronizou) |

> A temperatura é lida pelo DHT22 e exibida no OLED, mas **não é publicada** —
> não entra no cálculo de risco (que usa umidade + inclinação).

---

## 2. `argos/estacao/{id}/risco`

Nível de risco calculado na borda. **Publicado somente quando o nível muda.**
QoS 0, **retido** (o dashboard recebe o último nível ao assinar).

```json
{
  "id": "sp-mboi-mirim-01",
  "nivel": "critico",
  "score": 0.82,
  "ts": "2026-06-04T14:05:00Z"
}
```

| Campo | Tipo | Descrição |
|---|---|---|
| `id` | string | Identificador da estação |
| `nivel` | string | `baixo` \| `medio` \| `alto` \| `critico` |
| `score` | número | Score de risco 0.00–1.00 |
| `ts` | string | Timestamp ISO-8601 UTC |

> A cor de cada nível é fixa (tabela abaixo) e fica a cargo do dashboard — não
> trafega no payload.

### Escala de risco 

```text
humNorm  = clamp((umidade - 40) / 60, 0, 1)
tiltNorm = clamp(inclinacao / 30, 0, 1)
score    = humNorm * 0.4 + tiltNorm * 0.6
if (variacao_inclinacao > 5° entre leituras) score = 1.0  // gatilho mecânico
```

| Nível | Score | Cor | LED |
|---|---|---|---|
| 🟢 baixo | 0.00–0.24 | `#22C55E` | verde |
| 🟡 medio | 0.25–0.49 | `#EAB308` | amarelo |
| 🟠 alto | 0.50–0.74 | `#F97316` | laranja |
| 🔴 critico | 0.75–1.00 | `#EF4444` | vermelho (piscando) |

---

## 3. `argos/estacao/{id}/status`

Heartbeat da estação. **Publicado a cada 30 s.** QoS 0, não retido.

```json
{
  "id": "sp-mboi-mirim-01",
  "online": true,
  "wifi_rssi": -67,
  "uptime_s": 3840,
  "ts": "2026-06-04T14:05:00Z"
}
```

| Campo | Tipo | Unidade | Descrição |
|---|---|---|---|
| `id` | string | — | Identificador da estação |
| `online` | bool | — | Sempre `true` enquanto publica |
| `wifi_rssi` | número | dBm | Força do sinal Wi-Fi |
| `uptime_s` | número | s | Segundos desde o boot |
| `ts` | string | ISO-8601 UTC | Timestamp |

Permite ao dashboard saber se cada estação da frota está viva e com bom sinal.

---

## Teste rápido (mosquitto)

Assine todos os tópicos da frota:

```bash
mosquitto_sub -h c175cd285334458eab19178117bdbc91.s1.eu.hivemq.cloud \
  -p 8883 --capath /etc/ssl/certs \
  -u FelipeRM564723 -P '<senha>' \
  -t 'argos/estacao/#' -v
```
