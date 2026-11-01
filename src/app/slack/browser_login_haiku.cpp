// "Sign in with Summit" on air/OS: browser_login.h's flow through Summit's
// WebDriver service (summitdriver, WebKit's WebDriver) instead of
// Chromium's DevTools protocol, which air/OS has no browser for.
//
// The sign-in opens as a window (marked "Automated") of the user's own
// Summit, with their extensions, password manager and Slack sessions. Summit
// opens one automation socket for the life of the process (a second
// --automation is refused as busy), so msga keeps to one fixed address: it
// asks Summit to open it (a start with --automation, which Summit hands to
// the copy that runs with the default profile, or the start of Summit itself
// when none runs) and runs summitdriver with --target there. The driver then
// never starts or quits a browser of its own.
//
// The user signs in there; msga polls WebDriver's cookie command for the `d`
// cookie, opens the web client in the same window and reads its
// localConfig_v2 for the workspaces and their tokens, as the CDP path does.
// The user's profile may keep an expired cookie or an earlier session's
// tokens, so Slack is asked before a session is taken (wdVerify). Deleting
// the WebDriver session closes the window.
//
// The session asks for no page-load waits ("pageLoadStrategy":"none"):
// otherwise every WebDriver command waits for the page, and Slack's heavy
// pages on a machine short of memory outlast any sensible request timeout.
#include "app/slack/browser_login.h"

#include "base/file.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>

#include <Entry.h>
#include <Roster.h>

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
// After the window first shows a signed-in page, before Slack is asked
// anything: the web client finishes its own boot first.
constexpr int kSettleMs = 4000;
// No answer from the driver for this long: it is wedged (a window gone under
// it); give up rather than show "Verifying" for ever.
constexpr int kDriverSilentMs = 60000;
// Signing in may take a while: SSO, a second factor, a code from the mail.
constexpr int kOverallTimeoutMs = 20 * 60 * 1000;
constexpr int kHttpTimeoutMs    = 30000;

// The web client: it boots at once in a browser that is signed in, and
// sends one that isn't to Slack's sign-in, coming back here afterwards.
constexpr const char *kClientUrl = "https://app.slack.com/client";

// Where the window shows a finished sign-in: the web client on a team (its
// id in the path), or the page that hands over to the desktop app.
bool signedInPage(std::string_view url) {
    if (str::startsWith(url, "https://app.slack.com/client/T") ||
        str::startsWith(url, "https://app.slack.com/client/E"))
        return true;
    const size_t host = url.find("://");
    const size_t path = host == std::string_view::npos ? host : url.find('/', host + 3);
    return path != std::string_view::npos &&
           url.substr(0, path).find(".slack.com") != std::string_view::npos &&
           str::startsWith(url.substr(path), "/ssb/");
}
// Where msga asks Summit to listen for WebDriver (loopback only), and how
// long Summit gets to open it: a start of Summit, or the hand-off of the
// arguments to the one running.
constexpr const char *kAutomationAddress = "127.0.0.1:47089";
constexpr int         kSummitStartMs     = 5000;
constexpr int         kHandOffMs         = 1500;

// summitdriver (the summit_webkit package), and the browser it starts.
std::string findDriver() {
    for (const char *p : {"/boot/system/bin/summitdriver",
                          "/boot/home/config/non-packaged/bin/summitdriver",
                          "/boot/system/non-packaged/bin/summitdriver"})
        if (file::exists(p))
            return p;
    return {};
}

// The installed Summit (the summit package, or a copy of the user's own).
std::string summitPath() {
    for (const char *p : {"/boot/system/apps/Summit", "/boot/home/config/non-packaged/apps/Summit",
                          "/boot/system/non-packaged/apps/Summit"})
        if (file::exists(p))
            return p;
    return {};
}

bool summitInstalled() {
    return !summitPath().empty();
}

// Asks the user's Summit to open its automation socket at
// kAutomationAddress: started as Deskbar would when it isn't running.
// The milliseconds to give it, or -1 when it could not be started.
int openAutomation() {
    entry_ref ref;
    if (get_ref_for_path(summitPath().c_str(), &ref) != B_OK)
        return -1;
    const bool  running = be_roster->IsRunning(&ref);
    const char *argv[]  = {"--automation", kAutomationAddress, nullptr};
    const status_t st   = be_roster->Launch(&ref, 2, argv);
    if (st != B_OK && st != B_ALREADY_RUNNING)
        return -1;
    return running ? kHandOffMs : kSummitStartMs;
}

