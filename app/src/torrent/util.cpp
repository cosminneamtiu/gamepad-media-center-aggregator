/*
    GMCA — engine utilities (see torrent/util.hpp).
*/

#include "torrent/util.hpp"

#include <chrono>
#include <random>

namespace torrent {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string randomBytes(size_t n) {
    static thread_local std::mt19937 rng((unsigned)std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string s;
    s.resize(n);
    for (size_t i = 0; i < n; i++) s[i] = (char)dist(rng);
    return s;
}

std::string makePeerId(const std::string& prefix) {
    std::string id = prefix;
    if (id.size() > 20) id.resize(20);
    id += randomBytes(20 - id.size());
    return id;
}

}  // namespace torrent
