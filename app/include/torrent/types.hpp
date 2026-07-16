/*
    GMCA — Shared value types for the torrent engine.

    InfoHash is the 20-byte SHA-1 of the info dictionary — the torrent's identity
    and the key exchanged in the peer handshake. PeerAddr is an IPv4/host + port
    peer endpoint. EngineConfig and Stats are the knobs and the read-only status
    surfaced to the UI (peers / speed / buffered %).
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "torrent/sha1.hpp"

namespace torrent {

/// 20-byte SHA-1 of the bencoded info dict (BEP-3). Same shape as a SHA-1 digest.
using InfoHash = Sha1Digest;

/// A peer endpoint discovered from a tracker/PEX (or injected manually for the
/// deterministic local test). `host` is a dotted IPv4 or a hostname; the socket
/// shim resolves it.
struct PeerAddr {
    std::string host;
    uint16_t port = 0;

    bool operator==(const PeerAddr& o) const { return host == o.host && port == o.port; }
    std::string str() const { return host + ":" + std::to_string(port); }
};

/// Outgoing peer-connection encryption policy (MSE/PE, Vuze Message Stream
/// Encryption). We are always the connecting side.
///   Plaintext — legacy BEP-3 handshake in the clear (no MSE).
///   Prefer    — try MSE (advertise plaintext+RC4); on a peer that cannot speak
///               MSE at all, fall back to a one-shot plaintext reconnect.
///   Forced    — MSE only, advertise RC4 only (never send/accept plaintext).
enum class Encryption { Plaintext, Prefer, Forced };

/// Engine tuning. Defaults target desktop; the console ports dial these down
/// (fewer peers, smaller RAM window) — see TORRENT_ENGINE_SPEC.md.
struct EngineConfig {
    uint16_t httpPort = 0;                       // 0 = pick an ephemeral port for the local HTTP server
    int maxPeers = 40;                           // cap concurrent peer connections (console: 8-16)
    int blockPipelineDepth = 16;                 // outstanding block requests per peer
    int readAheadPieces = 24;                    // aggressive-download window ahead of the playhead
    int64_t ramBudgetBytes = 128 * 1024 * 1024;  // sliding buffer ceiling (console: 32-64 MiB)
    bool enableWebSeed = true;                   // BEP-19 url-list fallback (cold-swarm robustness)
    bool enableTrackers = true;                  // BEP-3/12/15 announces
    bool enablePex = true;                       // BEP-11 peer exchange
    // MSE/PE: try encryption by default so the engine reaches the (majority of)
    // peers/ISPs that require or prefer it — the plaintext-only handshake was
    // reaching too few real peers (see TORRENT_STREAMING.md hardening notes).
    Encryption encryption = Encryption::Prefer;
    int peerConnectTimeoutMs = 8000;
    std::string peerIdPrefix = "-GM0001-";  // Azureus-style client id prefix (8 chars)
};

/// Read-only snapshot for the UI / PoC logs.
struct Stats {
    int peersConnected = 0;
    int peersKnown = 0;
    int64_t downloadedBytes = 0;
    double downloadRateBps = 0.0;
    int piecesTotal = 0;
    int piecesHave = 0;
    bool metadataReady = false;
    int64_t contiguousReadyBytes = 0;  // contiguous bytes ready from the file head
    int webSeeds = 0;
};

}  // namespace torrent
