#include <cstdlib>
#include "Hook/Hooks_NetPacket.h"
#include "Hook/Hooks_Package.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Logging/Log.h"
#include "SFPlatform/include/DirectoryWatch.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ConfigFileWatcher {
namespace {

std::atomic<bool> g_running{false};
std::thread g_watcherThread;
std::string g_configPath;
std::string g_defaultLuaDir;

constexpr uint32_t kDebounceMs = 500;

bool SameFileName(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

bool ContainsConfigChange(
    const std::vector<SFPlatform::DirectoryWatch::Change>& changes,
    std::string_view targetFileName) {
    for (const auto& change : changes) {
        if (SameFileName(change.relativePath, targetFileName)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> BuildLuaWatchDirs() {
    return LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), g_defaultLuaDir);
}

void RestartLuaWatcher() {
    std::vector<std::string> watchDirs = BuildLuaWatchDirs();

    LuaFileWatcher::Stop();
    LuaConfig::ReloadDirectories(watchDirs);
    LuaFileWatcher::Start(watchDirs);

    Hooks_Package::NotifyLicenseChanged();
    LOG_INFO("Lua directories refreshed after config reload: {}", static_cast<uint32_t>(watchDirs.size()));
}

constexpr std::string_view kProbeFileName = "manifest_probe.txt";

void ProcessProbeFile() {
    const std::filesystem::path path =
        std::filesystem::path(g_configPath).parent_path() / kProbeFileName;

    std::ifstream in(path);
    if (!in) return;

    uint32_t requested = 0, accepted = 0;
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, last - first + 1);

        uint32_t depotId = 0;
        if (std::from_chars(line.data(), line.data() + line.size(), depotId).ec != std::errc{} ||
            depotId == 0) {
            LOG_WARN("manifest_probe: ignoring unparseable line \"{}\"", line);
            continue;
        }
        ++requested;
        if (Hooks_NetPacket::ProbeManifest(depotId)) ++accepted;
    }
    LOG_INFO("manifest_probe: {} depot(s) read, {} request(s) sent", requested, accepted);
}

void ReloadConfig() {
    LOG_INFO("Reloading config: {}", g_configPath);

    const Config::LoadResult result = Config::Load(g_configPath);
    if (!result.applied) {
        LOG_WARN("Config reload skipped; keeping previous valid config");
        return;
    }

    Log::ApplyConfigLevel();

    if (result.luaPathsChanged) {
        RestartLuaWatcher();
    }
}

void WatcherThread() {
    const std::filesystem::path configPath(g_configPath);
    const std::filesystem::path dirPath = configPath.parent_path();
    const std::string targetFileName = configPath.filename().string();

    SFPlatform::DirectoryWatch::Watch watch;
    if (!watch.Open(dirPath.string(), 4096)) {
        LOG_WARN("Failed to open config watch directory: {}", dirPath.string());
        return;
    }
    if (!watch.IssueRead()) {
        return;
    }

    LOG_INFO("Watching config file: {}", g_configPath);

    SFPlatform::DirectoryWatch::Watch* watchPtr = &watch;
    std::vector<SFPlatform::DirectoryWatch::Watch*> watches{watchPtr};

    struct Touched { bool config = false; bool probe = false; };

    auto drainEvent = [&]() {
        const auto changes = watch.Drain();
        Touched touched;
        touched.config = ContainsConfigChange(changes, targetFileName);
        touched.probe  = ContainsConfigChange(changes, kProbeFileName);
        watch.IssueRead();
        return touched;
    };

    while (g_running) {
        auto waitResult = SFPlatform::DirectoryWatch::WaitAny(watches, 1000);

        if (!g_running) break;
        if (waitResult.status == SFPlatform::DirectoryWatch::WaitStatus::Timeout) continue;
        if (waitResult.status != SFPlatform::DirectoryWatch::WaitStatus::Signaled) continue;

        Touched touched = drainEvent();
        while (g_running) {
            auto debounceResult = SFPlatform::DirectoryWatch::WaitAny(watches, kDebounceMs);
            if (!g_running) break;
            if (debounceResult.status == SFPlatform::DirectoryWatch::WaitStatus::Timeout) break;
            if (debounceResult.status != SFPlatform::DirectoryWatch::WaitStatus::Signaled) break;
            const Touched more = drainEvent();
            touched.config = touched.config || more.config;
            touched.probe  = touched.probe  || more.probe;
        }

        if (touched.config) ReloadConfig();
        if (touched.probe)  ProcessProbeFile();
    }

    watch.Cancel();
    LOG_INFO("Config watcher stopped");
}

} // namespace

void Start(const std::string& configPath, const std::string& defaultLuaDir) {
    if (g_running.exchange(true)) {
        LOG_WARN("Config watcher already running");
        return;
    }

    /*
     * Join before the static std::thread is destroyed, not after.
     *
     * ~thread() on a joinable thread calls std::terminate, and the destructor
     * of a namespace-scope thread object is registered at static-init time --
     * before this function ever runs. Exit handlers run in reverse
     * registration order, so an atexit registered here runs FIRST, and Stop()
     * gets to join while the object is still alive.
     *
     * The module's own unload hook calls Stop() too, but that lives in
     * .fini_array, which glibc runs after the __cxa_atexit list has already
     * destroyed this object. That ordering is why every Steam exit ended in
     * abort with a core dump: 114 of them on the machine this was found on,
     * all after Steam had finished its own shutdown, which is why nothing
     * looked wrong.
     */
    static bool once = (std::atexit(Stop), true);
    (void)once;

    g_configPath = configPath;
    g_defaultLuaDir = defaultLuaDir;
    g_watcherThread = std::thread(WatcherThread);
}

void Stop() {
    if (!g_running) return;
    g_running = false;
    if (g_watcherThread.joinable()) {
        g_watcherThread.join();
    }
}

} // namespace ConfigFileWatcher
