// secret.h on air/OS (Haiku): the user's keyring through BKeyStore
// (keystore_server; air/OS asks once whether msga may use it). Each value is
// a password key with identifier "msga/<key>", secondary identifier
// "msga". The settings store (old_settings.h, an INI file in
// ~/config/settings/msga) keeps a marker for each key in the keyring, so a
// key that was never stored costs no keystore call (and no permission
// prompt before the user has signed in to anything). When the keystore
// refuses (no server, access denied) the value itself goes to the settings
// store, from where a later read moves it into the keyring. Test processes
// use only the settings store, as everywhere.
#include "base/old_settings.h"
#include "base/process.h"
#include "base/secret.h"
#include "base/str.h"

#include <Key.h>
#include <KeyStore.h>

namespace secret {

namespace {

constexpr const char *kSecondary = "msga";
// In the settings store: "the value is in the keyring".
constexpr std::string_view kMarker = "@air/OS keyring@";

std::string identifier(std::string_view key) {
    return str::concat({"msga/", key});
}

bool useKeyring() {
    return !base::testProcess();
}

bool keyringRead(std::string_view key, std::string *out) {
    BKeyStore    store;
    BPasswordKey k;
    if (store.GetKey(B_KEY_TYPE_PASSWORD, identifier(key).c_str(), kSecondary, false, k) != B_OK)
        return false;
    *out = k.Password() ? k.Password() : "";
    return true;
}

void keyringRemove(std::string_view key) {
    BKeyStore    store;
    BPasswordKey k;
    if (store.GetKey(B_KEY_TYPE_PASSWORD, identifier(key).c_str(), kSecondary, false, k) == B_OK)
        store.RemoveKey(k);
}

bool keyringWrite(std::string_view key, std::string_view value) {
    // Passwords are C strings: a value with a NUL in it stays in the file.
    if (value.find('\0') != std::string_view::npos)
        return false;
    keyringRemove(key); // AddKey refuses to replace
    BKeyStore          store;
    const std::string  v(value);
    BPasswordKey       k(v.c_str(), B_KEY_PURPOSE_GENERIC, identifier(key).c_str(), kSecondary);
    return store.AddKey(k) == B_OK;
}

} // namespace

bool available() {
    return true;
}

bool write(std::string_view key, std::string_view value) {
    if (value.empty()) {
        remove(key);
        return true;
    }
    if (useKeyring() && value != kMarker && keyringWrite(key, value))
        return oldsettings::write(key, kMarker);
    return oldsettings::write(key, value);
}

std::string read(std::string_view key, bool *found) {
    const oldsettings::Value old = oldsettings::get(key);
    std::string              v   = old.text();
    bool                     has = old.kind != oldsettings::Value::Kind::None;
    if (has && v == kMarker) {
        has = useKeyring() && keyringRead(key, &v);
        if (!has)
            v.clear();
    } else if (has && !v.empty() && useKeyring() && keyringWrite(key, v)) {
        // A value the keyring refused before: into it now.
        oldsettings::write(key, kMarker);
    }
    if (found)
        *found = has;
    return v;
}

void remove(std::string_view key) {
    if (useKeyring() && oldsettings::get(key).text() == kMarker)
        keyringRemove(key);
    oldsettings::remove(key);
}

} // namespace secret
