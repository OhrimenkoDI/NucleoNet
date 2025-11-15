#include <string.h>
#include "main.h"         // для HAL_GetTick
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"

// Если у тебя уже есть такая функция — не дублируй.
extern void ITM_Print_Port(uint32_t port, const char *s);


static struct tcp_pcb *mqtt_pcb = NULL;
static ip_addr_t mqtt_server_ip;
static uint8_t mqtt_connected = 0;
static uint32_t mqtt_last_send = 0;
static uint32_t mqtt_last_connect_try = 0;   // когда последний раз пытались коннектиться

#define MQTT_PORT 1883


static void mqtt_connect_to_broker(void);
static void mqtt_send_connect(struct tcp_pcb *tpcb);
static void mqtt_send_telemetry(struct tcp_pcb *tpcb);

static err_t mqtt_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t mqtt_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  mqtt_tcp_err_cb(void *arg, err_t err);
static err_t mqtt_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len);


// ===== Публичный интерфейс модуля MQTT =====

// Задать IP адрес брокера
void MQTT_InitBrokerIP(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    IP_ADDR4(&mqtt_server_ip, a, b, c, d);
}

void MQTT_Start(void)
{
    mqtt_last_connect_try = 0;   // чтобы сразу попробовать
}

// вызывать из main()
void MQTT_Process(void)
{
    uint32_t now = HAL_GetTick();

    // 1) Если не подключены и нет активного pcb — пробуем коннектиться раз в 3 секунды
    if (!mqtt_connected && mqtt_pcb == NULL) {
        if (now - mqtt_last_connect_try > 3000) {
            ITM_Print_Port(2, "MQTT: try connect\r\n");
            mqtt_last_connect_try = now;
            mqtt_connect_to_broker();
        }
    }

    // 2) Если подключены — шлём телеметрию раз в секунду
    if (mqtt_connected && (now - mqtt_last_send) >= 1000) {
        mqtt_send_telemetry(mqtt_pcb);
        mqtt_last_send = now;
    }
}

uint8_t MQTT_IsConnected(void)
{
    return mqtt_connected;
}


static void mqtt_send_subscribe(struct tcp_pcb *tpcb)
{
    const char *topic = "sensors/room1/set";
    uint16_t tlen = (uint16_t)strlen(topic);

    uint8_t buf[128];
    int i = 0;

    buf[i++] = 0x82;  // SUBSCRIBE, флаги по стандарту (QoS1)

    // remaining length: 2 (msgID) + 2 (topic len) + tlen + 1 (QoS)
    uint8_t rem_len = 2 + 2 + tlen + 1;
    buf[i++] = rem_len;

    // Message ID
    buf[i++] = 0x00;
    buf[i++] = 0x01;

    // Topic
    buf[i++] = (tlen >> 8) & 0xFF;
    buf[i++] = tlen & 0xFF;
    memcpy(&buf[i], topic, tlen);
    i += tlen;

    // Requested QoS
    buf[i++] = 0x00;   // QoS0 для этого топика

    tcp_write(tpcb, buf, i, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);

    ITM_Print_Port(2, "MQTT: SUBSCRIBE sent\r\n");
}


// Простейший MQTT-клиент (MQTT 3.1.1, QoS0, без авторизации)

static void mqtt_connect_to_broker(void)
{
    err_t err;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        ITM_Print_Port(2, "MQTT: tcp_new failed\r\n");
        return;
    }

    mqtt_pcb = pcb;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, mqtt_tcp_recv_cb);
    tcp_err(pcb, mqtt_tcp_err_cb);
    tcp_sent(pcb, mqtt_tcp_sent_cb);

    char buf[64];
    snprintf(buf, sizeof(buf), "MQTT: tcp_connect to %s:%d\r\n",
             ipaddr_ntoa(&mqtt_server_ip), MQTT_PORT);
    ITM_Print_Port(2, buf);

    err = tcp_connect(pcb, &mqtt_server_ip, MQTT_PORT, mqtt_tcp_connected_cb);
    if (err != ERR_OK) {
        ITM_Print_Port(2, "MQTT: tcp_connect ERR\r\n");
        tcp_close(pcb);
        mqtt_pcb = NULL;
    }
}

static err_t mqtt_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if (err != ERR_OK) {
        ITM_Print_Port(2, "MQTT: connected_cb ERR\r\n");
        tcp_close(tpcb);
        mqtt_pcb = NULL;
        return err;
    }

    ITM_Print_Port(2, "MQTT: TCP connected, send CONNECT\r\n");
    mqtt_send_connect(tpcb);
    return ERR_OK;
}

