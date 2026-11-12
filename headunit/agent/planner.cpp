// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// What job.plan works out: see planner.hpp.

#include "planner.hpp"

#include <stdio.h>
#include <sys/stat.h>

#include <set>

#include "ed25519.hpp"
#include "fsutil.hpp"
#include "hex.hpp"
#include "semver.hpp"
#include "sha256.hpp"

namespace nbt {

namespace {

const int64_t kMaxArchiveBytes = 0xFFFFFFFFLL;     // zip64 is not allowed, so an archive is below 4 GiB

const InstalledPackage* findIn(const std::vector<InstalledPackage>& all, const std::string& id) {
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].manifest.id == id) return &all[i];
    }
    return 0;
}

void eraseFrom(std::vector<InstalledPackage>* all, const std::string& id) {
    for (size_t i = 0; i < all->size(); ++i) {
        if ((*all)[i].manifest.id == id) {
            all->erase(all->begin() + (long)i);
            return;
        }
    }
}

std::string number(uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return text;
}

void addProblem(Plan* plan, const char* code, const std::string& id, const std::string& message) {
    Problem p;
    p.code = code;
    p.id = id;
    p.message = message;
    plan->problems.push_back(p);
}

bool isHex(const std::string& text, size_t chars) {
    if (text.size() != chars) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// What a record of the package would say, before it is installed: enough for the checks of the steps after it.
InstalledPackage recordToBe(const Manifest& m, const std::string& keyHex) {
    InstalledPackage p;
    p.manifest = m;
    p.key = keyHex;
    for (size_t i = 0; i < m.placements.size(); ++i) {
        const Placement& placement = m.placements[i];
        InstalledFile f;
        f.dest = placement.dest;
        f.sha256 = m.contents[placement.content].sha256;
        f.size = m.contents[placement.content].size;
        f.mode = m.files[placement.file].mode;
        f.config = m.files[placement.file].config;
        p.files.push_back(f);
    }
    for (size_t i = 0; i < m.links.size(); ++i) {
        InstalledLink l;
        l.path = m.links[i].path;
        l.target = m.links[i].target;
        p.links.push_back(l);
    }
    return p;
}

// The real paths a package has files and links at.
void pathsOf(const Store& store, const InstalledPackage& p, std::set<std::string>* out) {
    for (size_t i = 0; i < p.files.size(); ++i) out->insert(store.resolve(p.manifest, p.files[i].dest));
    for (size_t i = 0; i < p.links.size(); ++i) out->insert(store.resolve(p.manifest, p.links[i].path));
}

// A package the job installs: a target, or a dependency taken from the offers.
struct Chosen {
    size_t offer;
    const OfferedManifest* offered;
};

// Dependencies before their dependants, in the order the targets were given. `state`: 0 not seen, 1 being
// visited, 2 placed.
void placeInOrder(size_t c, const std::vector<Chosen>& chosen, const std::map<std::string, size_t>& byId,
                  std::vector<int>* state, std::vector<size_t>* order, Plan* plan) {
    (*state)[c] = 1;
    const Manifest& m = chosen[c].offered->manifest;
    for (size_t i = 0; i < m.depends.size(); ++i) {
        std::map<std::string, size_t>::const_iterator it = byId.find(m.depends[i].id);
        if (it == byId.end() || it->second == c) continue;
        if ((*state)[it->second] == 1) {
            addProblem(plan, "conflict", m.id, m.id + " and " + m.depends[i].id + " depend on each other, so neither can go first");
        } else if ((*state)[it->second] == 0) {
            placeInOrder(it->second, chosen, byId, state, order, plan);
        }
    }
    (*state)[c] = 2;
    order->push_back(c);
}

}  // namespace

const std::string& strongerRestart(const std::string& a, const std::string& b) {
    if (a == "required" || b == "none") return a;
    if (b == "required" || a == "none") return b;
    return a;   // both hmi-restart
}

std::string installedStateOf(const std::vector<InstalledPackage>& installed) {
    std::set<std::string> lines;
    for (size_t i = 0; i < installed.size(); ++i) {
        lines.insert(installed[i].manifest.id + " " + installed[i].manifest.version + " " + installed[i].key);
    }
    std::string out;
    for (std::set<std::string>::const_iterator it = lines.begin(); it != lines.end(); ++it) out += *it + "\n";
    return out;
}

