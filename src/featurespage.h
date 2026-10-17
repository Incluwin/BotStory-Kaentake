#pragma once

// The launcher's Features page: one checkbox per switchable module in feature_list.h, saved to config.ini
// [features] next to the launcher. Opened by "Launch Game.exe --features" or by holding Shift while the launcher
// starts.
//
// Returns true if the launcher should go on to patch and start the game (Play), false to exit (Save, Cancel or
// the window closed).
bool FeaturesPageRun();
