#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize input handler
 * @return 0 on success, -1 on failure
 */
int input_handler_init(void);

/**
 * @brief Process SDL keyboard and mouse events
 * @return 0 on success, -1 on failure, 1 if quit requested
 */
int input_handler_process_events(void);

/**
 * @brief Cleanup input handler
 */
void input_handler_cleanup(void);

/**
 * @brief Get current mouse position
 * @param x Pointer to store X coordinate
 * @param y Pointer to store Y coordinate
 * @return 0 on success, -1 on failure
 */
int input_handler_get_mouse_position(int* x, int* y);

#ifdef __cplusplus
}
#endif
