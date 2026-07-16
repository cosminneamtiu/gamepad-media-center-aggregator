/*
    GMCA — TorrentEngine implementation (see torrent/engine.hpp).
*/

#include "torrent/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "torrent/http_client.hpp"
#include "torrent/log.hpp"
#include "torrent/util.hpp"

namespace torrent {

namespace {
constexpr int kMetadataPieceSize = 16384;  // BEP-9 metadata is split in 16 KiB pieces
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

TorrentEngine::TorrentEngine(EngineConfig cfg) : cfg_(std::move(cfg)) {
    net::globalInit();
    peerId_ = makePeerId(cfg_.peerIdPrefix);
    lastRateSampleMs_ = nowMs();
}

TorrentEngine::~TorrentEngine() { close(); }

// ---- entry points -----------------------------------------------------------

std::string TorrentEngine::open(const std::string& magnetOrInfoHash, int fileIdx) {
    MagnetInfo mi;
    if (!parseMagnet(magnetOrInfoHash, mi) || !mi.hasInfoHash) {
        logError("engine: could not parse magnet/infohash: %s", magnetOrInfoHash.c_str());
        return "";
    }
    infoHash_ = mi.infoHash;
    wantFileIdx_ = fileIdx;
    logInfo("engine: opening infohash %s (%s)", infoHashHex(infoHash_).c_str(),
        mi.displayName.empty() ? "no name" : mi.displayName.c_str());
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        for (auto& t : mi.trackers) trackers_.push_back(t);
        for (auto& w : mi.webSeeds) webSeeds_.push_back(w);
    }
    meta0ReqMs_ = 0;
    running_ = true;
    engineThread_ = std::thread([this] { engineLoop(); });
    announcerThread_ = std::thread([this] { announcerLoop(); });

    // Magnet flow: the local URL only exists once metadata is in hand (we need
    // the file length). Block up to 60 s; the caller may also poll waitForMetadata.
    if (!waitForMetadata(60000)) {
        logWarn("engine: metadata not acquired within timeout");
        return "";
    }
    return localUrl();
}

std::string TorrentEngine::openTorrentFile(const std::string& rawTorrent, int fileIdx) {
    if (!parseTorrentFile(rawTorrent, meta_)) {
        logError("engine: could not parse .torrent file");
        return "";
    }
    infoHash_ = meta_.infoHash;
    wantFileIdx_ = fileIdx;
    logInfo("engine: opened .torrent %s (%s, %lld bytes, %d pieces)", infoHashHex(infoHash_).c_str(),
        meta_.name.c_str(), (long long)meta_.totalLength, meta_.numPieces());
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        for (auto& tier : meta_.trackerTiers)
            for (auto& t : tier) trackers_.push_back(t);
        for (auto& w : meta_.webSeeds) webSeeds_.push_back(w);
    }
    running_ = true;
    // Metadata is already known -> stand up the data stage before the loop runs.
    startDataStage(wantFileIdx_);
    engineThread_ = std::thread([this] { engineLoop(); });
    announcerThread_ = std::thread([this] { announcerLoop(); });
    return localUrl();
}

void TorrentEngine::addPeer(const std::string& host, uint16_t port) { enqueuePeers({{host, port}}); }

void TorrentEngine::addTracker(const std::string& url) {
    std::lock_guard<std::mutex> lk(trackerMutex_);
    trackers_.push_back(url);
}

void TorrentEngine::addWebSeed(const std::string& url) {
    std::lock_guard<std::mutex> lk(trackerMutex_);
    webSeeds_.push_back(url);
}

void TorrentEngine::setPlayhead(int64_t fileByteOffset) {
    if (store_) store_->setPlayhead(fileIdx_, fileByteOffset);
}

std::string TorrentEngine::localUrl() const { return http_ ? http_->url() : std::string(); }

// ---- data stage bring-up ----------------------------------------------------

