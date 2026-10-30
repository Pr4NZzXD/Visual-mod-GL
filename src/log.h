#pragma once

#include <string>

namespace vm {

// Opens visualmobile.log in the game's own folder (Android/data/<package>/files).
void logInit();
// printf-style log line: goes to logcat and to the file (flushed at once, so the last lines survive a crash).
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// The last lines of the log, kept in memory too (so the menu can show them even when the file cannot be read).
std::string logTail(int maxLines);
// Folder for the log and the settings file ("" if it could not be made).
const char* dataDir();

}  // namespace vm
