// TLS for the POSIX transport on air/OS (Haiku): the system's OpenSSL 3 over
// our non-blocking sockets, verified against the system's CA store. What
// tls.cpp does with mbedTLS elsewhere, with what every air/OS install
// already has loaded: no TLS library in the binary, the platform's CA
// bundle (/boot/system/data/ssl/CARootCertificates.pem) and its
// accelerated ciphers.
//
// The last session per host:port is kept and offered on the next
// connection there, as tls.cpp does: a reconnect costs one key exchange
// instead of a full handshake.
#include "base/str.h"
#include "net/posix/posix.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <unistd.h>

namespace net::detail {

namespace {

// Where air/OS keeps the trusted roots ($SSL_CERT_FILE wins).
constexpr const char *kCaBundles[] = {
    "/boot/system/data/ssl/CARootCertificates.pem",
    "/system/data/ssl/CARootCertificates.pem",
};

struct Shared {
    SSL_CTX *ctx = nullptr;
    bool     ok  = false;
};

struct SharedSlot {
    std::mutex mutex;
    Shared    *s = nullptr;
    Shared     mem;
};

Shared *shared() {
    SharedSlot                 &slot = immortal<SharedSlot>();
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (slot.s)
        return slot.s;
    Shared *sh = &slot.mem;
    *sh        = {};
    slot.s     = sh;
    sh->ctx    = SSL_CTX_new(TLS_client_method());
    if (!sh->ctx)
        return sh;
    SSL_CTX_set_min_proto_version(sh->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(sh->ctx, SSL_VERIFY_PEER, nullptr);
    // Sessions are ours to keep (keepSession); OpenSSL's internal cache is
    // a server-side thing for clients.
    SSL_CTX_set_session_cache_mode(sh->ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_set_mode(sh->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    bool        loaded = false;
    const char *env    = std::getenv("SSL_CERT_FILE");
    if (env && *env)
        loaded = SSL_CTX_load_verify_locations(sh->ctx, env, nullptr) == 1;
    for (const char *path : kCaBundles) {
        if (loaded)
            break;
        loaded = access(path, R_OK) == 0 && SSL_CTX_load_verify_locations(sh->ctx, path, nullptr) == 1;
    }
    if (!loaded)
        loaded = SSL_CTX_set_default_verify_paths(sh->ctx) == 1;
    ERR_clear_error();
    sh->ok = loaded;
    return sh;
}

// ── Session cache ───────────────────────────────────────────────────────────

constexpr size_t kMaxSessions = 16;

struct SessionCache {
    struct Entry {
        std::string  key;
        SSL_SESSION *session;
    };
    std::mutex         mutex;
    std::vector<Entry> entries; // the most recently stored last
};

SessionCache &sessions() {
    return immortal<SessionCache>();
}

std::atomic<int64_t> g_resumed{0};

} // namespace

struct TlsConn {
    SSL        *ssl    = nullptr;
    int         fd     = -1;
    int         sysErr = 0;
    std::string key; // "host:port"
    bool        offered = false;
};

namespace {

void keepSession(TlsConn *c) {
    SSL_SESSION *s = SSL_get1_session(c->ssl);
    if (!s)
        return;
    if (!SSL_SESSION_is_resumable(s)) {
        SSL_SESSION_free(s);
        return;
    }
    SSL_SESSION *drop[2] = {nullptr, nullptr};
    {
        SessionCache               &sc = sessions();
        std::lock_guard<std::mutex> lock(sc.mutex);
        for (size_t i = 0; i < sc.entries.size(); ++i)
            if (sc.entries[i].key == c->key) {
                drop[0] = sc.entries[i].session;
                sc.entries.erase(sc.entries.begin() + ptrdiff_t(i));
                break;
            }
        if (sc.entries.size() >= kMaxSessions) {
            drop[1] = sc.entries.front().session;
            sc.entries.erase(sc.entries.begin());
        }
        sc.entries.push_back({c->key, s});
    }
    for (SSL_SESSION *d : drop)
        if (d)
            SSL_SESSION_free(d);
}

void offerSession(TlsConn *c) {
    SessionCache               &sc = sessions();
    std::lock_guard<std::mutex> lock(sc.mutex);
    for (const auto &e : sc.entries)
        if (e.key == c->key) {
            c->offered = SSL_set_session(c->ssl, e.session) == 1;
            return;
        }
}

void forgetSession(const std::string &key) {
    SSL_SESSION *drop = nullptr;
    {
        SessionCache               &sc = sessions();
        std::lock_guard<std::mutex> lock(sc.mutex);
        for (size_t i = 0; i < sc.entries.size(); ++i)
            if (sc.entries[i].key == key) {
                drop = sc.entries[i].session;
                sc.entries.erase(sc.entries.begin() + ptrdiff_t(i));
                break;
            }
    }
    if (drop)
        SSL_SESSION_free(drop);
}

// TLS 1.3 tickets arrive after the handshake: keep each as it comes.
int onNewSession(SSL *ssl, SSL_SESSION *) {
    if (auto *c = static_cast<TlsConn *>(SSL_get_app_data(ssl)))
        keepSession(c);
    return 0; // we took our own reference
}

// Our short reasons, in the words tls.cpp uses.
std::string describe(TlsConn *c, int sslErr) {
    if (sslErr == SSL_ERROR_SYSCALL && c->sysErr)
        return str::concat({"connect: ", std::strerror(c->sysErr)});
    if (sslErr == SSL_ERROR_SYSCALL && errno)
        return str::concat({"connect: ", std::strerror(errno)});
    const long verify = SSL_get_verify_result(c->ssl);
    if (verify != X509_V_OK) {
        switch (verify) {
        case X509_V_ERR_HOSTNAME_MISMATCH:
            return "tls: hostname mismatch";
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
            return "tls: certificate expired";
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
            return "tls: untrusted certificate";
        default:
            return str::concat({"tls: bad certificate ", X509_verify_cert_error_string(verify)});
        }
    }
    const unsigned long e = ERR_peek_last_error();
    ERR_clear_error();
    if (sslErr == SSL_ERROR_ZERO_RETURN || (sslErr == SSL_ERROR_SYSCALL && !e))
        return "tls: connection closed";
    if (e) {
        char buf[160];
        ERR_error_string_n(e, buf, sizeof buf);
        return str::concat({"tls: ", buf});
    }
    return str::concat({"tls: error ", str::number(sslErr)});
}

long result(TlsConn *c, int rc, short *want, std::string *error) {
    const int e = SSL_get_error(c->ssl, rc);
    if (e == SSL_ERROR_WANT_READ) {
        *want = POLLIN;
        return Stream::Again;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        *want = POLLOUT;
        return Stream::Again;
    }
    *error = describe(c, e);
    return Stream::Fail;
}

} // namespace

int64_t tlsResumptions() {
    return g_resumed.load(std::memory_order_relaxed);
}

void releaseTls() {
    std::vector<SessionCache::Entry> old;
    {
        SessionCache               &sc = sessions();
        std::lock_guard<std::mutex> lock(sc.mutex);
        old.swap(sc.entries);
    }
    for (const auto &e : old)
        SSL_SESSION_free(e.session);
    SharedSlot                 &slot = immortal<SharedSlot>();
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.s)
        return;
    SSL_CTX_free(slot.s->ctx);
    slot.s->ctx = nullptr;
    slot.s      = nullptr;
}

TlsConn *tlsNew(int fd, const Url &url, std::string *error) {
    Shared *sh = shared();
    if (!sh->ok) {
        *error = sh->ctx ? "tls: no CA certificates" : "tls: setup failed";
        return nullptr;
    }
    auto *c = new TlsConn;
    c->fd   = fd;
    c->key  = str::concat({url.host, ":", str::number(url.port)});
    c->ssl  = SSL_new(sh->ctx);
    // SNI, and the name the certificate must carry.
    if (!c->ssl || SSL_set_fd(c->ssl, fd) != 1 ||
        SSL_set_tlsext_host_name(c->ssl, url.host.c_str()) != 1 ||
        SSL_set1_host(c->ssl, url.host.c_str()) != 1) {
        if (c->ssl)
            SSL_free(c->ssl);
        delete c;
        ERR_clear_error();
        *error = "tls: setup failed";
        return nullptr;
    }
    SSL_set_app_data(c->ssl, c);
    SSL_CTX_sess_set_new_cb(sh->ctx, onNewSession);
    SSL_set_connect_state(c->ssl);
    offerSession(c);
    return c;
}

void tlsFree(TlsConn *c) {
    if (SSL_is_init_finished(c->ssl))
        SSL_shutdown(c->ssl); // best effort, never waits (non-blocking)
    SSL_set_app_data(c->ssl, nullptr);
    SSL_free(c->ssl);
    ERR_clear_error();
    delete c;
}

long tlsHandshake(TlsConn *c, short *want, std::string *error) {
    errno        = 0;
    const int rc = SSL_do_handshake(c->ssl);
    if (rc == 1) {
        if (c->offered && SSL_session_reused(c->ssl))
            g_resumed.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    c->sysErr    = errno;
    const long r = result(c, rc, want, error);
    if (r == Stream::Fail && c->offered)
        forgetSession(c->key); // in case the server chokes on it: not again
    return r;
}

long tlsRead(TlsConn *c, char *buf, size_t n, short *want, std::string *error) {
    size_t got = 0;
    errno      = 0;
    const int rc = SSL_read_ex(c->ssl, buf, n, &got);
    if (rc == 1)
        return long(got);
    const int e = SSL_get_error(c->ssl, rc);
    // Most servers skip close_notify; a bare TCP close is the end too.
    if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && errno == 0)) {
        ERR_clear_error();
        return 0;
    }
    c->sysErr = errno;
    return result(c, rc, want, error);
}

long tlsWrite(TlsConn *c, const char *buf, size_t n, short *want, std::string *error) {
    size_t done = 0;
    errno       = 0;
    const int rc = SSL_write_ex(c->ssl, buf, n, &done);
    if (rc == 1)
        return long(done);
    c->sysErr = errno;
    return result(c, rc, want, error);
}

bool tlsPending(const TlsConn *c) {
    return SSL_pending(c->ssl) > 0;
}

} // namespace net::detail
