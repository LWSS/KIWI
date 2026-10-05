#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Probe verbs of the test modes (-kiwitest scripts and the -kiwiremote suite,
// tools\kiwitest\kiwiremote.py): geometry dumps, validity, camera gestures and state,
// picking / projection, viewport screenshots, and driving modal tools the way the viewport
// does (cursor, press, lollipop, keys through the funnel then the hotkey table, panel
// options).  Output is [DATA] lines in the test log / the remote reply.
//
// Returns false when words[0] is not a probe verb; otherwise `error` is empty on success.
#include <string>
#include <vector>

bool KiwiTestProbe_Execute( const std::vector<std::string> &words, std::string &error );
