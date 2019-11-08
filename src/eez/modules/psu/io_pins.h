/*
 * EEZ Modular Firmware
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
 
#pragma once

static const uint8_t EXT_TRIG1 = 0;
static const uint8_t EXT_TRIG2 = 1;
static const uint8_t DOUT1 = 2;
static const uint8_t DOUT2 = 3;
static const uint8_t NUM_IO_PINS = 4;

namespace eez {
namespace psu {
namespace io_pins {

enum Polarity {
    POLARITY_NEGATIVE,
    POLARITY_POSITIVE
};

enum Function {
    FUNCTION_NONE,
    FUNCTION_INPUT,
    FUNCTION_OUTPUT,
    FUNCTION_FAULT,
    FUNCTION_INHIBIT,
    FUNCTION_ON_COUPLE,
    FUNCTION_TINPUT,
    FUNCTION_TOUTPUT
};

void init();
void tick(uint32_t tickCount);
void onTrigger();
void refresh();

// When PSU is in inhibited state all outputs are disabled and execution of LIST on channels is stopped.
bool isInhibited();

void setPinState(int pin, bool state);
bool getPinState(int pin);

int ioPinRead(int pin);
void ioPinWrite(int pin, int state);

}
}
} // namespace eez::psu::io_pins