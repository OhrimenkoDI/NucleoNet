#include <stdio.h>
#include <string.h>
#include "usb_host.h"
#include "usbh_def.h"

#include "Znp_parser.h"
#include "itm_ports.h"
#include "Znp_app.h"

// здесь глобальные переменные
#include "app_state.h"


extern volatile uint8_t cdc_tx_busy;
// эти три — из usb_host.c
extern uint8_t  dummy_rx[256];
extern volatile uint8_t  cdc_rx_ready;
extern volatile uint32_t cdc_rx_len;
extern uint8_t  cdc_tx_buf[256];
extern USBH_HandleTypeDef hUsbHostFS;


// Номера ITM-портов (<i> "окна")
#define ITM_PORT_INFO       0   // общая инфа
#define ITM_PORT_UNHANDLED  1   // нераспознанные / не обработанные
#define ITM_PORT_TEMP       2   // температура
#define ITM_PORT_SOCKET     3   // розетка / мощность / состояние

#define APP_EP        0x01      // наш endpoint контроллера
#define APP_PROFILEID 0x0104    // ZigBee HA
#define APP_DEVICEID  0x0000    // On/Off Switch (нам подходит)


// Если что-то из этого уже есть в main.c — выбираем ОДНО место хранения.
// Вариант 1 (рекомендую): держать всё znp-состояние здесь статически.
const uint8_t PLUG_IEEE_LSB[8] = {
    0xC0, 0xF4, 0x6B, 0xC2, 0x98, 0x38, 0xC1, 0xA4
};

uint16_t plugNwk       = 0xF3D8;
static uint8_t  appEpRegistered = 0;
static uint8_t  zclSeq        = 1;
static uint8_t  afTransId     = 1;



static int af_parse_incoming(AfIncomingMsg *m,
                             const uint8_t *p, uint8_t L)
{
    // Формат AF_INCOMING_MSG (0x44 0x81), согласно Z-Stack:
    // 0-1  GroupID
    // 2-3  ClusterID
    // 4-5  SrcAddr
    // 6    SrcEndpoint
    // 7    DstEndpoint
    // 8    WasBroadcast
    // 9    LinkQuality
    // 10   SecurityUse
    // 11-14 TimeStamp (4 байта, LE)
    // 15   TransSeqNumber
    // 16   DataLen
    // 17.. Data[DataLen]

    if (L < 17)
        return -1;

    uint8_t i = 0;

    m->group_id      = (uint16_t)p[i] | ((uint16_t)p[i+1] << 8); i += 2;
    m->cluster_id    = (uint16_t)p[i] | ((uint16_t)p[i+1] << 8); i += 2;
    m->src_addr      = (uint16_t)p[i] | ((uint16_t)p[i+1] << 8); i += 2;
    m->src_endpoint  = p[i++];
    m->dst_endpoint  = p[i++];
    m->was_broadcast = p[i++];
    m->lqi           = p[i++];
    m->security_use  = p[i++];

    m->timestamp =
        ((uint32_t)p[i])       |
        ((uint32_t)p[i+1] << 8)  |
        ((uint32_t)p[i+2] << 16) |
        ((uint32_t)p[i+3] << 24);
    i += 4;

    m->trans_seq = p[i++];

    if (i >= L)
        return -1;

    m->data_len = p[i++];

    if (i + m->data_len > L)
        return -1;

    m->data = &p[i];

    return 0;
}

static int32_t get_be32(const uint8_t *x)
{
    // Tuya значение: в Delphi собиралось как
    // b0=Data[12], b1=Data[11], b2=Data[10], b3=Data[9]
    // т.е. в кадре оно лежит big-endian.
    return ((int32_t)x[0] << 24) |
           ((int32_t)x[1] << 16) |
           ((int32_t)x[2] << 8)  |
           (int32_t)x[3];
}
static void handle_af_onoff(const AfIncomingMsg *m)
{
    // Cluster 0x0006: On/Off
    if (m->data_len < 7)
        return;

    uint8_t state = m->data[6] ? 1 : 0;

    char buf[64];
    snprintf(buf, sizeof(buf),
             "0x%04X Plug %s\r\n",
             m->src_addr, state ? "On" : "Off");

    http_on1 = state ? 1 : 0;

    ITM_Print_Port(3, buf);
}

