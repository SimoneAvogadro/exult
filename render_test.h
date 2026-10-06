/*
 *  Copyright (C) 2026  The Exult Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef RENDER_TEST_H
#define RENDER_TEST_H 1

#include <string>

class BaseGameInfo;

// --render-test "<k=v,...>": renders a region of the map headless at scale 1
// and at the hi-res render scales and checks them against each other
// (render_test.cc). Returns the exit code: 0 pass, 1 fail, 2 bad arguments.
int Render_test(BaseGameInfo* game, const std::string& spec);

#endif    // RENDER_TEST_H
