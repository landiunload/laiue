#pragma once

#include "api.h"

#include <stdint.h>
#include <wchar.h>

/* Process services for an application whose composition root links only
 * laiue::bootstrap. They are the platform boundary the engine itself uses, so
 * such an application needs no OS headers to find its files, measure time,
 * pace a loop or stop cleanly. Results crossing the C ABI are fixed-width:
 * non-zero means success. */

#define LAIUE_APPLICATION_PATH_CAPACITY 1024u

/* Directory of the running executable, NUL-terminated, without a trailing
 * separator. On failure output holds an empty string. */
LAIUE_MOD_API uint32_t LaiueApplicationExecutableDirectory(wchar_t *output, uint32_t capacity);

/* Seconds of a monotonic clock with an unspecified origin. It never goes back
 * and does not follow wall-clock adjustments. */
LAIUE_MOD_API double LaiueApplicationMonotonicSeconds(void);

LAIUE_MOD_API void LaiueApplicationSleepMilliseconds(uint32_t milliseconds);

/* Turns Ctrl+C and console close on Windows, SIGINT and SIGTERM on POSIX into
 * a request flag instead of an abrupt exit, so a server or a long simulation
 * can finish its tick and save. Installing clears an earlier request. */
LAIUE_MOD_API uint32_t LaiueApplicationInstallTerminationHandler(void);
LAIUE_MOD_API void LaiueApplicationRemoveTerminationHandler(void);
LAIUE_MOD_API uint32_t LaiueApplicationTerminationRequested(void);
