#include "http_server.h"
#include "http_page.h"

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include <string.h>
#include <stdio.h>

#include "itm_ports.h"   // твоя функция ITM_Print_Port()

// здесь глобальные переменные
#include "app_state.h"

// === Глобальные переменные состояния (объявлены в http_server.h как extern).
// Настоящие определения должны быть где-то в одном .c (например, main.c):
// float  http_t1 = 21.5f, http_t2 = 22.0f, http_t3 = 23.3f;
// uint8_t http_on1 = 0, http_on2 = 0, http_on3 = 0;

static struct tcp_pcb *http_listen_pcb = NULL;

// Прототипы внутренних функций
static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t http_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  http_err_cb(void *arg, err_t err);
static err_t http_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len);

static void http_close(struct tcp_pcb *tpcb);
static void http_handle_request(struct tcp_pcb *tpcb, const char *data, u16_t len);
static void http_send_response(struct tcp_pcb *tpcb,
                               const char *status_line,
                               const char *content_type,
                               const char *body);
static void http_send_state_json(struct tcp_pcb *tpcb);
static void http_handle_set_query(struct tcp_pcb *tpcb, const char *url);

// === слабая реализация хука ===
__attribute__((weak)) void HTTP_OnOutputChanged(uint8_t index, uint8_t new_state)
{
    // По умолчанию только печатаем в ITM.
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP: on%u changed to %u\r\n",
             (unsigned)index, (unsigned)new_state);
    ITM_Print_Port(1, buf);
}

// === Публичная функция инициализации ===
void HTTP_Server_Init(void)
{
    err_t err;

    http_listen_pcb = tcp_new();
    if (http_listen_pcb == NULL) {
        ITM_Print_Port(1, "HTTP: tcp_new failed\r\n");
        return;
    }

    err = tcp_bind(http_listen_pcb, IP_ADDR_ANY, 80);  // порт 80
    if (err != ERR_OK) {
        ITM_Print_Port(1, "HTTP: tcp_bind failed\r\n");
        tcp_close(http_listen_pcb);
        http_listen_pcb = NULL;
        return;
    }

    http_listen_pcb = tcp_listen(http_listen_pcb);
    tcp_accept(http_listen_pcb, http_accept_cb);

    ITM_Print_Port(1, "HTTP: server listening on port 80\r\n");
}

// === Callbacks lwIP ===
static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, http_recv_cb);
    tcp_err(newpcb, http_err_cb);
    tcp_sent(newpcb, http_sent_cb);

    ITM_Print_Port(1, "HTTP: new connection\r\n");
    return ERR_OK;
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (p == NULL) {
        ITM_Print_Port(1, "HTTP: connection closed by client\r\n");
        http_close(tpcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    char buf[512];
    u16_t total = p->tot_len;
    if (total >= sizeof(buf)) {
        total = sizeof(buf) - 1;
    }

    pbuf_copy_partial(p, buf, total, 0);
    buf[total] = '\0';

    ITM_Print_Port(1, "HTTP: request received\r\n");
    ITM_Print_Port(1, buf);

    http_handle_request(tpcb, buf, total);

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void http_err_cb(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP: error cb err=%d\r\n", err);
    ITM_Print_Port(1, buf);
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);

    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP: sent %u bytes, closing\r\n", (unsigned)len);
    ITM_Print_Port(1, buf);

    http_close(tpcb);   // <-- закрываем здесь, когда всё ACK'нулось
    return ERR_OK;
}



// === Закрытие соединения ===
static void http_close(struct tcp_pcb *tpcb)
{
    if (tpcb) {
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_close(tpcb);
    }
}

// === Разбор HTTP запроса (очень простой) ===
static void http_handle_request(struct tcp_pcb *tpcb, const char *data, u16_t len)
{
    (void)len;

    // Ожидаем строку вида: "GET /... HTTP/1.1\r\n"
    if (strncmp(data, "GET ", 4) != 0) {
        http_send_response(tpcb,
                           "HTTP/1.1 405 Method Not Allowed\r\n",
                           "text/plain",
                           "Only GET supported\r\n");
        return;
    }

    const char *url_start = data + 4;
    const char *space = strchr(url_start, ' ');
    if (!space) {
        http_send_response(tpcb,
                           "HTTP/1.1 400 Bad Request\r\n",
                           "text/plain",
                           "Bad Request\r\n");
        return;
    }

    // Выделяем URL во временный буфер
    char url[128];
    size_t url_len = (size_t)(space - url_start);
    if (url_len >= sizeof(url))
        url_len = sizeof(url) - 1;
    memcpy(url, url_start, url_len);
    url[url_len] = '\0';

    // Роутинг
    if (strcmp(url, "/") == 0) {
        // Отдаём HTML-страницу
        http_send_response(tpcb,
                           "HTTP/1.1 200 OK\r\n",
                           "text/html; charset=utf-8",
                           http_index_html);
    }
    else if (strcmp(url, "/state") == 0) {
        http_send_state_json(tpcb);
    }
    else if (strncmp(url, "/set", 4) == 0) {
        http_handle_set_query(tpcb, url);
    }
    else {
        http_send_response(tpcb,
                           "HTTP/1.1 404 Not Found\r\n",
                           "text/plain",
                           "Not found\r\n");
    }
}

// === Отправка ответа ===
static void http_send_response(struct tcp_pcb *tpcb,
                               const char *status_line,
                               const char *content_type,
                               const char *body)
{
    if (tpcb == NULL) {
        return;
    }

    char header[256];
    int body_len = (int)strlen(body);

    int hdr_len = snprintf(header, sizeof(header),
                           "%s"
                           "Content-Type: %s\r\n"
                           "Connection: close\r\n"
                           "Content-Length: %d\r\n"
                           "\r\n",
                           status_line,
                           content_type,
                           body_len);

    if (hdr_len <= 0 || hdr_len >= (int)sizeof(header)) {
        ITM_Print_Port(1, "HTTP: header snprintf error\r\n");
        http_close(tpcb);
        return;
    }

    err_t err;

    // 1. Заголовок
    err = tcp_write(tpcb, header, hdr_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "HTTP: tcp_write(header) err=%d\r\n", err);
        ITM_Print_Port(1, buf);
        http_close(tpcb);
        return;
    }

    // 2. Тело
    if (body_len > 0) {
        err = tcp_write(tpcb, body, body_len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            char buf[64];
            snprintf(buf, sizeof(buf), "HTTP: tcp_write(body) err=%d\r\n", err);
            ITM_Print_Port(1, buf);
            http_close(tpcb);
            return;
        }
    }

    tcp_output(tpcb);

}

