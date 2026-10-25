#pragma once
#include <windows.h>

// Points in the client's own loop that bypass.cpp owns. CWvsApp::Run is replaced there and calls its own
// CallUpdate, so a module hooking CWvsApp::CallUpdate would never run; it registers here instead, from its attach
// function. A FEATURE_LIVE module checks features::IsActive in its handler, since these calls go around its thunks.
namespace clientloop {

using FrameHandler = void (*)();
using MessageFilter = bool (*)(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* plResult);

// Every CallUpdate (each rendered frame, on every stage), before the game's 30 ms update steps; in the order added.
void OnFrame(FrameHandler pHandler);

// Each message CWndMan::TranslateMessage gets, in the order added. Return true to keep it from the game: the window
// procedure then returns *plResult at once (set it first). False lets the next filter and then the game have it.
void OnMessage(MessageFilter pFilter);

}  // namespace clientloop
