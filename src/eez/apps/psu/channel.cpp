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

#include <math.h>
#include <stdio.h>
#include <assert.h>

#include <eez/system.h>
#if OPTION_WATCHDOG
#include <eez/apps/psu/watchdog.h>
#endif
#include <eez/apps/psu/board.h>
#include <eez/apps/psu/calibration.h>
#include <eez/apps/psu/channel_dispatcher.h>
#include <eez/apps/psu/dac.h>
#include <eez/apps/psu/event_queue.h>
#include <eez/apps/psu/io_pins.h>
#include <eez/apps/psu/ioexp.h>
#include <eez/apps/psu/list_program.h>
#include <eez/apps/psu/persist_conf.h>
#include <eez/apps/psu/profile.h>
#include <eez/apps/psu/trigger.h>
#include <eez/modules/psu/adc.h>
#include <eez/scpi/regs.h>
#include <eez/sound.h>
#include <eez/index.h>

namespace eez {

using namespace scpi;

namespace psu {

////////////////////////////////////////////////////////////////////////////////

static const char *CH_BOARD_NAMES[] = { "None", "DCP505", "DCP405", "DCP405", "DCM220" };
static const char *CH_REVISION_NAMES[] = { "None", "R1B3", "R1B1", "R2B5", "R1B1" };
static const char *CH_BOARD_AND_REVISION_NAMES[] = { "None", "DCP505_R1B3", "DCP405_R1B1", "DCP405_R2B5", "DCM220_R1B1" };
    
static uint16_t CH_BOARD_REVISION_FEATURES[] = {
    // CH_BOARD_REVISION_NONE
    0,

    // CH_BOARD_REVISION_DCP505_R1B3
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_POWER | CH_FEATURE_OE | CH_FEATURE_DPROG |
    CH_FEATURE_RPROG | CH_FEATURE_RPOL,

    // CH_BOARD_REVISION_DCP405_R1B1
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_POWER | CH_FEATURE_OE | CH_FEATURE_DPROG |
    CH_FEATURE_RPROG | CH_FEATURE_RPOL,

    // CH_BOARD_REVISION_DCP405_R2B5
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_POWER | CH_FEATURE_OE | CH_FEATURE_DPROG |
    CH_FEATURE_RPROG | CH_FEATURE_RPOL,

    // CH_BOARD_REVISION_DCM220_R1B1
    CH_FEATURE_VOLT | CH_FEATURE_CURRENT | CH_FEATURE_POWER | CH_FEATURE_OE | CH_FEATURE_DPROG |
    CH_FEATURE_RPROG | CH_FEATURE_RPOL
};

////////////////////////////////////////////////////////////////////////////////

int CH_NUM = 0;

#define CHANNEL(INDEX) Channel(INDEX)
Channel channels[CH_MAX];
#undef CHANNEL

////////////////////////////////////////////////////////////////////////////////

Channel &Channel::get(int channelIndex) {
	assert(channelIndex >= 0 && channelIndex < CH_NUM);
    return channels[channelIndex];
}

Channel *Channel::getBySlotIndex(int slotIndex) {
    for (int i = 0; i < CH_NUM; i++) {
    	if (channels[i].slotIndex == slotIndex) {
            return &channels[i];
        }
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////

void Channel::Value::init(float set_, float step_, float limit_) {
    set = set_;
    step = step_;
    limit = limit_;
    resetMonValues();
}

void Channel::Value::resetMonValues() {
    mon_adc = 0;
    mon = 0;
    mon_last = 0;
    mon_dac = 0;

    mon_index = -1;
    mon_dac_index = -1;

    mon_measured = false;
}

void Channel::Value::addMonValue(float value, float prec) {
    value = roundPrec(value, prec);

    mon_last = value;

    if (mon_index == -1) {
        mon_index = 0;
        for (int i = 0; i < NUM_ADC_AVERAGING_VALUES; ++i) {
            mon_arr[i] = value;
        }
        mon_total = NUM_ADC_AVERAGING_VALUES * value;
        mon = value;
    } else {
        mon_total -= mon_arr[mon_index];
        mon_total += value;
        mon_arr[mon_index] = value;
        mon_index = (mon_index + 1) % NUM_ADC_AVERAGING_VALUES;
        mon = roundPrec(mon_total / NUM_ADC_AVERAGING_VALUES, prec);
    }

    mon_measured = true;
}

void Channel::Value::addMonDacValue(float value, float prec) {
    value = roundPrec(value, prec);

    if (mon_dac_index == -1) {
        mon_dac_index = 0;
        for (int i = 0; i < NUM_ADC_AVERAGING_VALUES; ++i) {
            mon_dac_arr[i] = value;
        }
        mon_dac_total = NUM_ADC_AVERAGING_VALUES * value;
        mon_dac = value;
    } else {
        mon_dac_total -= mon_dac_arr[mon_dac_index];
        mon_dac_total += value;
        mon_dac_arr[mon_dac_index] = value;
        mon_dac_index = (mon_dac_index + 1) % NUM_ADC_AVERAGING_VALUES;
        mon_dac = roundPrec(mon_dac_total / NUM_ADC_AVERAGING_VALUES, prec);
    }
}

////////////////////////////////////////////////////////////////////////////////

static struct {
    unsigned OE_SAVED : 1;
    unsigned CH1_OE : 1;
    unsigned CH2_OE : 1;
} g_savedOE;

void Channel::saveAndDisableOE() {
    if (!g_savedOE.OE_SAVED) {
        if (CH_NUM > 0) {
            g_savedOE.CH1_OE = Channel::get(0).isOutputEnabled() ? 1 : 0;
            Channel::get(0).outputEnable(false);

            if (CH_NUM > 1) {
                g_savedOE.CH2_OE = Channel::get(1).isOutputEnabled() ? 1 : 0;
                Channel::get(1).outputEnable(false);
            }
        }
        g_savedOE.OE_SAVED = 1;
    }
}

void Channel::restoreOE() {
    if (g_savedOE.OE_SAVED) {
        if (CH_NUM > 0) {
            Channel::get(0).outputEnable(g_savedOE.CH1_OE ? true : false);
            if (CH_NUM > 1) {
                Channel::get(1).outputEnable(g_savedOE.CH2_OE ? true : false);
            }
        }
        g_savedOE.OE_SAVED = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////

#ifdef EEZ_PLATFORM_SIMULATOR

void Channel::Simulator::setLoadEnabled(bool value) {
    load_enabled = value;
    profile::save();
}

bool Channel::Simulator::getLoadEnabled() {
    return load_enabled;
}

void Channel::Simulator::setLoad(float value) {
    load = value;
    profile::save();
}

float Channel::Simulator::getLoad() {
    return load;
}

void Channel::Simulator::setVoltProgExt(float value) {
    voltProgExt = value;
    profile::save();
}

float Channel::Simulator::getVoltProgExt() {
    return voltProgExt;
}

#endif

////////////////////////////////////////////////////////////////////////////////

Channel::Channel()
    : index(1)
    , ioexp(*this)
    , adc(*this)
    , dac(*this)
    , onTimeCounter(1)
{
}

void Channel::set(uint8_t slotIndex_, uint8_t boardRevision_, float U_MIN_, float U_DEF_, float U_MAX_,
                 float U_MAX_CONF_, float U_MIN_STEP_, float U_DEF_STEP_, float U_MAX_STEP_,
                 float U_CAL_VAL_MIN_, float U_CAL_VAL_MID_, float U_CAL_VAL_MAX_,
                 float U_CURR_CAL_, bool OVP_DEFAULT_STATE_, float OVP_MIN_DELAY_,
                 float OVP_DEFAULT_DELAY_, float OVP_MAX_DELAY_, float I_MIN_, float I_DEF_,
                 float I_MAX_, float I_MAX_CONF_, float I_MIN_STEP_, float I_DEF_STEP_,
                 float I_MAX_STEP_, float I_CAL_VAL_MIN_, float I_CAL_VAL_MID_,
                 float I_CAL_VAL_MAX_, float I_VOLT_CAL_, bool OCP_DEFAULT_STATE_,
                 float OCP_MIN_DELAY_, float OCP_DEFAULT_DELAY_, float OCP_MAX_DELAY_,
                 bool OPP_DEFAULT_STATE_, float OPP_MIN_DELAY_, float OPP_DEFAULT_DELAY_,
                 float OPP_MAX_DELAY_, float OPP_MIN_LEVEL_, float OPP_DEFAULT_LEVEL_,
                 float OPP_MAX_LEVEL_, float SOA_VIN_, float SOA_PREG_CURR_,
                 float SOA_POSTREG_PTOT_, float PTOT_)
{
	slotIndex = slotIndex_;
    boardRevision = boardRevision_;

    U_MIN = U_MIN_;
    U_DEF = U_DEF_;
    U_MAX = U_MAX_,
    U_MAX_CONF = U_MAX_CONF_;
    U_MIN_STEP = U_MIN_STEP_;
    U_DEF_STEP = U_DEF_STEP_;
    U_MAX_STEP = U_MAX_STEP_;
    U_CAL_VAL_MIN = U_CAL_VAL_MIN_;
    U_CAL_VAL_MID = U_CAL_VAL_MID_;
    U_CAL_VAL_MAX = U_CAL_VAL_MAX_;
    U_CURR_CAL = U_CURR_CAL_;
    OVP_DEFAULT_STATE = OVP_DEFAULT_STATE_;
    OVP_MIN_DELAY = OVP_MIN_DELAY_;
    OVP_DEFAULT_DELAY = OVP_DEFAULT_DELAY_;
    OVP_MAX_DELAY = OVP_MAX_DELAY_;
    I_MIN = I_MIN_;
    I_DEF = I_DEF_;
    I_MAX = I_MAX_;
    I_MAX_CONF = I_MAX_CONF_;
    I_MIN_STEP = I_MIN_STEP_;
    I_DEF_STEP = I_DEF_STEP_;
    I_MAX_STEP = I_MAX_STEP_;
    I_CAL_VAL_MIN = I_CAL_VAL_MIN_;
    I_CAL_VAL_MID = I_CAL_VAL_MID_;
    I_CAL_VAL_MAX = I_CAL_VAL_MAX_;
    I_VOLT_CAL = I_VOLT_CAL_;
    OCP_DEFAULT_STATE = OCP_DEFAULT_STATE_;
    OCP_MIN_DELAY = OCP_MIN_DELAY_;
    OCP_DEFAULT_DELAY = OCP_DEFAULT_DELAY_;
    OCP_MAX_DELAY = OCP_MAX_DELAY_;
    OPP_DEFAULT_STATE = OPP_DEFAULT_STATE_;
    OPP_MIN_DELAY = OPP_MIN_DELAY_;
    OPP_DEFAULT_DELAY = OPP_DEFAULT_DELAY_;
    OPP_MAX_DELAY = OPP_MAX_DELAY_;
    OPP_MIN_LEVEL = OPP_MIN_LEVEL_;
    OPP_DEFAULT_LEVEL = OPP_DEFAULT_LEVEL_;
    OPP_MAX_LEVEL = OPP_MAX_LEVEL_;
    SOA_VIN = SOA_VIN_;
    SOA_PREG_CURR = SOA_PREG_CURR_;
    SOA_POSTREG_PTOT = SOA_POSTREG_PTOT_;
    PTOT = PTOT_;

    if (boardRevision == CH_BOARD_REVISION_DCP405_R1B1 || boardRevision == CH_BOARD_REVISION_DCP405_R2B5) {
        VOLTAGE_GND_OFFSET = 0.86f;
        CURRENT_GND_OFFSET = 0.11f;
    } else if (boardRevision == CH_BOARD_REVISION_DCP505_R1B3) {
        VOLTAGE_GND_OFFSET = 1.05f;
        CURRENT_GND_OFFSET = 0.11f;
    } else {
        VOLTAGE_GND_OFFSET = 0;
        CURRENT_GND_OFFSET = 0;
    }

    u.min = roundChannelValue(UNIT_VOLT, U_MIN);
    u.max = roundChannelValue(UNIT_VOLT, U_MAX);
    u.def = roundChannelValue(UNIT_VOLT, U_DEF);

    i.min = roundChannelValue(UNIT_AMPER, I_MIN);
    i.max = roundChannelValue(UNIT_AMPER, I_MAX);
    i.def = roundChannelValue(UNIT_AMPER, I_DEF);

    // negligibleAdcDiffForVoltage2 = (int)((AnalogDigitalConverter::ADC_MAX -
    // AnalogDigitalConverter::ADC_MIN) / (2 * 100 * (U_MAX - U_MIN))) + 1;
    // negligibleAdcDiffForVoltage3 = (int)((AnalogDigitalConverter::ADC_MAX -
    // AnalogDigitalConverter::ADC_MIN) / (2 * 1000 * (U_MAX - U_MIN))) + 1;
    // calculateNegligibleAdcDiffForCurrent();

#ifdef EEZ_PLATFORM_SIMULATOR
    simulator.load_enabled = true;
    simulator.load = 10;
#endif

    uBeforeBalancing = NAN;
    iBeforeBalancing = NAN;

    flags.currentCurrentRange = CURRENT_RANGE_HIGH;
    flags.currentRangeSelectionMode = CURRENT_RANGE_SELECTION_ALWAYS_HIGH;
    flags.autoSelectCurrentRange = 0;

    flags.displayValue1 = DISPLAY_VALUE_VOLTAGE;
    flags.displayValue2 = DISPLAY_VALUE_CURRENT;
    ytViewRate = GUI_YT_VIEW_RATE_DEFAULT;

    autoRangeCheckLastTickCount = 0;

    flags.cvMode = 0;
    flags.ccMode = 0;
}

int Channel::reg_get_ques_isum_bit_mask_for_channel_protection_value(ProtectionValue &cpv) {
    if (IS_OVP_VALUE(this, cpv))
        return QUES_ISUM_OVP;
    if (IS_OCP_VALUE(this, cpv))
        return QUES_ISUM_OCP;
    return QUES_ISUM_OPP;
}

void Channel::protectionEnter(ProtectionValue &cpv) {
    channel_dispatcher::outputEnable(*this, false);

    cpv.flags.tripped = 1;

    int bit_mask = reg_get_ques_isum_bit_mask_for_channel_protection_value(cpv);
    setQuesBits(bit_mask, true);

    int16_t eventId = event_queue::EVENT_ERROR_CH1_OVP_TRIPPED + (index - 1);

    if (IS_OVP_VALUE(this, cpv)) {
        if (flags.rprogEnabled && channel_dispatcher::getUProtectionLevel(*this) == channel_dispatcher::getUMax(*this)) {
            g_rprogAlarm = true;
        }
        doRemoteProgrammingEnable(false);
    } else if (IS_OCP_VALUE(this, cpv)) {
        eventId += 1;
    } else {
        eventId += 2;
    }

    event_queue::pushEvent(eventId);

    onProtectionTripped();
}

void Channel::protectionCheck(ProtectionValue &cpv) {
    bool state;
    bool condition;
    float delay;

    if (IS_OVP_VALUE(this, cpv)) {
        state = flags.rprogEnabled || prot_conf.flags.u_state;
        condition = channel_dispatcher::getUMon(*this) >= channel_dispatcher::getUProtectionLevel(*this) ||
            (flags.rprogEnabled && channel_dispatcher::getUMonDac(*this) >= channel_dispatcher::getUProtectionLevel(*this));
        delay = prot_conf.u_delay;
        delay -= PROT_DELAY_CORRECTION;
    } else if (IS_OCP_VALUE(this, cpv)) {
        state = prot_conf.flags.i_state;
        condition = channel_dispatcher::getIMon(*this) >= channel_dispatcher::getISet(*this);
        delay = prot_conf.i_delay;
        delay -= PROT_DELAY_CORRECTION;
    } else {
        state = prot_conf.flags.p_state;
        condition = channel_dispatcher::getUMon(*this) * channel_dispatcher::getIMon(*this) >
                    channel_dispatcher::getPowerProtectionLevel(*this);
        delay = prot_conf.p_delay;
    }

    if (state && isOutputEnabled() && condition) {
        if (delay > 0) {
            if (cpv.flags.alarmed) {
                if (micros() - cpv.alarm_started >= delay * 1000000UL) {
                    cpv.flags.alarmed = 0;

                    // if (IS_OVP_VALUE(this, cpv)) {
                    //    DebugTrace("OVP condition: CV_MODE=%d, CC_MODE=%d, I DIFF=%d mA, I MON=%d
                    //    mA", (int)flags.cvMode, (int)flags.ccMode, (int)(fabs(i.mon_last - i.set)
                    //    * 1000), (int)(i.mon_last * 1000));
                    //}
                    // else if (IS_OCP_VALUE(this, cpv)) {
                    //    DebugTrace("OCP condition: CC_MODE=%d, CV_MODE=%d, U DIFF=%d mV",
                    //    (int)flags.ccMode, (int)flags.cvMode, (int)(fabs(u.mon_last - u.set) *
                    //    1000));
                    //}

                    protectionEnter(cpv);
                }
            } else {
                cpv.flags.alarmed = 1;
                cpv.alarm_started = micros();
            }
        } else {
            // if (IS_OVP_VALUE(this, cpv)) {
            //    DebugTrace("OVP condition: CV_MODE=%d, CC_MODE=%d, I DIFF=%d mA",
            //    (int)flags.cvMode, (int)flags.ccMode, (int)(fabs(i.mon_last - i.set) * 1000));
            //}
            // else if (IS_OCP_VALUE(this, cpv)) {
            //    DebugTrace("OCP condition: CC_MODE=%d, CV_MODE=%d, U DIFF=%d mV",
            //    (int)flags.ccMode, (int)flags.cvMode, (int)(fabs(u.mon_last - u.set) * 1000));
            //}

            protectionEnter(cpv);
        }
    } else {
        cpv.flags.alarmed = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////

void Channel::init() {
    if (!isInstalled()) {
        return;
    }
    bool last_save_enabled = profile::enableSave(false);

    ioexp.init();
    adc.init();
    dac.init();

    profile::enableSave(last_save_enabled);
}

void Channel::onPowerDown() {
    if (!isInstalled()) {
        return;
    }

    bool last_save_enabled = profile::enableSave(false);

    outputEnable(false);
    doRemoteSensingEnable(false);
    if (getFeatures() & CH_FEATURE_RPROG) {
        doRemoteProgrammingEnable(false);
    }

    clearProtection(false);

    profile::enableSave(last_save_enabled);
}

void Channel::reset() {
    if (!isInstalled()) {
        return;
    }

    flags.outputEnabled = 0;
    flags.dpOn = 0;
    flags.senseEnabled = 0;
    flags.rprogEnabled = 0;

    flags.cvMode = 0;
    flags.ccMode = 0;

    ovp.flags.tripped = 0;
    ovp.flags.alarmed = 0;

    ocp.flags.tripped = 0;
    ocp.flags.alarmed = 0;

    opp.flags.tripped = 0;
    opp.flags.alarmed = 0;

    flags.currentCurrentRange = CURRENT_RANGE_HIGH;
    flags.currentRangeSelectionMode = CURRENT_RANGE_SELECTION_ALWAYS_HIGH;
    flags.autoSelectCurrentRange = 0;

    // CAL:STAT ON if valid calibrating data for both voltage and current exists in the nonvolatile
    // memory, otherwise OFF.
    doCalibrationEnable(isCalibrationExists());

    // OUTP:PROT:CLE OFF
    // [SOUR[n]]:VOLT:PROT:TRIP? 0
    // [SOUR[n]]:CURR:PROT:TRIP? 0
    // [SOUR[n]]:POW:PROT:TRIP? 0
    clearProtection(false);

    // [SOUR[n]]:VOLT:SENS INTernal
    doRemoteSensingEnable(false);

    if (getFeatures() & CH_FEATURE_RPROG) {
        // [SOUR[n]]:VOLT:PROG INTernal
        doRemoteProgrammingEnable(false);
    }

    // [SOUR[n]]:VOLT:PROT:DEL
    // [SOUR[n]]:VOLT:PROT:STAT
    // [SOUR[n]]:CURR:PROT:DEL
    // [SOUR[n]]:CURR:PROT:STAT
    // [SOUR[n]]:POW:PROT[:LEV]
    // [SOUR[n]]:POW:PROT:DEL
    // [SOUR[n]]:POW:PROT:STAT -> set all to default
    clearProtectionConf();

    // [SOUR[n]]:CURR
    // [SOUR[n]]:CURR:STEP
    // [SOUR[n]]:VOLT
    // [SOUR[n]]:VOLT:STEP -> set all to default
    u.init(U_MIN, U_DEF_STEP, u.max);
    i.init(I_MIN, I_DEF_STEP, i.max);

    maxCurrentLimitCause = MAX_CURRENT_LIMIT_CAUSE_NONE;
    p_limit = roundChannelValue(UNIT_WATT, PTOT);

    resetHistory();

    flags.displayValue1 = DISPLAY_VALUE_VOLTAGE;
    flags.displayValue2 = DISPLAY_VALUE_CURRENT;
    ytViewRate = GUI_YT_VIEW_RATE_DEFAULT;

    flags.voltageTriggerMode = TRIGGER_MODE_FIXED;
    flags.currentTriggerMode = TRIGGER_MODE_FIXED;
    flags.triggerOutputState = 1;
    flags.triggerOnListStop = TRIGGER_ON_LIST_STOP_OUTPUT_OFF;
    trigger::setVoltage(*this, U_MIN);
    trigger::setCurrent(*this, I_MIN);
    list::resetChannelList(*this);

#ifdef EEZ_PLATFORM_SIMULATOR
    simulator.setLoadEnabled(false);
    simulator.load = 10;
#endif
}

void Channel::resetHistory() {
    uHistory[0] = u.mon_last;
    iHistory[0] = i.mon_last;

    for (int i = 1; i < CHANNEL_HISTORY_SIZE; ++i) {
        uHistory[i] = 0;
        iHistory[i] = 0;
    }

    historyPosition = 0;
    historyLastTick = micros();
}

void Channel::clearCalibrationConf() {
    cal_conf.flags.u_cal_params_exists = 0;
    cal_conf.flags.i_cal_params_exists_range_high = 0;
    cal_conf.flags.i_cal_params_exists_range_low = 0;

    cal_conf.u.min.dac = cal_conf.u.min.val = cal_conf.u.min.adc = U_CAL_VAL_MIN;
    cal_conf.u.mid.dac = cal_conf.u.mid.val = cal_conf.u.mid.adc =
        (U_CAL_VAL_MIN + U_CAL_VAL_MAX) / 2;
    cal_conf.u.max.dac = cal_conf.u.max.val = cal_conf.u.max.adc = U_CAL_VAL_MAX;
    cal_conf.u.minPossible = U_MIN;
    cal_conf.u.maxPossible = U_MAX;

    cal_conf.i[0].min.dac = cal_conf.i[0].min.val = cal_conf.i[0].min.adc = I_CAL_VAL_MIN;
    cal_conf.i[0].mid.dac = cal_conf.i[0].mid.val = cal_conf.i[0].mid.adc =
        (I_CAL_VAL_MIN + I_CAL_VAL_MAX) / 2;
    cal_conf.i[0].max.dac = cal_conf.i[0].max.val = cal_conf.i[0].max.adc = I_CAL_VAL_MAX;
    cal_conf.i[0].minPossible = I_MIN;
    cal_conf.i[0].maxPossible = I_MAX;

    cal_conf.i[1].min.dac = cal_conf.i[1].min.val = cal_conf.i[1].min.adc = I_CAL_VAL_MIN / 100;
    cal_conf.i[1].mid.dac = cal_conf.i[1].mid.val = cal_conf.i[1].mid.adc =
        (I_CAL_VAL_MIN + I_CAL_VAL_MAX) / 2 / 100;
    cal_conf.i[1].max.dac = cal_conf.i[1].max.val = cal_conf.i[1].max.adc = I_CAL_VAL_MAX / 100;
    cal_conf.i[1].minPossible = I_MIN;
    cal_conf.i[1].maxPossible = I_MAX / 100;

    strcpy(cal_conf.calibration_date, "");
    strcpy(cal_conf.calibration_remark, CALIBRATION_REMARK_INIT);
}

void Channel::clearProtectionConf() {
    prot_conf.flags.u_state = OVP_DEFAULT_STATE;
    prot_conf.flags.i_state = OCP_DEFAULT_STATE;
    prot_conf.flags.p_state = OPP_DEFAULT_STATE;

    prot_conf.u_delay = OVP_DEFAULT_DELAY;
    prot_conf.u_level = u.max;
    prot_conf.i_delay = OCP_DEFAULT_DELAY;
    prot_conf.p_delay = OPP_DEFAULT_DELAY;
    prot_conf.p_level = OPP_DEFAULT_LEVEL;

    temperature::sensors[temp_sensor::CH1 + slotIndex].prot_conf.state = OTP_CH_DEFAULT_STATE;
    temperature::sensors[temp_sensor::CH1 + slotIndex].prot_conf.level = OTP_CH_DEFAULT_LEVEL;
    temperature::sensors[temp_sensor::CH1 + slotIndex].prot_conf.delay = OTP_CH_DEFAULT_DELAY;
}

bool Channel::test() {
    if (!isInstalled()) {
        return true;
    }

    bool last_save_enabled = profile::enableSave(false);

    flags.powerOk = 0;

    outputEnable(false);
    doRemoteSensingEnable(false);
    if (getFeatures() & CH_FEATURE_RPROG) {
        doRemoteProgrammingEnable(false);
    }

    ioexp.test();
    adc.test();
    dac.test();

    profile::enableSave(last_save_enabled);
    profile::save();

    return isOk();
}

bool Channel::isInstalled() {
    return boardRevision != CH_BOARD_REVISION_NONE;
}

bool Channel::isPowerOk() {
    return flags.powerOk;
}

TestResult Channel::getTestResult() {
    if (!isInstalled()) {
        return TEST_SKIPPED;
    }
    if (ioexp.g_testResult == TEST_NONE || adc.g_testResult == TEST_NONE || dac.g_testResult == TEST_NONE) {
        return TEST_NONE;
    }
    if (ioexp.g_testResult == TEST_OK && adc.g_testResult == TEST_OK && dac.g_testResult == TEST_OK) {
        return TEST_OK;
    }
    return TEST_FAILED;
}

bool Channel::isTestFailed() {
    return ioexp.g_testResult == TEST_FAILED || adc.g_testResult == TEST_FAILED || dac.g_testResult == TEST_FAILED;
}

bool Channel::isTestOk() {
    return ioexp.g_testResult == TEST_OK && adc.g_testResult == TEST_OK && dac.g_testResult == TEST_OK;
}

bool Channel::isOk() {
    return isPowerUp() && isPowerOk() && isTestOk();
}

void Channel::voltageBalancing() {
    // DebugTrace("Channel voltage balancing: CH1_Umon=%f, CH2_Umon=%f",
    // Channel::get(0).u.mon_last, Channel::get(1).u.mon_last);
    if (isNaN(uBeforeBalancing)) {
        uBeforeBalancing = u.set;
    }
    doSetVoltage((Channel::get(0).u.mon_last + Channel::get(1).u.mon_last) / 2);
}

void Channel::currentBalancing() {
    // DebugTrace("CH%d channel current balancing: CH1_Imon=%f, CH2_Imon=%f", index,
    // Channel::get(0).i.mon_last, Channel::get(1).i.mon_last);
    if (isNaN(iBeforeBalancing)) {
        iBeforeBalancing = i.set;
    }
    doSetCurrent((Channel::get(0).i.mon_last + Channel::get(1).i.mon_last) / 2);
}

void Channel::restoreVoltageToValueBeforeBalancing() {
    if (!isNaN(uBeforeBalancing)) {
        // DebugTrace("Restore voltage to value before balancing: %f", uBeforeBalancing);
        profile::enableSave(false);
        setVoltage(uBeforeBalancing);
        profile::enableSave(true);
        uBeforeBalancing = NAN;
    }
}

void Channel::restoreCurrentToValueBeforeBalancing() {
    if (!isNaN(iBeforeBalancing)) {
        // DebugTrace("Restore current to value before balancing: %f", index, iBeforeBalancing);
        profile::enableSave(false);
        setCurrent(iBeforeBalancing);
        profile::enableSave(true);
        iBeforeBalancing = NAN;
    }
}

void Channel::tick(uint32_t tick_usec) {
    if (!isOk()) {
        return;
    }

    ioexp.tick(tick_usec);

#if !CONF_SKIP_PWRGOOD_TEST
    testPwrgood();
#endif

    if (getFeatures() & CH_FEATURE_RPOL) {
        unsigned rpol = !ioexp.testBit(IOExpander::IO_BIT_IN_RPOL);

        if (rpol != flags.rpol) {
            flags.rpol = rpol;
            setQuesBits(QUES_ISUM_RPOL, flags.rpol ? true : false);
        }

        if (rpol && isOutputEnabled()) {
            channel_dispatcher::outputEnable(*this, false);
            event_queue::pushEvent(event_queue::EVENT_ERROR_CH1_REMOTE_SENSE_REVERSE_POLARITY_DETECTED + index - 1);
            onProtectionTripped();
        }
    }

    if (!io_pins::isInhibited()) {
        setCvMode(ioexp.testBit(IOExpander::IO_BIT_IN_CV_ACTIVE));
        setCcMode(ioexp.testBit(IOExpander::IO_BIT_IN_CC_ACTIVE));
    }

    adc.tick(tick_usec);
    onTimeCounter.tick(tick_usec);

    // turn off DP after delay
    if (delayed_dp_off && (micros() - delayed_dp_off_start) >= DP_OFF_DELAY_PERIOD * 1000000L) {
        delayed_dp_off = false;
        doDpEnable(false);
    }

    /// Output power is monitored and if its go below DP_NEG_LEV
    /// that is negative value in Watts (default -1 W),
    /// and that condition lasts more then DP_NEG_DELAY seconds (default 5 s),
    /// down-programmer circuit has to be switched off.
    if (isOutputEnabled()) {
        if (u.mon_last * i.mon_last >= DP_NEG_LEV || tick_usec < dpNegMonitoringTime) {
            dpNegMonitoringTime = tick_usec;
        } else {
            if (tick_usec - dpNegMonitoringTime > DP_NEG_DELAY * 1000000UL) {
                if (flags.dpOn) {
                    DebugTrace("CH%d, neg. P, DP off: %f", index, u.mon_last * i.mon_last);
                    dpNegMonitoringTime = tick_usec;
                    generateError(SCPI_ERROR_CH1_DOWN_PROGRAMMER_SWITCHED_OFF + (index - 1));
                    doDpEnable(false);
                } else {
                    DebugTrace("CH%d, neg. P, output off: %f", index, u.mon_last * i.mon_last);
                    generateError(SCPI_ERROR_CH1_OUTPUT_FAULT_DETECTED - (index - 1));
                    channel_dispatcher::outputEnable(*this, false);
                }
            } else if (tick_usec - dpNegMonitoringTime > 500 * 1000UL) {
                if (flags.dpOn) {
                    if (channel_dispatcher::isSeries()) {
                        Channel &channel = Channel::get(index == 1 ? 1 : 0);
                        channel.voltageBalancing();
                        dpNegMonitoringTime = tick_usec;
                    } else if (channel_dispatcher::isParallel()) {
                        Channel &channel = Channel::get(index == 1 ? 1 : 0);
                        channel.currentBalancing();
                        dpNegMonitoringTime = tick_usec;
                    }
                }
            }
        }
    }

    // update history values
    uint32_t ytViewRateMicroseconds = (int)round(ytViewRate * 1000000L);
    while (tick_usec - historyLastTick >= ytViewRateMicroseconds) {
        uint32_t historyIndex = historyPosition % CHANNEL_HISTORY_SIZE;
        uHistory[historyIndex] = u.mon_last;
        iHistory[historyIndex] = i.mon_last;
        ++historyPosition;
        historyLastTick += ytViewRateMicroseconds;
    }

    doAutoSelectCurrentRange(tick_usec);
}

float Channel::remapAdcDataToVoltage(int16_t adc_data) {
    return remap((float)adc_data, (float)AnalogDigitalConverter::ADC_MIN, U_MIN,
                 (float)AnalogDigitalConverter::ADC_MAX, U_MAX_CONF);
}

float Channel::remapAdcDataToCurrent(int16_t adc_data) {
    return remap((float)adc_data, (float)AnalogDigitalConverter::ADC_MIN, I_MIN,
                 (float)AnalogDigitalConverter::ADC_MAX, getDualRangeMax());
}

int16_t Channel::remapVoltageToAdcData(float value) {
    float adc_value = remap(value, U_MIN, (float)AnalogDigitalConverter::ADC_MIN, U_MAX,
                            (float)AnalogDigitalConverter::ADC_MAX);
    return (int16_t)clamp(adc_value, (float)(-AnalogDigitalConverter::ADC_MAX - 1),
                          (float)AnalogDigitalConverter::ADC_MAX);
}

int16_t Channel::remapCurrentToAdcData(float value) {
    float adc_value = remap(value, I_MIN, (float)AnalogDigitalConverter::ADC_MIN, getDualRangeMax(),
                            (float)AnalogDigitalConverter::ADC_MAX);
    return (int16_t)clamp(adc_value, (float)(-AnalogDigitalConverter::ADC_MAX - 1),
                          (float)AnalogDigitalConverter::ADC_MAX);
}

float Channel::getValuePrecision(Unit unit, float value) const {
    if (unit == UNIT_VOLT) {
        return getVoltagePrecision();
    } else if (unit == UNIT_AMPER) {
        return getCurrentPrecision(value);
    } else if (unit == UNIT_WATT) {
        return getPowerPrecision();
    } else if (unit == UNIT_CELSIUS) {
        return 1;
    } else if (unit == UNIT_SECOND) {
        return 0.001f;
    }
    return 1;
}

float Channel::getVoltagePrecision() const {
    float precision = 0.005f; // 5 mV;

    if (calibration::isEnabled()) {
        precision /= 10;
    }

    return precision;
}

float Channel::getCurrentPrecision(float value) const {
    float precision = 0.0005f; // 0.5mA

    if (hasSupportForCurrentDualRange()) {
        if ((!isNaN(value) && value <= 0.05f && isMicroAmperAllowed()) || flags.currentCurrentRange == CURRENT_RANGE_LOW) {
            precision = 0.000005f; // 5uA
        }
    }
    
    if (calibration::isEnabled()) {
        precision /= 10;
    }

    return precision;
}

float Channel::getPowerPrecision() const {
    return 0.001f; // 1 mW;
}

bool Channel::isMicroAmperAllowed() const {
    return flags.currentRangeSelectionMode != CURRENT_RANGE_SELECTION_ALWAYS_HIGH;
}

float Channel::roundChannelValue(Unit unit, float value) const {
    if (unit == UNIT_VOLT) {
        return roundPrec(value, getVoltagePrecision());
    }
    
    if (unit == UNIT_AMPER) {
        return roundPrec(value, getCurrentPrecision(value));
    }
    
    if (unit == UNIT_WATT) {
        return roundPrec(value, getPowerPrecision());
    }
    
    return value;
}

void Channel::adcDataIsReady(int16_t data, bool startAgain) {
    uint8_t nextStartReg0 = 0;

    switch (adc.start_reg0) {

    case AnalogDigitalConverter::ADC_REG0_READ_U_MON: {
#ifdef DEBUG
        debug::g_uMon[index - 1].set(data);
#endif

        u.mon_adc = data;

        float value;
        if (isVoltageCalibrationEnabled()) {

            value = remapAdcDataToVoltage(data);

#if !defined(EEZ_PLATFORM_SIMULATOR)
            value -= VOLTAGE_GND_OFFSET;
#endif

            value = remap(value, cal_conf.u.min.adc, cal_conf.u.min.val, cal_conf.u.max.adc,
                          cal_conf.u.max.val);
        } else {
            value = remapAdcDataToVoltage(data);

#if !defined(EEZ_PLATFORM_SIMULATOR)
            value -= VOLTAGE_GND_OFFSET;
#endif
        }

        u.addMonValue(value, getVoltagePrecision());

        nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_I_MON;
    } break;

    case AnalogDigitalConverter::ADC_REG0_READ_I_MON: {
#ifdef DEBUG
        debug::g_iMon[index - 1].set(data);
#endif

        // if (abs(i.mon_adc - data) > negligibleAdcDiffForCurrent) {
        //    i.mon_adc = data;
        //}
        i.mon_adc = data;

        float value = remapAdcDataToCurrent(data) - getDualRangeGndOffset();

        if (isCurrentCalibrationEnabled()) {
            value = remap(value, cal_conf.i[flags.currentCurrentRange].min.adc,
                          cal_conf.i[flags.currentCurrentRange].min.val,
                          cal_conf.i[flags.currentCurrentRange].max.adc,
                          cal_conf.i[flags.currentCurrentRange].max.val);
        }

        i.addMonValue(value, getCurrentPrecision());

        if (isOutputEnabled()) {
            if (isRemoteProgrammingEnabled()) {
                nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_U_SET;
            } else {
                nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_U_MON;
            }
        } else {
            u.resetMonValues();
            i.resetMonValues();

            nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_U_SET;
        }
    } break;

    case AnalogDigitalConverter::ADC_REG0_READ_U_SET: {
#ifdef DEBUG
        debug::g_uMonDac[index - 1].set(data);
#endif

        float value = remapAdcDataToVoltage(data);

#if !defined(EEZ_PLATFORM_SIMULATOR)
        if (!flags.rprogEnabled) {
            value -= VOLTAGE_GND_OFFSET;
        }
#endif

        // if (isVoltageCalibrationEnabled()) {
        //    u.mon_dac = remap(value, cal_conf.u.min.adc, cal_conf.u.min.val, cal_conf.u.max.adc,
        //    cal_conf.u.max.val);
        //} else {
        //    u.mon_dac = value;
        //}

        u.addMonDacValue(value, getVoltagePrecision());

        if (isOutputEnabled() && isRemoteProgrammingEnabled()) {
            nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_U_MON;
        } else {
            nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_I_SET;
        }
    } break;

    case AnalogDigitalConverter::ADC_REG0_READ_I_SET: {
#ifdef DEBUG
        debug::g_iMonDac[index - 1].set(data);
#endif

        float value = remapAdcDataToCurrent(data) - getDualRangeGndOffset();

        // if (isCurrentCalibrationEnabled()) {
        //    i.mon_dac = remap(value,
        //        cal_conf.i[flags.currentCurrentRange].min.adc,
        //        cal_conf.i[flags.currentCurrentRange].min.val,
        //        cal_conf.i[flags.currentCurrentRange].max.adc,
        //        cal_conf.i[flags.currentCurrentRange].max.val);
        //} else {
        //    i.mon_dac = value;
        //}

        i.addMonDacValue(value, getCurrentPrecision());

        if (isOutputEnabled()) {
            nextStartReg0 = AnalogDigitalConverter::ADC_REG0_READ_U_MON;
        }
    } break;
    }

    if (startAgain) {
        adc.start(nextStartReg0);
    }
}

void Channel::setCcMode(bool cc_mode) {
    if (cc_mode != flags.ccMode) {
        flags.ccMode = cc_mode;

        setOperBits(OPER_ISUM_CC, cc_mode);
        setQuesBits(QUES_ISUM_VOLT, cc_mode);

        if (channel_dispatcher::isParallel()) {
        	Channel::get(index == 1 ? 1 : 0).restoreCurrentToValueBeforeBalancing();
        }
    }
}

void Channel::setCvMode(bool cv_mode) {
    if (cv_mode != flags.cvMode) {
        flags.cvMode = cv_mode;

        setOperBits(OPER_ISUM_CV, cv_mode);
        setQuesBits(QUES_ISUM_CURR, cv_mode);

        if (channel_dispatcher::isSeries()) {
        	Channel::get(index == 1 ? 1 : 0).restoreVoltageToValueBeforeBalancing();
        }
    }
}

void Channel::protectionCheck() {
    if (channel_dispatcher::isCoupled() && index == 2) {
        // protections of coupled channels are checked on channel 1
        return;
    }

    protectionCheck(ovp);
    protectionCheck(ocp);
    protectionCheck(opp);
}

void Channel::eventAdcData(int16_t adc_data, bool startAgain) {
    if (!isPowerUp())
        return;

    adcDataIsReady(adc_data, startAgain);
    protectionCheck();
}

void Channel::adcReadMonDac() {
    adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_SET);
    delay(ADC_TIMEOUT_MS);
    adc.tick(micros());
    delay(ADC_TIMEOUT_MS);
    adc.tick(micros());
}

void Channel::adcReadAll() {
    if (isOutputEnabled()) {
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_SET);
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
    } else {
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
        delay(ADC_TIMEOUT_MS);
        adc.tick(micros());
    }
}

void Channel::doDpEnable(bool enable) {
    // DP bit is active low
    ioexp.changeBit(IOExpander::IO_BIT_OUT_DP_ENABLE, !enable);
    setOperBits(OPER_ISUM_DP_OFF, !enable);
    flags.dpOn = enable;
    if (enable) {
        dpNegMonitoringTime = micros();
    }
}

void Channel::executeOutputEnable(bool enable) {
    ioexp.changeBit(IOExpander::IO_BIT_OUT_OUTPUT_ENABLE, enable);

    setOperBits(OPER_ISUM_OE_OFF, !enable);

#if defined(EEZ_PLATFORM_STM32)
    ioexp.changeBit(
		g_slots[slotIndex].moduleType == MODULE_TYPE_DCP405 ?
			IOExpander::DCP405_IO_BIT_OUT_OE_UNCOUPLED_LED :
			IOExpander::DCP505_IO_BIT_OUT_OE_UNCOUPLED_LED,
			enable
	);
#endif

    if (hasSupportForCurrentDualRange()) {
        doSetCurrentRange();
    }

    if (enable) {
        // enable DP
        delayed_dp_off = false;
        doDpEnable(true);

        dpNegMonitoringTime = micros();
    } else {
        setCvMode(false);
        setCcMode(false);

        if (calibration::isEnabled()) {
            calibration::stop();
        }

        // turn off DP after some delay
        delayed_dp_off = true;
        delayed_dp_off_start = micros();
    }

    restoreVoltageToValueBeforeBalancing();
    restoreCurrentToValueBeforeBalancing();

    if (enable) {
        // start ADC conversion
        adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);

        onTimeCounter.start();
    } else {
        onTimeCounter.stop();
    }
}

void Channel::doOutputEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }

    flags.outputEnabled = enable;

    if (!io_pins::isInhibited()) {
        executeOutputEnable(enable);
    }
}

void Channel::onInhibitedChanged(bool inhibited) {
    if (isOutputEnabled()) {
        if (inhibited) {
            executeOutputEnable(false);
        } else {
            executeOutputEnable(true);
        }
    }
}

void Channel::doRemoteSensingEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }
    flags.senseEnabled = enable;
    ioexp.changeBit(ioexp.IO_BIT_OUT_REMOTE_SENSE, enable);
    setOperBits(OPER_ISUM_RSENS_ON, enable);
}

void Channel::doRemoteProgrammingEnable(bool enable) {
    if (enable && !isOk()) {
        return;
    }
    flags.rprogEnabled = enable;
    if (enable) {
        setVoltageLimit(u.max);
        setVoltage(u.min);
        prot_conf.u_level = u.max;
        prot_conf.flags.u_state = true;
    }
    ioexp.changeBit(ioexp.IO_BIT_OUT_REMOTE_PROGRAMMING, enable);
    setOperBits(OPER_ISUM_RPROG_ON, enable);
}

void Channel::update() {
    if (!isOk()) {
        return;
    }

    if (index == 1) {
        doCalibrationEnable(persist_conf::devConf.flags.ch1CalEnabled && isCalibrationExists());
    } else if (index == 2) {
        doCalibrationEnable(persist_conf::devConf.flags.ch2CalEnabled && isCalibrationExists());
    }

    bool last_save_enabled = profile::enableSave(false);

    setVoltage(u.set);
    setCurrent(i.set);
    doOutputEnable(flags.outputEnabled);
    doRemoteSensingEnable(flags.senseEnabled);
    if (getFeatures() & CH_FEATURE_RPROG) {
        doRemoteProgrammingEnable(flags.rprogEnabled);
    }

    profile::enableSave(last_save_enabled);
}

void Channel::outputEnable(bool enable) {
    if (enable != flags.outputEnabled) {
        doOutputEnable(enable);
        event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_OUTPUT_ENABLED
                                       : event_queue::EVENT_INFO_CH1_OUTPUT_DISABLED) +
                               index - 1);
        profile::save();
    }
}

