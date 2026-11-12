#pragma once

// The settings and the camera view as one string, to share a configuration:
// "key=value&key=value...", URL-safe. The web build keeps it in the page's
// URL hash; natively, --settings takes it and the settings window shows it.
// Unknown keys are ignored, and missing ones keep their current values, so
// strings from older or newer builds still apply what they can.

#include "bookmarks.h"
#include "settings.h"

#include <string>

std::string encodeShareString(const Settings& _settings, const CameraPose& _pose);

// Applies what the string sets. Returns false if nothing in it was usable.
// _pose is only changed (and _hasPose set) when the string has a view.
bool decodeShareString(const std::string& _text, Settings& _settings, CameraPose& _pose, bool& _hasPose);
