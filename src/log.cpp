#include "log.h"

#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace vm {

static std::mutex gLogMutex;
static std::string gDir;
static std::string gLogPath;
static std::deque<std::string> gTail;

const char* dataDir() { return gDir.c_str(); }

static std::string packageName() {
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return "";
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return std::string(buf);
}

void logInit() {
    std::string pkg = packageName();
    if (!pkg.empty()) {
        std::string base = "/storage/emulated/0/Android/data/" + pkg;
        std::string dir = base + "/files";
        mkdir(base.c_str(), 0777);
        mkdir(dir.c_str(), 0777);
        if (access(dir.c_str(), W_OK) == 0) gDir = dir;
    }
    if (!gDir.empty()) {
        gLogPath = gDir + "/visualmobile.log";
        std::string prev = gDir + "/visualmobile.prev.log";
        rename(gLogPath.c_str(), prev.c_str());
    }
    logf("==== VisualMobile native starting (pid %d, package %s) ====", (int)getpid(), pkg.c_str());
    logf("data folder: %s", gDir.empty() ? "(none, logcat only)" : gDir.c_str());
}

std::string logTail(int maxLines) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    std::string out;
    int start = (int)gTail.size() - maxLines;
    if (start < 0) start = 0;
    for (size_t i = (size_t)start; i < gTail.size(); i++) {
        // keep each line short so the menu can show it on one line
        out += gTail[i].substr(0, 58);
        if (i + 1 < gTail.size()) out += "\n";
    }
    return out;
}

void logf(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_INFO, "VisualMobile", "%s", msg);

    std::lock_guard<std::mutex> lock(gLogMutex);
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    gTail.push_back(std::string(stamp) + msg);
    if (gTail.size() > 60) gTail.pop_front();
    if (gLogPath.empty()) return;
    FILE* f = fopen(gLogPath.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s%s\n", stamp, msg);
    fclose(f);
}

}  // namespace vm
