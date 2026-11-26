/**
 * @file
 * @brief Classification of the last OS error of the calling thread, so no caller reads errno
 *        (or WSAGetLastError()) itself.
 */
#ifndef OS_ERROR_H
#define OS_ERROR_H

#include <stdbool.h>

int		os_last_error(void);
const char*	os_error_text(int code);
const char*	os_last_error_text(void);
bool		os_would_block(void);
bool		os_interrupted(void);
bool		os_retry(void);			/**< would block or interrupted: try again later */
bool		os_out_of_handles(void);	/**< accept/open failed for lack of descriptors */
bool		os_conn_aborted(void);
bool		os_conn_refused(void);

#endif
