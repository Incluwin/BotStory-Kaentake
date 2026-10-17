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
//
// config.ini [config] base=<name> (e.g. "cosmic") marks a client built on a
// WZ set other than the Community Repack: the patcher then also applies
// <patch_url>/bases/<name>/manifest.txt, whose entries replace the main
// manifest's and download from <patch_url>/bases/<name>/.
bool PatcherRun();