bool Channel::isOutputEnabled() {
    return isPowerUp() && flags.outputEnabled;
}

void Channel::doCalibrationEnable(bool enable) {
    flags._calEnabled = enable;

    if (enable) {
        u.min = roundChannelValue(UNIT_VOLT, MAX(cal_conf.u.minPossible, U_MIN));
        if (u.limit < u.min)
            u.limit = u.min;
        if (u.set < u.min)
            setVoltage(u.min);

        u.max = roundChannelValue(UNIT_VOLT, MIN(cal_conf.u.maxPossible, U_MAX));
        if (u.limit > u.max)
            u.limit = u.max;
        if (u.set > u.max)
            setVoltage(u.max);

        i.min = roundChannelValue(UNIT_AMPER, MAX(cal_conf.i[0].minPossible, I_MIN));
        if (i.min < I_MIN)
            i.min = I_MIN;
        if (i.limit < i.min)
            i.limit = i.min;
        if (i.set < i.min)
            setCurrent(i.min);

        i.max = roundChannelValue(UNIT_AMPER, MIN(cal_conf.i[0].maxPossible, I_MAX));
        if (i.limit > i.max)
            i.limit = i.max;
        if (i.set > i.max)
            setCurrent(i.max);
    } else {
        u.min = roundChannelValue(UNIT_VOLT, U_MIN);
        u.max = roundChannelValue(UNIT_VOLT, U_MAX);

        i.min = roundChannelValue(UNIT_AMPER, I_MIN);
        i.max = roundChannelValue(UNIT_AMPER, I_MAX);
    }

    u.def = roundChannelValue(UNIT_VOLT, u.min);
    i.def = roundChannelValue(UNIT_AMPER, i.min);

    if (g_isBooted) {
    	setVoltage(u.set);
    	setCurrent(i.set);
    }
}

