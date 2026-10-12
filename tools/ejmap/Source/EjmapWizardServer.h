/*
  EjmapWizardServer.h - THE SERVER BOUNDARY (Sean's review, Kathy 12 Oct; feat/ejmap-wizard; docs/WIZARD_SPEC.md section 6).

  1. THE BASE URL: https://www.echojay.ai (not api.echojay.ai); every endpoint under /api/wizard/v1/.
  2. THE UPLOAD PATH: a bundle that fits Vercel's 4.5 MB request body goes direct (POST /contributions, multipart); a bigger one goes
     through signed blob storage: POST /contributions/upload-url -> PUT the zip to the signed URL -> POST /contributions/complete with the
     blob reference and the manifest.
  3. THE CATEGORY TABLE: EJ Map keeps its own internal names; what the wizard SENDS (and what /lookup returns) uses the server's 12, in
     Sean's spellings. One table, here; an outgoing manifest whose categories are not all among the 12 is refused before packing.
  4. THE STUBS (the server is Sean's): sign-in, upload (both paths) and promo, writing under <work>/server_stub/. Nothing leaves the Mac.
     SERVER-SIDE, awaiting Sean: the account -> mapper-token bridge, the review / holding area, the promo rules + issuing + email.

  Pinned in RoundTripTest.cpp (WZ-SRV-*).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace ejmap::wizserver
{
// --- 1. the base URL -------------------------------------------------------------------------------------------------------------------
inline juce::String baseUrl() { return "https://www.echojay.ai"; }
inline juce::String endpoint (const juce::String& path) { return baseUrl() + "/api/wizard/v1/" + path; }

// --- 2. the upload path --------------------------------------------------------------------------------------------------------------
// 4.5 MB as Vercel states it (decimal); the multipart body is the zip + the manifest + headers, so 64 KB of headroom is kept
inline constexpr juce::int64 kDirectBodyMaxBytes = 4500000, kMultipartHeadroomBytes = 64 * 1024;
enum class UploadPath { direct, signedBlob };
inline UploadPath uploadPathFor (juce::int64 bundleBytes, juce::int64 manifestBytes)
{ return bundleBytes + manifestBytes + kMultipartHeadroomBytes <= kDirectBodyMaxBytes ? UploadPath::direct : UploadPath::signedBlob; }

// --- 3. the category table -------------------------------------------------------------------------------------------------------------
inline const juce::StringArray& serverCategories()
{
    static const juce::StringArray k { "compressor", "limiter", "eq", "de-esser", "delay", "reverb", "saturation", "gate", "transient_shaper", "channel_strip", "amp_sim", "pitch" };
    return k;
}
// internal (EJ Map / the wizard) -> the server's name; "" = an internal label, never sent as a category (multiband, modulation, meter,
// utility, instrument, midi, the signature's "dynamics"). A saturation row that read an amp sim's cabinet response is amp_sim.
inline juce::String toServer (const juce::String& internal, bool cabinetRead = false)
{
    const auto c = internal.toLowerCase();
    if (c == "compressor" || c == "wizard") return "compressor";
    if (c == "limiter" || c == "limitercomp") return "limiter";
    if (c == "eq" || c == "wizard_eq") return "eq";
    if (c == "deesser" || c == "de-esser") return "de-esser";
    if (c == "delay") return "delay";
    if (c == "reverb") return "reverb";
    if (c == "saturation") return cabinetRead ? "amp_sim" : "saturation";
    if (c == "amp_sim") return "amp_sim";
    if (c == "gate") return "gate";
    if (c == "transient" || c == "transient_shaper") return "transient_shaper";
    if (c == "strips" || c == "strip" || c == "channel_strip") return "channel_strip";
    if (c == "tuners" || c == "tuner" || c == "pitch") return "pitch";
    return {};
}
inline bool isServerCategory (const juce::String& c) { return serverCategories().contains (c); }
// every category an outgoing manifest carries must be one of the 12: profiles[].kind, other_categories[].category,
// needs_category[].leaning (when not null); held[] and out_of_scope[] carry none
inline juce::StringArray invalidOutgoing (const juce::var& manifest)
{
    juce::StringArray bad;
    auto each = [&] (const char* list, const char* field)
    {
        if (const auto* a = manifest.getProperty (list, {}).getArray())
            for (const auto& e : *a)
            {
                const auto v = e.getProperty (field, {});
                if (v.isVoid() || (juce::String (field) == "leaning" && v.toString().isEmpty())) continue;
                if (! isServerCategory (v.toString())) bad.add (juce::String (list) + "." + field + " '" + v.toString() + "' (" + e.getProperty ("product", "").toString() + ")");
            }
    };
    each ("profiles", "kind"); each ("other_categories", "category"); each ("needs_category", "leaning");
    for (const char* list : { "held", "out_of_scope" })
        if (const auto* a = manifest.getProperty (list, {}).getArray())
            for (const auto& e : *a) if (e.hasProperty ("category") || e.hasProperty ("kind")) bad.add (juce::String (list) + " carries a category (" + e.getProperty ("product", "").toString() + ")");
    return bad;
}

// --- 4. the stubs (Sean's endpoints; nothing leaves the Mac) -----------------------------------------------------------------------------
namespace stub
{
inline juce::var signIn (const juce::File& work, const juce::String& email, const juce::String& password)
{   // POST /sign-in {email, password} -> {account_id, display_name, token, expires}   [SERVER-SIDE: the account -> mapper-token bridge]
    if (! email.contains ("@") || password.isEmpty()) return {};
    auto* o = new juce::DynamicObject(); o->setProperty ("account_id", "acct_stub_" + juce::SHA256 (email.toLowerCase().toUTF8()).toHexString().substring (0, 10));
    o->setProperty ("email", email); o->setProperty ("display_name", email.upToFirstOccurrenceOf ("@", false, false)); o->setProperty ("stub", true);
    o->setProperty ("endpoint", endpoint ("sign-in"));
    const juce::var v (o);   // ONE owner
    work.getChildFile ("account.json").replaceWithText (juce::JSON::toString (v));
    return v;
}
// the server's genuine check, stubbed: the manifest schema only   [SERVER-SIDE: the review / holding area]
inline juce::var receiptFor (const juce::File& work, const juce::File& stored, const juce::var& m, const juce::var& account, const juce::String& path)
{
    const bool genuine = stored.existsAsFile() && m.getProperty ("schema", "").toString() == "ej_contribution/0";
    const auto id = "rcpt_stub_" + juce::String::toHexString (juce::Time::currentTimeMillis());
    auto* r = new juce::DynamicObject(); r->setProperty ("receipt_id", id); r->setProperty ("account_id", account.getProperty ("account_id", {}));
    r->setProperty ("genuine", genuine); r->setProperty ("path", path); r->setProperty ("bytes", stored.getSize());
    r->setProperty ("profiles_received", m.getProperty ("profiles", {}).size()); r->setProperty ("confirmations_received", m.getProperty ("confirmations", {}).size());
    r->setProperty ("crashed_reported", m.getProperty ("crashed", {}).size()); r->setProperty ("needs_category_received", m.getProperty ("needs_category", {}).size());
    const juce::var v (r);
    const auto dir = work.getChildFile ("server_stub/uploads"); dir.createDirectory();
    dir.getChildFile (id + ".receipt.json").replaceWithText (juce::JSON::toString (v));
    return v;
}
// the upload, both paths: direct (POST /contributions, multipart) up to 4.5 MB; else the signed blob (upload-url -> PUT -> complete)
inline juce::var upload (const juce::File& work, const juce::File& zip, const juce::File& manifest, const juce::var& account)
{
    const auto m = juce::JSON::parse (manifest.loadFileAsString());
    if (uploadPathFor (zip.getSize(), manifest.getSize()) == UploadPath::direct)
    {   // POST <base>/api/wizard/v1/contributions (multipart: manifest + bundle)
        const auto dir = work.getChildFile ("server_stub/uploads"); dir.createDirectory();
        const auto stored = dir.getChildFile ("direct_" + juce::String::toHexString (juce::Time::currentTimeMillis()) + ".zip"); zip.copyFileTo (stored);
        auto r = receiptFor (work, stored, m, account, "direct"); r.getDynamicObject()->setProperty ("endpoint", endpoint ("contributions")); return r;
    }
    // 1) POST /contributions/upload-url {bytes, bundle_sha256} -> {upload_url, blob_ref, expires}: the stub's "signed URL" is a local folder
    const auto blobRef = "blob_stub_" + juce::SHA256 (zip).toHexString().substring (0, 16);
    const auto signedDir = work.getChildFile ("server_stub/signed_blob"); signedDir.createDirectory();
    const auto target = signedDir.getChildFile (blobRef + ".zip");
    // 2) PUT the zip to upload_url
    if (! zip.copyFileTo (target)) return {};
    // 3) POST /contributions/complete {blob_ref, manifest} -> the receipt
    auto r = receiptFor (work, target, m, account, "signed_blob");
    r.getDynamicObject()->setProperty ("blob_ref", blobRef); r.getDynamicObject()->setProperty ("upload_url", "file://" + target.getFullPathName());
    r.getDynamicObject()->setProperty ("endpoint", endpoint ("contributions/complete"));
    return r;
}
inline juce::var promo (const juce::File& work, const juce::var& account, const juce::var& receipt, bool rewardWithoutNewProfiles)
{   // POST /promo {receipt_id} -> {code, issued_now, emailed_to}; one code per account   [SERVER-SIDE: promo rules + issuing + email]
    if (! (bool) receipt.getProperty ("genuine", false)) return {};
    if ((int) receipt.getProperty ("profiles_received", 0) == 0 && ! rewardWithoutNewProfiles) return {};
    const auto ledgerF = work.getChildFile ("server_stub/promo_ledger.json"); auto ledger = juce::JSON::parse (ledgerF.loadFileAsString());
    if (! ledger.isObject()) ledger = juce::var (new juce::DynamicObject());
    const auto acct = account.getProperty ("account_id", "").toString();
    auto* o = new juce::DynamicObject();
    if (ledger.hasProperty (acct)) { o->setProperty ("code", ledger[acct.toRawUTF8()]); o->setProperty ("issued_now", false); }
    else
    {
        const auto code = "EJ-FREE-" + juce::SHA256 ((acct + "|wizard-v1").toUTF8()).toHexString().substring (0, 8).toUpperCase();
        ledger.getDynamicObject()->setProperty (acct, code); ledgerF.getParentDirectory().createDirectory(); ledgerF.replaceWithText (juce::JSON::toString (ledger));
        o->setProperty ("code", code); o->setProperty ("issued_now", true);
        const auto outbox = work.getChildFile ("server_stub/outbox"); outbox.createDirectory();
        outbox.getChildFile (acct + ".txt").replaceWithText ("To: " + account.getProperty ("email", "").toString() + "\nSubject: Your free EchoJay month\n\nThank you for helping EchoJay learn your plugins. Your code: " + code + "\n");
    }
    o->setProperty ("emailed_to", account.getProperty ("email", {}));
    return juce::var (o);
}
} // namespace stub
} // namespace ejmap::wizserver
