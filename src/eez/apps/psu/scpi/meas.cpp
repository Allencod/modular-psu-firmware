/*
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

#include <eez/apps/psu/channel_dispatcher.h>
#include <eez/apps/psu/scpi/psu.h>
#include <eez/apps/psu/temperature.h>

namespace eez {
namespace psu {
namespace scpi {

////////////////////////////////////////////////////////////////////////////////

scpi_result_t scpi_cmd_measureScalarCurrentDcQ(scpi_t *context) {
    // TODO migrate to generic firmware
    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    char buffer[256] = { 0 };
    strcatFloatValue(buffer, channel_dispatcher::getIMon(*channel), UNIT_AMPER, channel->index - 1);
    SCPI_ResultCharacters(context, buffer, strlen(buffer));

    return SCPI_RES_OK;
}

scpi_result_t scpi_cmd_measureScalarPowerDcQ(scpi_t *context) {
    // TODO migrate to generic firmware
    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    char buffer[256] = { 0 };
    strcatFloat(buffer,
                channel_dispatcher::getUMon(*channel) * channel_dispatcher::getIMon(*channel),
                getNumSignificantDecimalDigits(UNIT_WATT, channel->index - 1, false));
    SCPI_ResultCharacters(context, buffer, strlen(buffer));

    return SCPI_RES_OK;
}

scpi_result_t scpi_cmd_measureScalarVoltageDcQ(scpi_t *context) {
    // TODO migrate to generic firmware
    Channel *channel = param_channel(context);
    if (!channel) {
        return SCPI_RES_ERR;
    }

    char buffer[256] = { 0 };
    strcatFloat(buffer, channel_dispatcher::getUMon(*channel),
                getNumSignificantDecimalDigits(UNIT_VOLT, channel->index - 1, false));
    SCPI_ResultCharacters(context, buffer, strlen(buffer));

    return SCPI_RES_OK;
}

scpi_result_t scpi_cmd_measureScalarTemperatureThermistorDcQ(scpi_t *context) {
    // TODO migrate to generic firmware
    int32_t sensor;
    if (!param_temp_sensor(context, sensor)) {
        return SCPI_RES_ERR;
    }

    char buffer[256] = { 0 };
    strcatFloat(buffer, temperature::sensors[sensor].measure(),
                getNumSignificantDecimalDigits(UNIT_CELSIUS, -1, false));
    SCPI_ResultCharacters(context, buffer, strlen(buffer));

    return SCPI_RES_OK;
}

} // namespace scpi
} // namespace psu
} // namespace eez