// "Sign in with Summit" on air/OS: browser_login.h's flow through Summit's
// WebDriver service (summitdriver, WebKit's WebDriver) instead of
// Chromium's DevTools protocol, which air/OS has no browser for.
//
// msga starts summitdriver on a free loopback port; a new WebDriver session
// starts Summit with a throwaway profile and opens Slack's sign-in page in a
// window of its own (Summit marks it "Automated"). The user signs in there;
// msga polls WebDriver's cookie command for the `d` cookie, then opens the
// web client in the same window and reads its localConfig_v2 for the
// workspaces and their tokens, exactly what the CDP path reads. Deleting the
// session closes the window (and the Summit it started); the profile is
// wiped afterwards.
//
// The session asks for no page-load waits ("pageLoadStrategy":"none"):
// otherwise every WebDriver command waits for the page, and Slack's heavy
// pages on a machine short of memory outlast any sensible request timeout.
//
// A Summit already running with the user's own profile takes the session
// instead (Summit is single launch): the window opens beside the user's, and
// a Slack session they already have there is picked up at once.
#include "app/slack/browser_login.h"

#include "base/crypto.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdlib>

namespace slack {

namespace {

constexpr int kPollIntervalMs   = 500;
constexpr int kDriverTimeoutMs  = 20000;  // summitdriver started → answering /status
constexpr int kTeamGraceMs      = 15000;  // the web client's boot after the login
// Signing in may take a while: SSO, a second factor, a code from the mail.
constexpr int kOverallTimeoutMs = 20 * 60 * 1000;
constexpr int kHttpTimeoutMs    = 30000;

constexpr const char *kSignInUrl     = "https://slack.com/signin";
constexpr const char *kClientUrl     = "https://app.slack.com/client";
constexpr const char *kProfilePrefix = "msga-signin-";

// summitdriver (the summit_webkit package), and the browser it starts.
std::string findDriver() {
    for (const char *p : {"/boot/system/bin/summitdriver",
                          "/boot/home/config/non-packaged/bin/summitdriver",
                          "/boot/system/non-packaged/bin/summitdriver"})
        if (file::exists(p))
            return p;
    return {};
}

bool summitInstalled() {
    return file::exists("/boot/system/apps/Summit") ||
           file::exists("/boot/home/config/non-packaged/apps/Summit") ||
           file::exists("/boot/system/non-packaged/apps/Summit");
}

std::string makeProfileDir(plat::App &app) {
    std::string root = app.standardDir(plat::StandardDir::Temp);
    if (root.empty())
        root = "/tmp";
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::string rnd = crypto::randomHex(6);
        if (rnd.empty())
            return {};
        const std::string dir = file::join(root, str::concat({kProfilePrefix, rnd}));
        if (!file::exists(dir) && file::makeDirs(dir))
            return dir;
    }
    return {};
}

// Only ever a directory this flow made.
void wipeProfile(const std::string &path) {
    if (!path.empty() && str::startsWith(file::baseName(path), kProfilePrefix))
        file::removeTree(path);
}

// DELETE /session/ID now, blocking (at most a few seconds): for teardown
// while the app quits, when no request or timer would run any more.
void deleteSessionNow(int port, const std::string &session) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) {
        const std::string req = str::concat(
            {"DELETE /session/", session, " HTTP/1.1\r\nHost: 127.0.0.1:", str::number(port),
             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"}
        );
        if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) == ssize_t(req.size())) {
            char buf[512];
            while (::recv(fd, buf, sizeof buf, 0) > 0) {
            }
        }
    }
    ::close(fd);
}

std::string quoted(std::string_view s) {
    std::string out;
    json::escapeString(out, s);
    return out;
}

} // namespace

std::string browserLoginName() {
    if (const char *v = std::getenv("MSGA_BROWSER_LOGIN"); v && std::string_view(v) == "0")
        return {};
    return !findDriver().empty() && summitInstalled() ? "Summit" : std::string();
}

std::string BrowserLogin::parseDebuggerUrl(std::string_view) {
    return {}; // CDP only
}

BrowserLogin::BrowserLogin(plat::App &app, net::Client &client) : _app(app), _client(client) {}

BrowserLogin::~BrowserLogin() {
    _done = true; // no callbacks out of a half-destroyed object
    // The dialog (or the app) goes now: no request or timer of shutdown()
    // may run any more, so the window and the driver go here and now.
    if (!_wdSession.empty() && _port) {
        deleteSessionNow(_port, _wdSession);
        _wdSession.clear();
    }
    shutdown();
    if (_proc && _proc->running())
        _proc->kill(false);
}

void BrowserLogin::setProgress(std::string s) {
    if (_progress)
        _progress(std::move(s));
}