void TorrentEngine::startDataStage(int fileIdx) {
    if (!meta_.valid()) {
        logError("engine: startDataStage with invalid metadata");
        return;
    }
    fileIdx_ = (fileIdx >= 0 && fileIdx < (int)meta_.files.size()) ? fileIdx : meta_.largestFileIndex();
    metaNumPieces_ = meta_.numPieces();
    const FileEntry* fe = meta_.file(fileIdx_);
    fileName_ = fe ? fe->path : meta_.name;
    // basename for the HTTP path
    size_t slash = fileName_.find_last_of('/');
    if (slash != std::string::npos) fileName_ = fileName_.substr(slash + 1);

    store_ = std::make_unique<PieceStore>(meta_, cfg_.ramBudgetBytes);
    {
        std::lock_guard<std::mutex> lk(pickerMutex_);
        picker_ = std::make_unique<PiecePicker>(*store_, cfg_);
    }
    store_->setPlayhead(fileIdx_, 0);

    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        for (auto& tier : meta_.trackerTiers)
            for (auto& t : tier) trackers_.push_back(t);
        for (auto& w : meta_.webSeeds) webSeeds_.push_back(w);
    }

    http_ = std::make_unique<HttpServer>(*store_, fileIdx_, fileName_);
    if (!http_->start(cfg_.httpPort)) {
        logError("engine: local HTTP server failed to bind");
    } else {
        logInfo("engine: local HTTP server at %s", http_->url().c_str());
    }

    // Size the have-bitmap of any peer connected during the metadata phase.
    for (auto& p : peers_)
        if (p && p->alive()) p->onMetadataReady(metaNumPieces_);

    bool hasWebSeed;
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        hasWebSeed = !webSeeds_.empty();
    }
    if (cfg_.enableWebSeed && hasWebSeed && meta_.files.size() == 1)
        webSeedThread_ = std::thread([this] { webSeedLoop(); });

    metadataReady_ = true;
    logInfo("engine: data stage ready — file '%s' (%lld bytes, %d pieces, pieceLen %lld)", fileName_.c_str(),
        (long long)(fe ? fe->length : 0), meta_.numPieces(), (long long)meta_.pieceLength);
}

// ---- peer discovery ---------------------------------------------------------

void TorrentEngine::enqueuePeers(const std::vector<PeerAddr>& peers) {
    std::lock_guard<std::mutex> lk(peersMutex_);
    for (const auto& a : peers) {
        std::string key = a.str();
        if (knownPeers_.count(key)) continue;
        knownPeers_.insert(key);
        pendingPeers_.push_back(a);
    }
}

void TorrentEngine::connectPending() {
    // Drop dead peers and release their in-flight requests.
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!(*it)->alive()) {
            // MSE plaintext fallback: a peer that never completed its BitTorrent
            // handshake despite an MSE attempt may simply not speak MSE — retry it
            // once in the clear so we don't shrink the reachable pool.
            if (cfg_.encryption == Encryption::Prefer && (*it)->usedMse() && !(*it)->handshaked()) {
                std::string key = (*it)->addr().str();
                if (!plaintextTried_.count(key)) {
                    plaintextTried_.insert(key);
                    plaintextRetry_.push_back((*it)->addr());
                }
            }
            std::lock_guard<std::mutex> lk(pickerMutex_);
            if (picker_) picker_->releasePeer(it->get());
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    while ((int)peers_.size() < cfg_.maxPeers) {
        PeerAddr addr;
        Encryption mode;
        if (!plaintextRetry_.empty()) {
            addr = plaintextRetry_.back();
            plaintextRetry_.pop_back();
            mode = Encryption::Plaintext;
        } else {
            std::lock_guard<std::mutex> lk(peersMutex_);
            if (pendingPeers_.empty()) break;
            addr = pendingPeers_.back();
            pendingPeers_.pop_back();
            mode = cfg_.encryption;
        }
        auto peer = std::make_unique<PeerConnection>(addr, this, mode);
        if (peer->startConnect()) {
            peers_.push_back(std::move(peer));
        }
    }
}

// ---- metadata (BEP-9) -------------------------------------------------------

void TorrentEngine::scheduleMetadataRequests() {
    // Capable peers: handshake done + advertised a ut_metadata id.
    std::vector<PeerConnection*> capable;
    for (auto& p : peers_)
        if (p->ready() && p->utMetadataId() > 0) capable.push_back(p.get());
    if (capable.empty()) return;
    int64_t now = nowMs();

    if (metaNumPieces_ == 0 && metadataSize_ == 0) {
        // Bootstrap: ask one peer for piece 0; its data reply carries total_size.
        if (now - meta0ReqMs_ > 3000) {
            capable[0]->sendMetadataRequest(0);
            meta0ReqMs_ = now;
        }
        return;
    }
    // Size known: (re)request every missing metadata piece, round-robin.
    int rr = 0;
    for (int i = 0; i < (int)metaReceived_.size(); i++) {
        if (metaReceived_[i]) continue;
        if (now - metaRequestedMs_[i] < 3000) continue;
        capable[rr % capable.size()]->sendMetadataRequest(i);
        metaRequestedMs_[i] = now;
        rr++;
    }
}

