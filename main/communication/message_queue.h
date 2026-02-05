#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSG_QUEUE_MAX_MESSAGES 128
#define MSG_QUEUE_MAX_PAYLOAD 4096

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t sub_cmd;
    uint8_t payload[MSG_QUEUE_MAX_PAYLOAD];
    size_t payload_len;
} message_queue_item_t;

typedef struct {
    message_queue_item_t items[MSG_QUEUE_MAX_MESSAGES];
    int read_idx;
    int write_idx;
    int count;
} message_queue_t;

/**
 * Initialize message queue
 * @param queue Queue to initialize
 */
void message_queue_init(message_queue_t *queue);

/**
 * Enqueue a message
 * @param queue Queue
 * @param type Message type
 * @param seq Sequence number
 * @param sub_cmd Sub-command
 * @param payload Payload data
 * @param payload_len Payload length
 * @return 0 on success, -1 if queue full or payload too large
 */
int message_queue_enqueue(message_queue_t *queue,
                         uint8_t type, uint8_t seq, uint8_t sub_cmd,
                         const uint8_t *payload, size_t payload_len);

/**
 * Dequeue a message
 * @param queue Queue
 * @param type Output: message type
 * @param seq Output: sequence number
 * @param sub_cmd Output: sub-command
 * @param payload Output: pointer to payload (valid until next dequeue)
 * @param payload_len Output: payload length
 * @return 1 if message dequeued, 0 if queue empty
 */
int message_queue_dequeue(message_queue_t *queue,
                         uint8_t *type, uint8_t *seq, uint8_t *sub_cmd,
                         const uint8_t **payload, size_t *payload_len);

/**
 * Get number of messages in queue
 * @param queue Queue
 * @return Number of messages
 */
int message_queue_count(const message_queue_t *queue);

/**
 * Check if queue is empty
 * @param queue Queue
 * @return 1 if empty, 0 otherwise
 */
int message_queue_is_empty(const message_queue_t *queue);

/**
 * Check if queue is full
 * @param queue Queue
 * @return 1 if full, 0 otherwise
 */
int message_queue_is_full(const message_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif // MESSAGE_QUEUE_H