void BrowserLogin::start(std::function<void(std::string)> progress, Done done) {
    _progress    = std::move(progress);
    _onDone      = std::move(done);
    _driverExe   = findDriver();
    _browserName = "Summit";
    if (_driverExe.empty() || !summitInstalled()) {
        // Never from inside start(): callers rely on done running later.
        _app.post([this] { finish({}, {}, "no_browser"); });
        return;
    }
    _profile = makeProfileDir(_app);
    _port    = net::freeLoopbackPort();
    if (_profile.empty() || _port == 0) {
        _app.post([this] { finish({}, {}, "launch_failed"); });
        return;
    }
    _proc = std::make_shared<base::Process>();
    if (!_proc->start(_driverExe, {"--port=" + str::number(_port), "--host=127.0.0.1"})) {
        _app.post([this] { finish({}, {}, "launch_failed"); });
        return;
    }
    _startMs = double(base::monotonicMs());
    setProgress(i18n::arg(i18n::tr("Opening %1\xE2\x80\xA6"), _browserName));
    _pollTimer = _app.addTimer(kPollIntervalMs, true, [this] { wdStep(); });
}

std::vector<TeamSession> BrowserLogin::hostTeams() const {
    return teamsFromHosts(_hosts);
}

// One WebDriver command; `done` gets the HTTP status (0: no answer) and the
// answer's "value".
void BrowserLogin::wd(std::string method, std::string path, std::string body, WdDone done) {
    net::Request req;
    req.method    = std::move(method);
    req.url       = str::concat({"http://127.0.0.1:", str::number(_port), path});
    req.timeoutMs = kHttpTimeoutMs;
    if (req.method == "POST") {
        req.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
        req.body = body.empty() ? std::string("{}") : std::move(body);
    }
    _wdReq = _client.send(std::move(req), [this, done = std::move(done)](net::Response r) {
        _wdReq = 0;
        if (_done)
            return;
        json::Document doc;
        if (r.status == 0 || !doc.parse(r.body, nullptr)) {
            done(r.status, json::Value());
            return;
        }
        done(r.status, doc.root()["value"]);
    });
}

void BrowserLogin::wdStep() {
    if (_done || _wdReq || _verifier)
        return;
    const double now = double(base::monotonicMs());
    if (now - _startMs > kOverallTimeoutMs) {
        finish(_cookie, _cookie.empty() ? std::vector<TeamSession>() : hostTeams(),
               _cookie.empty() ? "timeout" : "");
        return;
    }
    if (!_proc->running()) {
        LOG_INFO("signin", "summitdriver exited (%d)", _proc->exitStatus());
        finish(_cookie, hostTeams(), _cookie.empty() ? "launch_failed" : "");
        return;
    }
    switch (_wdState) {
    case 0: // the driver is up
        if (now - _startMs > kDriverTimeoutMs) {
            finish({}, {}, "no_devtools");
            return;
        }
        wd("GET", "/status", {}, [this](int status, const json::Value &v) {
            if (status == 200 && v["ready"].boolean(true))
                _wdState = 1;
        });
        return;
    case 1: { // a session: Summit with a throwaway profile
        const std::string caps = str::concat(
            {R"({"capabilities":{"alwaysMatch":{"pageLoadStrategy":"none",)",
             R"("summit:browserOptions":{"args":["--profile",)",
             quoted(_profile),
             "]}}}}"}
        );
        _wdState = 2;
        wd("POST", "/session", caps, [this](int status, const json::Value &v) {
            const std::string_view id = v["sessionId"].str();
            if (status != 200 || id.empty()) {
                LOG_INFO("signin", "WebDriver session failed (%d)", status);
                finish({}, {}, "cdp_failed");
                return;
            }
            _wdSession = std::string(id);
            LOG_INFO("signin", "WebDriver session %s on port %d", _wdSession.c_str(), _port);
            setProgress(i18n::tr("Sign in to Slack in the browser window\xE2\x80\xA6"));
            wd("POST",
               str::concat({"/session/", _wdSession, "/url"}),
               str::concat({R"({"url":)", quoted(kSignInUrl), "}"}),
               [this](int, const json::Value &) { _wdState = 3; });
        });
        return;
    }
    case 3:
        wdPoll();
        return;
    default:
        return;
    }
}

