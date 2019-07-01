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

#include <stdio.h>
#include <string.h>

#include <eez/apps/psu/simulator/serial.h>

UARTClass Serial;
UARTClass SerialUSB;

void UARTClass::begin(unsigned long baud, UARTModes config) {
}

void UARTClass::end() {
}

int UARTClass::write(const char *buffer, int size) {
    return fwrite(buffer, 1, size, stdout);
}

int UARTClass::print(const char *data) {
    return write(data, strlen(data));
}

int UARTClass::println(const char *data) {
    return printf("%s\n", data);
}

int UARTClass::print(int value) {
    return printf("%d", value);
}

int UARTClass::println(int value) {
    return printf("%d\n", value);
}

int UARTClass::print(float value, int numDigits) {
    // TODO numDigits
    return printf("%f", value);
}

int UARTClass::println(float value, int numDigits) {
    // TODO numDigits
    return printf("%f\n", value);
}

int UARTClass::available(void) {
    return input.size();
}

int UARTClass::read(void) {
    int ch = input.front();
    input.pop();
    return ch;
}

void UARTClass::put(int ch) {
    input.push(ch);
}

void UARTClass::flush() {
}
