#include "log.h"

#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace vm {

static std::mutex gLogMutex;
static std::string gDir;
static std::string gLogPath;

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

void logf(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_INFO, "VisualMobile", "%s", msg);

    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gLogPath.empty()) return;
    FILE* f = fopen(gLogPath.c_str(), "a");
    if (!f) return;
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    fprintf(f, "%02d:%02d:%02d %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
    fclose(f);
}

}  // namespace vm
