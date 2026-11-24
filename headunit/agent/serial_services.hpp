// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_SERVICES_HPP
#define NBT_AGENT_SERIAL_SERVICES_HPP

// The Bluetooth serial services that other programs on the head unit register: the agent's stand-in for each,
// which keeps the place in the car's Bluetooth stack that such a program's registration needs, and the removal of a
// registration whose program is not there any more.
//
// The car's Bluetooth stack keeps a registration until someone takes it away, the car's Bluetooth is switched off or
// the stack restarts. A program that ends without taking its own away, because it crashed or was stopped, leaves it
// behind: the car goes on offering the service, a phone that knows it goes on opening links to it, and nothing
// answers them. The car has one serial link for a phone, so those links keep the phone's app from reaching the
// agent. And the stack advertises only the first four registrations it holds: one made after the agent's, with the
// car's own ahead of both, may be in no service record at all (spp_service.hpp).
//
// The stack does not say what is registered or by whom. So the agent has to be told: a package declares the
// services its program registers (install.serialServices in its manifest), and a person can list more, for a
// program that no package installed, in a file. With that list:
//
//   * the agent's client of the car's Bluetooth service registers a stand-in under each declared name ahead of the
//     agent's own registration (SppService::setStandIns()), and so holds a place for the program below the agent's.
//     The program's own registration is then refused as "the name is there" (5): the program has to remove it by
//     name and register again, and so takes the stand-in's place
//   * a service whose program is not running is swept: what its name holds is replaced by the stand-in, unless the
//     name is known to hold the stand-in already, in which case it is left alone; so a registration the program left
//     behind goes, and the place stays held (SppService::replaceWithStandIn()). That is done
//       - when the agent's own serial service has been registered, which is when the agent starts, whenever the
//         stack comes back, and whenever the car's Bluetooth has been switched on again (SerialListener)
//       - during an install step, when the hook that stops a package's programs has had its turn, for that
//         package's services (Engine, through StepObserver)
//       - when asked to, by whoever sees a sign of a service that nothing answers; no more often than every
//         kSerialSweepEveryS
//   * a service whose program is running is never touched, but for the registration of its stand-in in a round,
//     which the stack refuses (5) while the program's own registration is there; and from then on its name is not
//     known to hold the stand-in (SerialServiceRemover::serialServiceRuns()). The programs are looked at in every
//     sweep, and before every registration check and the stop of Bluetooth serial (namesRunning())
//
// Nothing is removed on a doubt: if the machine cannot say whether a program is running, its services are left
// alone. Removing the service of a program that does run would be put right by that program registering again,
// and until then phones would not find it.
//
// The file is <state>/serial-services: one service to a line, its name, a space, the file name of its program.
// A line that starts with # is a comment.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>

#include <set>
#include <string>
#include <vector>

#include "engine.hpp"
#include "manifest.hpp"
#include "platform.hpp"
#include "serial_log.hpp"
#include "store.hpp"

namespace nbt {

// The least time between two sweeps that sweepIfDue() makes.
const double kSerialSweepEveryS = 30.0;

struct SerialServiceOwner {
    std::string name;      // the service's name, which its registration is filed under
    std::string program;   // the file name of the program that registers it
    std::string from;      // who says so: a package's id, or the file's name
};

// Reads the file of services that no package declares. *out gets its entries, each with `from` set to the file's
// name; a line that is not a service's name and a program's file name is passed over and put into *complaints. A
// file that is not there is an empty list.
void readSerialServicesFile(const std::string& path, std::vector<SerialServiceOwner>* out,
                            std::vector<std::string>* complaints);

// What registers the stand-ins and takes a registration away: the agent's client of the car's Bluetooth service.
class SerialServiceRemover {
public:
    virtual ~SerialServiceRemover() {}
    enum Replace {
        kReplacing,    // what the name holds is being removed, and the stand-in goes in its place
        kKeptOurs,     // the name holds the agent's stand-in: nothing is done
        kNotNow        // the car's Bluetooth service cannot be asked now
    };
    // For a declared service whose program is not running. From any thread.
    virtual Replace replaceSerialService(const std::string& name) = 0;
    // The program of a declared service is running: its name is not known to hold the stand-in from here. From any
    // thread.
    virtual void serialServiceRuns(const std::string& /*name*/) {}
    // The names declared now, each once, in the order of the list; told whenever the list is set, and when the remover
    // is. From any thread.
    virtual void serialServicesDeclared(const std::vector<std::string>& /*names*/) {}
};

class SerialServices : public StepObserver {
public:
    // All three outlive this object.
    SerialServices(const Platform* platform, const Store* store, SerialLog* log);
    virtual ~SerialServices();

    // Who removes a registration; 0 for nobody, which is how it starts, and then a sweep does nothing. It is told
    // the names declared now at once.
    void setRemover(SerialServiceRemover* remover);

    // Reads anew what is declared: by the installed packages, and in the file. On the thread that runs install
    // steps, because it reads the store.
    void reload();
    // In place of reload(), for a caller that has the list.
    void setDeclared(const std::vector<SerialServiceOwner>& declared);
    std::vector<SerialServiceOwner> declared();
    // The programs that are declared for any service, each once, sorted.
    std::vector<std::string> programs();

    // Whether a program declared for a serial service is running now, which is who would read a serial link that
    // is not the agent's. A program whose state cannot be told counts as running. From any thread.
    enum Readers { kNoneDeclared, kNoneRunning, kSomeMayRun };
    Readers readers();
    // Whether a program of software put on by hand runs, which may read a serial link too: one whose executable is
    // in the folder of a check's that stops programs before its fix (checks.hpp). *names: their file names. Only
    // kProgramRunning says that one does. From any thread.
    ProgramState handInstalledRunning(std::string* names);

    // Asks for what every declared service that no program answers for holds to be replaced by the agent's
    // stand-in, and tells the remover of each service whose program runs. Returns how many replacements were asked
    // for; a name known to hold the stand-in already is not one, and is said once until that changes. `why` goes
    // into the log. From any thread.
    unsigned sweep(const char* why);
    // The declared services whose program is running now, each once: what their names hold is not the stand-in
    // (SerialServiceRemover::serialServiceRuns()). A program whose state cannot be told is not among them. From any
    // thread.
    std::vector<std::string> namesRunning();
    // The same, unless a sweep was made less than kSerialSweepEveryS ago: for a sign that can come many times.
    unsigned sweepIfDue(const char* why);
    // Test only: another time in place of kSerialSweepEveryS.
    void setSweepEvery(double seconds);

    // StepObserver. The package's services are swept: the ones its new manifest declares, and the ones the
    // installed version declared.
    virtual void programsStopped(const Manifest& manifest);
    virtual void stepEnded() { reload(); }

private:
    SerialServices(const SerialServices&);
    SerialServices& operator=(const SerialServices&);

    unsigned sweepOf(const std::vector<SerialServiceOwner>& owners, const std::string& why);
    std::vector<std::string> namesLocked() const;

    const Platform* platform_;
    const Store* store_;
    SerialLog* log_;
    pthread_mutex_t mu_;                        // everything below
    SerialServiceRemover* remover_;
    std::vector<SerialServiceOwner> declared_;
    double lastSweep_;                          // 0: none yet
    double sweepEveryS_;
    std::set<std::string> keptSaid_;            // the names a sweep has said hold the stand-in, until that changes
};

}  // namespace nbt

#endif
