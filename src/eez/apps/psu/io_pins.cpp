/*
 * EEZ PSU Firmware
 * Copyright (C) 2017-present, Envox d.o.o.
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

#include <assert.h>

#if defined EEZ_PLATFORM_STM32
#include <main.h>
#endif

#include <eez/apps/psu/psu.h>

#include <eez/apps/psu/fan.h>
#include <eez/apps/psu/io_pins.h>
#include <eez/apps/psu/persist_conf.h>
#include <eez/system.h>

namespace eez {
namespace psu {
namespace io_pins {

static struct {
    unsigned outputFault : 2;
    unsigned outputEnabled : 2;
    unsigned toutputPulse : 1;
    unsigned inhibited : 1;
} g_lastState = { 2, 2, 0, 0 };

static uint32_t g_toutputPulseStartTickCount;

static bool g_digitalOutputPinState[2] = { false, false };

#if defined EEZ_PLATFORM_STM32

int ioPinRead(int pin) {
    if (pin == EXT_TRIG1) {
    	return HAL_GPIO_ReadPin(UART_RX_DIN1_GPIO_Port, UART_RX_DIN1_Pin) ? 1 : 0;
    } {
    	assert(pin == EXT_TRIG2);
    	return HAL_GPIO_ReadPin(DIN2_GPIO_Port, DIN2_Pin) ? 1 : 0;
    }
}

void ioPinWrite(int pin, int state) {
    if (pin == DOUT1) {
    	HAL_GPIO_WritePin(UART_TX_DOUT1_GPIO_Port, UART_TX_DOUT1_Pin, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    } else {
        assert(pin == DOUT2);
        HAL_GPIO_WritePin(DOUT2_GPIO_Port, DOUT2_Pin, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

#endif

#if defined EEZ_PLATFORM_SIMULATOR

static int g_pins[NUM_IO_PINS];

int ioPinRead(int pin) {
    return g_pins[pin];
}

void ioPinWrite(int pin, int state) {
	g_pins[pin] = state;
}

#endif

uint8_t isOutputFault() {
    if (isPowerUp()) {
        if (fan::g_testResult == TEST_FAILED) {
            return 1;
        }
    }

    for (int i = 0; i < CH_NUM; ++i) {
        Channel &channel = Channel::get(i);
        if (channel.isTripped() || !channel.isTestOk()) {
            return 1;
        }
    }

    return 0;
}

uint8_t isOutputEnabled() {
    for (int i = 0; i < CH_NUM; ++i) {
        if (Channel::get(i).isOutputEnabled()) {
            return 1;
        }
    }
    return 0;
}

#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R5B12
void updateFaultPin(int i) {
    persist_conf::IOPin &outputPin = persist_conf::devConf2.ioPins[i];
    int pin = i == 1 ? DOUT1 : DOUT2;
    int state =
        (g_lastState.outputFault && outputPin.polarity == io_pins::POLARITY_POSITIVE) ||
                (!g_lastState.outputFault && outputPin.polarity == io_pins::POLARITY_NEGATIVE)
            ? 1
            : 0;
    ioPinWrite(pin, state);
    // DebugTrace("FUNCTION_FAULT %d %d", pin, state);
}

void updateOnCouplePin(int i) {
    persist_conf::IOPin &outputPin = persist_conf::devConf2.ioPins[i];
    int pin = i == 1 ? DOUT1 : DOUT2;
    int state =
        (g_lastState.outputEnabled && outputPin.polarity == io_pins::POLARITY_POSITIVE) ||
                (!g_lastState.outputEnabled && outputPin.polarity == io_pins::POLARITY_NEGATIVE)
            ? 1
            : 0;
    ioPinWrite(pin, state);
    // DebugTrace("FUNCTION_ON_COUPLE %d %d", pin, state);
}
#endif

void tick(uint32_t tickCount) {
    // execute input pins function
    unsigned inhibited = 0;

    persist_conf::IOPin &inputPin1 = persist_conf::devConf2.ioPins[0];
    if (inputPin1.function == io_pins::FUNCTION_INHIBIT) {
        int value = ioPinRead(EXT_TRIG1);
        inhibited = (value && inputPin1.polarity == io_pins::POLARITY_POSITIVE) || (!value && inputPin1.polarity == io_pins::POLARITY_NEGATIVE) ? 1 : 0;
    }

    persist_conf::IOPin &inputPin2 = persist_conf::devConf2.ioPinInput2;
    if (inputPin2.function == io_pins::FUNCTION_INHIBIT) {
        int value = ioPinRead(EXT_TRIG2);
        inhibited = (value && inputPin2.polarity == io_pins::POLARITY_POSITIVE) || (!value && inputPin2.polarity == io_pins::POLARITY_NEGATIVE) ? 1 : 0;
    }

    if (inhibited != g_lastState.inhibited) {
        g_lastState.inhibited = inhibited;
        for (int i = 0; i < CH_NUM; ++i) {
            Channel::get(i).onInhibitedChanged(inhibited ? true : false);
        }
    }

#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R5B12
    // end trigger output pulse
    if (g_lastState.toutputPulse) {
        int32_t diff = tickCount - g_toutputPulseStartTickCount;
        if (diff > CONF_TOUTPUT_PULSE_WIDTH_MS * 1000L) {
            for (int i = 1; i < 3; ++i) {
                persist_conf::IOPin &outputPin = persist_conf::devConf2.ioPins[i];
                if (outputPin.function == io_pins::FUNCTION_TOUTPUT) {
                    ioPinWrite(i == 1 ? DOUT1 : DOUT2,
                               outputPin.polarity == io_pins::POLARITY_POSITIVE ? 0 : 1);
                }
            }

            g_lastState.toutputPulse = 0;
        }
    }

    enum { UNKNOWN, UNCHANGED, CHANGED } trippedState = UNKNOWN, outputEnabledState = UNKNOWN;

    // execute output pins function
    for (int i = 1; i < 3; ++i) {
        persist_conf::IOPin &outputPin = persist_conf::devConf2.ioPins[i];

        if (outputPin.function == io_pins::FUNCTION_FAULT) {
            if (trippedState == UNKNOWN) {
                uint8_t outputFault = isOutputFault();
                if (g_lastState.outputFault != outputFault) {
                    g_lastState.outputFault = outputFault;
                    trippedState = CHANGED;
                } else {
                    trippedState = UNCHANGED;
                }
            }

            if (trippedState == CHANGED) {
                updateFaultPin(i);
            }
        } else if (outputPin.function == io_pins::FUNCTION_ON_COUPLE) {
            if (outputEnabledState == UNKNOWN) {
                uint8_t outputEnabled = isOutputEnabled();
                if (g_lastState.outputEnabled != outputEnabled) {
                    g_lastState.outputEnabled = outputEnabled;
                    outputEnabledState = CHANGED;
                } else {
                    outputEnabledState = UNCHANGED;
                }
            }

            if (outputEnabledState == CHANGED) {
                updateOnCouplePin(i);
            }
        }
    }
#endif
}

void onTrigger() {
#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R5B12
    // start trigger output pulse
    for (int i = 1; i < 3; ++i) {
        persist_conf::IOPin &outputPin = persist_conf::devConf2.ioPins[i];
        if (outputPin.function == io_pins::FUNCTION_TOUTPUT) {
            ioPinWrite(i == 1 ? DOUT1 : DOUT2,
                       outputPin.polarity == io_pins::POLARITY_POSITIVE ? 1 : 0);
            g_lastState.toutputPulse = 1;
            g_toutputPulseStartTickCount = micros();
        }
    }
#endif
}

void refresh() {
#if EEZ_PSU_SELECTED_REVISION == EEZ_PSU_REVISION_R5B12
    // refresh output pins
    for (int i = 1; i < 3; ++i) {
        persist_conf::IOPin &outputPin = persist_conf::devConf2.ioPins[i];

        if (outputPin.function == io_pins::FUNCTION_NONE) {
            ioPinWrite(i == 1 ? DOUT1 : DOUT2, 0);
        } else if (outputPin.function == io_pins::FUNCTION_FAULT) {
            updateFaultPin(i);
        } else if (outputPin.function == io_pins::FUNCTION_ON_COUPLE) {
            updateOnCouplePin(i);
        }
    }
#endif
}

bool isInhibited() {
    return g_lastState.inhibited ? true : false;
}

void setDigitalOutputPinState(int pin, bool state) {
    g_digitalOutputPinState[pin - 2] = state;

    if (persist_conf::devConf2.ioPins[pin - 2].polarity == io_pins::POLARITY_NEGATIVE) {
        state = !state;
    }

    if (pin == 3) {
        ioPinWrite(DOUT1, state);
    } else {
        ioPinWrite(DOUT2, state);
    }
}

bool getDigitalOutputPinState(int pin) {
    return g_digitalOutputPinState[pin - 2];
}

} // namespace io_pins
} // namespace psu
} // namespace eez
