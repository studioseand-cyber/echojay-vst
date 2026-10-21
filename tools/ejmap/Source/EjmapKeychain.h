/*
  EjmapKeychain.h - the wizard's tokens in the macOS Keychain (contract v1 §1.6, 21 Oct): one generic-password item per server
  (service "ai.echojay.wizard|<server host>", account "tokens"), its data the one string wizcontract::tokensToSecret makes. Never a file,
  a log line, an argument or an environment variable. The app uses this; the tests use MemoryTokenStore.
*/
#pragma once

#include "EjmapWizardContract.h"
#include <Security/Security.h>

namespace ejmap::wizcontract
{
inline juce::String keychainServiceFor (const juce::String& baseUrl) { return "ai.echojay.wizard|" + juce::URL (baseUrl).getDomain(); }

struct KeychainTokenStore : TokenStore
{
    explicit KeychainTokenStore (juce::String service) : svc (std::move (service)) {}

    std::optional<Tokens> load() override
    {
        auto q = query(); CFDictionarySetValue (q, kSecReturnData, kCFBooleanTrue); CFDictionarySetValue (q, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef out = nullptr; const auto rc = SecItemCopyMatching (q, &out); CFRelease (q);
        if (rc != errSecSuccess || out == nullptr) return std::nullopt;
        const auto data = (CFDataRef) out; const juce::String s = juce::String::fromUTF8 ((const char*) CFDataGetBytePtr (data), (int) CFDataGetLength (data)); CFRelease (out);
        return tokensFromSecret (s);
    }
    bool save (const Tokens& t) override
    {
        const auto secret = tokensToSecret (t);
        auto data = CFDataCreate (nullptr, (const UInt8*) secret.toRawUTF8(), (CFIndex) secret.getNumBytesAsUTF8());
        auto q = query();
        auto upd = CFDictionaryCreateMutable (nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFDictionarySetValue (upd, kSecValueData, data);
        auto rc = SecItemUpdate (q, upd); CFRelease (upd);
        if (rc == errSecItemNotFound)
        {   CFDictionarySetValue (q, kSecValueData, data); CFDictionarySetValue (q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
            rc = SecItemAdd (q, nullptr); }
        CFRelease (q); CFRelease (data); return rc == errSecSuccess;
    }
    void clear() override { auto q = query(); SecItemDelete (q); CFRelease (q); }

private:
    juce::String svc;
    CFMutableDictionaryRef query() const
    {
        auto q = CFDictionaryCreateMutable (nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue (q, kSecClass, kSecClassGenericPassword);
        auto s = svc.toCFString(); auto a = juce::String ("tokens").toCFString();
        CFDictionarySetValue (q, kSecAttrService, s); CFDictionarySetValue (q, kSecAttrAccount, a); CFRelease (s); CFRelease (a);
        return q;
    }
};
} // namespace ejmap::wizcontract
