/*
 * EEZ Generic Firmware
 * Copyright (C) 2018-present, Envox d.o.o.
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

#include <eez/gui/widget.h>

namespace eez {
namespace gui {

#define SCALE_NEEDLE_POSITION_LEFT 1
#define SCALE_NEEDLE_POSITION_RIGHT 2
#define SCALE_NEEDLE_POSITION_TOP 3
#define SCALE_NEEDLE_POSITION_BOTTOM 4

struct ScaleWidget {
    uint8_t needle_position; // SCALE_NEEDLE_POSITION_...
    uint8_t needle_width;
    uint8_t needle_height;
};

void ScaleWidget_draw(const WidgetCursor &widgetCursor);

} // namespace gui
} // namespace eez