void TorrentEngine::onMetadataPiece(int piece, int64_t totalSize, const uint8_t* data, int len) {
    if (metadataReady_) return;
    if (metadataSize_ == 0 && totalSize > 0) {
        metadataSize_ = totalSize;
        int count = (int)((totalSize + kMetadataPieceSize - 1) / kMetadataPieceSize);
        metaBuf_.assign((size_t)totalSize, 0);
        metaReceived_.assign(count, 0);
        metaRequestedMs_.assign(count, 0);
        logInfo("engine: metadata size %lld bytes (%d pieces)", (long long)totalSize, count);
    }
    if (metadataSize_ == 0) return;
    int count = (int)metaReceived_.size();
    if (piece < 0 || piece >= count || metaReceived_[piece]) return;
    int64_t off = (int64_t)piece * kMetadataPieceSize;
    int64_t space = (int64_t)metaBuf_.size() - off;
    int copy = (int)std::min<int64_t>(len, space);
    if (copy <= 0) return;
    std::memcpy(metaBuf_.data() + off, data, (size_t)copy);
    metaReceived_[piece] = 1;
    metaPiecesGot_++;
    if (metaPiecesGot_ == count) onMetadataComplete();
}

void TorrentEngine::onMetadataComplete() {
    std::string raw(reinterpret_cast<char*>(metaBuf_.data()), metaBuf_.size());
    if (!parseInfoDict(raw, infoHash_, meta_)) {
        logError("engine: metadata failed verification/parse — resetting to re-fetch");
        metaPiecesGot_ = 0;
        std::fill(metaReceived_.begin(), metaReceived_.end(), (uint8_t)0);
        std::fill(metaRequestedMs_.begin(), metaRequestedMs_.end(), (int64_t)0);
        return;
    }
    logInfo("engine: metadata verified — '%s' %lld bytes, %d files", meta_.name.c_str(), (long long)meta_.totalLength,
        (int)meta_.files.size());
    startDataStage(wantFileIdx_);
}

// ---- data requests ----------------------------------------------------------

void TorrentEngine::scheduleBlockRequests() {
    std::lock_guard<std::mutex> lk(pickerMutex_);
    if (!picker_) return;
    for (auto& p : peers_) {
        if (!p->ready() || p->peerChoking() || p->peerHas().empty()) continue;
        int need = cfg_.blockPipelineDepth - p->outstanding();
        if (need <= 0) continue;
        auto reqs = picker_->pickForPeer(p->peerHas(), need, p.get());
        for (const auto& r : reqs) p->sendRequest(r.piece, r.begin, r.length);
    }
}

void TorrentEngine::onPeerHandshake(PeerConnection* peer) {
    if (metadataReady_) peer->onMetadataReady(metaNumPieces_);
}

void TorrentEngine::onPeerBlock(PeerConnection*, int piece, int32_t begin, const uint8_t* data, int len) {
    if (!store_) return;
    store_->addBlock(piece, begin, data, len);
    std::lock_guard<std::mutex> lk(pickerMutex_);
    if (picker_) picker_->gotBlock(piece, begin);
}

void TorrentEngine::onPexPeers(const std::vector<PeerAddr>& peers) {
    if (cfg_.enablePex) enqueuePeers(peers);
}

// ---- PEX (BEP-11) -----------------------------------------------------------

void TorrentEngine::schedulePex() {
    if (!cfg_.enablePex) return;
    int64_t now = nowMs();
    if (lastPexMs_ == 0) lastPexMs_ = now;  // anchor the cadence on first entry
    // Participate early (encourages reciprocation from peers that gate PEX on it),
    // then keep to the BEP-11 minimum 60 s cadence.
    int64_t interval = pexRounds_ == 0 ? 5000 : 60000;
    if (now - lastPexMs_ < interval) return;

    // The useful contribution is the set of peers we are actually connected to.
    std::vector<PeerAddr> connected;
    for (auto& p : peers_)
        if (p->ready()) connected.push_back(p->addr());
    if (connected.empty()) return;

    for (auto& p : peers_) {
        if (!p->ready() || p->utPexId() == 0) continue;
        std::vector<PeerAddr> forPeer;
        forPeer.reserve(connected.size());
        for (auto& e : connected)
            if (!(e == p->addr())) forPeer.push_back(e);  // never advertise a peer to itself
        p->sendPex(forPeer);
    }
    lastPexMs_ = now;
    pexRounds_++;
}

