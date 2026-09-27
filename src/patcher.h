#pragma once

// Check the patch server for updated client files and pull any that differ
// from the local copy. Blocks until patching finishes or the user declines.
//
// Returns true if the launcher should proceed to start MapleStory.exe,
// false if the user chose to abort or an unrecoverable error occurred.
//
// The base URL (e.g. "http://<server-ip>:8485") is read from config.ini's
// [config] patch_url key. If unset, patching is skipped and the function
// returns true (backwards compat for installs that don't use the patcher).
bool PatcherRun();
