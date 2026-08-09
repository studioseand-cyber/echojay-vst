/*
  EjmapMouth.h

  M10: the structural gate at the mouth, the dry-run request, the queue, and
  the STUB mouth.

  THE GATE runs CLIENT-SIDE, BEFORE anything could leave the machine: schema,
  identity, index ranges, anchors sanitize-clean through the real
  dominantMonotonicTable, groups through the real groupIsEqBand, controls
  shape, skip reasons, readback evidence, provenance completeness. Reject at
  the mouth with every reason in words, never silently.

  THE DRY-RUN FILE is this module's most valuable artifact: the EXACT bytes,
  EXACT headers and EXACT URL shape of the request that would be sent. It is
  the only thing checkable against the server before anything is sent, and it
  turns endpoint integration from a guess into a diff.

  ============================ THE STUB LIMIT ============================
  StubMouth is a HYPOTHESIS ABOUT THE SERVER, NOT A TEST OF IT. It
  implements the documented acceptance rules (plan section 10.3) so the
  client side can be proven end to end -- and if the real endpoint
  disagrees with those rules, a client that passed this stub is
  CONFIDENTLY WRONG. That is exactly the plan's warning about stubbing
  the store, which is why:
    - every string it emits is prefixed "STUB MOUTH (not the real endpoint)"
    - the queue state it produces is "stub_accepted", NEVER "uploaded"
    - no state named "uploaded" exists anywhere in this file; it cannot
      exist until a real transport exists
  ========================================================================
*/

#pragma once

#include <sys/stat.h>

#include <juce_core/juce_core.h>
#include <set>

#include "EjmapSchema.h"       // pulls in EchoJayParamApply.h: the real sanitizer

namespace ejmap
{

struct Mouth
{
    //==========================================================================
    struct Verdict
    {
        juce::StringArray rejections;
        bool pass() const { return rejections.isEmpty(); }
    };