// === /state -> JSON ===
static void http_send_state_json(struct tcp_pcb *tpcb)
{
    char json[256];
    // Формат JSON: {"t1":23.4,"t2":25.1,"t3":22.0,"on1":1,"on2":0,"on3":1}
    int len = snprintf(json, sizeof(json),
                       "{\"t1\":%.2f,\"t2\":%.2f,\"t3\":%.2f,"
                       "\"on1\":%u,\"on2\":%u,\"on3\":%u}\r\n",
                       (double)http_t1,
                       (double)http_t2,
                       (double)http_t3,
                       (unsigned)http_on1,
                       (unsigned)http_on2,
                       (unsigned)http_on3);
    if (len < 0 || len >= (int)sizeof(json))
        return;

    http_send_response(tpcb,
                       "HTTP/1.1 200 OK\r\n",
                       "application/json",
                       json);
}

// === /set?on1=0&on2=1&on3=0 ===
static void http_handle_set_query(struct tcp_pcb *tpcb, const char *url)
{
    // Ищем '?'
    const char *q = strchr(url, '?');
    uint8_t new_on1 = http_on1;
    uint8_t new_on2 = http_on2;
    uint8_t new_on3 = http_on3;

    if (q) {
        q++; // после '?'
        // Простейший парсер параметров вида key=value&key2=value2...
        // Копируем в локальный буфер, чтобы удобно резать
        char params[128];
        size_t len = strlen(q);
        if (len >= sizeof(params))
            len = sizeof(params) - 1;
        memcpy(params, q, len);
        params[len] = '\0';

        char *tok = strtok(params, "&");
        while (tok != NULL) {
            if (strncmp(tok, "on1=", 4) == 0) {
                new_on1 = (uint8_t)(tok[4] == '0' ? 0 : 1);
            } else if (strncmp(tok, "on2=", 4) == 0) {
                new_on2 = (uint8_t)(tok[4] == '0' ? 0 : 1);
            } else if (strncmp(tok, "on3=", 4) == 0) {
                new_on3 = (uint8_t)(tok[4] == '0' ? 0 : 1);
            }
            tok = strtok(NULL, "&");
        }
    }

    // Проверяем изменения и генерируем события
    if (new_on1 != http_on1) {
        http_on1 = new_on1;
        HTTP_OnOutputChanged(1, http_on1);
    }
    if (new_on2 != http_on2) {
        http_on2 = new_on2;
        HTTP_OnOutputChanged(2, http_on2);
    }
    if (new_on3 != http_on3) {
        http_on3 = new_on3;
        HTTP_OnOutputChanged(3, http_on3);
    }

    http_send_response(tpcb,
                       "HTTP/1.1 200 OK\r\n",
                       "text/plain; charset=utf-8",
                       "OK\r\n");
}