static void mqtt_send_connect(struct tcp_pcb *tpcb)
{
    // MQTT CONNECT, протокол 3.1.1, clean session
    uint8_t buf[64];
    int i = 0;

    const char *client_id = "stm32f767";
    uint16_t client_id_len = (uint16_t)strlen(client_id);

    // Фиксированный заголовок
    buf[i++] = 0x10; // CONNECT
    // remaining length = 10 (VH) + (2 + client_id_len)
    uint8_t rem_len = 10 + 2 + client_id_len;
    buf[i++] = rem_len;

    // Variable header
    buf[i++] = 0x00;
    buf[i++] = 0x04;
    buf[i++] = 'M';
    buf[i++] = 'Q';
    buf[i++] = 'T';
    buf[i++] = 'T';
    buf[i++] = 0x04; // protocol level 4 = MQTT 3.1.1
    buf[i++] = 0x02; // connect flags: clean session
    buf[i++] = 0x00;
    buf[i++] = 60;   // keep alive = 60 сек

    // Payload: Client ID
    buf[i++] = (client_id_len >> 8) & 0xFF;
    buf[i++] = (client_id_len) & 0xFF;
    memcpy(&buf[i], client_id, client_id_len);
    i += client_id_len;

    tcp_write(tpcb, buf, i, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

static err_t mqtt_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (p == NULL) {
        ITM_Print_Port(2, "MQTT: recv NULL (connection closed)\r\n");
        tcp_close(tpcb);
        mqtt_pcb = NULL;
        mqtt_connected = 0;
        return ERR_OK;
    }

    uint8_t *data = (uint8_t *)p->payload;
    u16_t len = p->len;

    char dbg[128];
    snprintf(dbg, sizeof(dbg),
             "MQTT: recv len=%u, first bytes: %02X %02X %02X %02X\r\n",
             (unsigned)len,
             (len > 0 ? data[0] : 0),
             (len > 1 ? data[1] : 0),
             (len > 2 ? data[2] : 0),
             (len > 3 ? data[3] : 0));
    ITM_Print_Port(2, dbg);

    if (len >= 4) {
        uint8_t type    = data[0] & 0xF0;
        uint8_t rem_len = data[1];

        // CONNACK
        if (type == 0x20) {
            if (rem_len >= 2 && len >= 4 && data[3] == 0x00) {
                ITM_Print_Port(2, "MQTT: CONNACK OK\r\n");
                mqtt_connected = 1;
                mqtt_send_subscribe(tpcb);
            } else {
                snprintf(dbg, sizeof(dbg), "MQTT: CONNACK error rc=%02X\r\n", data[3]);
                ITM_Print_Port(2, dbg);
                mqtt_connected = 0;
            }
        }
        // PUBLISH QoS0 с маленьким remaining length (<128)
        else if (type == 0x30) {
            // минимум: 2 байта fixed header + 2 байта topic len
            if (rem_len + 2 <= len) {
                uint16_t topic_len = (data[2] << 8) | data[3];
                if (4 + topic_len <= len && rem_len >= 2 + topic_len) {
                    const char *topic   = (const char *)&data[4];
                    const char *payload = (const char *)&data[4 + topic_len];
                    uint16_t payload_len = rem_len - 2 - topic_len;

                    char dbg2[256];
                    snprintf(dbg2, sizeof(dbg2),
                             "MQTT: PUBLISH topic='%.*s' payload='%.*s'\r\n",
                             (int)topic_len,   topic,
                             (int)payload_len, payload);
                    ITM_Print_Port(2, dbg2);

                    // тут дальше можешь разбирать payload как JSON и т.д.
                }
            }
        }
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void mqtt_tcp_err_cb(void *arg, err_t err)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "MQTT: tcp_err_cb err=%d\r\n", err);
    ITM_Print_Port(2, buf);

    mqtt_pcb = NULL;
    mqtt_connected = 0;
}

static err_t mqtt_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    // Можно что-то логировать, но нам не обязательно
    return ERR_OK;
}

static void mqtt_send_telemetry(struct tcp_pcb *tpcb)
{
    if (tpcb == NULL || !mqtt_connected) return;

    // TODO: сюда подставь реальные данные с ZigBee
    int battery = 100;
    int humidity = 50;
    float temperature = 22.1f;

    char json[128];
    int json_len = snprintf(json, sizeof(json),
                            "{\"battery\":%d,\"humidity\":%d,\"temperature\":%.1f}",
                            battery, humidity, temperature);
    if (json_len <= 0 || json_len >= (int)sizeof(json))
        return;

    const char *topic = "sensors/room1";
    uint16_t topic_len = (uint16_t)strlen(topic);

    uint8_t buf[256];
    int i = 0;

    // PUBLISH fixed header, QoS0
    buf[i++] = 0x30; // PUBLISH, DUP=0, QoS=0, RETAIN=0

    uint16_t rem_len = 2 + topic_len + json_len;
    if (rem_len >= 128) {
        // Для простоты не поддерживаем большие пакеты
        return;
    }
    buf[i++] = (uint8_t)rem_len;

    // Topic
    buf[i++] = (topic_len >> 8) & 0xFF;
    buf[i++] = topic_len & 0xFF;
    memcpy(&buf[i], topic, topic_len);
    i += topic_len;

    // Payload
    memcpy(&buf[i], json, json_len);
    i += json_len;

    tcp_write(tpcb, buf, i, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

