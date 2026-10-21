#pragma once

#include "commandlineparser.h"

#include <QLockFile>
#include <QString>

// Dusk: record a host Dusk has vouched for as paired, by pinning its server
// certificate, so the client can stream to it without a PIN.
namespace CliAddHost
{

// The lock every client process that can save the host list takes for its
// lifetime, so add-host can refuse to write behind a running client's back
// (a running client rewrites the whole host list from memory when it saves).
QString clientLockPath();

// Takes the client lock if it is free and returns it, to be held for the
// life of the process. Returns nullptr if another process holds it; that
// must never stop the caller from starting.
QLockFile* takeClientLockIfFree();

// Runs add-host and returns the process exit code: 0 stored, 1 failed,
// 2 another client is running. Prints one line to stderr on failure.
int run(const AddHostCommandLineParser& args);

}
