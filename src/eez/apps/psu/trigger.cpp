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

#include <eez/apps/psu/psu.h>

#include <eez/apps/psu/channel_dispatcher.h>
#include <eez/apps/psu/io_pins.h>
#include <eez/apps/psu/list_program.h>
#include <eez/apps/psu/persist_conf.h>
#include <eez/apps/psu/profile.h>
#include <eez/scpi/regs.h>
#include <eez/apps/psu/trigger.h>
#include <eez/system.h>

#if OPTION_SD_CARD
#include <eez/apps/psu/dlog.h>
#endif

namespace eez {
namespace psu {
namespace trigger {

static struct {
    float u;
    float i;
} g_levels[CH_MAX];

enum State { STATE_IDLE, STATE_INITIATED, STATE_TRIGGERED, STATE_EXECUTING };
static State g_state;
static uint32_t g_triggeredTime;

bool g_triggerInProgress[CH_MAX];

void setState(State newState) {
    if (g_state != newState) {
        if (newState == STATE_INITIATED) {
            for (int i = 0; i < CH_NUM; ++i) {
                Channel &channel = Channel::get(i);
                channel.setOperBits(OPER_ISUM_TRIG, true);
            }
        }

        if (g_state == STATE_INITIATED) {
            for (int i = 0; i < CH_NUM; ++i) {
                Channel &channel = Channel::get(i);
                channel.setOperBits(OPER_ISUM_TRIG, false);
            }
        }

        g_state = newState;
    }
}

void reset() {
    bool changed = false;

    if (persist_conf::devConf2.triggerDelay != DELAY_DEFAULT) {
        persist_conf::devConf2.triggerDelay = DELAY_DEFAULT;
        changed = true;
    }

    if (persist_conf::devConf2.triggerSource != SOURCE_IMMEDIATE) {
        persist_conf::devConf2.triggerSource = SOURCE_IMMEDIATE;
        changed = true;
    }

    if (persist_conf::devConf2.flags.triggerContinuousInitializationEnabled) {
        persist_conf::devConf2.flags.triggerContinuousInitializationEnabled = 0;
        changed = true;
    }

    if (changed) {
        persist_conf::saveDevice2();
    }

    setState(STATE_IDLE);
}

void init() {
    setState(STATE_IDLE);

    if (isContinuousInitializationEnabled()) {
        initiate();
    }
}

void setDelay(float delay) {
    persist_conf::devConf2.triggerDelay = delay;
}

float getDelay() {
    return persist_conf::devConf2.triggerDelay;
}

void setSource(Source source) {
    persist_conf::devConf2.triggerSource = source;
}

Source getSource() {
    return (Source)persist_conf::devConf2.triggerSource;
}

void setVoltage(Channel &channel, float value) {
    value = roundPrec(value, channel.getVoltagePrecision());
    g_levels[channel.channelIndex].u = value;
}

float getVoltage(Channel &channel) {
    return g_levels[channel.channelIndex].u;
}

void setCurrent(Channel &channel, float value) {
    value = roundPrec(value, channel.getCurrentPrecision(value));
    g_levels[channel.channelIndex].i = value;
}

float getCurrent(Channel &channel) {
    return g_levels[channel.channelIndex].i;
}

void check(uint32_t currentTime) {
    if (currentTime - g_triggeredTime > persist_conf::devConf2.triggerDelay * 1000L) {
        startImmediately();
    }
}

int generateTrigger(Source source, bool checkImmediatelly) {
    bool seqTriggered =
        persist_conf::devConf2.triggerSource == source && g_state == STATE_INITIATED;
#if OPTION_SD_CARD
    bool dlogTriggered = dlog::g_triggerSource == source && dlog::isInitiated();
#endif

    if (!seqTriggered) {
#if OPTION_SD_CARD
        if (!dlogTriggered) {
            return SCPI_ERROR_TRIGGER_IGNORED;
        }
#else
        return SCPI_ERROR_TRIGGER_IGNORED;
#endif
    }

#if OPTION_SD_CARD
    if (dlogTriggered) {
        dlog::triggerGenerated(checkImmediatelly);
    }
#endif

    if (seqTriggered) {
        setState(STATE_TRIGGERED);

        g_triggeredTime = micros() / 1000;

        if (checkImmediatelly) {
            check(g_triggeredTime);
        }
    }

    return SCPI_RES_OK;
}

bool isTriggerFinished() {
    for (int i = 0; i < CH_NUM; ++i) {
        if (g_triggerInProgress[i]) {
            return false;
        }
    }
    return true;
}

void triggerFinished() {
    if (persist_conf::devConf2.flags.triggerContinuousInitializationEnabled) {
        setState(STATE_INITIATED);
    } else {
        setState(STATE_IDLE);
    }
}

void onTriggerFinished(Channel &channel) {
    if (channel.getVoltageTriggerMode() == TRIGGER_MODE_LIST) {
        int err;

        switch (channel.getTriggerOnListStop()) {
        case TRIGGER_ON_LIST_STOP_OUTPUT_OFF:
            channel_dispatcher::setVoltage(channel, 0);
            channel_dispatcher::setCurrent(channel, 0);
            channel_dispatcher::outputEnable(channel, false);
            break;
        case TRIGGER_ON_LIST_STOP_SET_TO_FIRST_STEP:
            if (!list::setListValue(channel, 0, &err)) {
                generateError(err);
            }
            break;
        case TRIGGER_ON_LIST_STOP_SET_TO_LAST_STEP:
            if (!list::setListValue(channel, list::maxListsSize(channel) - 1, &err)) {
                generateError(err);
            }
            break;
        case TRIGGER_ON_LIST_STOP_STANDBY:
            channel_dispatcher::setVoltage(channel, 0);
            channel_dispatcher::setCurrent(channel, 0);
            channel_dispatcher::outputEnable(channel, false);
            changePowerState(false);
            break;
        }
    }
}

void setTriggerFinished(Channel &channel) {
    if (channel_dispatcher::isCoupled() || channel_dispatcher::isTracked()) {
        for (int i = 0; i < CH_NUM; ++i) {
            g_triggerInProgress[i] = false;
        }
    } else {
        g_triggerInProgress[channel.channelIndex] = false;
    }

    onTriggerFinished(channel);

    if (isTriggerFinished()) {
        triggerFinished();
    }
}

int checkTrigger() {
    bool onlyFixed = true;

    for (int i = 0; i < CH_NUM; ++i) {
        Channel &channel = Channel::get(i);

        if (!channel.isOk()) {
            continue;
        }

        if (i == 0 || !(channel_dispatcher::isCoupled() || channel_dispatcher::isTracked())) {
            if (channel.getVoltageTriggerMode() != channel.getCurrentTriggerMode()) {
                return SCPI_ERROR_INCOMPATIBLE_TRANSIENT_MODES;
            }

            if (channel.getVoltageTriggerMode() != TRIGGER_MODE_FIXED) {
                if (channel.isRemoteProgrammingEnabled()) {
                    return SCPI_ERROR_EXECUTION_ERROR;
                }

                if (channel.getVoltageTriggerMode() == TRIGGER_MODE_LIST) {
                    if (list::isListEmpty(channel)) {
                        return SCPI_ERROR_LIST_IS_EMPTY;
                    }

                    if (!list::areListLengthsEquivalent(channel)) {
                        return SCPI_ERROR_LIST_LENGTHS_NOT_EQUIVALENT;
                    }

                    int err = list::checkLimits(i);
                    if (err) {
                        return err;
                    }
                } else {
                    if (g_levels[i].u > channel_dispatcher::getULimit(channel)) {
                        return SCPI_ERROR_VOLTAGE_LIMIT_EXCEEDED;
                    }

                    if (g_levels[i].i > channel_dispatcher::getILimit(channel)) {
                        return SCPI_ERROR_CURRENT_LIMIT_EXCEEDED;
                    }

                    if (g_levels[i].u * g_levels[i].i > channel_dispatcher::getPowerLimit(channel)) {
                        return SCPI_ERROR_POWER_LIMIT_EXCEEDED;
                    }
                }

                onlyFixed = false;
            }
        }
    }

    if (onlyFixed) {
        return SCPI_ERROR_CANNOT_INITIATE_WHILE_IN_FIXED_MODE;
    }

    return 0;
}

int startImmediately() {
    int err = checkTrigger();
    if (err) {
        return err;
    }

    setState(STATE_EXECUTING);
    for (int i = 0; i < CH_NUM; ++i) {
        Channel &channel = Channel::get(i);
        if (channel.isOk()) {
            g_triggerInProgress[i] = true;
        }
    }

    io_pins::onTrigger();

    for (int i = 0; i < CH_NUM; ++i) {
        Channel &channel = Channel::get(i);

        if (channel.isOk() && (i == 0 || !(channel_dispatcher::isCoupled() || channel_dispatcher::isTracked()))) {
            if (channel.getVoltageTriggerMode() == TRIGGER_MODE_LIST) {
                channel_dispatcher::setVoltage(channel, 0);
                channel_dispatcher::setCurrent(channel, 0);

                channel_dispatcher::outputEnable(
                    channel, channel_dispatcher::getTriggerOutputState(channel));

                list::executionStart(channel);
            } else {
                if (channel.getVoltageTriggerMode() == TRIGGER_MODE_STEP) {
                    channel_dispatcher::setVoltage(channel, g_levels[i].u);
                    channel_dispatcher::setCurrent(channel, g_levels[i].i);

                    channel_dispatcher::outputEnable(
                        channel, channel_dispatcher::getTriggerOutputState(channel));
                }

                setTriggerFinished(channel);
            }
        }
    }

    return SCPI_RES_OK;
}

int initiate() {
    if (persist_conf::devConf2.triggerSource == SOURCE_IMMEDIATE) {
        return startImmediately();
    } else {
        int err = checkTrigger();
        if (err) {
            return err;
        }
        setState(STATE_INITIATED);
    }
    return SCPI_RES_OK;
}

int enableInitiateContinuous(bool enable) {
    persist_conf::devConf2.flags.triggerContinuousInitializationEnabled = enable;
    if (enable) {
        return initiate();
    } else {
        return SCPI_RES_OK;
    }
}

bool isContinuousInitializationEnabled() {
    return persist_conf::devConf2.flags.triggerContinuousInitializationEnabled;
}

bool isIdle() {
    return g_state == STATE_IDLE;
}

bool isInitiated() {
    return g_state == STATE_INITIATED;
}

void abort() {
    list::abort();
    setState(STATE_IDLE);
}

void tick(uint32_t tick_usec) {
    if (g_state == STATE_TRIGGERED) {
        check(tick_usec / 1000);
    }
}

} // namespace trigger
} // namespace psu
} // namespace eez