void BrowserLogin::wdPoll() {
    const std::string base = str::concat({"/session/", _wdSession});
    // Where the window is (for the workspace hosts, should the web client
    // never boot), then the cookie.
    wd("GET", base + "/url", {}, [this, base](int status, const json::Value &v) {
        if (status != 200) {
            // The user closed the window (or quit Summit): hand back what
            // there is. Anything else (a slow page, a busy driver: a heavy
            // page on a machine short of memory answers late) is tried again
            // on the next tick.
            const std::string_view err = v["error"].str();
            if (err == "no such window" || err == "invalid session id")
                finish(_cookie, hostTeams(), _cookie.empty() ? "cancelled" : "");
            else if (status != 0)
                LOG_INFO("signin", "WebDriver url: %d %.*s", status, int(err.size()), err.data());
            return;
        }
        const std::string_view url = v.str();
        if (url.find("slack.com") != std::string_view::npos) {
            _hosts.emplace_back(url);
            std::sort(_hosts.begin(), _hosts.end());
            _hosts.erase(std::unique(_hosts.begin(), _hosts.end()), _hosts.end());
        }
        if (!_cookie.empty()) {
            if (!_wdClient) {
                // The web client writes the signed-in workspaces and their
                // tokens to localConfig_v2 as it boots.
                _wdClient   = true;
                _wdClientMs = double(base::monotonicMs());
                wd("POST", base + "/url", str::concat({R"({"url":)", quoted(kClientUrl), "}"}),
                   [](int, const json::Value &) {});
                return;
            }
            wd("POST",
               base + "/execute/sync",
               R"({"script":"return window.localStorage.getItem('localConfig_v2');","args":[]})",
               [this](int status, const json::Value &v) {
                   std::vector<TeamSession> teams;
                   if (status == 200 && v.isString())
                       teams = parseLocalConfig(v.str());
                   if (!teams.empty())
                       wdVerify(std::move(teams));
                   else if (double(base::monotonicMs()) - _wdClientMs > kTeamGraceMs) {
                       // No workspace list: the ones the pages named, if
                       // any, checked like the others; none, the address
                       // is asked for.
                       std::vector<TeamSession> hosts = hostTeams();
                       if (hosts.empty())
                           finish(_cookie, {}, {});
                       else
                           wdVerify(std::move(hosts));
                   }
               });
            return;
        }
        wd("GET", base + "/cookie/d", {}, [this](int status, const json::Value &v) {
            const std::string_view value = v["value"].str();
            if (status != 200 || !str::startsWith(value, "xoxd-") || value == _rejected)
                return; // not signed in yet (or still the session Slack refused)
            // Verbatim, still percent-encoded as Slack stores it.
            LOG_INFO("signin", "signed in: the session cookie is there");
            _cookie     = std::string(value);
            _cookieAtMs = double(base::monotonicMs());
            setProgress(i18n::tr("Signed in \xE2\x80\x94 looking up your workspaces\xE2\x80\xA6"));
        });
    });
}

// Asks Slack whether the cookie (with the workspaces' tokens, or ones
// scraped fresh) is good before handing it over. A refused one is set aside
// and the window goes back to the sign-in page.
void BrowserLogin::wdVerify(std::vector<TeamSession> teams) {
    setProgress(i18n::tr("Verifying your session\xE2\x80\xA6"));
    _verifier = std::make_unique<TokenDeriver>(_client);
    _verifier->run(_cookie, std::move(teams), [this](std::vector<Credentials> valid, std::string error) {
        // Not deleted from inside its own callback.
        TokenDeriver *v = _verifier.release();
        _app.post([v] { delete v; });
        if (_done)
            return;
        if (!valid.empty()) {
            std::vector<TeamSession> ok;
            for (const Credentials &c : valid)
                ok.push_back({c.token, c.workspaceUrl, c.teamId, c.teamName, c.iconUrl});
            finish(_cookie, std::move(ok), {});
            return;
        }
        LOG_INFO("signin", "Slack refused the browser's session (%s); waiting for a new sign-in", error.c_str());
        _rejected = _cookie;
        _cookie.clear();
        _wdClient = false;
        setProgress(i18n::tr("Slack didn't accept that session \xE2\x80\x94 sign in to Slack in the "
                             "Summit window\xE2\x80\xA6"));
        if (!_wdSession.empty())
            wd("POST",
               str::concat({"/session/", _wdSession, "/url"}),
               str::concat({R"({"url":)", quoted(kSignInUrl), "}"}),
               [](int, const json::Value &) {});
    });
}

void BrowserLogin::finish(std::string cookie, std::vector<TeamSession> teams, std::string error) {
    if (_done)
        return;
    _done = true;
    shutdown();
    Done d = std::move(_onDone);
    if (d)
        d(std::move(cookie), std::move(teams), std::move(error)); // may delete this
}

void BrowserLogin::shutdown() {
    for (uint64_t *t : {&_probeTimer, &_pollTimer, &_exitTimer})
        if (*t) {
            _app.cancelTimer(*t);
            *t = 0;
        }
    if (_wdReq) {
        _client.cancel(_wdReq);
        _wdReq = 0;
    }
    if (_wipeScheduled)
        return;
    _wipeScheduled = true;
    // Deleting the session closes its window and quits the Summit the
    // driver started; then the driver goes, and the profile (and the cookie
    // in it) with it.
    if (!_wdSession.empty() && _port) {
        // At once (the driver answers in milliseconds): whoever owns this
        // may destroy it, and kill the driver, right after.
        deleteSessionNow(_port, _wdSession);
        _wdSession.clear();
    }
    auto              proc = _proc;
    const std::string path = _profile;
    plat::App        *app  = &_app;
    _app.addTimer(3000, false, [app, proc, path] {
        if (proc && proc->running())
            proc->kill(false);
        app->addTimer(3000, false, [proc, path] {
            if (proc && proc->running())
                proc->kill(true);
            wipeProfile(path);
        });
    });
}

} // namespace slack
