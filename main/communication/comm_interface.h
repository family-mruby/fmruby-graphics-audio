#ifndef COMM_INTERFACE_H
#define COMM_INTERFACE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Communication interface abstraction
 * Allows switching between socket (Linux) and SPI (ESP32) communication
 */
typedef struct {
    /**
     * Initialize communication interface
     * @return 0 on success, -1 on error
     */
    int (*init)(void);

    /**
     * Send data through communication interface
     * @param data Data buffer to send
     * @param len Length of data
     * @return Number of bytes sent, or -1 on error
     */
    int (*send)(const uint8_t *data, size_t len);

    /**
     * Receive data from communication interface
     * @param buf Buffer to store received data
     * @param buf_size Size of buffer
     * @return Number of bytes received, 0 if no data, -1 on error
     */
    int (*receive)(uint8_t *buf, size_t buf_size);

    /**
     * Process communication (non-blocking)
     * Should be called regularly from main loop or task
     * @return Number of messages processed, or -1 on error
     */
    int (*process)(void);

    /**
     * Send ACK response
     * @param type Message type
     * @param seq Sequence number
     * @param response_data Optional response payload
     * @param response_len Length of response payload
     * @return 0 on success, -1 on error
     */
    int (*send_ack)(uint8_t type, uint8_t seq, const uint8_t *response_data, uint16_t response_len);

    /**
     * Check if communication interface is running
     * @return 1 if running, 0 otherwise
     */
    int (*is_running)(void);

    /**
     * Cleanup and shutdown communication interface
     */
    void (*cleanup)(void);
} comm_interface_t;

// Get the active communication interface
#ifdef CONFIG_IDF_TARGET_LINUX
extern const comm_interface_t* comm_get_interface(void);
#define COMM_INTERFACE (comm_get_interface())
#else
extern const comm_interface_t* comm_get_interface(void);
#define COMM_INTERFACE (comm_get_interface())
#endif

#ifdef __cplusplus
}
#endif

#endif // COMM_INTERFACE_H