// ---- engine loop ------------------------------------------------------------

void TorrentEngine::engineLoop() {
    int64_t lastTick = nowMs();
    while (running_) {
        connectPending();

        std::vector<net::PollItem> items;
        std::vector<PeerConnection*> ptrs;
        items.reserve(peers_.size());
        ptrs.reserve(peers_.size());
        for (auto& p : peers_) {
            if (!p->alive()) continue;
            net::PollItem it;
            it.fd = p->handle();
            it.wantRead = true;
            it.wantWrite = p->wantWrite();
            items.push_back(it);
            ptrs.push_back(p.get());
        }

        net::poll(items, 200);
        for (size_t i = 0; i < items.size(); i++) {
            PeerConnection* p = ptrs[i];
            if (!p->alive()) continue;
            if (items[i].error) {
                p->close();
                continue;
            }
            if (items[i].writable) p->onWritable();
            if (p->alive() && items[i].readable) p->onReadable();
        }

        int64_t now = nowMs();
        for (auto& p : peers_) p->onTick(now);

        if (!metadataReady_) {
            scheduleMetadataRequests();
        } else {
            scheduleBlockRequests();
            if (now - lastTick > 1000) {
                std::lock_guard<std::mutex> lk(pickerMutex_);
                if (picker_) picker_->tick();
                lastTick = now;
            }
        }
        schedulePex();
        updateStats();
    }
}

// ---- announcer --------------------------------------------------------------

void TorrentEngine::announcerLoop() {
    bool firstRound = true;
    while (running_) {
        std::vector<std::string> trackers;
        {
            std::lock_guard<std::mutex> lk(trackerMutex_);
            trackers = trackers_;
        }
        AnnounceContext ctx;
        ctx.infoHash = infoHash_;
        ctx.peerId = peerId_;
        ctx.port = cfg_.httpPort ? cfg_.httpPort : 6881;
        ctx.downloaded = store_ ? store_->bytesHave() : 0;
        ctx.left = meta_.valid() ? std::max<int64_t>(0, meta_.totalLength - ctx.downloaded) : 16384;
        ctx.event = firstRound ? 2 : 0;
        ctx.numWant = cfg_.maxPeers * 2;

        int minInterval = 1800;
        for (const auto& t : trackers) {
            if (!running_) break;
            AnnounceResult r = announce(t, ctx, 12000);
            if (r.ok) {
                logInfo("announce %s -> %d peers (interval %ds)", t.c_str(), (int)r.peers.size(), r.intervalSec);
                enqueuePeers(r.peers);
                minInterval = std::min(minInterval, std::max(60, r.intervalSec));
            } else {
                logWarn("announce %s failed: %s", t.c_str(), r.error.c_str());
            }
        }
        firstRound = false;

        // Re-announce cadence, capped for the PoC. Wake early if torn down.
        int waitS = std::min(minInterval, 120);
        for (int i = 0; i < waitS * 5 && running_; i++) sleepMs(200);
    }
}

// ---- web seed (BEP-19) ------------------------------------------------------

