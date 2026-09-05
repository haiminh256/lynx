#pragma once

#include <filesystem>
#include <string>
#include <set>
#include <mutex>
#include <thread>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

extern std::set<std::string> installed_packages;
extern std::mutex install_mutex;

inline unsigned int get_lynx_max_parallel() {

    if (const char* env_p = std::getenv("LYNX_MAX_PARALLEL")) {
        try {
            int val = std::stoi(env_p);
            if (val > 0) return static_cast<unsigned int>(val);
        } catch (...) {}
    }

    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) return 4;

    return cores;
}

fs::path get_lynx_cache_dir();
std::string sanitize_filename(std::string name);
void generate_bin_shims(const fs::path& package_path, const std::string& package_name);
std::string get_current_os();   // Tra ve: "win32", "darwin", "linux", ...
std::string get_current_arch(); // Tra ve: "x64", "arm64", "ia32", ...
bool is_platform_supported(const json& pkg_meta);
bool is_package_name_compatible(const std::string& pkg_name);