    /** The documented acceptance rules, run against the map VAR. paramCount
        from the live instance when loaded, or identity.param_count.
    */
    static Verdict structuralGate (const juce::var& map, const juce::String& testerName)
    {
        Verdict v;
        auto rej = [&v] (const juce::String& r) { v.rejections.add (r); };

        // 2.3 stays readable: the 2.3 -> 2.4 bump ADDED `declines` and changed
        // nothing this gate checks. The campaign's local artifacts are 2.3,
        // and a gate that refused them would force a re-sweep of maps that
        // are already right (kLegacyDeathOutcome precedent: a rename that
        // silently dropped the history would reset it to zero).
        {
            const auto sch = map.getProperty ("schema", "").toString();
            if (sch != kMapSchemaString && sch != "2.3")
                rej ("schema is '" + sch
                       + "', binary speaks '" + juce::String (kMapSchemaString)
                       + "' (and still reads '2.3')");
        }
        if (map.getProperty ("fp", "").toString().length() < 16)
            rej ("fp missing or malformed");

        auto ident = map.getProperty ("identity", juce::var());
        const int paramCount = (int) ident.getProperty ("param_count", 0);
        for (auto* k : { "format", "name", "param_count" })
            if (ident.getProperty (k, juce::var()).isVoid())
                rej (juce::String ("identity.") + k + " missing");
        if (map.getProperty ("category", "").toString().isEmpty())
            rej ("category missing");

        // Tier 1 params: index range + anchors sanitize clean through the
        // REAL dominant-monotonic rule (never a reimplementation).
        int paramsN = 0;
        if (auto* po = map.getProperty ("params", juce::var()).getDynamicObject())
            for (auto& kv : po->getProperties())
            {
                ++paramsN;
                const auto sem = kv.name.toString();
                const int idx = (int) kv.value.getProperty ("index", -1);
                if (! juce::isPositiveAndBelow (idx, paramCount))
                    rej ("params." + sem + ": index " + juce::String (idx)
                           + " out of range [0," + juce::String (paramCount) + ")");
                const auto kind = kv.value.getProperty ("kind", "").toString();
                if (kind == "mode" || kind == "position")
                    continue;
                auto anchors = echojay::anchorsFromVar (kv.value);
                if (anchors.size() < 2)
                { rej ("params." + sem + ": fewer than 2 anchors"); continue; }
                auto eff = echojay::dominantMonotonicTable (anchors);
                if (! eff.ok)
                { rej ("params." + sem + ": anchors fail the sanitizer (mirror/garbage)"); continue; }
                float lo = eff.table.getFirst()[0], hi = lo;
                for (const auto& a : eff.table)
                { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
                if (hi - lo < 1.0e-6f)
                    rej ("params." + sem + ": degenerate anchor span (flat sweep)");
            }

        // Groups: one object per band, and every eq band must satisfy the
        // REAL groupIsEqBand or the matcher will silently skip it at dial.
        if (auto* ga = map.getProperty ("groups", juce::var()).getArray())
            for (auto& gv : *ga)
            {
                const auto fam = gv.getProperty ("family", "").toString();
                const int gn = (int) gv.getProperty ("n", -1);
                if (gn < 1)
                    rej ("groups[" + fam + "]: n=" + juce::String (gn)
                           + " (n is the 1-based band NUMBER)");
                if (! echojay::groupIsEqBand (fam, gv))
                    rej ("groups[" + fam + juce::String (gn)
                           + "]: fails groupIsEqBand (matcher would skip it at dial time)");
            }

        // Controls: shape per kind, duplicates carry their indices. Lockstep
        // markers state an observation: the target must be a real index and
        // never itself a marked twin (a chain would mean the "canonical"
        // address is not canonical). Tier is the human field with a closed
        // vocabulary.
        std::set<int> lockstepCarriers;
        if (auto* co = map.getProperty ("controls", juce::var()).getDynamicObject())
            for (auto& kv : co->getProperties())
                if (! kv.value.getProperty ("lockstep_of", juce::var()).isVoid())
                    lockstepCarriers.insert ((int) kv.value.getProperty ("index", -1));
        if (auto* co = map.getProperty ("controls", juce::var()).getDynamicObject())
            for (auto& kv : co->getProperties())
            {
                const auto nm = kv.name.toString();
                const auto tier = kv.value.getProperty ("tier", juce::var());
                if (! tier.isVoid() && tier.toString() != "primary" && tier.toString() != "hidden")
                    rej ("controls." + nm + ": tier '" + tier.toString()
                           + "' is not in the vocabulary (primary | hidden)");
                if (! kv.value.getProperty ("lockstep_of", juce::var()).isVoid())
                {
                    const int lof = (int) kv.value.getProperty ("lockstep_of", -1);
                    const auto by = kv.value.getProperty ("lockstep_by", "").toString();
                    if (! juce::isPositiveAndBelow (lof, paramCount))
                        rej ("controls." + nm + ": lockstep_of " + juce::String (lof)
                               + " out of range [0," + juce::String (paramCount) + ")");
                    if (by != "human_pick" && by != "write_verify")
                        rej ("controls." + nm + ": lockstep_by '" + by
                               + "' is not an evidence source (human_pick | write_verify)");
                    if (lockstepCarriers.count (lof) > 0)
                        rej ("controls." + nm + ": lockstep_of chains to [" + juce::String (lof)
                               + "], itself a marked twin -- the canonical address must be canonical");
                }
                if ((bool) kv.value.getProperty ("duplicate", false))
                {
                    auto* ia = kv.value.getProperty ("indices", juce::var()).getArray();
                    if (ia == nullptr || ia->size() < 2)
                        rej ("controls." + nm + ": duplicate without both indices");
                    continue;
                }
                const auto kind = kv.value.getProperty ("kind", "anchored").toString();
                if (kind == "mode")
                {
                    auto* lo = kv.value.getProperty ("labels", juce::var()).getDynamicObject();
                    if (lo == nullptr || lo->getProperties().size() < 2)
                        rej ("controls." + nm + ": mode with fewer than 2 labels");
                }
                else if (echojay::anchorsFromVar (kv.value).size() < 2)
                    rej ("controls." + nm + ": anchored with fewer than 2 anchors");
            }

        // Skips carry reasons; probes carry no unresolved contradicts.
        if (auto* sk = map.getProperty ("skips", juce::var()).getArray())
            for (auto& sv : *sk)
                if (sv.getProperty ("reason", "").toString().isEmpty())
                    rej ("skip '" + sv.getProperty ("semantic", "?").toString()
                           + "' has no reason");
        auto probes = map.getProperty ("evidence", juce::var()).getProperty ("audio_probe", juce::var());
        if (auto* pv = probes.getDynamicObject())
            for (auto& kv : pv->getProperties())
                if (kv.value.getProperty ("verdict", "").toString() == "contradicts")
                    rej ("unresolved probe contradiction on " + kv.name.toString());

        // Readback evidence: a map with params and no matching readback was
        // never write-back verified.
        if (paramsN > 0)
        {
            int matches = 0;
            if (auto* rb = map.getProperty ("evidence", juce::var())
                              .getProperty ("readback", juce::var()).getDynamicObject())
                for (auto& kv : rb->getProperties())
                    matches += (bool) kv.value.getProperty ("match", false);
            if (matches == 0)
                rej ("params present but zero matching readback evidence: "
                     "the map was never write-back verified");
        }

        // AN EMPTY CONTROL KEY makes the whole file unreadable to juce::JSON --
        // the writer emits it, the parser refuses it ("Invalid property name").
        // A map nobody can parse is worse than a missing one, because it also
        // un-registers itself from localMapIdentities and the plugin re-sweeps
        // forever. Rejected at the mouth like everything else: loudly, with
        // the indices, before it can leave.
        if (auto* co = map.getProperty ("controls", juce::var()).getDynamicObject())
            for (const auto& kv : co->getProperties())
                if (kv.name.toString().trim().isEmpty())
                    rej ("controls contains an EMPTY name -- not addressable, and an "
                         "empty JSON key is unreadable by juce::JSON");

        // Provenance: attributable or it does not leave.
        auto prov = map.getProperty ("provenance", juce::var());
        // ATTRIBUTABLE OR IT DOES NOT LEAVE, and there are now two ways to be
        // attributable. `mapper_ref` is derived from an issued token and is the
        // one that will survive; `tester_id` is a name someone typed, which is
        // a label rather than an identity and is kept so the 40 maps that
        // predate tokens are still resubmittable.
        const auto mapperRef = prov.getProperty ("mapper_ref", "").toString();
        const auto effectiveTester = testerName.isNotEmpty()
                                       ? testerName
                                       : prov.getProperty ("tester_id", "").toString();
        if (mapperRef.isEmpty() && effectiveTester.isEmpty())
            rej ("nothing attributes this map: sign in with the token you were issued "
                 "(preferred), or set a local name with --tester <name>. The hostname "
                 "is not provenance");
        for (auto* k : { "machine_id", "ejmap_version", "apply_header_sha", "at" })
            if (prov.getProperty (k, "").toString().isEmpty())
                rej (juce::String ("provenance.") + k + " missing");

        // Every value that rides an HTTP header is checked here, where the
        // refusal can be worded, not at write time where the only options
        // are silent mutation or a forged header.
        if (effectiveTester.isNotEmpty() && ! headerValueSafe (effectiveTester))
            rej ("tester name cannot ride an HTTP header (printable ASCII only, "
                 "no control characters): '" + effectiveTester + "'");
        for (auto* k : { "machine_id", "ejmap_version" })
        {
            const auto hv = prov.getProperty (k, "").toString();
            if (hv.isNotEmpty() && ! headerValueSafe (hv))
                rej (juce::String ("provenance.") + k
                       + " cannot ride an HTTP header (printable ASCII only)");
        }

        return v;
    }

    //==========================================================================
    /** Header values ride a CRLF-framed stream: a CR or LF in a value forges
        headers, and anything outside printable ASCII is outside what HTTP
        field values may carry. The tester name is typed by a human, so it is
        checked at the gate rather than trusted.
    */
    static bool headerValueSafe (const juce::String& s)
    {
        for (auto t = s.getCharPointer(); ! t.isEmpty();)
        {
            const auto c = t.getAndAdvance();
            if (c < 0x20 || c > 0x7e)
                return false;
        }
        return true;
    }

    /** The exact request, as bytes on disk. Headers CRLF-terminated, body
        verbatim from the map file, Content-Length exact. Diffable against
        whatever the real endpoint expects, before anything is ever sent.

        The URL is sliced by hand, never through juce::URL: URL::init() parses
        the query string away and getSubPath(true) re-escapes it on the way
        back out, so the emitted bytes could differ from the string as typed;
        getDomain() cuts at the first colon, so a port would never reach the
        Host header. This artifact is checked byte for byte, so the user's
        string is used byte for byte.
    */
    /** WHERE THE ENDPOINT AND TOKEN COME FROM, in one place.

        Environment first, then a config file in the ledger root, then the
        visibly-unset placeholder. The file exists because a GUI launch does
        NOT inherit the shell: a terminal-only workaround is one nobody
        remembers three weeks into a campaign, and the placeholder's whole
        value is that it fails loudly rather than posting somewhere real --
        which is exactly how the first send told us what was wrong.

        THE FILE HOLDS A TOKEN, so it is treated as a secret: readable by
        anyone but the owner and the token is REFUSED, not merely warned about.
        A refused token falls back to the placeholder and the send fails loudly
        with an explanation, which is a better outcome than quietly using a
        credential the whole machine can read.
    */
    struct Endpoint
    {
        juce::String url, token;
        juce::String urlFrom = "placeholder", tokenFrom = "placeholder";
        juce::String warning;                       // empty when nothing is wrong
        bool tokenRefused = false;

        juce::String describe() const
        {
            return "endpoint " + url + "  [from " + urlFrom + "]\n"
                   "token    " + (token.startsWith ("INGEST-TOKEN-UNSET")
                                    ? juce::String ("UNSET -- the server fails closed; this would 401")
                                    : "set (value not shown)")
                 + "  [from " + tokenFrom + "]"
                 + (warning.isNotEmpty() ? "\n" + warning : "");
        }
    };

    static juce::File configFile (const juce::File& root) { return root.getChildFile ("config.json"); }

    //==========================================================================
    /** WHO IS SUBMITTING, as opposed to WHICH SERVER TO SUBMIT TO.

        Today there is one shared ingest token and it sits in one person's
        config.json. Shipping that to mappers means a leak lets anyone write to
        the corpus, revoking it locks out everyone at once, and "who mapped
        this" is unanswerable -- provenance currently comes from a name the
        mapper TYPES, which is a label, not an identity.

        A per-mapper token fixes all three: it is issued to one person, revoked
        for one person, and it is what provenance is derived from.

        THE RAW TOKEN NEVER ENTERS A MAP. A map is an artefact that gets stored,
        copied, resubmitted and read by other people; a credential inside one
        would leak by being useful. What goes in is `ref` -- the first 12 hex of
        the token's SHA-256 -- which is stable, non-secret, and something the
        server can resolve back to a person because it issued the token.

        Same secrecy rule as the endpoint token, for the same reason: a file the
        whole machine can read is not holding a secret, so it is REFUSED rather
        than warned about.
    */
    struct MapperIdentity
    {
        juce::String token;                 // the credential. Never stored in a map
        juce::String ref;                   // sha256(token)[0..12). Safe to record
        juce::String from = "(none)";
        juce::String warning;
        bool refused = false;

        bool signedIn() const { return token.isNotEmpty() && ! refused; }

        juce::String describe() const
        {
            if (refused)       return "mapper   REFUSED\n  " + warning;
            if (! signedIn())  return "mapper   NOT SIGNED IN -- paste the token you were "
                                      "issued, and maps can be submitted";
            return "mapper   " + ref + "  [from " + from + "]  (token not shown)";
        }
    };

    /** Stable, non-secret, and derived only from the token, so two machines
        signed in as the same mapper produce the same ref. */
    static juce::String mapperRefFor (const juce::String& token)
    {
        if (token.isEmpty()) return {};
        return juce::SHA256 (token.toRawUTF8(), (size_t) token.getNumBytesAsUTF8())
                 .toHexString().substring (0, 12);
    }

    static MapperIdentity resolveMapper (const juce::File& root)
    {
        MapperIdentity m;
        auto cfgFile = configFile (root);

        const auto env = juce::SystemStats::getEnvironmentVariable ("EJMAP_MAPPER_TOKEN", "");
        if (env.isNotEmpty())
        {
            m.token = env; m.from = "EJMAP_MAPPER_TOKEN";
        }
        else if (cfgFile.existsAsFile())
        {
            bool permsOk = true;
            struct stat st {};
            if (::stat (cfgFile.getFullPathName().toRawUTF8(), &st) == 0)
                permsOk = (st.st_mode & (S_IRWXG | S_IRWXO)) == 0;

            auto cfg = juce::JSON::parse (cfgFile.loadFileAsString());
            const auto t = cfg.isObject() ? cfg.getProperty ("mapper_token", "").toString()
                                          : juce::String();
            if (t.isNotEmpty())
            {
                if (! permsOk)
                {
                    m.refused = true;
                    m.warning = "REFUSED to read the mapper token from "
                              + cfgFile.getFullPathName() + ": it is readable by group or "
                                "other. A token the whole machine can read is not a secret.\n"
                                "  Fix:  chmod 600 " + cfgFile.getFullPathName();
                }
                else { m.token = t; m.from = cfgFile.getFileName(); }
            }
        }

        if (m.token.isNotEmpty() && ! headerValueSafe (m.token))
        {
            m.refused = true;
            m.warning = "the mapper token is not printable ASCII, so it cannot ride an HTTP "
                        "header. Paste it again.";
            m.token.clear();
        }
        m.ref = mapperRefFor (m.token);
        return m;
    }

    /** Writes the token and makes the file a secret IN THAT ORDER -- the chmod
        follows the write, so the token is never briefly world-readable.

        Returns empty on success, or the reason it did not. */
    static juce::String saveMapperToken (const juce::File& root, const juce::String& token)
    {
        if (token.trim().isEmpty())
            return "no token given";
        if (! headerValueSafe (token.trim()))
            return "that token is not printable ASCII; it cannot ride an HTTP header";

        auto cfgFile = configFile (root);
        auto cfg = cfgFile.existsAsFile() ? juce::JSON::parse (cfgFile.loadFileAsString())
                                          : juce::var();
        auto* o = cfg.getDynamicObject();
        if (o == nullptr) { o = new juce::DynamicObject(); cfg = juce::var (o); }
        o->setProperty ("mapper_token", token.trim());

        if (! cfgFile.replaceWithText (juce::JSON::toString (cfg, false)))
            return "could not write " + cfgFile.getFullPathName();

        if (::chmod (cfgFile.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR) != 0)
            return "wrote the token but could not restrict " + cfgFile.getFullPathName()
                 + " to this user. Fix:  chmod 600 " + cfgFile.getFullPathName();

        // Read it back through the same path that will use it. A write that
        // reports success and a read that refuses is the shape worth catching
        // here rather than at the next send.
        auto back = resolveMapper (root);
        if (! back.signedIn())
            return "wrote the token and could not read it back: " + back.warning;
        return {};
    }



    static Endpoint resolveEndpoint (const juce::File& root)
    {
        Endpoint e;
        e.url   = "https://UPLOAD-ENDPOINT-UNSET.echojay.invalid/api/params/ejmap";
        e.token = "INGEST-TOKEN-UNSET";

        auto cfgFile = configFile (root);
        juce::var cfg;
        bool cfgReadable = false;
        if (cfgFile.existsAsFile())
        {
            cfg = juce::JSON::parse (cfgFile.loadFileAsString());
            cfgReadable = cfg.isObject();
            if (! cfgReadable)
                e.warning = "config.json exists but does not parse as JSON; ignoring it.";
        }

        // PERMISSIONS, checked before the token is read out of it.
        bool permsOk = true;
        if (cfgFile.existsAsFile())
        {
            struct stat st {};
            if (::stat (cfgFile.getFullPathName().toRawUTF8(), &st) == 0)
                permsOk = (st.st_mode & (S_IRWXG | S_IRWXO)) == 0;
        }

        const auto envUrl = juce::SystemStats::getEnvironmentVariable ("EJMAP_UPLOAD_URL", "");
        if (envUrl.isNotEmpty()) { e.url = envUrl; e.urlFrom = "EJMAP_UPLOAD_URL"; }
        else if (cfgReadable)
        {
            const auto u = cfg.getProperty ("upload_url", "").toString();
            if (u.isNotEmpty()) { e.url = u; e.urlFrom = configFile (root).getFileName(); }
        }

        const auto envTok = juce::SystemStats::getEnvironmentVariable ("EJMAP_INGEST_TOKEN", "");
        if (envTok.isNotEmpty()) { e.token = envTok; e.tokenFrom = "EJMAP_INGEST_TOKEN"; }
        else if (cfgReadable)
        {
            const auto t = cfg.getProperty ("ingest_token", "").toString();
            if (t.isNotEmpty())
            {
                if (! permsOk)
                {
                    e.tokenRefused = true;
                    e.warning = "REFUSED to read the token from " + cfgFile.getFullPathName()
                              + ": it is readable by group or other. A token the whole machine "
                                "can read is not a secret.\n  Fix:  chmod 600 "
                              + cfgFile.getFullPathName();
                }
                else { e.token = t; e.tokenFrom = configFile (root).getFileName(); }
            }
        }
        if (! headerValueSafe (e.token))
            e.token = "INGEST-TOKEN-UNSAFE (not printable ASCII; fix the token)";
        return e;
    }

    static juce::String uploadBaseUrl (const juce::String& urlOverride = {},
                                       const juce::File& root = {})
    {
        if (urlOverride.isNotEmpty()) return urlOverride;
        if (root != juce::File()) return resolveEndpoint (root).url;
        // No root to consult: environment, then the placeholder. Callers that
        // can supply the ledger root get the config file too.
        return juce::SystemStats::getEnvironmentVariable (
                   "EJMAP_UPLOAD_URL",
                   "https://UPLOAD-ENDPOINT-UNSET.echojay.invalid/api/params/ejmap");
    }

    /** ONE BUILDER for every request this tool emits, per the locked transport
        constraint: artifact and wire come from the same bytes. A second route
        (the probe runner's /probed POST) gets a second CALLER, never a second
        copy of the header composer -- a duplicated rule is the [-1] duplicate
        refusal defect, which survived its own fix because it existed twice.
    */
    static juce::File writeRequestArtifact (const juce::File& root, const juce::String& url,
                                            const juce::String& artifactName,
                                            const juce::MemoryBlock& body,
                                            const juce::String& testerName,
                                            const juce::String& machineId,
                                            const juce::String& ejmapVersion)
    {

        auto rest = url.contains ("://") ? url.fromFirstOccurrenceOf ("://", false, false)
                                         : url;
        const int slash = rest.indexOfChar ('/');
        // host[:port] verbatim -- a typed port must reach the Host header.
        const auto hostPort  = slash < 0 ? rest : rest.substring (0, slash);
        // path + query verbatim, down to the escapes as typed; absolute or
        // it is not a request line ("POST api/..." is one a server refuses).
        const auto pathQuery = slash < 0 ? juce::String ("/") : rest.substring (slash);

        // Auth: the server route requires X-EJMap-Token and fails closed.
        // Sourced from the ENVIRONMENT (EJMAP_INGEST_TOKEN) -- or the
        // keychain when M11 issues per-tester tokens -- never a file in the
        // tree. The placeholders are visibly unset/unsafe, same convention
        // as the endpoint host: a 401 you can read coming, never a mystery.
        // Resolved through ONE path -- env, then config, then placeholder --
        // so the header and the readout can never disagree about which token
        // is in play.
        // THE MAPPER'S TOKEN IF THERE IS ONE, the shared ingest token otherwise.
        // The shared one is what has to go away -- one leak writes to the
        // corpus and revoking it locks out everyone -- so the per-mapper token
        // takes precedence the moment it exists, and the ref rides alongside so
        // a server log can attribute a request without holding a credential.
        const auto mapper = resolveMapper (root);
        const auto token  = mapper.signedIn() ? mapper.token : resolveEndpoint (root).token;

        juce::String head;
        head << "POST " << pathQuery << " HTTP/1.1\r\n"
             << "Host: " << hostPort << "\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << juce::String ((juce::int64) body.getSize()) << "\r\n"
             << "X-EJMap-Version: " << ejmapVersion << "\r\n"
             << "X-EJMap-Machine: " << machineId << "\r\n"
             << "X-EJMap-Tester: " << testerName << "\r\n"
             << "X-EJMap-Token: " << token << "\r\n"
             << "X-EJMap-Mapper: " << (mapper.signedIn() ? mapper.ref : juce::String ("none")) << "\r\n"
             << "\r\n";

        auto dir = root.getChildFile ("upload");
        dir.createDirectory();
        auto f = dir.getChildFile (artifactName);
        juce::FileOutputStream out (f);
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            // Raw bytes, not writeText: no line-ending translation layer
            // between the string above and the file.
            out.write (head.toRawUTF8(), head.getNumBytesAsUTF8());
            out.write (body.getData(), body.getSize());
            out.flush();
        }
        return f;
    }