bool parsePlanRequest(const json::Value& params, PlanRequest* out, std::string* why) {
    *out = PlanRequest();
    json::Value install = params.find("install"), remove = params.find("remove"), offers = params.find("offers");
    if ((install.present() && !install.isArray()) || (remove.present() && !remove.isArray()) ||
        (offers.present() && !offers.isArray())) {
        *why = "install, remove and offers are arrays";
        return false;
    }
    std::set<std::string> ids;
    for (size_t i = 0; i < install.size(); ++i) {
        json::Value id = install.at(i).find("id"), version = install.at(i).find("version");
        if (!id.isString() || !validPackageId(id.string()) || !version.isString() || !validVersion(version.string())) {
            *why = "an install target needs an id and an exact version";
            return false;
        }
        if (!ids.insert(id.string()).second) {
            *why = id.string() + " is named twice";
            return false;
        }
        InstallTarget t;
        t.id = id.string();
        t.version = version.string();
        out->install.push_back(t);
    }
    for (size_t i = 0; i < remove.size(); ++i) {
        json::Value id = remove.at(i).find("id"), deleteData = remove.at(i).find("deleteData");
        if (!id.isString() || !validPackageId(id.string()) || (deleteData.present() && !deleteData.isBool())) {
            *why = "a removal target needs an id, and deleteData is true or false";
            return false;
        }
        if (!ids.insert(id.string()).second) {
            *why = id.string() + " is named twice";
            return false;
        }
        RemoveTarget t;
        t.id = id.string();
        t.deleteData = deleteData.isBool() && deleteData.boolean();
        out->remove.push_back(t);
    }
    if (out->install.empty() && out->remove.empty()) {
        *why = "nothing to install and nothing to remove";
        return false;
    }
    for (size_t i = 0; i < offers.size(); ++i) {
        json::Value o = offers.at(i);
        json::Value id = o.find("id"), version = o.find("version"), sha = o.find("sha256"), size = o.find("size");
        json::Value manifestSha = o.find("manifestSha256"), signature = o.find("signature");
        if (!id.isString() || !validPackageId(id.string()) || !version.isString() || !validVersion(version.string()) ||
            !sha.isString() || !validSha256Hex(sha.string()) || !size.isInteger() || size.integer() < 1 ||
            size.integer() > kMaxArchiveBytes || !manifestSha.isString() || !validSha256Hex(manifestSha.string()) ||
            !signature.isString() || !isHex(signature.string(), kEd25519SignatureBytes * 2)) {
            *why = "offer " + number(i + 1) + " is not as the message reference says";
            return false;
        }
        Offer offer;
        offer.id = id.string();
        offer.version = version.string();
        offer.sha256 = sha.string();
        offer.size = (uint64_t)size.integer();
        offer.manifestSha256 = manifestSha.string();
        offer.signature = signature.string();
        out->offers.push_back(offer);
    }
    return true;
}

void checkOfferedManifest(const Offer& offer, const std::string& bytes, const std::vector<TrustedKey>& keys,
                          OfferedManifest* out) {
    *out = OfferedManifest();
    uint8_t digest[kSha256Bytes];
    sha256(bytes.data(), bytes.size(), digest);
    if (toHex(digest, sizeof digest) != offer.manifestSha256) {
        out->error.fail("fetch-failed", "the manifest that arrived does not have the offered hash");
        return;
    }
    // As with an archive, the signature is checked before the manifest is parsed.
    uint8_t signature[kEd25519SignatureBytes];
    int key = -1;
    if (fromHex(offer.signature, signature, sizeof signature)) {
        for (size_t i = 0; i < keys.size() && key < 0; ++i) {
            if (ed25519Verify(keys[i].key, bytes.data(), bytes.size(), signature)) key = (int)i;
        }
    }
    if (key < 0) {
        out->error.fail("bad-signature", "no key the agent trusts verifies the manifest's signature");
        return;
    }
    if (!parseManifest(bytes.data(), bytes.size(), &out->manifest, &out->error)) return;
    out->keyHex = toHex(keys[(size_t)key].key, sizeof keys[(size_t)key].key);
    out->keyEmbedded = keys[(size_t)key].embedded;
}