void Channel::calibrationEnable(bool enabled) {
    if (enabled != isCalibrationEnabled()) {
        doCalibrationEnable(enabled);
        event_queue::pushEvent((enabled ? event_queue::EVENT_INFO_CH1_CALIBRATION_ENABLED
                                        : event_queue::EVENT_WARNING_CH1_CALIBRATION_DISABLED) +
                               index - 1);
        persist_conf::saveCalibrationEnabledFlag(*this, enabled);
    }
}

void Channel::calibrationEnableNoEvent(bool enabled) {
    if (enabled != isCalibrationEnabled()) {
        doCalibrationEnable(enabled);
    }
}

bool Channel::isCalibrationEnabled() {
    return flags._calEnabled;
}

bool Channel::isVoltageCalibrationEnabled() {
    return flags._calEnabled && cal_conf.flags.u_cal_params_exists;
}

bool Channel::isCurrentCalibrationEnabled() {
    return flags._calEnabled && ((flags.currentCurrentRange == CURRENT_RANGE_HIGH &&
                                  cal_conf.flags.i_cal_params_exists_range_high) ||
                                 (flags.currentCurrentRange == CURRENT_RANGE_LOW &&
                                  cal_conf.flags.i_cal_params_exists_range_low));
}

void Channel::remoteSensingEnable(bool enable) {
    if (enable != flags.senseEnabled) {
        doRemoteSensingEnable(enable);
        event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_REMOTE_SENSE_ENABLED
                                       : event_queue::EVENT_INFO_CH1_REMOTE_SENSE_DISABLED) +
                               index - 1);
        profile::save();
    }
}

