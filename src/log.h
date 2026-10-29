#pragma once

namespace vm {

// Opens visualmobile.log in the game's own folder (Android/data/<package>/files).
void logInit();
// printf-style log line: goes to logcat and to the file (flushed at once, so the last lines survive a crash).
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Folder for the log and the settings file ("" if it could not be made).
const char* dataDir();

}  // namespace vm
