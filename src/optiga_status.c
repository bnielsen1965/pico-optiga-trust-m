/*
 * optiga_status.c
 *
 * The single place where vendor optiga_lib_status_t codes are mapped
 * to pico_optiga error codes. Keep this a pure lookup: the mapping is
 * exercised by the native unit test (tests/test_optiga_status.c).
 *
 * Vendor code layout (optiga_lib_return_codes.h): [A][B][CD] — bit 15
 * set means an OPTIGA chip error (OPTIGA_DEVICE_ERROR | chip code, see
 * the SRM); bits 14-8 select the layer; bits 7-0 the nature.
 */

#include "optiga_status.h"

#include "optiga_lib_return_codes.h"
#include "pico_optiga/optiga.h"

int pico_optiga_map_status(uint16_t optiga_status) {
    switch (optiga_status) {
    case OPTIGA_LIB_SUCCESS:
        return PICO_OPTIGA_OK;
    case OPTIGA_LIB_BUSY:
        return PICO_OPTIGA_EBUSY;

    /* Util layer (0x03xx) */
    case OPTIGA_UTIL_ERROR:
        return PICO_OPTIGA_EIO;
    case OPTIGA_UTIL_ERROR_INVALID_INPUT:
        return PICO_OPTIGA_EINVAL;
    case OPTIGA_UTIL_ERROR_MEMORY_INSUFFICIENT:
        return PICO_OPTIGA_EIO;
    case OPTIGA_UTIL_ERROR_INSTANCE_IN_USE:
        return PICO_OPTIGA_EBUSY;

    /* Command layer (0x02xx) */
    case OPTIGA_CMD_ERROR:
        return PICO_OPTIGA_EIO;
    case OPTIGA_CMD_ERROR_INVALID_INPUT:
        return PICO_OPTIGA_EINVAL;
    case OPTIGA_CMD_ERROR_MEMORY_INSUFFICIENT:
        return PICO_OPTIGA_EIO;

    /* Comms layer (0x01xx) */
    case OPTIGA_COMMS_ERROR:
        return PICO_OPTIGA_EIO;
    case OPTIGA_COMMS_ERROR_INVALID_INPUT:
        return PICO_OPTIGA_EINVAL;
    case OPTIGA_COMMS_ERROR_MEMORY_INSUFFICIENT:
        return PICO_OPTIGA_EIO;
    case OPTIGA_COMMS_ERROR_STACK_MEMORY:
        return PICO_OPTIGA_EIO;
    case OPTIGA_COMMS_ERROR_FATAL:
        return PICO_OPTIGA_EIO;
    case OPTIGA_COMMS_ERROR_HANDSHAKE:
        /* 0x0107: the chip holds a shielded-connection pairing the
         * host did not complete (pre-paired chip). Not a bus problem. */
        return PICO_OPTIGA_EHANDSHAKE;
    case OPTIGA_COMMS_ERROR_SESSION:
        return PICO_OPTIGA_EIO;

    default:
        /* Bit 15 set: OPTIGA device error (0x8000 | chip code). */
        if ((optiga_status & 0x8000u) != 0u) {
            return PICO_OPTIGA_EDEVICE;
        }
        return PICO_OPTIGA_EIO;
    }
}