bool Channel::isRemoteSensingEnabled() {
    return flags.senseEnabled;
}

void Channel::remoteProgrammingEnable(bool enable) {
    if (enable != flags.rprogEnabled) {
        doRemoteProgrammingEnable(enable);
        event_queue::pushEvent((enable ? event_queue::EVENT_INFO_CH1_REMOTE_PROG_ENABLED
                                       : event_queue::EVENT_INFO_CH1_REMOTE_PROG_DISABLED) +
                               index - 1);
        profile::save();
    }
}

bool Channel::isRemoteProgrammingEnabled() {
    return flags.rprogEnabled;
}

void Channel::doSetVoltage(float value) {
    u.set = value;
    u.mon_dac = 0;

    if (prot_conf.u_level < u.set) {
        prot_conf.u_level = u.set;
    }

    if (U_MAX != U_MAX_CONF) {
        value = remap(value, 0, 0, U_MAX_CONF, U_MAX);
    }

    if (isVoltageCalibrationEnabled()) {
        value = remap(value, cal_conf.u.min.val, cal_conf.u.min.dac, cal_conf.u.max.val,
                      cal_conf.u.max.dac);
    }

#if !defined(EEZ_PLATFORM_SIMULATOR)
    value += VOLTAGE_GND_OFFSET;
#endif

    dac.set_voltage(value);
}

