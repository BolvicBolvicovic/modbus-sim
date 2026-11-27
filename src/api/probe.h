#ifndef API_PROBE_H
#define API_PROBE_H

/**
 * @file
 * @brief Message of the last failed mogbus_probe() on the calling thread.
 */

/** @brief Text of the last mogbus_probe() failure of this thread; "" after a success. */
const char*	probe_last_error(void);

#endif
