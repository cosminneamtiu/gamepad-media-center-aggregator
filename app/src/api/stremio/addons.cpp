/*
    GMCA — Stremio addon engine implementation (see stremio/addons.hpp).
    Re-syncs the account's addon collection, then loads the configured
    transportUrls' manifests once and routes resource queries across them.
*/

#include "api/stremio/addons.hpp"
#include "api/stremio/auth.hpp"
#include "utils/config.hpp"
#include <borealis/core/logger.hpp>
#include <algorithm>

namespace stremio {

void AddonEngine::ensureLoaded() {
    std::lock_guard<std::mutex> lock(mtx);
    if (loaded) return;

    // Re-sync the account's addon collection before loading manifests. Stremio
    // (re)configures an addon by REPLACING its transportUrl (e.g. Torrentio's
    // debrid apikey lives in the URL path) and the official clients pull the
    // collection at every startup — a list snapshotted at login goes stale and
    // keeps serving raw-torrent streams instead of debrid links (GH #46). On
    // failure (offline, expired key) keep the stored list; an empty collection
    // is ignored too (a live account always has the default addons) rather
    // than wiping a working list.
    const std::string& authKey = AppConfig::instance().getToken();
    if (!authKey.empty()) {
        try {
            std::vector<std::string> fresh = fetchAddonCollection(authKey);
            if (!fresh.empty()) AppConfig::instance().setStremioAddons(fresh);
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio: addon collection sync failed: {}", ex.what());
        }
    }

    // AppConfig::instance().getStremioAddons() returns the configured list of
    // transportUrls (each ending in /manifest.json). Provided by the config layer.
    const std::vector<std::string>& transports = AppConfig::instance().getStremioAddons();
    addons.clear();
    addons.reserve(transports.size());
    for (const auto& transport : transports) {
        try {
            nlohmann::json j = getSync(transport);
            if (j.empty()) {
                brls::Logger::warning("stremio: empty manifest from {}", transport);
                continue;
            }
            Addon a;
            a.transportUrl = transport;
            a.base = baseFromTransport(transport);
            a.manifest = parseManifest(j);
            addons.push_back(std::move(a));
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio: manifest load failed {}: {}", transport, ex.what());
        }
    }
    loaded = true;
}

void AddonEngine::invalidate() {
    std::lock_guard<std::mutex> lock(mtx);
    loaded = false;
}

std::vector<Addon> AddonEngine::addonsFor(
    const std::string& resource, const std::string& type, const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<Addon> out;
    for (const auto& a : addons)
        if (a.supports(resource, type, id)) out.push_back(a);
    return out;
}

bool AddonEngine::hasResource(const std::string& resource) const {
    std::lock_guard<std::mutex> lock(mtx);
    if (!loaded) return true;  // unknown yet -> assume yes; never hint prematurely
    for (const auto& a : addons)
        if (a.manifest.resources.count(resource)) return true;
    return false;
}

std::vector<std::pair<Addon, Catalog>> AddonEngine::allCatalogs() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::pair<Addon, Catalog>> out;
    for (const auto& a : addons) {
        if (a.manifest.resources.count("catalog") == 0) continue;
        for (const auto& c : a.manifest.catalogs) out.emplace_back(a, c);
    }
    return out;
}

std::vector<std::pair<Addon, Catalog>> AddonEngine::catalogsForType(const std::string& stremioType) {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::pair<Addon, Catalog>> out;
    for (const auto& a : addons) {
        if (a.manifest.resources.count("catalog") == 0) continue;
        for (const auto& c : a.manifest.catalogs)
            if (c.browsable && c.type == stremioType) out.emplace_back(a, c);
    }
    return out;
}

std::vector<std::string> AddonEngine::browsableTypes() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> out;
    for (const auto& a : addons) {
        if (a.manifest.resources.count("catalog") == 0) continue;
        for (const auto& c : a.manifest.catalogs) {
            if (!c.browsable) continue;
            if (std::find(out.begin(), out.end(), c.type) == out.end()) out.push_back(c.type);
        }
    }
    return out;
}

std::string AddonEngine::resourceUrl(const Addon& addon, const std::string& resource, const std::string& type,
    const std::string& id, const std::vector<std::pair<std::string, std::string>>& extra) const {
    std::string url = addon.base + "/" + resource + "/" + type + "/" + encodeURIComponent(id);
    if (!extra.empty()) {
        std::string joined;
        for (size_t i = 0; i < extra.size(); ++i) {
            if (i) joined += "&";
            joined += extra[i].first + "=" + encodeURIComponent(extra[i].second);
        }
        url += "/" + joined;
    }
    url += ".json";
    return url;
}

}  // namespace stremio
