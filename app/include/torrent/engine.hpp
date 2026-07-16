/*
    GMCA — TorrentEngine: in-process, streaming-first, ephemeral.

    One instance per playback (TORRENT_STREAMING.md §1/§6). It orchestrates:
      - metadata acquisition (BEP-9 ut_metadata from a magnet, or a .torrent);
      - peer discovery (trackers BEP-3/12/15 + PEX BEP-11 + manual/injected peers);
      - the non-blocking peer event loop (BEP-3 wire protocol);
      - the sequential+deadline picker feeding an in-RAM piece store;
      - a WebSeed (BEP-19) worker as a cold-swarm HTTP fallback;
      - the local HTTP server that mpv opens.

    Public surface intentionally matches the esquisse in TORRENT_STREAMING.md §6:
    open() -> local URL, setPlayhead(), stats(), close(). The Stremio integration
    (future) calls open() from resolvePlayback and hands the URL to MPVCore::setUrl
    (app/src/api/stremio/backend.cpp:896, app/src/activity/player_view.cpp:323).

    Threads: engine loop (peers), announcer (trackers), web-seed worker, and the
    HTTP server's own thread(s). close() tears everything down — the buffer is RAM
    and discarded, so sleep/exit cannot corrupt on-disk state.
*/

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "torrent/http_server.hpp"
#include "torrent/metadata.hpp"
#include "torrent/peer.hpp"
#include "torrent/piece_picker.hpp"
#include "torrent/storage.hpp"
#include "torrent/tracker.hpp"
#include "torrent/types.hpp"

namespace torrent {

class TorrentEngine : public PeerHost {
public:
    explicit TorrentEngine(EngineConfig cfg = {});
    ~TorrentEngine() override;

    /// Start from a magnet URI or a bare infohash (hex-40/base32-32). Metadata is
    /// fetched over BEP-9. `fileIdx` selects the video file (-1 = largest).
    /// Returns the local HTTP URL once the server is up, or "" on failure.
    std::string open(const std::string& magnetOrInfoHash, int fileIdx = -1);

    /// Start from the raw bytes of a .torrent file (metadata already in hand).
    std::string openTorrentFile(const std::string& rawTorrent, int fileIdx = -1);

    /// Inject a peer directly (deterministic local test / direct add, no tracker).
    void addPeer(const std::string& host, uint16_t port);
    void addTracker(const std::string& url);
    void addWebSeed(const std::string& url);

    /// Move the playhead (byte offset within the pinned file) — biases the picker.
    void setPlayhead(int64_t fileByteOffset);

    Stats stats() const;
    std::string localUrl() const;
    bool metadataReady() const override { return metadataReady_.load(); }
    void close();

    // --- convenience waits for the PoC / tests ---
    bool waitForMetadata(int timeoutMs);
    bool waitForContiguous(int64_t bytes, int timeoutMs);

    // --- PeerHost ---
    const InfoHash& hostInfoHash() const override { return infoHash_; }
    const std::string& hostPeerId() const override { return peerId_; }
    int metadataNumPieces() const override { return metaNumPieces_; }
    int64_t metadataTotalSize() const override { return metadataSize_; }
    void onPeerHandshake(PeerConnection*) override;
    void onPeerBlock(PeerConnection*, int piece, int32_t begin, const uint8_t* data, int len) override;
    void onMetadataPiece(int piece, int64_t totalSize, const uint8_t* data, int len) override;
    void onPexPeers(const std::vector<PeerAddr>&) override;

private:
    void engineLoop();
    void announcerLoop();
    void webSeedLoop();

    void enqueuePeers(const std::vector<PeerAddr>&);
    void connectPending();
    void scheduleMetadataRequests();
    void scheduleBlockRequests();
    void schedulePex();
    void onMetadataComplete();
    void startDataStage(int fileIdx);
    void updateStats();

    EngineConfig cfg_;
    InfoHash infoHash_{};
    std::string peerId_;
    int wantFileIdx_ = -1;

    std::atomic<bool> running_{false};
    std::atomic<bool> metadataReady_{false};

    // metadata acquisition (magnet flow)
    TorrentMetadata meta_;
    int metaNumPieces_ = 0;
    int64_t metadataSize_ = 0;
    std::vector<uint8_t> metaBuf_;
    std::vector<uint8_t> metaReceived_;  // 1 byte per 16 KiB metadata piece
    std::vector<int64_t> metaRequestedMs_;
    int metaPiecesGot_ = 0;
    int64_t meta0ReqMs_ = 0;  // last "request metadata piece 0" time (bootstrap)

    // data stage
    std::unique_ptr<PieceStore> store_;
    std::unique_ptr<PiecePicker> picker_;
    std::unique_ptr<HttpServer> http_;
    int fileIdx_ = -1;
    std::string fileName_;
    mutable std::mutex pickerMutex_;

    // peers
    std::vector<std::unique_ptr<PeerConnection>> peers_;
    std::mutex peersMutex_;
    std::vector<PeerAddr> pendingPeers_;
    std::unordered_set<std::string> knownPeers_;
    // MSE plaintext fallback (Prefer policy): a peer that failed the MSE handshake
    // before its BitTorrent handshake gets one clear-text reconnect. Engine-thread
    // only (touched from connectPending), so no lock needed.
    std::vector<PeerAddr> plaintextRetry_;
    std::unordered_set<std::string> plaintextTried_;
    // BEP-11 outgoing PEX cadence.
    int64_t lastPexMs_ = 0;
    int pexRounds_ = 0;

    // trackers / web seeds
    std::mutex trackerMutex_;
    std::vector<std::string> trackers_;
    std::vector<std::string> webSeeds_;

    // threads
    std::thread engineThread_;
    std::thread announcerThread_;
    std::thread webSeedThread_;

    // stats
    mutable std::mutex statsMutex_;
    Stats stats_;
    int64_t lastRateSampleMs_ = 0;
    int64_t lastRateBytes_ = 0;

    // web seed dedupe tag for the picker
    const void* webSeedTag_ = reinterpret_cast<const void*>(0x1);
};

}  // namespace torrent
