/*
 * optiga_status.h — internal.
 *
 * Maps Infineon host library status codes (optiga_lib_status_t) to
 * pico_optiga error codes. Pure function with no pico-sdk or hardware
 * dependency, so it can be unit-tested natively (see tests/).
 */

#ifndef PICO_OPTIGA_STATUS_H
#define PICO_OPTIGA_STATUS_H

#include <stdint.h>

/* optiga_status: a vendor status code (optiga_lib_status_t is uint16_t).
 * Returns PICO_OPTIGA_OK (0) on success, a negative PICO_OPTIGA_E* code
 * on failure. */
int pico_optiga_map_status(uint16_t optiga_status);

#endif /* PICO_OPTIGA_STATUS_H */