void Channel::setVoltage(float value) {
    value = roundPrec(value, getVoltagePrecision());

    doSetVoltage(value);

    uBeforeBalancing = NAN;
    restoreCurrentToValueBeforeBalancing();

    profile::save();
}

void Channel::doSetCurrent(float value) {
    if (hasSupportForCurrentDualRange()) {
        if (dac.isTesting()) {
            setCurrentRange(CURRENT_RANGE_HIGH);
        } else if (!calibration::isEnabled()) {
            if (flags.currentRangeSelectionMode == CURRENT_RANGE_SELECTION_USE_BOTH) {
                setCurrentRange(value > 0.05f ? CURRENT_RANGE_HIGH : CURRENT_RANGE_LOW);
            } else if (flags.currentRangeSelectionMode == CURRENT_RANGE_SELECTION_ALWAYS_HIGH) {
                setCurrentRange(CURRENT_RANGE_HIGH);
            } else {
                setCurrentRange(CURRENT_RANGE_LOW);
            }
        }
    }

    i.set = value;
    i.mon_dac = 0;

    if (I_MAX != I_MAX_CONF) {
        value = remap(value, 0, 0, I_MAX_CONF, I_MAX);
    }

    if (isCurrentCalibrationEnabled()) {
        value = remap(value, cal_conf.i[flags.currentCurrentRange].min.val,
                      cal_conf.i[flags.currentCurrentRange].min.dac,
                      cal_conf.i[flags.currentCurrentRange].max.val,
                      cal_conf.i[flags.currentCurrentRange].max.dac);
    }

    value += getDualRangeGndOffset();

    dac.set_current(value);
}

