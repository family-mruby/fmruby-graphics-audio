#include "message_queue.h"
#include <string.h>
#include <stdio.h>

void message_queue_init(message_queue_t *queue) {
    if (!queue) return;

    memset(queue, 0, sizeof(message_queue_t));
    queue->read_idx = 0;
    queue->write_idx = 0;
    queue->count = 0;
}

int message_queue_enqueue(message_queue_t *queue,
                         uint8_t type, uint8_t seq, uint8_t sub_cmd,
                         const uint8_t *payload, size_t payload_len) {
    if (!queue) return -1;

    if (queue->count >= MSG_QUEUE_MAX_MESSAGES) {
        fprintf(stderr, "[MSG_QUEUE] Queue full, dropping message\n");
        return -1;
    }

    if (payload_len > MSG_QUEUE_MAX_PAYLOAD) {
        fprintf(stderr, "[MSG_QUEUE] Payload too large: %zu > %d\n",
                payload_len, MSG_QUEUE_MAX_PAYLOAD);
        return -1;
    }

    message_queue_item_t *item = &queue->items[queue->write_idx];
    item->type = type;
    item->seq = seq;
    item->sub_cmd = sub_cmd;
    item->payload_len = payload_len;

    if (payload && payload_len > 0) {
        memcpy(item->payload, payload, payload_len);
    }

    queue->write_idx = (queue->write_idx + 1) % MSG_QUEUE_MAX_MESSAGES;
    queue->count++;

    return 0;
}

int message_queue_dequeue(message_queue_t *queue,
                         uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
                         const uint8_t **payload, size_t *payload_len) {
    if (!queue || !type || !seq || !sub_cmd || !payload || !payload_len) {
        return 0;
    }

    if (queue->count == 0) {
        return 0;  // Queue empty
    }

    message_queue_item_t *item = &queue->items[queue->read_idx];

    *type = item->type;
    *seq = item->seq;
    *sub_cmd = item->sub_cmd;
    *payload = item->payload;
    *payload_len = item->payload_len;

    queue->read_idx = (queue->read_idx + 1) % MSG_QUEUE_MAX_MESSAGES;
    queue->count--;

    return 1;  // Message dequeued
}

int message_queue_count(const message_queue_t *queue) {
    return queue ? queue->count : 0;
}

int message_queue_is_empty(const message_queue_t *queue) {
    return queue ? (queue->count == 0) : 1;
}

int message_queue_is_full(const message_queue_t *queue) {
    return queue ? (queue->count >= MSG_QUEUE_MAX_MESSAGES) : 0;
}
