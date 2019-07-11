/* / mcu / sound.h
 * EEZ PSU Firmware
 * Copyright (C) 2015-present, Envox d.o.o.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <eez/apps/psu/psu.h>

#include <stdio.h>

#include <eez/apps/psu/datetime.h>
#include <eez/apps/psu/scpi/psu.h>
#include <eez/apps/psu/serial_psu.h>
#include <eez/scpi/commands.h>
#include <eez/sound.h>
#if OPTION_WATCHDOG && (EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R3B4 ||                      \
                        EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R5B12)
#include <eez/apps/psu/watchdog.h>
#endif
#include <eez/apps/psu/idle.h>
#include <eez/system.h>

namespace eez {
namespace psu {
namespace scpi {

////////////////////////////////////////////////////////////////////////////////

#define SCPI_COMMAND(P, C) scpi_result_t C(scpi_t *context);
SCPI_COMMANDS
#undef SCPI_COMMAND

#define SCPI_COMMAND(P, C) { P, C },
static const scpi_command_t scpi_commands[] = { SCPI_COMMANDS SCPI_CMD_LIST_END };

////////////////////////////////////////////////////////////////////////////////

void init(scpi_t &scpi_context, scpi_psu_t &scpi_psu_context, scpi_interface_t *interface,
          char *input_buffer, size_t input_buffer_length, scpi_error_t *error_queue_data,
          int16_t error_queue_size) {
    SCPI_Init(&scpi_context, scpi_commands, interface, scpi_units_def, IDN_MANUFACTURER, IDN_MODEL,
              persist_conf::devConf.serialNumber, FIRMWARE, input_buffer, input_buffer_length,
              error_queue_data, error_queue_size);

    scpi_psu_context.selected_channel_index = 1;
#if OPTION_SD_CARD
    scpi_psu_context.currentDirectory[0] = 0;
#endif
    scpi_psu_context.isBufferOverrun = false;
    scpi_psu_context.bufferOverrunTime = 0;

    scpi_context.user_context = &scpi_psu_context;
}

void emptyBuffer(scpi_t &context) {
    SCPI_Input(&context, 0, 0);
}

void onBufferOverrun(scpi_t &context) {
    emptyBuffer(context);
    scpi_psu_t *psu_context = (scpi_psu_t *)context.user_context;
    psu_context->isBufferOverrun = true;
    psu_context->bufferOverrunTime = micros();
}

void input(scpi_t &context, const char *str, size_t size) {
    idle::noteScpiActivity();

    scpi_psu_t *psu_context = (scpi_psu_t *)context.user_context;
    if (psu_context->isBufferOverrun) {
        // wait for 500ms of idle input to declare buffer overrun finished
        uint32_t tickCount = micros();
        int32_t diff = tickCount - psu_context->bufferOverrunTime;
        if (diff > 500000) {
            psu_context->isBufferOverrun = false;
        } else {
            psu_context->bufferOverrunTime = tickCount;
            return;
        }
    }

    int result = SCPI_Input(&context, str, size);
    if (result == -1) {
        onBufferOverrun(context);
    }
}

void printError(int_fast16_t err) {
    sound::playBeep();

    if (serial::g_testResult == TEST_OK) {
        char errorOutputBuffer[256];

        SERIAL_PORT.print("**ERROR");

        char datetime_buffer[20] = { 0 };
        if (datetime::getDateTimeAsString(datetime_buffer)) {
            sprintf(errorOutputBuffer, " [%s]", datetime_buffer);
            SERIAL_PORT.print(errorOutputBuffer);
        }

        sprintf(errorOutputBuffer, ": %d,\"%s\"", (int16_t)err, SCPI_ErrorTranslate(err));
        SERIAL_PORT.println(errorOutputBuffer);
    }

#if OPTION_WATCHDOG && (EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R3B4 ||                      \
                        EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R5B12)
    watchdog::tick(micros());
#endif
}

void resultChoiceName(scpi_t *context, scpi_choice_def_t *choice, int tag) {
    for (; choice->name; ++choice) {
        if (choice->tag == tag) {
            char text[64];

            // copy choice name while upper case letter or digit, examples: IMMediate -> IMM, PIN1
            // -> PIN1
            const char *src = choice->name;
            char *dst = text;
            while (*src && (isUperCaseLetter(*src) || isDigit(*src))) {
                *dst++ = *src++;
            }
            *dst = 0;

            SCPI_ResultText(context, text);
            break;
        }
    }
}

void resetContext(scpi_t *context) {
    scpi_psu_t *psuContext = (scpi_psu_t *)context->user_context;

    psuContext->selected_channel_index = 1;

#if OPTION_SD_CARD
    psuContext->currentDirectory[0] = 0;
#endif

    SCPI_ErrorClear(context);
}

} // namespace scpi
} // namespace psu
} // namespace eez