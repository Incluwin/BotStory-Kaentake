#pragma once

// The launcher's Features page: one checkbox per switchable module in feature_list.h, saved to config.ini
// [features] next to the launcher. Opens at every launch until its "show at launch" box is unchecked
// ([features] page_at_launch=0); "Launch Game.exe --features" or holding Shift while the launcher starts opens it
// anyway.
//
// Returns true if the launcher should go on to patch and start the game (Play), false to exit (Save, Cancel or
// the window closed).
bool FeaturesPageRun();

// True unless config.ini has page_at_launch=0.
bool FeaturesPageAtLaunch();
