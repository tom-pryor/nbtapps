// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PLANNER_HPP
#define NBT_AGENT_PLANNER_HPP

// What `job.plan` works out (plan document, "Message reference", job.plan): from the targets and the offers of a
// controller, the steps a job would run and everything that speaks against running it. Planning changes nothing
// on the head unit.
//
// The planner cannot fetch. It is asked again and again with the manifests fetched so far, and answers either
// "fetch the manifest of this offer" or with the finished plan:
//
//   1. each install target needs an offer with its id and version               no-offer
//      and that offer's manifest: fetched, hashed, its signature checked against the trusted keys, parsed
//                                    fetch-failed, bad-signature, bad-manifest, unsupported-manifest, offer-mismatch
//   2. a dependency that no installed package will satisfy is taken from the offers: the highest offered version
//      in range, whose manifest is fetched and checked the same way. With no such offer it is listed as missing
//   3. removals come first, a package before the ones it depends on              not-installed, required-by
//   4. installs follow, dependencies before their dependants, each checked against the installed set as the steps
//      before it will have left it      reserved-id, key-mismatch, downgrade, already-installed, agent-too-old,
//                                       conflict, file-owned, stock-mismatch, no-space
//   5. the set the job leaves behind must hold together: an upgrade may not take a package out of the range that
//      an installed package needs                                                conflict
//   6. the agent's own package, org.nbtapp.agent, is alone in its job, because the job ends with the agent's
//      restart (conflict), and the agent has to be one that a launcher starts    no-launcher
//
// HMI ids are inside the archive, which a plan does not have, so hmi-id-taken shows only when the step runs. The
// engine makes every check again then, because the head unit may have changed.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "engine.hpp"
#include "json.hpp"
#include "manifest.hpp"
#include "package.hpp"

namespace nbt {

struct Offer {
    std::string id, version;       // hints; the manifest decides
    std::string sha256;            // of the archive
    uint64_t size;                 // of the archive
    std::string manifestSha256;
    // A trusted key's signature over the manifest, as hex: the archive's manifest.sig, or a catalogue's detached
    // signature for a package its author signed. The install checks this one, not manifest.sig.
    std::string signature;

    Offer() : size(0) {}
};

struct InstallTarget {
    std::string id, version;
};

struct RemoveTarget {
    std::string id;
    bool deleteData;

    RemoveTarget() : deleteData(false) {}
};

struct PlanRequest {
    std::vector<InstallTarget> install;
    std::vector<RemoveTarget> remove;
    std::vector<Offer> offers;
};

// Reads the parameters of job.plan. False with *why set (for a bad-request error) if they are not as the message
// reference says.
bool parsePlanRequest(const json::Value& params, PlanRequest* out, std::string* why);

// The manifest of one offer, as far as the agent got with it.
struct OfferedManifest {
    Error error;            // set if it could not be fetched or was refused; the rest is then empty
    Manifest manifest;
    std::string keyHex;     // the trusted key that verified the offer's signature
    bool keyEmbedded;

    OfferedManifest() : keyEmbedded(false) {}
};

// Checks the bytes fetched for an offer's manifestSha256: their hash, the offer's signature over them, and the
// manifest itself. The offer's id and version are compared later, by the planner.
void checkOfferedManifest(const Offer& offer, const std::string& bytes, const std::vector<TrustedKey>& keys,
                          OfferedManifest* out);

struct PlanStep {
    std::string action;             // "install", "upgrade" or "remove"
    std::string id, version;        // for a removal, the installed version
    std::string oldVersion;         // upgrades only
    std::string sha256;             // the archive, for install and upgrade
    uint64_t size;
    std::string manifestSha256;
    std::string signature;          // the offer's, as hex, which the install checks again
    bool deleteData;                // removals only

    PlanStep() : size(0), deleteData(false) {}
};

struct Plan {
    std::vector<PlanStep> steps;
    std::vector<MissingDependency> missing;
    std::vector<Problem> problems;
    std::vector<std::string> flags;
    uint64_t fetchBytes, installBytes;
    std::string restart;            // "none", "hmi-restart" or "required"
    // What the installed set looked like: job.start compares it with what it finds (plan-stale).
    std::string installedState;

    Plan() : fetchBytes(0), installBytes(0), restart("none") {}
    bool runnable() const { return missing.empty() && problems.empty(); }
};

// The stronger of two restart needs.
const std::string& strongerRestart(const std::string& a, const std::string& b);

// One line that changes whenever a package is installed, upgraded or removed.
std::string installedStateOf(const std::vector<InstalledPackage>& installed);

class Planner {
public:
    explicit Planner(Engine* engine) : engine_(engine) {}

    // `fetched` holds the manifests fetched so far, by manifestSha256. Returns true with *plan filled in, or false
    // with *need set to the offer (its index in request.offers) whose manifest has to be fetched first.
    // If the installed records cannot be read, the plan is finished and holds an `internal` problem.
    bool resolve(const PlanRequest& request, const std::map<std::string, OfferedManifest>& fetched, Plan* plan,
                 size_t* need);

private:
    Engine* engine_;
};

}  // namespace nbt

#endif