void Channel::setCurrent(float value) {
    value = roundPrec(value, getCurrentPrecision(value));

    doSetCurrent(value);

    iBeforeBalancing = NAN;
    restoreVoltageToValueBeforeBalancing();

    profile::save();
}

bool Channel::isCalibrationExists() {
    return (flags.currentCurrentRange == CURRENT_RANGE_HIGH &&
            cal_conf.flags.i_cal_params_exists_range_high) ||
           (flags.currentCurrentRange == CURRENT_RANGE_LOW &&
            cal_conf.flags.i_cal_params_exists_range_low) ||
           cal_conf.flags.u_cal_params_exists;
}

bool Channel::isTripped() {
    return ovp.flags.tripped || ocp.flags.tripped || opp.flags.tripped ||
           temperature::isAnySensorTripped(this);
}

void Channel::clearProtection(bool clearOTP) {
    event_queue::Event lastEvent;
    event_queue::getLastErrorEvent(&lastEvent);

    ovp.flags.tripped = 0;
    ovp.flags.alarmed = 0;
    setQuesBits(QUES_ISUM_OVP, false);
    if (lastEvent.eventId == event_queue::EVENT_ERROR_CH1_OVP_TRIPPED + (index - 1)) {
        event_queue::markAsRead();
    }

    ocp.flags.tripped = 0;
    ocp.flags.alarmed = 0;
    setQuesBits(QUES_ISUM_OCP, false);
    if (lastEvent.eventId == event_queue::EVENT_ERROR_CH1_OCP_TRIPPED + (index - 1)) {
        event_queue::markAsRead();
    }

    opp.flags.tripped = 0;
    opp.flags.alarmed = 0;
    setQuesBits(QUES_ISUM_OPP, false);
    if (lastEvent.eventId == event_queue::EVENT_ERROR_CH1_OPP_TRIPPED + (index - 1)) {
        event_queue::markAsRead();
    }

    if (clearOTP) {
        temperature::clearChannelProtection(this);
    }
}