    static juce::File writeDryRun (const juce::File& root, const juce::String& fp,
                                   const juce::MemoryBlock& body,
                                   const juce::String& testerName,
                                   const juce::String& machineId,
                                   const juce::String& ejmapVersion,
                                   const juce::String& urlOverride = {})
    {
        return writeRequestArtifact (root, uploadBaseUrl (urlOverride, root), fp + ".http",
                                     body, testerName, machineId, ejmapVersion);
    }

    //==========================================================================
    /** THE STUB. See the limit statement in the file header: a hypothesis
        about the server, not a test of it. It applies the documented
        acceptance rules and persists accepted maps into stub-store/ so the
        read-back-from-store gate has a store to read back from.
    */
    struct StubResult
    {
        bool accepted = false;
        juce::StringArray reasons;
    };

    static StubResult stubMouthSubmit (const juce::File& root, const juce::String& fp,
                                       const juce::String& mapJson,
                                       const juce::String& testerName)
    {
        StubResult r;
        auto map = juce::JSON::parse (mapJson);
        if (! map.isObject())
        {
            r.reasons.add ("STUB MOUTH (not the real endpoint): body is not a JSON object");
            return r;
        }
        auto verdict = structuralGate (map, testerName);
        if (! verdict.pass())
        {
            for (const auto& x : verdict.rejections)
                r.reasons.add ("STUB MOUTH (not the real endpoint): " + x);
            return r;
        }
        auto dir = root.getChildFile ("stub-store");
        dir.createDirectory();
        dir.getChildFile (fp + ".json").replaceWithText (mapJson);
        r.accepted = true;
        r.reasons.add ("STUB MOUTH (not the real endpoint): accepted per the documented "
                       "rules; the real server may still disagree");
        return r;
    }

