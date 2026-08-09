// Standalone regression test — torrent::net::poll() readiness shim.
//
//   c++ -std=gnu++17 \
//       -Iapp/include \
//       tests/test_socket_poll.cpp -o /tmp/t && /tmp/t
//
// History: the shim used select()/FD_SET. On Switch, libnx socket fds are
// newlib handles shared with every open file (well above newlib's
// FD_SETSIZE=64), so select() failed with EINVAL on every call and the whole
// engine event loop went dark (no handshakes / DHT / µTP -> torrents visible
// but unplayable). The shim now rides poll(2); this test locks the semantics
// (readable / writable / error / timeout / empty set) with socket fds pushed
// ABOVE 64 by dummy file descriptors, reproducing the Switch fd numbering.
//
// Implementation TUs are included directly (tests/run.sh compiles each test
// standalone; socket.cpp + log.cpp have no dependencies beyond app/include).

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../app/src/torrent/log.cpp"
#include "../app/src/torrent/util.cpp"
#include "../app/src/torrent/socket.cpp"

using namespace torrent;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    CHECK(net::globalInit());

    // Push the next socket fds above newlib's FD_SETSIZE=64 (the Switch
    // scenario: media-center files already occupy the low handle numbers).
    std::vector<int> dummies;
    for (int i = 0; i < 80; i++) {
        int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        dummies.push_back(fd);
    }

    // ---- UDP: datagram makes the receiver readable ------------------------
    net::UdpSocket rx, tx;
    CHECK(rx.open());
    CHECK(tx.open());
    CHECK(rx.handle() >= 64);  // the regression scenario is actually engaged

    // Bind rx to an ephemeral loopback port via a raw sockaddr on its fd.
    {
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        CHECK(::bind(rx.handle(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t alen = sizeof(addr);
        CHECK(::getsockname(rx.handle(), reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
        uint16_t port = ntohs(addr.sin_port);
        CHECK(port != 0);
        const char msg[] = "gmca-poll-test";
        CHECK(tx.sendTo("127.0.0.1", port, msg, sizeof(msg)) == (int)sizeof(msg));
    }
    {
        std::vector<net::PollItem> items(1);
        items[0].fd = rx.handle();
        items[0].wantRead = true;
        int r = net::poll(items, 2000);
        CHECK(r > 0);
        CHECK(items[0].readable);
        CHECK(!items[0].error);
        char buf[64];
        std::string fromIp;
        uint16_t fromPort = 0;
        int n = rx.recvFrom(buf, sizeof(buf), &fromIp, &fromPort);
        CHECK(n == (int)sizeof("gmca-poll-test"));
        CHECK(fromIp == "127.0.0.1" && fromPort != 0);
    }

    // ---- TCP: listener readable on connect; client writable once connected -
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 64);
    {
        int one = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        CHECK(::listen(listener, 4) == 0);
    }
    net::TcpSocket client;
    CHECK(client.open());
    {
        sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
        char ip[32] = "127.0.0.1";
        CHECK(client.startConnect(ip, ntohs(addr.sin_port)));  // EINPROGRESS ok

        std::vector<net::PollItem> items(2);
        items[0].fd = listener;
        items[0].wantRead = true;
        items[1].fd = client.handle();
        items[1].wantWrite = true;
        int r = net::poll(items, 2000);
        CHECK(r > 0);
        CHECK(items[0].readable);   // pending connection on the listener
        CHECK(items[1].writable);   // loopback connect completed
        CHECK(!items[0].error && !items[1].error);

        int accepted = ::accept(listener, nullptr, nullptr);
        CHECK(accepted >= 0);
        CHECK(client.checkConnected() == 1);
        ::close(accepted);
    }

    // ---- idle: nothing ready -> 0 within the timeout ----------------------
    {
        net::UdpSocket quiet;
        CHECK(quiet.open());
        std::vector<net::PollItem> items(1);
        items[0].fd = quiet.handle();
        items[0].wantRead = true;
        int r = net::poll(items, 50);
        CHECK(r == 0);
        CHECK(!items[0].readable && !items[0].writable && !items[0].error);
    }

    // ---- empty set: emulated timeout, no crash ----------------------------
    {
        std::vector<net::PollItem> items;
        int r = net::poll(items, 10);
        CHECK(r == 0);
    }

    // ---- negative fd entries are skipped ----------------------------------
    {
        std::vector<net::PollItem> items(2);
        items[0].fd = net::invalidHandle();
        items[0].wantRead = true;
        items[1].fd = rx.handle();
        items[1].wantRead = true;
        int r = net::poll(items, 50);
        CHECK(r == 0);  // rx drained above; the invalid fd must not break poll
    }

    ::close(listener);
    for (int fd : dummies) ::close(fd);
    net::globalShutdown();

    if (failures == 0) {
        printf("OK: test_socket_poll\n");
        return 0;
    }
    printf("%d failure(s)\n", failures);
    return 1;
}
