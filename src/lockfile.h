#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <filesystem>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

struct LockPackage {
    std::string version;
    std::string resolved;
    std::string integrity;
    std::map<std::string, std::string> dependencies;
};

class LockfileManager {
private:
    std::string lockfile_path;
    std::map<std::string, LockPackage> packages;
    std::mutex lockfile_mutex;

public:
    LockfileManager(const std::string& path = "lynx-lock.json");
    std::map<std::string, std::string> get_dependencies(const std::string& name);
    
    bool get_package_info(const std::string& name, LockPackage& out_pkg);

    void load();
    void save();
    
    void add_package(const std::string& name, 
                     const std::string& version, 
                     const std::string& resolved, 
                     const std::string& integrity,
                     const std::map<std::string, std::string>& deps);

    void remove_package(const std::string& name);
    bool has_package(const std::string& name, const std::string& version = "");
    void clear();
};

extern LockfileManager g_lockfile;