#include "lockfile.h"
#include <fstream>
#include <iostream>

LockfileManager g_lockfile("lynx-lock.json");

LockfileManager::LockfileManager(const std::string& path) : lockfile_path(path) {
    load();
}

void LockfileManager::load() {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    if (!fs::exists(lockfile_path)) return;

    std::ifstream file(lockfile_path);
    if (!file.is_open()) return;

    try {
        json j;
        file >> j;
        file.close();

        packages.clear();
        if (j.contains("packages") && j["packages"].is_object()) {
            for (auto& [pkg_name, pkg_data] : j["packages"].items()) {
                LockPackage lp;
                if (pkg_data.contains("version")) lp.version = pkg_data["version"].get<std::string>();
                if (pkg_data.contains("resolved")) lp.resolved = pkg_data["resolved"].get<std::string>();
                if (pkg_data.contains("integrity")) lp.integrity = pkg_data["integrity"].get<std::string>();
                if (pkg_data.contains("dependencies") && pkg_data["dependencies"].is_object()) {
                    for (auto& [dep_k, dep_v] : pkg_data["dependencies"].items()) {
                        lp.dependencies[dep_k] = dep_v.get<std::string>();
                    }
                }
                packages[pkg_name] = lp;
            }
        }
    } catch (...) {
        if (file.is_open()) file.close();
    }
}

void LockfileManager::save() {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    json j;
    j["name"] = "lynx-lockfile";
    j["lockfileVersion"] = 1;
    j["packages"] = json::object();

    for (const auto& [name, pkg] : packages) {
        json pkg_json;
        pkg_json["version"] = pkg.version;
        pkg_json["resolved"] = pkg.resolved;
        pkg_json["integrity"] = pkg.integrity;
        
        if (!pkg.dependencies.empty()) {
            json deps_json = json::object();
            for (const auto& [dep_k, dep_v] : pkg.dependencies) {
                deps_json[dep_k] = dep_v;
            }
            pkg_json["dependencies"] = deps_json;
        }

        j["packages"][name] = pkg_json;
    }

    std::ofstream file(lockfile_path);
    if (file.is_open()) {
        file << j.dump(2) << std::endl;
        file.close();
    }
}

void LockfileManager::add_package(const std::string& name, 
                                  const std::string& version, 
                                  const std::string& resolved, 
                                  const std::string& integrity,
                                  const std::map<std::string, std::string>& deps) {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    LockPackage lp;
    lp.version = version;
    lp.resolved = resolved;
    lp.integrity = integrity;
    lp.dependencies = deps;
    packages[name] = lp;
}

void LockfileManager::remove_package(const std::string& name) {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    packages.erase(name);
}

bool LockfileManager::has_package(const std::string& name, const std::string& version) {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    auto it = packages.find(name);
    if (it != packages.end()) {
        return version.empty() || it->second.version == version;
    }
    return false;
}

bool LockfileManager::get_package_info(const std::string& name, LockPackage& out_pkg) {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    auto it = packages.find(name);
    if (it != packages.end()) {
        out_pkg = it->second;
        return true;
    }
    return false;
}

void LockfileManager::clear() {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    packages.clear();
}

std::map<std::string, std::string> LockfileManager::get_dependencies(const std::string& name) {
    std::lock_guard<std::mutex> lock(lockfile_mutex);
    auto it = packages.find(name);
    if (it != packages.end()) {
        return it->second.dependencies;
    }
    return {};
}