void Channel::disableProtection() {
    if (!isTripped()) {
        prot_conf.flags.u_state = 0;
        prot_conf.flags.i_state = 0;
        prot_conf.flags.p_state = 0;
        temperature::disableChannelProtection(this);
    }
}

void Channel::setQuesBits(int bit_mask, bool on) {
    reg_set_ques_isum_bit(this->index - 1, bit_mask, on);
}

void Channel::setOperBits(int bit_mask, bool on) {
    reg_set_oper_isum_bit(this->index - 1, bit_mask, on);
}

const char *Channel::getCvModeStr() {
    if (isCvMode())
        return "CV";
    else if (isCcMode())
        return "CC";
    else
        return "UR";
}

const char *Channel::getBoardName() {
    return CH_BOARD_NAMES[boardRevision];
}

const char *Channel::getRevisionName() {
    return CH_REVISION_NAMES[boardRevision];
}

const char *Channel::getBoardAndRevisionName() {
    return CH_BOARD_AND_REVISION_NAMES[boardRevision];
}

uint16_t Channel::getFeatures() {
    return CH_BOARD_REVISION_FEATURES[boardRevision];
}

float Channel::getVoltageLimit() const {
    return u.limit;
}

float Channel::getVoltageMaxLimit() const {
    return u.max;
}

void Channel::setVoltageLimit(float limit) {
    limit = roundPrec(limit, getVoltagePrecision());

    u.limit = limit;
    if (u.set > u.limit) {
        setVoltage(u.limit);
    }
    profile::save();
}

float Channel::getCurrentLimit() const {
    return i.limit;
}

void Channel::setCurrentLimit(float limit) {
    limit = roundPrec(limit, getCurrentPrecision(limit));

    if (limit > getMaxCurrentLimit()) {
        limit = getMaxCurrentLimit();
    }
    i.limit = limit;
    if (i.set > i.limit) {
        setCurrent(i.limit);
    }
    profile::save();
}

float Channel::getMaxCurrentLimit() const {
    float limit;
    if (hasSupportForCurrentDualRange() && flags.currentRangeSelectionMode == CURRENT_RANGE_SELECTION_ALWAYS_LOW) {
        limit = 0.05f;
    } else {
        limit = isMaxCurrentLimited() ? ERR_MAX_CURRENT : i.max;
    }
    return roundChannelValue(UNIT_AMPER, limit);
}

