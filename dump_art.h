/*
 *  dump_art.h - The engine's reference set for hi-res art production
 *  (--dump-art).
 *
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

#ifndef DUMP_ART_H
#define DUMP_ART_H 1

#include <string>

class BaseGameInfo;

// --dump-art <dir>: writes the flats, the terrains and their tables, the way
// the engine sees them, into dir for the hi-res art tools (dump_art.cc).
// Returns the exit code: 0 done, 1 failed, 2 bad arguments.
int Dump_art(BaseGameInfo* game, const std::string& dir);

#endif    // DUMP_ART_H