void TorrentEngine::webSeedLoop() {
    std::vector<std::string> seeds;
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        seeds = webSeeds_;
    }
    // Single-file torrents only (multi-file web-seed path mapping = TODO).
    while (running_ && store_) {
        int piece = -1;
        {
            std::lock_guard<std::mutex> lk(pickerMutex_);
            if (picker_) piece = picker_->pickPieceForWebSeed(webSeedTag_);
        }
        if (piece < 0) {
            sleepMs(200);
            continue;
        }
        int64_t begin = (int64_t)piece * meta_.pieceLength;
        int64_t plen = store_->pieceLength(piece);
        int64_t last = begin + plen - 1;

        bool got = false;
        for (const auto& base : seeds) {
            if (!running_) break;
            std::string url = base;
            if (!url.empty() && url.back() == '/') url += fileName_;  // BEP-19 dir form
            http::Response resp = http::getRange(url, begin, last, 30000);
            if (!resp.ok()) {
                if (resp.status != 0)
                    logWarn("webseed %s piece %d: HTTP %ld", url.c_str(), piece, resp.status);
                else
                    logWarn("webseed %s piece %d: %s", url.c_str(), piece, resp.error.c_str());
                continue;
            }
            const std::string& body = resp.body;
            if ((int64_t)body.size() == plen) {
                // Range honored (206) — commit just the requested piece.
                if (store_->commitPiece(piece, reinterpret_cast<const uint8_t*>(body.data()), plen) ==
                    PieceStore::AddResult::Complete)
                    got = true;
                if (got) break;
            } else if ((int64_t)body.size() == meta_.totalLength) {
                // BEP-19 lets a server ignore Range and return the whole file. For a
                // single-file torrent that lets us commit EVERY missing piece from
                // one body — the optimal path for a non-Range web seed.
                int committed = 0;
                for (int p = 0; p < meta_.numPieces(); p++) {
                    if (store_->have(p)) continue;
                    int64_t poff = (int64_t)p * meta_.pieceLength;
                    int64_t pl = store_->pieceLength(p);
                    if (store_->commitPiece(p, reinterpret_cast<const uint8_t*>(body.data()) + poff, pl) ==
                        PieceStore::AddResult::Complete)
                        committed++;
                }
                logInfo("webseed %s ignored Range; committed %d pieces from the full file", url.c_str(), committed);
                got = committed > 0;
                if (got) break;
            } else {
                logWarn("webseed %s piece %d: unexpected body %zu bytes (want %lld or whole file %lld)", url.c_str(),
                    piece, body.size(), (long long)plen, (long long)meta_.totalLength);
            }
        }
        // Release the piece claim regardless (success => store now has it).
        std::lock_guard<std::mutex> lk(pickerMutex_);
        if (picker_) picker_->releasePeer(webSeedTag_);
        if (!got) sleepMs(300);  // back off on failure
    }
}

// ---- stats / waits ----------------------------------------------------------

void TorrentEngine::updateStats() {
    int64_t now = nowMs();
    Stats s;
    for (auto& p : peers_)
        if (p->ready()) s.peersConnected++;
    {
        std::lock_guard<std::mutex> lk(peersMutex_);
        s.peersKnown = (int)knownPeers_.size();
    }
    s.metadataReady = metadataReady_;
    if (store_) {
        s.downloadedBytes = store_->bytesHave();
        s.piecesTotal = store_->numPieces();
        s.piecesHave = store_->piecesHave();
        s.contiguousReadyBytes = store_->contiguousBytesFromHead(fileIdx_);
    }
    {
        std::lock_guard<std::mutex> lk(trackerMutex_);
        s.webSeeds = (int)webSeeds_.size();
    }
    int64_t dt = now - lastRateSampleMs_;
    if (dt >= 1000) {
        s.downloadRateBps = (double)(s.downloadedBytes - lastRateBytes_) * 1000.0 / (double)dt;
        lastRateSampleMs_ = now;
        lastRateBytes_ = s.downloadedBytes;
    } else {
        std::lock_guard<std::mutex> lk(statsMutex_);
        s.downloadRateBps = stats_.downloadRateBps;
    }
    std::lock_guard<std::mutex> lk(statsMutex_);
    stats_ = s;
}

Stats TorrentEngine::stats() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

bool TorrentEngine::waitForMetadata(int timeoutMs) {
    int64_t deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (metadataReady_) return true;
        if (!running_) return false;
        sleepMs(50);
    }
    return metadataReady_;
}

bool TorrentEngine::waitForContiguous(int64_t bytes, int timeoutMs) {
    int64_t deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (store_ && store_->contiguousBytesFromHead(fileIdx_) >= bytes) return true;
        if (!running_) return false;
        sleepMs(100);
    }
    return store_ && store_->contiguousBytesFromHead(fileIdx_) >= bytes;
}

// ---- teardown ---------------------------------------------------------------

void TorrentEngine::close() {
    if (!running_.exchange(false)) {
        // Not started or already closed; still join any spawned threads.
    }
    if (store_) store_->stop();  // unblock the HTTP server's readFile
    if (engineThread_.joinable()) engineThread_.join();
    if (announcerThread_.joinable()) announcerThread_.join();
    if (webSeedThread_.joinable()) webSeedThread_.join();
    if (http_) {
        http_->stop();
        http_.reset();
    }
    peers_.clear();
    {
        std::lock_guard<std::mutex> lk(pickerMutex_);
        picker_.reset();
    }
    store_.reset();
}

}  // namespace torrent