bool Planner::resolve(const PlanRequest& request, const std::map<std::string, OfferedManifest>& fetched, Plan* plan,
                      size_t* need) {
    *plan = Plan();
    const Store& store = engine_->store();
    std::vector<InstalledPackage> installed;
    std::string why;
    if (!store.loadAll(&installed, &why)) {
        addProblem(plan, "internal", "", why);
        return true;
    }
    plan->installedState = installedStateOf(installed);

    std::set<std::string> removing;
    for (size_t i = 0; i < request.remove.size(); ++i) removing.insert(request.remove[i].id);

    // ---- 1. the targets, each with its offer and that offer's manifest
    std::vector<Chosen> chosen;
    std::map<std::string, size_t> chosenById;
    for (size_t i = 0; i < request.install.size(); ++i) {
        const InstallTarget& t = request.install[i];
        size_t k = 0;
        while (k < request.offers.size() && !(request.offers[k].id == t.id && request.offers[k].version == t.version)) ++k;
        if (k == request.offers.size()) {
            addProblem(plan, "no-offer", t.id, "there is no offer for " + t.id + " " + t.version);
            continue;
        }
        std::map<std::string, OfferedManifest>::const_iterator it = fetched.find(request.offers[k].manifestSha256);
        if (it == fetched.end()) {
            *need = k;
            return false;
        }
        const OfferedManifest& offered = it->second;
        if (!offered.error.code.empty()) {
            addProblem(plan, offered.error.code.c_str(), t.id, offered.error.message);
            continue;
        }
        if (offered.manifest.id != t.id || offered.manifest.version != t.version) {
            addProblem(plan, "offer-mismatch", t.id,
                       "the offered manifest is of " + offered.manifest.id + " " + offered.manifest.version);
            continue;
        }
        Chosen c;
        c.offer = k;
        c.offered = &offered;
        chosenById[t.id] = chosen.size();
        chosen.push_back(c);
    }

    // ---- 2. dependencies that nothing installed will satisfy, from the offers
    std::set<std::string> unresolved;   // dependencies already reported, one way or another
    for (size_t c = 0; c < chosen.size(); ++c) {
        const Manifest& m = chosen[c].offered->manifest;
        for (size_t i = 0; i < m.depends.size(); ++i) {
            const ManifestDependency& d = m.depends[i];
            if (d.id == m.id || chosenById.count(d.id) != 0) continue;
            const InstalledPackage* have = findIn(installed, d.id);
            if (removing.count(d.id) != 0) {
                if (unresolved.insert(d.id).second) {
                    addProblem(plan, "required-by", d.id, m.id + ", which this job installs, depends on it");
                }
                continue;
            }
            if (have != 0 && versionInRange(have->manifest.version, d.range)) continue;

            size_t best = request.offers.size();
            for (size_t k = 0; k < request.offers.size(); ++k) {
                const Offer& o = request.offers[k];
                if (o.id != d.id || !versionInRange(o.version, d.range)) continue;
                int order = 0;
                if (best == request.offers.size() ||
                    (compareVersionStrings(o.version, request.offers[best].version, &order) && order > 0)) {
                    best = k;
                }
            }
            if (best == request.offers.size()) {
                MissingDependency miss;
                miss.id = d.id;
                miss.range = d.range;
                miss.requiredBy = m.id;
                plan->missing.push_back(miss);
                unresolved.insert(d.id);
                continue;
            }
            const Offer& offer = request.offers[best];
            std::map<std::string, OfferedManifest>::const_iterator it = fetched.find(offer.manifestSha256);
            if (it == fetched.end()) {
                *need = best;
                return false;
            }
            const OfferedManifest& offered = it->second;
            if (!offered.error.code.empty()) {
                addProblem(plan, offered.error.code.c_str(), d.id, offered.error.message);
                unresolved.insert(d.id);
                continue;
            }
            if (offered.manifest.id != offer.id || offered.manifest.version != offer.version) {
                addProblem(plan, "offer-mismatch", d.id,
                           "the offered manifest is of " + offered.manifest.id + " " + offered.manifest.version);
                unresolved.insert(d.id);
                continue;
            }
            Chosen dep;
            dep.offer = best;
            dep.offered = &offered;
            chosenById[d.id] = chosen.size();
            chosen.push_back(dep);
        }
    }

    // An update of the agent ends with the agent's restart, and what came after it in the job would be run by
    // a version that is not confirmed yet. So it is alone in its job.
    if (chosenById.count(kAgentPackageId) != 0 && (chosen.size() > 1 || !request.remove.empty())) {
        addProblem(plan, "conflict", kAgentPackageId, "an update of the agent is alone in its job: plan it by itself, and the rest after it");
    }

    // ---- 3. removals first, a package before the ones it depends on
    std::vector<InstalledPackage> sim = installed;   // the installed set as the steps so far will have left it
    std::set<std::string> undecided;                 // real paths those steps touch
    std::vector<RemoveTarget> pending;
    for (size_t i = 0; i < request.remove.size(); ++i) {
        if (findIn(installed, request.remove[i].id) == 0) {
            addProblem(plan, "not-installed", request.remove[i].id, request.remove[i].id + " is not installed");
        } else {
            pending.push_back(request.remove[i]);
        }
    }
    for (bool progress = true; progress && !pending.empty();) {
        progress = false;
        for (size_t i = 0; i < pending.size(); ++i) {
            std::vector<Problem> problems;
            checkRemove(pending[i].id, sim, &problems);
            if (!problems.empty()) continue;
            const InstalledPackage* package = findIn(sim, pending[i].id);
            PlanStep step;
            step.action = "remove";
            step.id = pending[i].id;
            step.version = package->manifest.version;
            step.deleteData = pending[i].deleteData;
            plan->steps.push_back(step);
            pathsOf(store, *package, &undecided);
            eraseFrom(&sim, pending[i].id);
            pending.erase(pending.begin() + (long)i);
            progress = true;
            break;
        }
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        // Not said twice, if a package this job installs has been named as the reason already.
        if (unresolved.count(pending[i].id) == 0) checkRemove(pending[i].id, sim, &plan->problems);
    }

    // ---- 4. installs, dependencies first, each checked against what the steps before it leave behind
    std::vector<int> state(chosen.size(), 0);
    std::vector<size_t> order;
    for (size_t c = 0; c < chosen.size(); ++c) {
        if (state[c] == 0) placeInOrder(c, chosen, chosenById, &state, &order, plan);
    }
    uint64_t largestUnpacked = 0;
    uint64_t placedBesideStaging = 0;   // bytes the job installs on the file system that holds staging
    const std::string staging = engine_->platform()->stagingDir();
    for (size_t n = 0; n < order.size(); ++n) {
        const Chosen& c = chosen[order[n]];
        const OfferedManifest& offered = *c.offered;
        const Manifest& m = offered.manifest;
        const Offer& offer = request.offers[c.offer];
        const InstalledPackage* old = findIn(sim, m.id);

        PlanStep step;
        // The agent's own package comes after the version that runs, whether or not that one has a record.
        bool own = m.id == kAgentPackageId;
        step.action = old != 0 || own ? "upgrade" : "install";
        step.id = m.id;
        step.version = m.version;
        if (own) step.oldVersion = engine_->agentVersion();
        else if (old != 0) step.oldVersion = old->manifest.version;
        step.sha256 = offer.sha256;
        step.size = offer.size;
        step.manifestSha256 = offer.manifestSha256;
        plan->steps.push_back(step);

        std::vector<Problem> problems;
        std::vector<MissingDependency> missing;
        checkInstall(m, offered.keyHex, offered.keyEmbedded, sim, engine_->agentVersion(), &problems, &missing);
        for (size_t i = 0; i < missing.size(); ++i) {
            if (unresolved.count(missing[i].id) != 0) continue;
            std::map<std::string, size_t>::const_iterator it = chosenById.find(missing[i].id);
            if (it != chosenById.end()) {
                Problem p;
                p.code = "conflict";
                p.id = m.id;
                p.message = "needs " + missing[i].id + " " + missing[i].range + ", and the job installs " +
                            chosen[it->second].offered->manifest.version;
                problems.push_back(p);
            } else {
                plan->missing.push_back(missing[i]);
            }
        }
        if (problems.empty()) {
            // The file checks, as far as a plan can make them. A fault of the file system is no reason to refuse a
            // plan: the step meets it again and reports it.
            Error error;
            if (!engine_->precheck(m, sim, undecided, &error) && error.code != "io-error" && error.code != "internal") {
                Problem p;
                p.code = error.code;
                p.id = m.id;
                p.message = error.message;
                problems.push_back(p);
            }
        }
        plan->problems.insert(plan->problems.end(), problems.begin(), problems.end());

        if (old != 0) {
            pathsOf(store, *old, &undecided);
            eraseFrom(&sim, m.id);
        }
        sim.push_back(recordToBe(m, offered.keyHex));

        std::vector<std::string> flags = riskFlags(m);
        for (size_t i = 0; i < flags.size(); ++i) {
            bool have = false;
            for (size_t k = 0; k < plan->flags.size(); ++k) have = have || plan->flags[k] == flags[i];
            if (!have) plan->flags.push_back(flags[i]);
        }
        for (size_t i = 0; i < m.placements.size(); ++i) {
            uint64_t size = m.contents[m.placements[i].content].size;
            plan->installBytes += size;
            if (fs::sameDevice(store.resolve(m, m.placements[i].dest), staging)) placedBesideStaging += size;
        }
        uint64_t unpacked = 0;
        for (size_t i = 0; i < m.contents.size(); ++i) unpacked += m.contents[i].size;
        if (unpacked > largestUnpacked) largestUnpacked = unpacked;
        plan->restart = strongerRestart(plan->restart, m.reboot);

        // What the blob cache already holds of the archive does not have to be fetched.
        uint64_t cached = 0;
        struct stat st;
        if (fs::kindOf(store.blobPath(offer.sha256)) == fs::kFile) {
            cached = offer.size;
        } else if (stat((store.blobPath(offer.sha256) + ".part").c_str(), &st) == 0 && (uint64_t)st.st_size <= offer.size) {
            cached = (uint64_t)st.st_size;
        }
        plan->fetchBytes += offer.size - cached;
    }
    // The flags always come in one order, whatever order the steps brought them in.
    static const char* const kFlagOrder[] = {"runs-scripts", "modifies-system", "needs-reboot"};
    std::vector<std::string> sorted;
    for (size_t f = 0; f < sizeof kFlagOrder / sizeof kFlagOrder[0]; ++f) {
        for (size_t k = 0; k < plan->flags.size(); ++k) {
            if (plan->flags[k] == kFlagOrder[f]) sorted.push_back(plan->flags[k]);
        }
    }
    plan->flags = sorted;

    // ---- 5. what the job leaves behind: a package it does not touch must still find what it depends on
    for (size_t i = 0; i < sim.size(); ++i) {
        const Manifest& m = sim[i].manifest;
        if (chosenById.count(m.id) != 0) continue;
        for (size_t k = 0; k < m.depends.size(); ++k) {
            const ManifestDependency& d = m.depends[k];
            if (d.id == m.id || chosenById.count(d.id) == 0) continue;
            const InstalledPackage* have = findIn(sim, d.id);
            if (have != 0 && !versionInRange(have->manifest.version, d.range)) {
                addProblem(plan, "conflict", d.id,
                           "the installed " + m.id + " needs " + d.id + " " + d.range + ", and the job installs " +
                               have->manifest.version);
            }
        }
    }

    // ---- 6. room on the file system that holds staging: every archive still to fetch, the largest package
    // unpacked beside them, and whatever the job installs on that same file system. Each step checks its own
    // files again when it runs; this is the one place that sees them together with the archives.
    uint64_t free = 0;
    uint64_t needed = plan->fetchBytes + largestUnpacked + placedBesideStaging;
    uint64_t reserve = engine_->platform()->reserveBytes(staging);
    if (!order.empty() && engine_->platform()->freeBytes(staging, &free) && free < needed + reserve) {
        addProblem(plan, "no-space", chosen[order[0]].offered->manifest.id,
                   "the job needs " + number(needed) + " bytes on the file system that holds staging, and " +
                       number(reserve) + " are kept free; " + number(free) + " are free");
    }
    return true;
}

}  // namespace nbt
