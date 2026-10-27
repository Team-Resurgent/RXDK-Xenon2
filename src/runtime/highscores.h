// The high-score table between sessions (highscores.cpp).
#pragma once

#include "runtime/m68k.h"

void highscores_load();     // after the image is in memory, before the game runs
void highscores_frame();    // every frame: saves the table once a change settles
