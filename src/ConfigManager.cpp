#include "ConfigManager.h"

#include <string>
#include <memory>
#include <mutex>
#include <sstream>

#include <libconfig.h++>

using namespace Lichtenstein::Server;

/// This will hold the shared instance 
static std::shared_ptr<ConfigManager> instance;

/**
 * Reads the configuration from the specified path.
 */
ConfigManager::ConfigManager(const std::string &path) {
    // make sure we lock access to the config object even during construction
    std::lock_guard<std::mutex> guard(this->cfgLock);

    this->cfg = std::make_unique<libconfig::Config>();
    this->cfg->setOption(libconfig::Config::OptionAutoConvert, true);

    // any errors are propagated out
    try {
        this->cfg->readFile(path.c_str());
    } catch (const libconfig::FileIOException &io) {
        throw IOException(io.what());
    } catch (const libconfig::ParseException &parse) {
        std::stringstream what;
        what << parse.what() << "; " << parse.getError();

        throw ParseException(what.str(), parse.getLine());
    }
}

/**
 * Reads the configuration file from the given path, and uses it to create the
 * shared config manager instance.
 */
void ConfigManager::readConfig(const std::string &path) {
    instance = std::make_shared<ConfigManager>(path);
}

/**
 * Returns the shared config reader instance. The `readConfig()` method must be
 * called prior to this to actually allocate the instance.
 */
std::shared_ptr<ConfigManager> ConfigManager::sharedInstance() {
    return instance;
}


/**
 * Looks up a particular key in the settings file.
 */
libconfig::Setting &ConfigManager::getPrimitive(const std::string &path) {
    try {
        std::lock_guard<std::mutex> guard(this->cfgLock);
        return this->cfg->lookup(path);
    } catch(const libconfig::SettingNotFoundException &e) {
        throw KeyException(e.what());
    }
}