static void handle_af_meter(const AfIncomingMsg *m)
{
    // Cluster 0x0B04: Electrical Measurement (Tuya розетка)
    if (m->data_len < 0x12)
        return;

    uint16_t U = (uint16_t)m->data[6]  | ((uint16_t)m->data[7]  << 8);
    uint16_t I = (uint16_t)m->data[11] | ((uint16_t)m->data[12] << 8); // мА
    uint16_t P = (uint16_t)m->data[16] | ((uint16_t)m->data[17] << 8); // W

    char buf[96];
    snprintf(buf, sizeof(buf),
             "0x%04X Plug %u V %u.%03u A %u W\r\n",
             m->src_addr,
             U,
             I / 1000, I % 1000,
             P);
    ITM_Print_Port(3, buf);
}

static void handle_af_tuya(const AfIncomingMsg *m)
{
    // Tuya специфичный кластер EF00
    // В Delphi:
    //   dp      = Data[5]
    //   type    = Data[6]
    //   len_hi  = Data[7]
    //   len_lo  = Data[8]
    //   value   = Data[9..]
    if (m->data_len < 13)
        return;

    const uint8_t *d = m->data;

    uint8_t  dp      = d[5];
    uint8_t  dp_type = d[6];
    uint16_t dp_len  = ((uint16_t)d[7] << 8) | d[8];

    if (9 + dp_len > m->data_len)
        return;

    const uint8_t *val = &d[9];

    // Нас интересуют DP длиной 4 байта (температура, влажность, батарея)
    if (dp_len == 4 && dp_type == 0x02) { // 0x02 - тип "value" у Tuya
        int32_t raw = get_be32(val);

        char buf[96];

        switch (dp) {
        case 0x01: // температура * 10
        {
            float t = raw / 10.0f;
            int t_int = (int)t;
            int t_frac = (int)((t - t_int) * 10);

            snprintf(buf, sizeof(buf),
                     "0x%04X Temp %d.%d C\r\n",
                     m->src_addr, t_int, t_frac);
            ITM_Print_Port(2, buf);
            break;
        }
        case 0x02: // влажность %
        {
            snprintf(buf, sizeof(buf),
                     "0x%04X Hum %ld %%\r\n",
                     m->src_addr, (long)raw);
            ITM_Print_Port(2, buf);
            break;
        }
        case 0x04: // батарея %
        {
            snprintf(buf, sizeof(buf),
                     "0x%04X Battery %ld %%\r\n",
                     m->src_addr, (long)raw);
            ITM_Print_Port(2, buf);
            break;
        }
        default:
            // Неизвестный DP — в "не обработано"
            snprintf(buf, sizeof(buf),
                     "0x%04X Tuya DP 0x%02X len=%u\r\n",
                     m->src_addr, dp, dp_len);
            ITM_Print_Port(1, buf);
            break;
        }
    }
}



static uint8_t znp_fcs(const uint8_t *p, uint8_t n)
{
  uint8_t x = 0;
  for (uint8_t i = 0; i < n; i++) x ^= p[i];
  return x;
}

