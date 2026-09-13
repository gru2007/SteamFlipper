#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "SFPlatform/include/Thread.h"
#include "Utils/SteamMetadata/ManifestCache.h"
#include "SFPlatform/include/Dialog.h"
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    // Synchronous pre-seed budget. Most manifests are Cloudflare edge hits
    // (~100-300 ms), so a typical game finishes well under the total budget; the
    // short per-fetch timeout keeps one slow/missing manifest from stalling
    // Steam, and once the total budget is spent the remaining depots fall back to
    // a detached (async) fetch so a huge or slow install never freezes Steam.
    constexpr uint32_t kPreseedFetchTimeoutMs = 5000;
    constexpr int64_t  kPreseedBudgetMs       = 15000;

    // When a pre-seed sweep finds manifests that are not archived yet (404), the
    // depots are collected here and, after a short quiet period, surfaced in one
    // MessageBox — so a whole multi-depot / multi-DLC install produces a single
    // "these aren't ready, try again later" prompt instead of one box per depot.
    constexpr int    kMissNotifyDebounceMs = 3000;   // quiet window before showing
    constexpr size_t kMissNotifyMaxLines   = 20;     // cap the listed depots
    std::mutex                                 g_missingMutex;
    std::set<std::pair<uint32_t, uint64_t>>    g_missing;      // pending batch, depot->gid
    std::set<std::pair<uint32_t, uint64_t>>    g_notified;     // already boxed this session
    std::atomic<uint64_t>                      g_missGen{0};   // debounce generation

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    // depotId -> (appId, Steam's own manifest GID). Recorded before the
    // override pass below, so this is what Steam believes rather than what we
    // told it.
    struct DepotSeen { AppId_t appId; uint64 gid; };
    std::unordered_map<uint32, DepotSeen> g_depotsSeen;
    std::mutex g_depotsSeenMutex;

    void RecordDepots(const CUtlVector<DepotEntry>* vec) {
        if (!vec) return;
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        for (uint32 i = 0; i < vec->m_Size; ++i) {
            const DepotEntry& e = vec->m_Memory.m_pMemory[i];
            if (!e.DepotId || !e.ManifestGid) continue;
            g_depotsSeen[e.DepotId] = {e.AppId, e.ManifestGid};
        }
    }

    // Record a depot whose manifest is not archived yet (404) and, after a short
    // quiet window, show a single MessageBox listing everything collected. The
    // debounce (a generation counter) coalesces the burst of a whole install —
    // including the separate BuildDepotDependency calls for a game's DLC apps —
    // into one prompt. The box is shown on this detached waiter, never on Steam's
    // BuildDepotDependency thread, so it cannot freeze Steam.
    void ReportMissing(uint32_t depot, uint64_t gid) {
        {
            std::lock_guard<std::mutex> lock(g_missingMutex);
            // Once per depot per session: an active-but-stuck download re-requests
            // the code every ~30 s, but the user only needs to be told once.
            if (!g_notified.emplace(depot, gid).second) return;
            g_missing.emplace(depot, gid);
        }
        const uint64_t gen = ++g_missGen;

        SFPlatform::Thread::StartDetached([gen]() -> uint32_t {
            std::this_thread::sleep_for(std::chrono::milliseconds(kMissNotifyDebounceMs));
            // A newer miss arrived during the wait — let its waiter show the box.
            if (g_missGen.load() != gen) return 0;

            std::set<std::pair<uint32_t, uint64_t>> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_missingMutex);
                snapshot.swap(g_missing);
            }
            if (snapshot.empty()) return 0;

            std::string body = std::format(
                "Missing {} manifest(s) from the cache - not archived yet.\n"
                "They've been queued; try the download again in a little while.\n\n"
                "depot;manifestid\n", snapshot.size());

            size_t shown = 0;
            for (const auto& [depot, gid] : snapshot) {
                if (shown >= kMissNotifyMaxLines) {
                    body += std::format("...and {} more", snapshot.size() - shown);
                    break;
                }
                body += std::format("{};{}\n", depot, gid);
                ++shown;
            }

            SFPlatform::Dialog::ShowWarning("SteamFlipper - manifests not ready", body);
            return 0;
        });
    }

    // Synchronously ensure the archived manifest for each OST-served depot is on
    // disk before we hand the depot list back to Steam. A depot is OST-served
    // when it is pinned by a lua (an override) or lua-unlocked but not genuinely
    // owned — genuinely-owned depots get working codes from Steam and are left
    // alone, mirroring HandleSend's policy. Fetches run within a shared wall-clock
    // deadline; past it, remaining depots are fetched detached so Steam is never
    // hung. Already-cached depots cost only a stat.
    void PreseedDepots(AppId_t appId, const CUtlVector<DepotEntry>* vec,
                       const std::unordered_map<uint64_t, LuaConfig::ManifestOverride>& overrides,
                       std::chrono::steady_clock::time_point deadline)
    {
        if (!vec) return;
        for (uint32 i = 0; i < vec->m_Size; ++i) {
            const DepotEntry& e = vec->m_Memory.m_pMemory[i];
            if (!e.DepotId || !e.ManifestGid) continue;

            const bool pinned   = overrides.count(e.DepotId) != 0;
            const bool unlocked = LuaConfig::HasDepot(e.DepotId, false) &&
                                  !LuaConfig::IsOwned(e.AppId);
            if (!pinned && !unlocked) continue;   // owned/normal depot: Steam handles it

            const AppId_t app   = appId;
            const uint32  depot = e.DepotId;
            const uint64  gid   = e.ManifestGid;   // pinned gid already patched in

            if (std::chrono::steady_clock::now() < deadline) {
                // Within budget: block until the manifest is on disk (or the short
                // per-fetch timeout elapses). A miss just returns false and Steam
                // falls through to its normal request-code path. This sweep runs
                // at startup for the whole library, so it stays SILENT on 404 -
                // the "not ready" prompt is raised only on a real download attempt
                // (HandleSend), never here.
                ManifestCache::EnsureCached(app, depot, gid, kPreseedFetchTimeoutMs);
            } else {
                // Budget spent: stop blocking Steam; fetch the remainder async.
                SFPlatform::Thread::StartDetached([app, depot, gid]() -> uint32_t {
                    ManifestCache::EnsureCached(app, depot, gid);
                    return 0;
                });
            }
        }
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        if (!result) return result;

        // Before the override pass, so what is cached is Steam's GID.
        RecordDepots(pDepotInfo);
        RecordDepots(pSharedDepotInfo);

        const auto& overrides = LuaConfig::GetManifestOverrides();

        // Apply manifest overrides in place (only depots a lua explicitly pins).
        if (!overrides.empty() && pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        }

        // Pre-seed <steam>\depotcache SYNCHRONOUSLY for every depot OST is
        // responsible for, using the gid Steam just reported (or the pinned gid
        // patched above). Doing this before returning means the manifest is on
        // disk before Steam's first manifest-request-code call, so an unlocked
        // install starts on the first press instead of failing and needing a
        // retry. Bounded: each fetch has a short timeout, and once the total
        // budget is spent the rest fall back to a detached fetch so a large or
        // slow install can never hang Steam's thread. Already-cached depots cost
        // only a stat (EnsureCached's fs::exists short-circuit), so this is cheap
        // except on a genuinely new install.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kPreseedBudgetMs);
        PreseedDepots(AppId, pDepotInfo, overrides, deadline);
        PreseedDepots(AppId, pSharedDepotInfo, overrides, deadline);
        return result;
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNHOOK_END();
    }

    bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid) {
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        auto it = g_depotsSeen.find(depotId);
        if (it == g_depotsSeen.end()) return false;
        outAppId = it->second.appId;
        outGid   = it->second.gid;
        return true;
    }

    void ReportMissingManifest(uint32_t depotId, uint64_t gid) {
        ReportMissing(depotId, gid);
    }
}
