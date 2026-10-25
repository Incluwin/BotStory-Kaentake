#pragma once
#include <windows.h>

// Points in the client's own loop that bypass.cpp owns. CWvsApp::Run is replaced there and calls its own
// CallUpdate, so a module hooking CWvsApp::CallUpdate would never run; it registers here instead, from its attach
// function. A FEATURE_LIVE module checks features::IsActive in its handler, since these calls go around its thunks.
namespace clientloop {

using FrameHandler = void (*)();
using MessageFilter = bool (*)(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* plResult);
using DropFilter = bool (*)(void* pDraggable, void* pFrom, void* pTo, int rx, int ry, int* pnResult);

// Every CallUpdate (each rendered frame, on every stage), before the game's 30 ms update steps; in the order added.
void OnFrame(FrameHandler pHandler);

// Each message CWndMan::TranslateMessage gets, in the order added. Return true to keep it from the game: the window
// procedure then returns *plResult at once (set it first). False lets the next filter and then the game have it.
void OnMessage(MessageFilter pFilter);

// CWvsContext::OnLeaveGame (back to the login screen), before the game's own: a module closes its windows and forgets
// the connection's state. In the order added.
void OnLeaveGame(FrameHandler pHandler);

// CWvsContext::ClearFieldUI (the Cash Shop or the MTS replaces the field), before the game's own.
void OnClearFieldUI(FrameHandler pHandler);

// CDraggableItem::OnDropped: an item drag ended over pTo (both IUIMsgHandler pointers, pTo null over no window). Return
// true to take the drop: the game never sees it and *pnResult is its result. False lets the next filter and then the
// game have it.
void OnItemDropped(DropFilter pFilter);

}  // namespace clientloop
