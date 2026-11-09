/*
 * test_optiga_status.c — native unit test for the vendor status code
 * mapping (src/optiga_status.c). Plain asserts, no framework.
 */

#include <assert.h>
#include <stdio.h>

#include "optiga_lib_return_codes.h"
#include "optiga_status.h"
#include "pico_optiga/optiga.h"

int main(void) {
    /* Success and the busy sentinel. */
    assert(pico_optiga_map_status(OPTIGA_LIB_SUCCESS) == PICO_OPTIGA_OK);
    assert(pico_optiga_map_status(OPTIGA_LIB_BUSY) == PICO_OPTIGA_EBUSY);

    /* Util layer (0x03xx). */
    assert(pico_optiga_map_status(OPTIGA_UTIL_ERROR) == PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_UTIL_ERROR_INVALID_INPUT) ==
           PICO_OPTIGA_EINVAL);
    assert(pico_optiga_map_status(OPTIGA_UTIL_ERROR_MEMORY_INSUFFICIENT) ==
           PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_UTIL_ERROR_INSTANCE_IN_USE) ==
           PICO_OPTIGA_EBUSY);

    /* Command layer (0x02xx). */
    assert(pico_optiga_map_status(OPTIGA_CMD_ERROR) == PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_CMD_ERROR_INVALID_INPUT) ==
           PICO_OPTIGA_EINVAL);
    assert(pico_optiga_map_status(OPTIGA_CMD_ERROR_MEMORY_INSUFFICIENT) ==
           PICO_OPTIGA_EIO);

    /* Comms layer (0x01xx). */
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR) == PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR_INVALID_INPUT) ==
           PICO_OPTIGA_EINVAL);
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR_MEMORY_INSUFFICIENT) ==
           PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR_STACK_MEMORY) ==
           PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR_FATAL) ==
           PICO_OPTIGA_EIO);
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR_HANDSHAKE) ==
           PICO_OPTIGA_EHANDSHAKE);
    assert(pico_optiga_map_status(OPTIGA_COMMS_ERROR_SESSION) ==
           PICO_OPTIGA_EIO);

    /* Device errors: bit 15 set (0x8000 | chip code, see the SRM). */
    assert(pico_optiga_map_status(OPTIGA_DEVICE_ERROR) ==
           PICO_OPTIGA_EDEVICE);
    assert(pico_optiga_map_status(0x8101u) == PICO_OPTIGA_EDEVICE);
    assert(pico_optiga_map_status(0x8202u) == PICO_OPTIGA_EDEVICE);

    /* Unknown codes: no bit 15 -> generic I/O error. */
    assert(pico_optiga_map_status(0x0999u) == PICO_OPTIGA_EIO);

    printf("test_optiga_status: all assertions passed\n");
    return 0;
}