static int CDC_SendRaw(const uint8_t *buf, uint16_t len, const char *tag)
{
    uint32_t t0;
    USBH_StatusTypeDef st;

    // Ждём окончание предыдущей передачи
    t0 = HAL_GetTick();
    while (cdc_tx_busy) {
        MX_USB_HOST_Process();
        if (HAL_GetTick() - t0 > 10000) {
            printf("%s: wait busy TIMEOUT1\r\n", tag);
            return -1;
        }
    }

    // Лог что именно шлём
    printf("%s:", tag);
    for (uint16_t i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\r\n");

    // Копируем в общий буфер (чтобы передача жила после выхода из функции)
    if (len > sizeof(cdc_tx_buf)) {
        printf("%s: len too big\r\n", tag);
        return -2;
    }
    memcpy(cdc_tx_buf, buf, len);

    cdc_tx_busy = 1;
    st = USBH_CDC_Transmit(&hUsbHostFS, cdc_tx_buf, len);
    if (st != USBH_OK) {
        cdc_tx_busy = 0;
        printf("%s: Transmit ERROR %d\r\n", tag, st);
        return -3;
    }

    // Ждём завершения передачи с прокруткой стека
    t0 = HAL_GetTick();
    while (cdc_tx_busy) {
        MX_USB_HOST_Process();
        if (HAL_GetTick() - t0 > 10000) {
            cdc_tx_busy = 0;
            printf("%s: TX TIMEOUT2\r\n", tag);
            return -4;
        }
    }

    return 0;
}



int ZNP_Send(uint8_t cmd0, uint8_t cmd1, const uint8_t *data, uint8_t len)
{
    uint8_t buf[4 + 255 + 1];
    uint16_t i = 0;

    buf[i++] = 0xFE;
    buf[i++] = len;
    buf[i++] = cmd0;
    buf[i++] = cmd1;

    if (len && data) {
        memcpy(&buf[i], data, len);
        i += len;
    }

    buf[i++] = znp_fcs(&buf[1], (uint8_t)(3 + len));

    return CDC_SendRaw(buf, i, "TX ZNP");
}


void send_onoff(uint8_t on)
{
    if (!plugNwk) {
        printf("send_onoff: plugNwk=0, skip\r\n");
        return;
    }
    if (!appEpRegistered) {
        printf("send_onoff: appEp not registered, skip\r\n");
        return;
    }

    uint8_t buf[32];
    uint8_t i = 0;
    uint16_t dst = plugNwk;

    buf[i++] = (uint8_t)(dst & 0xFF);
    buf[i++] = (uint8_t)(dst >> 8);
    buf[i++] = 0x01;        // DstEndpoint: пока считаем 0x01
    buf[i++] = APP_EP;      // SrcEndpoint: наш EP из AF_REGISTER

    buf[i++] = 0x06;        // ClusterID = 0x0006 L
    buf[i++] = 0x00;        // H

    buf[i++] = afTransId++;

    buf[i++] = 0x00;        // Options
    buf[i++] = 0x0F;        // Radius

    // ZCL
    uint8_t fc  = 0x01;               // cluster-specific, client->server
    uint8_t cmd = on ? 0x01 : 0x00;   // ON / OFF

    buf[i++] = 3;         // DataLen
    buf[i++] = fc;
    buf[i++] = zclSeq++;
    buf[i++] = cmd;

    ZNP_Send(0x24, 0x01, buf, i);
    printf("send_onoff(%u) to 0x%04X\n", on, dst);
}

void ZNP_Send_ZDO_NWK_ADDR_REQ_Plug(void)
{
    uint8_t p[10];
    uint8_t i = 0;

    // IEEE устройства (LSB-first)
    memcpy(&p[i], PLUG_IEEE_LSB, 8);
    i += 8;

    p[i++] = 0x00; // ReqType = 0x00 (single device response)
    p[i++] = 0x00; // StartIndex = 0

    // CMD0=0x25 (ZDO | SREQ), CMD1=0x00 (NWK_ADDR_REQ)
    ZNP_Send(0x25, 0x00, p, i);
    printf("ZDO_NWK_ADDR_REQ sent for plug IEEE\r\n");
}




void ZNP_Send_SYS_RESET_REQ(uint8_t type /*0x00=Hard,0x01=Soft*/)
{
  (void)ZNP_Send(0x41, 0x00, &type, 1);
}

int ZNP_Send_SYS_PING(void)
{
	  static int i;
  i=ZNP_Send(0x21, 0x01, NULL, 0);
  return  i;
}


int Send_EF_raw(void)
{
    uint8_t b = 0xEF;
    return CDC_SendRaw(&b, 1, "TX EF");
}

void ZB_START_REQUEST(void)
{
	   uint8_t p[1];
	   uint8_t i = 0;

	   p[i++] = 0x00;


	   int r = ZNP_Send(0x24, 0x00, p, i);
	   printf("AF_REGISTER send rc=%d (EP=0x%02X)\r\n", r, APP_EP);

}


void ZNP_AF_REGISTER(void)
{
    uint8_t p[16];
    uint8_t i = 0;

    p[i++] = APP_EP;

    p[i++] = (uint8_t)(APP_PROFILEID & 0xFF);
    p[i++] = (uint8_t)(APP_PROFILEID >> 8);

    p[i++] = (uint8_t)(APP_DEVICEID & 0xFF);
    p[i++] = (uint8_t)(APP_DEVICEID >> 8);

    p[i++] = 0x01; // DevVer
    p[i++] = 0x00; // LatencyReq

    p[i++] = 0x00; // NumInClusters = 0

    p[i++] = 0x01; // NumOutClusters = 1
    p[i++] = 0x06; // OutCluster 0x0006 L
    p[i++] = 0x00; // H

    int r = ZNP_Send(0x24, 0x00, p, i);
    printf("AF_REGISTER send rc=%d (EP=0x%02X)\r\n", r, APP_EP);
}


// Хелпер: вывести байты кадра в hex в нужный порт
static void ITM_PrintHex(uint8_t port, const uint8_t *data, uint8_t len)
{
    char buf[4];
    for (uint8_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        ITM_Print_Port(port, buf);
    }
    ITM_Print_Port(port, "\r\n");
}

// Хелпер для краткого префикса
static void ITM_LogFrame(uint8_t port,
                         const char *title,
                         uint8_t cmd0, uint8_t cmd1,
                         const uint8_t *payload, uint8_t len)
{
    char head[64];
    snprintf(head, sizeof(head),
             "%s CMD0=0x%02X CMD1=0x%02X LEN00=%u: ",
             title, cmd0, cmd1, len);
    ITM_Print_Port(port, head);
    ITM_PrintHex(port, payload, len);
}

/*
 * Главный обработчик проверенных кадров ZNP.
 * Вызывается парсером из Znp_parser.c
 */
void ZNP_HandleFrame(uint8_t cmd0, uint8_t cmd1,
                     const uint8_t *payload, uint8_t len)

{
	uint8_t debug_buffer[256];
	 memcpy(debug_buffer, payload, len);

    // Общее сообщение о входящем кадре
    ITM_Print_Port(ITM_PORT_INFO, "ZNP RX frame\r\n");
    ITM_LogFrame(ITM_PORT_INFO, "", cmd0, cmd1, payload, len);


    // 0x45 0x80 — ZDO_NWK_ADDR_RSP: узнаём NWK по IEEE розетки
    if (cmd0 == 0x45 && cmd1 == 0x80 && len >= 11) {
        uint8_t status = payload[0];
        const uint8_t *ieee = &payload[1];
        uint16_t nwk = (uint16_t)payload[9] | ((uint16_t)payload[10] << 8);

        ITM_Print_Port(ITM_PORT_INFO,
                       "ZDO_NWK_ADDR_RSP: status=0x");
        // можно сделать маленький хелпер под hex, но не обязательно
        char buf[32];
        snprintf(buf, sizeof(buf), "%02X nwk=0x%04X\r\n", status, nwk);
        ITM_Print_Port(ITM_PORT_INFO, buf);

        if (status == 0x00 && memcmp(ieee, PLUG_IEEE_LSB, 8) == 0) {
            plugNwk = nwk;
            snprintf(buf, sizeof(buf),
                     "Plug NWK found: 0x%04X\r\n", plugNwk);
            ITM_Print_Port(ITM_PORT_SOCKET, buf);
        }
        return;
    }

    // 0x64 0x00 — AF_REGISTER_SRSP (у тебя это уже было, но для полноты)
    if (cmd0 == 0x64 && cmd1 == 0x00 && len >= 1) {
        uint8_t st = payload[0];
        char buf[32];
        snprintf(buf, sizeof(buf),
                 "AF_REGISTER_SRSP: status=0x%02X\r\n", st);
        ITM_Print_Port(ITM_PORT_INFO, buf);

        if (st == 0x00) {
            appEpRegistered = 1;
            ITM_Print_Port(ITM_PORT_INFO,
                           "<--- AF_REGISTER ok\r\n");
        } else {
            ITM_Print_Port(ITM_PORT_INFO,
                           "<--- AF_REGISTER error\r\n");
        }
        return;
    }

    // 0x64 0x01 — AF_DATA_REQUEST_SRSP: просто логнем в INFO
    if (cmd0 == 0x64 && cmd1 == 0x01 && len >= 1) {
        uint8_t st = payload[0];
        char buf[48];
        snprintf(buf, sizeof(buf),
                 "AF_DATA_REQUEST_SRSP: status=0x%02X\r\n", st);
        ITM_Print_Port(ITM_PORT_INFO, buf);
        return;
    }






    // ----- Примеры переноса логики из твоего Delphi-кода -----

    // 1) "координатор готов"
    // FE 02 61 01 ...
    if (cmd0 == 0x61 && cmd1 == 0x01 && len == 2) {
        ITM_Print_Port(ITM_PORT_INFO,
                       "<--- Coordinator READY\r\n");

        return;
    }

    // 2) DEVICE_ANNOUNCE / сетевое объявление координатора
    // FE 03 45 C4 ...
    if (cmd0 == 0x45 && cmd1 == 0xC4 && len == 3) {
        ITM_Print_Port(ITM_PORT_INFO,
                       "<--- Net public from Coordinator\r\n");
        return;
    }

    // 3) Ответ на запрос состояния сети (пример из комментария)
    // FE 06 41 80 ...
    if (cmd0 == 0x41 && cmd1 == 0x80 && len == 6) {
        ITM_Print_Port(ITM_PORT_INFO,
                       "<--- ответ на запрос состояния сети\r\n");
        return;
    }

    // 4) AF_REGISTER_SRSP успешный
    // FE 01 64 00 [status]
    if (cmd0 == 0x64 && cmd1 == 0x00 && len >= 1) {
        uint8_t st = payload[0];
        if (st == 0x00) {
            appEpRegistered = 1;
            ITM_Print_Port(ITM_PORT_INFO,
                           "<--- AF_REGISTER ok\r\n");
        } else {
            ITM_Print_Port(ITM_PORT_INFO,
                           "<--- AF_REGISTER error\r\n");
        }
        return;
    }

    // 5) MAC + IEEE (0x45 C1 / 0x45 CA) — просто логируем
    if (cmd0 == 0x45 && cmd1 == 0xC1 && len >= 0x0D) {
        ITM_Print_Port(ITM_PORT_INFO, "<--- MAC-IEEE (C1)\r\n");
        // при желании распарсить MAC/IEEE и вывести
        return;
    }

    if (cmd0 == 0x45 && cmd1 == 0xCA && len >= 0x0C) {
        ITM_Print_Port(ITM_PORT_INFO, "<--- MAC-IEEE (CA)\r\n");
        return;
    }

    // 6) AF_INCOMING_MSG (0x44 0x81) — тут живут Tuya, розетки и т.п.
    // AF_INCOMING_MSG
    if (cmd0 == 0x44 && cmd1 == 0x81)
    {
        AfIncomingMsg msg;
        if (af_parse_incoming(&msg, payload, len) != 0) {
            ITM_Print_Port(1, "AF_INCOMING_MSG parse error\r\n");
            return;
        }

        ITM_Print_Port(0, "ZNP RX AF_INCOMING_MSG\r\n");

        switch (msg.cluster_id) {
        case 0x0006: // On/Off
            handle_af_onoff(&msg);
            break;

        case 0x0B04: // Electrical Measurement
            handle_af_meter(&msg);
            break;

        case 0xEF00: // Tuya specific
            handle_af_tuya(&msg);
            break;

        default:
        {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "AF unhandled cluster=0x%04X src=0x%04X\r\n",
                     msg.cluster_id, msg.src_addr);
            ITM_Print_Port(1, buf);
            break;
        }
        }

        return;
    }

    // ----- Всё, что не распознали выше -----
    ITM_Print_Port(ITM_PORT_UNHANDLED,
                   "<--- ZNP frame not handled\r\n");
}
