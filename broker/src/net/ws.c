#include <string.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

#define LOG_TAG "ws"
#include <dslink/log.h>
#include <dslink/err.h>
#include <broker/sys/throughput.h>
#include <wslay_event.h>

#include "broker/remote_dslink.h"
#include "broker/net/ws.h"

#define BROKER_WS_RESP "HTTP/1.1 101 Switching Protocols\r\n" \
                            "Upgrade: websocket\r\n" \
                            "Connection: Upgrade\r\n" \
                            "Sec-WebSocket-Accept: %s\r\n\r\n"

void broker_ws_send_init(Socket *sock, const char *accept) {
    char buf[1024];
    int bLen = snprintf(buf, sizeof(buf), BROKER_WS_RESP, accept);
    dslink_socket_write(sock, buf, (size_t) bLen);
}
int broker_count_json_msg(json_t *json) {
    int messages = 0;
    json_t * requests = json_object_get(json, "requests");
    json_t * responses = json_object_get(json, "responses");
    if (json_is_array(requests)) {
        messages += json_array_size(requests);
    }
    if (json_is_array(responses)) {
        size_t  idx;
        json_t * value;
        json_array_foreach(responses, idx, value) {
            json_t *updates = json_object_get(value, "updates");
            size_t updatesSize = json_array_size(updates);
            if (updatesSize > 0) {
                messages += updatesSize;
            } else {
                messages ++;
            }
        }
    }
    return messages;
}
int broker_ws_send_obj(RemoteDSLink *link, json_t *obj) {
    ++link->msgId;
    json_object_set_new_nocheck(obj, "msg", json_integer(link->msgId));
    char *data = json_dumps(obj, JSON_PRESERVE_ORDER | JSON_COMPACT);
    json_object_del(obj, "msg");

    if (!data) {
        return DSLINK_ALLOC_ERR;
    }
    int sentBytes = broker_ws_send(link, data);
    if (throughput_output_needed()) {
        int sentMessages = broker_count_json_msg(obj);
        throughput_add_output(sentBytes, sentMessages);
    }
    dslink_free(data);
    return 0;
}
#ifdef BROKER_WS_SEND_THREAD_MODE
void broker_send_ws_thread(void *arg) {

    int ret;
    RemoteDSLink *link = (RemoteDSLink*)arg;
    while(1) {
        uv_sem_wait(&link->ws_send_sem);

        if(link->closing_send_thread ==1) {
            log_debug("Closing ws send thread\n");
            break;
        }


        do {
            ret = wslay_event_send(link->ws);
            if (ret != 0) {
                log_debug("Send error in thread: %d\n", ret);
            } else {
                log_debug("Message sent: %d\n", ret);
            }

        } while(link->ws->queued_msg_count != 0);
    }
}
#endif

int broker_ws_send(RemoteDSLink *link, const char *data) {
    if (!link->ws) {
        return -1;
    }
    struct wslay_event_msg msg;
    msg.msg = (const uint8_t *) data;
    msg.msg_length = strlen(data);
    msg.opcode = WSLAY_TEXT_FRAME;
    wslay_event_queue_msg(link->ws, &msg);

#ifdef BROKER_WS_SEND_THREAD_MODE
    uv_sem_post(&link->ws_send_sem);
#else
    wslay_event_send(link->ws);
#endif

    log_debug("Message sent to %s: %s\n", (char *) link->dsId->data, data);

    return (int)msg.msg_length;
}

int broker_ws_generate_accept_key(const char *buf, size_t bufLen,
                                  char *out, size_t outLen) {
    char data[256];
    memset(data, 0, sizeof(data));
    int len = snprintf(data, sizeof(data), "%.*s%s", (int) bufLen, buf,
                       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    unsigned char sha1[20];
    mbedtls_sha1((unsigned char *) data, (size_t) len, sha1);
    return mbedtls_base64_encode((unsigned char *) out, outLen,
                                 &outLen, sha1, sizeof(sha1));
}
