// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SERIAL_SERVICES_HPP
#define NBT_AGENT_SERIAL_SERVICES_HPP

// The Bluetooth serial services that other programs on the head unit register, and the removal of one whose
// program is not there any more.
//
// The car's Bluetooth stack keeps a registration until someone takes it away or the stack restarts. A program
// that ends without taking its own away, because it crashed or was stopped, leaves it behind: the car goes on
// offering the service, a phone that knows it goes on opening links to it, and nothing answers them. The car has
// one serial link for a phone, so those links keep the phone's app from reaching the agent.
//
// The stack does not say what is registered or by whom. So the agent has to be told: a package declares the
// services its program registers (install.serialServices in its manifest), and a person can list more, for a
// program that no package installed, in a file. With that, a service whose program is not running is one that was
// left behind, and the agent asks the stack to remove it:
//
//   * when its own serial service has been registered, which is when the agent starts and whenever the stack
//     comes back (SerialListener)
//   * during an install step, when the hook that stops a package's programs has had its turn, for that package's
//     services (Engine, through StepObserver)
//   * when asked to, by whoever sees a sign of a service that nothing answers; no more often than every
//     kSerialSweepEveryS
//
// Nothing is removed on a doubt: if the machine cannot say whether a program is running, its services are left
// alone. Removing the service of a program that does run would be put right by that program registering again,
// and until then phones would not find it.
//
// The file is <state>/serial-services: one service to a line, its name, a space, the file name of its program.
// A line that starts with # is a comment.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>

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

// What takes a registration away: the agent's client of the car's Bluetooth service.
class SerialServiceRemover {
public:
    virtual ~SerialServiceRemover() {}
    // Asks for the registration under `name` to be removed. False if it cannot be asked for now. From any thread.
    virtual bool removeSerialService(const std::string& name) = 0;
};

class SerialServices : public StepObserver {
public:
    // All three outlive this object.
    SerialServices(const Platform* platform, const Store* store, SerialLog* log);
    virtual ~SerialServices();

    // Who removes a registration; 0 for nobody, which is how it starts, and then a sweep does nothing.
    void setRemover(SerialServiceRemover* remover);

    // Reads anew what is declared: by the installed packages, and in the file. On the thread that runs install
    // steps, because it reads the store.
    void reload();
    // In place of reload(), for a caller that has the list.
    void setDeclared(const std::vector<SerialServiceOwner>& declared);
    std::vector<SerialServiceOwner> declared();
    // The programs that are declared for any service, each once, sorted.
    std::vector<std::string> programs();

    // Asks for the removal of every declared service that no program answers for. Returns how many removals were
    // asked for. `why` goes into the log. From any thread.
    unsigned sweep(const char* why);
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

    const Platform* platform_;
    const Store* store_;
    SerialLog* log_;
    pthread_mutex_t mu_;                        // everything below
    SerialServiceRemover* remover_;
    std::vector<SerialServiceOwner> declared_;
    double lastSweep_;                          // 0: none yet
    double sweepEveryS_;
};

}  // namespace nbt

#endif