// One blocking request to the driver (at most a few seconds): for teardown,
// when no request or timer may run any more.
void requestNow(int port, const std::string &method, const std::string &path) {
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
            {method, " ", path, " HTTP/1.1\r\nHost: 127.0.0.1:", str::number(port),
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

// Ends the session: its window first (a session on a Summit the driver did
// not start leaves its windows open otherwise), then the session.
void deleteSessionNow(int port, const std::string &session) {
    requestNow(port, "DELETE", str::concat({"/session/", session, "/window"}));
    requestNow(port, "DELETE", str::concat({"/session/", session}));
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
    _port = net::freeLoopbackPort();
    if (_port == 0) {
        _app.post([this] { finish({}, {}, "launch_failed"); });
        return;
    }
    _proc = std::make_shared<base::Process>();
    const int wait = openAutomation();
    if (wait < 0 ||
        !_proc->start(
            _driverExe,
            {"--port=" + str::number(_port), "--host=127.0.0.1",
             std::string("--target=") + kAutomationAddress}
        )) {
        _app.post([this] { finish({}, {}, "launch_failed"); });
        return;
    }
    _startMs = double(base::monotonicMs());
    _summitReadyMs = _startMs + wait;
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
        if (r.status != 0)
            _wdHeardMs = double(base::monotonicMs());
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
    if (_wdState >= 3 && _wdHeardMs > 0 && now - _wdHeardMs > kDriverSilentMs) {
        LOG_INFO("signin", "summitdriver stopped answering");
        finish({}, {}, "cdp_failed");
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
    case 1: { // a session, in the user's Summit
        if (now < _summitReadyMs)
            return;
        const std::string caps = R"({"capabilities":{"alwaysMatch":{"pageLoadStrategy":"none"}}})";
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
               str::concat({R"({"url":)", quoted(kClientUrl), "}"}),
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

// Watches the window, never steers it: the user signs in at their own pace
// (Slack's sign-in sets interim cookies on the way, and taking the window
// away then cuts the sign-in short). Once it shows a signed-in page, the
// browser's `d` cookies and the web client's workspaces are checked with
// Slack.
void BrowserLogin::wdPoll() {
    const std::string base = str::concat({"/session/", _wdSession});
    wd("GET", base + "/url", {}, [this, base](int status, const json::Value &v) {
        if (status != 200) {
            // The user closed the window (or quit Summit): hand back what
            // there is. Anything else (a slow page, a busy driver: a heavy
            // page on a machine short of memory answers late) is tried again
            // on the next tick.
            const std::string_view err = v["error"].str();
            if (err == "no such window" || err == "invalid session id")
                finish({}, {}, "cancelled");
            else if (status != 0)
                LOG_INFO("signin", "WebDriver url: %d %.*s", status, int(err.size()), err.data());
            return;
        }
        const std::string url(v.str());
        if (url.find("slack.com") != std::string::npos) {
            _hosts.emplace_back(url);
            std::sort(_hosts.begin(), _hosts.end());
            _hosts.erase(std::unique(_hosts.begin(), _hosts.end()), _hosts.end());
        }
        if (!signedInPage(url)) {
            _wdClient = false;
            return; // still signing in
        }
        if (!_wdClient) {
            _wdClient   = true;
            _wdClientMs = double(base::monotonicMs());
            LOG_INFO("signin", "the window shows a signed-in page");
            setProgress(i18n::tr("Signed in \xE2\x80\x94 looking up your workspaces\xE2\x80\xA6"));
        }
        if (double(base::monotonicMs()) - _wdClientMs < kSettleMs)
            return;
        const bool handoff = url.find("/ssb/") != std::string::npos;
        // Every cookie, not WebDriver's first `d`: a profile that used Slack
        // before can hold more than one.
        wd("GET", base + "/cookie", {}, [this, base, handoff](int status, const json::Value &v) {
            if (status != 200)
                return;
            std::vector<std::string> found;
            for (const json::Value c : v) {
                const std::string_view value = c["value"].str();
                if (c["name"].str() != "d" || !str::startsWith(value, "xoxd-"))
                    continue;
                // Verbatim, still percent-encoded as Slack stores it.
                const std::string d(value);
                if (std::find(_rejected.begin(), _rejected.end(), d) == _rejected.end() &&
                    std::find(found.begin(), found.end(), d) == found.end())
                    found.push_back(d);
            }
            if (found.empty())
                return; // only sessions Slack refused: the sign-in isn't done
            _cookies = std::move(found);
            _cookie  = _cookies.front();
            if (handoff) {
                // The desktop-app hand-off page never boots the web client;
                // its address names the workspace.
                std::vector<TeamSession> hosts = hostTeams();
                if (hosts.empty())
                    finish(_cookie, {}, {});
                else
                    wdVerify(std::move(hosts));
                return;
            }
            wd("POST",
               base + "/execute/sync",
               R"({"script":"return window.localStorage.getItem('localConfig_v2');","args":[]})",
               [this](int status, const json::Value &v) {
                   std::vector<TeamSession> teams;
                   if (status == 200 && v.isString())
                       teams = parseLocalConfig(v.str());
                   // The workspaces only, never their stored tokens: in a
                   // profile that signed in before they can be an earlier
                   // session's, and Slack revokes a fresh session presented
                   // with a token that isn't its own. Fresh ones are scraped
                   // from each workspace with the cookie.
                   for (TeamSession &t : teams)
                       t.token.clear();
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
        });
    });
}

// Asks Slack whether a cookie (with the workspaces' tokens, or ones
// scraped fresh) is good before handing it over: each of the browser's `d`
// cookies in turn. While the web client may still be booting (its
// localConfig_v2 can hold the previous session's tokens until it has), a
// refusal is tried again on the next tick; after that the refused cookies are
// set aside and the window goes back to the sign-in page.
void BrowserLogin::wdVerify(std::vector<TeamSession> teams, size_t cookie) {
    if (cookie >= _cookies.size())
        return;
    _cookie = _cookies[cookie];
    setProgress(i18n::tr("Verifying your session\xE2\x80\xA6"));
    _verifier = std::make_unique<TokenDeriver>(_client);
    _verifier->run(_cookie, teams, [this, teams, cookie](std::vector<Credentials> valid, std::string error) {
        // Not deleted from inside its own callback.
        TokenDeriver *v = _verifier.release();
        _app.post([v] { delete v; });
        if (_done)
            return;
        if (!valid.empty()) {
            LOG_INFO("signin", "Slack accepted the session (cookie %zu): %zu workspace(s)", cookie + 1,
                     valid.size());
            std::vector<TeamSession> ok;
            for (const Credentials &c : valid)
                ok.push_back({c.token, c.workspaceUrl, c.teamId, c.teamName, c.iconUrl});
            finish(_cookie, std::move(ok), {});
            return;
        }
        LOG_INFO("signin", "Slack refused cookie %zu of %zu (%s)", cookie + 1, _cookies.size(),
                 error.c_str());
        if (cookie + 1 < _cookies.size()) {
            wdVerify(teams, cookie + 1);
            return;
        }
        // The web client may not have booted yet: read its workspaces again.
        if (double(base::monotonicMs()) - _wdClientMs < kTeamGraceMs) {
            _cookie = _cookies.front();
            return;
        }
        // The window is left alone: if the session is stale, the web client
        // itself sends the user to Slack's sign-in, and a new cookie comes.
        LOG_INFO("signin", "no session Slack accepts; waiting for the sign-in to finish");
        _rejected.insert(_rejected.end(), _cookies.begin(), _cookies.end());
        _cookies.clear();
        _cookie.clear();
        setProgress(i18n::tr("Waiting for you to finish signing in to Slack in the Summit "
                             "window\xE2\x80\xA6"));
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
    // Deleting the session closes its window (the user's Summit stays);
    // then the driver goes.
    if (!_wdSession.empty() && _port) {
        // At once (the driver answers in milliseconds): whoever owns this
        // may destroy it, and kill the driver, right after.
        deleteSessionNow(_port, _wdSession);
        _wdSession.clear();
    }
    auto       proc = _proc;
    plat::App *app  = &_app;
    _app.addTimer(3000, false, [app, proc] {
        if (proc && proc->running())
            proc->kill(false);
        app->addTimer(3000, false, [proc] {
            if (proc && proc->running())
                proc->kill(true);
        });
    });
}

} // namespace slack
