#pragma once
#include <string>

#include <jansson.h>
#include <engine/Module.hpp>

namespace rackdroid {

/** Modules that were made on this device, from the chat: C++ that Termux has
compiled (nova/bridge/nova_build.py), one library to a module and version, kept
by MadeModules.kt. To Rack they are the models of one plugin, NovaMade.

A model of it stays the same while the code behind it is exchanged, without a
restart: the modules of the patch are taken out and put back, with their cables,
as the new code makes them. Where there is no code to run - it has crashed, or
was made for another version of the app - a stand-in with the same knobs and
jacks keeps the place, so the patch does not come apart. */

/** The operations "made_...": see made_modules.cpp. Throws std::exception.
Render thread. */
std::string madeOp(const std::string& op, json_t* req);

/** Render thread, every frame: what has gone wrong since the last one. */
void madeModulesStep();

/** Empty, or that a module is stopped, and why. Render thread. */
std::string madeStatus(rack::engine::Module* module);

} // namespace rackdroid