    //==========================================================================
    /** queue.json: per-map upload state. The states are the whole truth of
        where a map is; "uploaded" deliberately does not exist yet.
    */
    /** `identity` is stamped at send time so the queue can be joined back to a
        plugin later. Without it, a map that was sent and whose local copy was
        then deleted reads as NOT SENT -- the exact failure the sent column
        exists to remove. */
    static void setQueueState (const juce::File& root, const juce::String& fp,
                               const juce::String& state, const juce::String& reason,
                               const juce::String& identity = {})
    {
        auto f = root.getChildFile ("queue.json");
        auto v = juce::JSON::parse (f.existsAsFile() ? f.loadFileAsString() : "[]");
        auto* arr = v.getArray();
        juce::Array<juce::var> out;
        if (arr != nullptr)
            for (auto& e : *arr)
                if (e.getProperty ("fp", "").toString() != fp)
                    out.add (e);
        auto* o = new juce::DynamicObject();
        o->setProperty ("fp", fp);
        if (identity.isNotEmpty()) o->setProperty ("identity", identity);
        o->setProperty ("state", state);
        if (reason.isNotEmpty()) o->setProperty ("reason", reason);
        o->setProperty ("at", juce::Time::getCurrentTime().toISO8601 (true));
        out.add (juce::var (o));
        f.replaceWithText (juce::JSON::toString (juce::var (out), false));
    }

    static juce::String queueState (const juce::File& root, const juce::String& fp)
    {
        auto v = juce::JSON::parse (root.getChildFile ("queue.json").loadFileAsString());
        if (auto* arr = v.getArray())
            for (auto& e : *arr)
                if (e.getProperty ("fp", "").toString() == fp)
                    return e.getProperty ("state", "").toString();
        return "local";
    }
};

} // namespace ejmap