bool Channel::isMaxCurrentLimited() const {
    return getMaxCurrentLimitCause() != MAX_CURRENT_LIMIT_CAUSE_NONE;
}

MaxCurrentLimitCause Channel::getMaxCurrentLimitCause() const {
    if (psu::isMaxCurrentLimited()) {
        return psu::getMaxCurrentLimitCause();
    }
    return maxCurrentLimitCause;
}

void Channel::limitMaxCurrent(MaxCurrentLimitCause cause) {
    if (cause != maxCurrentLimitCause) {
        maxCurrentLimitCause = cause;

        if (isMaxCurrentLimited()) {
            if (isOutputEnabled() && i.mon_last > ERR_MAX_CURRENT) {
                setCurrent(0);
            }

            if (i.limit > ERR_MAX_CURRENT) {
                setCurrentLimit(ERR_MAX_CURRENT);
            }
        }
    }
}

void Channel::unlimitMaxCurrent() {
    limitMaxCurrent(MAX_CURRENT_LIMIT_CAUSE_NONE);
}

float Channel::getPowerLimit() const {
    return p_limit;
}

float Channel::getPowerMaxLimit() const {
    return PTOT;
}

void Channel::setPowerLimit(float limit) {
    limit = roundPrec(limit, getPowerPrecision());

    p_limit = limit;
    if (u.set * i.set > p_limit) {
        // setVoltage(p_limit / i.set);
        setCurrent(p_limit / u.set);
    }
    profile::save();
}

#if !CONF_SKIP_PWRGOOD_TEST
void Channel::testPwrgood() {
    if (!ioexp.testBit(IOExpander::IO_BIT_IN_PWRGOOD)) {
        DebugTrace("Ch%d PWRGOOD bit changed to 0, gpio=%d", index, (int)ioexp.gpio);
        flags.powerOk = 0;
        generateError(SCPI_ERROR_CH1_FAULT_DETECTED - (index - 1));
        powerDownBySensor();
        return;
    }
}
#endif

TriggerMode Channel::getVoltageTriggerMode() {
    return (TriggerMode)flags.voltageTriggerMode;
}

void Channel::setVoltageTriggerMode(TriggerMode mode) {
    flags.voltageTriggerMode = mode;
}

TriggerMode Channel::getCurrentTriggerMode() {
    return (TriggerMode)flags.currentTriggerMode;
}

void Channel::setCurrentTriggerMode(TriggerMode mode) {
    flags.currentTriggerMode = mode;
}

bool Channel::getTriggerOutputState() {
    return flags.triggerOutputState ? true : false;
}

void Channel::setTriggerOutputState(bool enabled) {
    flags.triggerOutputState = enabled ? 1 : 0;
}

TriggerOnListStop Channel::getTriggerOnListStop() {
    return (TriggerOnListStop)flags.triggerOnListStop;
}

void Channel::setTriggerOnListStop(TriggerOnListStop value) {
    flags.triggerOnListStop = value;
}

float Channel::getDualRangeGndOffset() {
#ifdef EEZ_PLATFORM_SIMULATOR
    return 0;
#else
    return flags.currentCurrentRange == CURRENT_RANGE_LOW ? (CURRENT_GND_OFFSET / 100)
                                                          : CURRENT_GND_OFFSET;
#endif
}

void Channel::setCurrentRangeSelectionMode(CurrentRangeSelectionMode mode) {
    flags.currentRangeSelectionMode = mode;
    profile::save();

    if (flags.currentRangeSelectionMode == CURRENT_RANGE_SELECTION_ALWAYS_LOW) {
        if (i.set > 0.05f) {
            i.set = 0.05f;
        }

        if (i.limit > 0.05f) {
            i.limit = 0.05f;
        }
    }

    setCurrent(i.set);
}

void Channel::enableAutoSelectCurrentRange(bool enable) {
    flags.autoSelectCurrentRange = enable;
    profile::save();

    if (!flags.autoSelectCurrentRange) {
        setCurrent(i.set);
    }
}

float Channel::getDualRangeMax() {
    return flags.currentCurrentRange == CURRENT_RANGE_LOW ? (I_MAX / 100) : I_MAX;
}

// void Channel::calculateNegligibleAdcDiffForCurrent() {
//    if (flags.currentCurrentRange == CURRENT_RANGE_LOW) {
//        negligibleAdcDiffForCurrent = (int)((AnalogDigitalConverter::ADC_MAX -
//        AnalogDigitalConverter::ADC_MIN) / (2 * 10000 * (I_MAX/10 - I_MIN))) + 1;
//    } else {
//        negligibleAdcDiffForCurrent = (int)((AnalogDigitalConverter::ADC_MAX -
//        AnalogDigitalConverter::ADC_MIN) / (2 * 1000 * (I_MAX - I_MIN))) + 1;
//    }
//}

void Channel::doSetCurrentRange() {
    if (boardRevision == CH_BOARD_REVISION_DCP405_R1B1) {
	    ioexp.changeBit(IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_500MA, false);
    }

	if (flags.outputEnabled) {
		if (flags.currentCurrentRange == 0) {
			// 5A
			DebugTrace("CH%d: Switched to 5A range", (int)index);
			ioexp.changeBit(IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_5A, true);
			ioexp.changeBit(boardRevision == CH_BOARD_REVISION_DCP405_R1B1 ? 
                IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_50MA :
                IOExpander::DCP405_R2B5_IO_BIT_OUT_CURRENT_RANGE_50MA, false);
			// calculateNegligibleAdcDiffForCurrent();
		} else {
			// 50mA
			DebugTrace("CH%d: Switched to 50mA range", (int)index);
			ioexp.changeBit(boardRevision == CH_BOARD_REVISION_DCP405_R1B1 ? 
                IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_50MA :
                IOExpander::DCP405_R2B5_IO_BIT_OUT_CURRENT_RANGE_50MA, true);
			ioexp.changeBit(IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_5A, false);
			// calculateNegligibleAdcDiffForCurrent();
		}
	} else {
		ioexp.changeBit(IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_5A, true);
		ioexp.changeBit(boardRevision == CH_BOARD_REVISION_DCP405_R1B1 ? 
            IOExpander::DCP405_IO_BIT_OUT_CURRENT_RANGE_50MA :
            IOExpander::DCP405_R2B5_IO_BIT_OUT_CURRENT_RANGE_50MA, false);
	}
}

void Channel::setCurrentRange(uint8_t currentCurrentRange) {
    if (hasSupportForCurrentDualRange()) {
        if (currentCurrentRange != flags.currentCurrentRange) {
            flags.currentCurrentRange = currentCurrentRange;
            doSetCurrentRange();
            if (isOutputEnabled()) {
                adc.start(AnalogDigitalConverter::ADC_REG0_READ_U_MON);
            }
        }
    }
}

void Channel::doAutoSelectCurrentRange(uint32_t tickCount) {
    if (isOutputEnabled()) {
        if (autoRangeCheckLastTickCount != 0) {
            if (tickCount - autoRangeCheckLastTickCount >
                CURRENT_AUTO_RANGE_SWITCHING_DELAY_MS * 1000L) {
                if (flags.autoSelectCurrentRange &&
                    flags.currentRangeSelectionMode == CURRENT_RANGE_SELECTION_USE_BOTH &&
                    hasSupportForCurrentDualRange() && !dac.isTesting() &&
                    !calibration::isEnabled()) {
                    if (flags.currentCurrentRange == CURRENT_RANGE_LOW) {
                        if (i.set > 0.05f && isCcMode()) {
                            doSetCurrent(i.set);
                        }
                    } else if (i.mon_measured) {
                        if (i.mon_last < 0.05f) {
                            setCurrentRange(1);
                            dac.set_current((uint16_t)65535);
                        }
                    }
                }
                autoRangeCheckLastTickCount = tickCount;
            }
        } else {
            autoRangeCheckLastTickCount = tickCount;
        }
    } else {
        autoRangeCheckLastTickCount = 0;
    }
}

} // namespace psu
} // namespace eez
