#include "installer.h"
#include "utils.h"
#include "lockfile.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <sstream>
#include <system_error>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

static std::atomic<uint64_t> g_temp_counter{0};

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool is_prerelease = false;

    static SemVer parse(const std::string& v_str) {
        SemVer sv;
        std::string clean = v_str;

        while (!clean.empty() && (clean.front() == '^' || clean.front() == '~' || 
                                  clean.front() == '=' || clean.front() == '>' || clean.front() == '<')) {
            clean = clean.substr(1);
        }

        if (clean.find('-') != std::string::npos) {
            sv.is_prerelease = true;
            clean = clean.substr(0, clean.find('-'));
        }

        std::stringstream ss(clean);
        std::string part;
        if (std::getline(ss, part, '.')) try { sv.major = std::stoi(part); } catch (...) {}
        if (std::getline(ss, part, '.')) try { sv.minor = std::stoi(part); } catch (...) {}
        if (std::getline(ss, part, '.')) try { sv.patch = std::stoi(part); } catch (...) {}
        return sv;
    }

    bool operator>(const SemVer& other) const {
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        return patch > other.patch;
    }

    bool operator>=(const SemVer& other) const {
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        return patch >= other.patch;
    }

    bool operator==(const SemVer& other) const {
        return major == other.major && minor == other.minor && patch == other.patch;
    }
};

static std::string resolve_best_version(const json& parsed_data, const std::string& range_req) {
    if (!parsed_data.contains("versions") || parsed_data["versions"].empty()) {
        return "";
    }

    std::string range = range_req;
    range.erase(std::remove_if(range.begin(), range.end(), ::isspace), range.end());

    if (range.empty() || range == "*" || range == "latest") {
        if (parsed_data.contains("dist-tags") && parsed_data["dist-tags"].contains("latest")) {
            return parsed_data["dist-tags"]["latest"].get<std::string>();
        }
    }

    if (parsed_data["versions"].contains(range)) {
        return range;
    }

    std::string op = "^";
    std::string clean_range = range;

    if (clean_range.rfind(">=", 0) == 0) { op = ">="; clean_range = clean_range.substr(2); }
    else if (clean_range.rfind("<=", 0) == 0) { op = "<="; clean_range = clean_range.substr(2); }
    else if (!clean_range.empty() && (clean_range[0] == '^' || clean_range[0] == '~' || clean_range[0] == '=')) {
        op = clean_range[0];
        clean_range = clean_range.substr(1);
    }

    SemVer target_sv = SemVer::parse(clean_range);
    std::string best_ver_str = "";
    SemVer best_sv{-1, -1, -1, true};

    for (auto it = parsed_data["versions"].begin(); it != parsed_data["versions"].end(); ++it) {
        std::string ver_str = it.key();
        SemVer curr_sv = SemVer::parse(ver_str);

        if (curr_sv.is_prerelease && !target_sv.is_prerelease) {
            continue;
        }

        bool match = false;
        if (op == "^") {
            if (curr_sv.major == target_sv.major && curr_sv >= target_sv) {
                match = true;
            }
        } else if (op == "~") {
            if (curr_sv.major == target_sv.major && curr_sv.minor == target_sv.minor && curr_sv.patch >= target_sv.patch) {
                match = true;
            }
        } else if (op == "=") {
            if (curr_sv == target_sv) {
                match = true;
            }
        } else if (op == ">=") {
            if (curr_sv >= target_sv) {
                match = true;
            }
        }

        if (match) {
            if (best_ver_str.empty() || curr_sv > best_sv) {
                best_sv = curr_sv;
                best_ver_str = ver_str;
            }
        }
    }

    if (!best_ver_str.empty()) {
        return best_ver_str;
    }

    if (parsed_data.contains("dist-tags") && parsed_data["dist-tags"].contains("latest")) {
        return parsed_data["dist-tags"]["latest"].get<std::string>();
    }

    return parsed_data["versions"].rbegin().key();
}

std::string PackageInstaller::make_unique_temp(const std::string& package_name) {
    uint64_t n = g_temp_counter.fetch_add(1);
    std::ostringstream oss;
    oss << "lynx_meta_" << sanitize_filename(package_name) << "_"
        << n << "_"
        << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << ".json";

    fs::path cache_dir = get_lynx_cache_dir();
    fs::path temp_path = cache_dir / "tmp" / oss.str();

    std::error_code ec;
    fs::create_directories(temp_path.parent_path(), ec);

    return temp_path.string();
}

void PackageInstaller::safe_remove(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        fs::remove(p, ec);
    }
}

bool PackageInstaller::install_single_package(const std::string& raw_input) {
    std::string package_name = raw_input;
    std::string requested_version = "";

    size_t at_pos = raw_input.find('@');
    if (at_pos == 0) {
        at_pos = raw_input.find('@', 1);
    }
    if (at_pos != std::string::npos && at_pos > 0) {
        package_name = raw_input.substr(0, at_pos);
        requested_version = raw_input.substr(at_pos + 1);
    }

    std::string cache_key = package_name + (requested_version.empty() ? "" : "@" + requested_version);

    {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (installed_packages.count(cache_key)) return true;
        installed_packages.insert(cache_key);
    }

    fs::path project_node_modules = fs::current_path() / "node_modules" / package_name;
    std::error_code ec;

    if (fs::exists(project_node_modules, ec) && g_lockfile.has_package(package_name, requested_version)) {
        LockPackage lp;
        g_lockfile.get_package_info(package_name, lp);
        std::string display_ver = lp.version.empty() ? requested_version : lp.version;

        {
            std::lock_guard<std::mutex> lock(install_mutex);
            skipped_packages.push_back(package_name + "@" + display_ver);
        }

        std::map<std::string, std::string> deps = g_lockfile.get_dependencies(package_name);
        if (!deps.empty()) {
            std::vector<std::string> child_deps;
            for (const auto& [dep_name, dep_ver] : deps) {
                child_deps.push_back(dep_name + "@" + dep_ver);
            }
            install_packages_parallel(child_deps);
        }
        return true;
    }

    LockPackage locked_pkg;
    std::string target_version = "";
    std::string tarball_url = "";
    std::string integrity = "";
    std::map<std::string, std::string> dep_map;

    if (g_lockfile.get_package_info(package_name, locked_pkg) && 
        (requested_version.empty() || locked_pkg.version == requested_version)) {
        
        target_version = locked_pkg.version;
        tarball_url = locked_pkg.resolved;
        integrity = locked_pkg.integrity;
        dep_map = locked_pkg.dependencies;
        
        {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cout << "[Lynx]: Found " << package_name << "@" << target_version << " in lockfile. Fast-installing...\n";
        }
    } else {

        std::string temp_file = make_unique_temp(package_name);
        TempFileCleaner cleaner{temp_file};
        std::string url = "https://registry.npmjs.org/" + package_name;

        safe_remove(temp_file);

        std::string curl_command =
            "curl -s -L -H \"Accept: application/vnd.npm.install-v1+json\" \"" + url +
            "\" -o \"" + temp_file + "\"";

        int curl_ret = std::system(curl_command.c_str());
        if (curl_ret != 0 || !fs::exists(temp_file, ec) || fs::file_size(temp_file, ec) == 0) {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cerr << "[Lynx ERROR]: Network error or package " << package_name << " not found!\n";
            safe_remove(temp_file);
            return false;
        }

        try {
            json parsed_data;
            {
                std::ifstream file(temp_file);
                if (!file.is_open()) return false;
                file >> parsed_data;
            }
            safe_remove(temp_file);

            target_version = resolve_best_version(parsed_data, requested_version);

            if (target_version.empty() || !parsed_data["versions"].contains(target_version)) {
                std::lock_guard<std::mutex> lock(install_mutex);
                std::cerr << "[Lynx ERROR]: Version " << requested_version << " not found for " << package_name << "\n";
                return false;
            }

            json current_version_meta = parsed_data["versions"][target_version];
            tarball_url = current_version_meta["dist"]["tarball"].get<std::string>();
            
            if (current_version_meta["dist"].contains("integrity")) {
                integrity = current_version_meta["dist"]["integrity"].get<std::string>();
            } else if (current_version_meta["dist"].contains("shasum")) {
                integrity = current_version_meta["dist"]["shasum"].get<std::string>();
            }

            if (current_version_meta.contains("dependencies") && !current_version_meta["dependencies"].empty()) {
                for (auto& [dep_name, dep_ver] : current_version_meta["dependencies"].items()) {
                    dep_map[dep_name] = dep_ver.get<std::string>();
                }
            }

            if (current_version_meta.contains("optionalDependencies") && !current_version_meta["optionalDependencies"].empty()) {
                for (auto& [opt_name, opt_ver] : current_version_meta["optionalDependencies"].items()) {
                    dep_map[opt_name] = opt_ver.get<std::string>();
                }
            }
        } catch (...) {
            safe_remove(temp_file);
            return false;
        }
    }

    if (!is_package_in_cas(package_name, target_version)) {
        std::string archive_name = sanitize_filename(package_name) + "-" + target_version + ".tgz";
        fs::path cache_dir = get_lynx_cache_dir();
        fs::path target_cache_file = cache_dir / archive_name;
        fs::path global_extract_dir = cache_dir / "extracted" / (sanitize_filename(package_name) + "_" + target_version);

        if (!fs::exists(target_cache_file, ec) || fs::file_size(target_cache_file, ec) == 0) {
            {
                std::lock_guard<std::mutex> lock(install_mutex);
                std::cout << "[Lynx]: Fetching " << package_name << "@" << target_version << "...\n" << std::flush;
            }
            fs::path tmp_tgz = cache_dir / (archive_name + ".part." + std::to_string(g_temp_counter.fetch_add(1)));
            std::string curl_download_cmd = "curl -s -L \"" + tarball_url + "\" -o \"" + tmp_tgz.string() + "\"";
            int dl_ret = std::system(curl_download_cmd.c_str());

            if (dl_ret == 0 && fs::exists(tmp_tgz, ec) && fs::file_size(tmp_tgz, ec) > 0) {
                fs::rename(tmp_tgz, target_cache_file, ec);
            } else {
                safe_remove(tmp_tgz);
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(install_mutex);
            if (fs::exists(global_extract_dir, ec)) {
                fs::remove_all(global_extract_dir, ec);
            }
            fs::create_directories(global_extract_dir, ec);

            std::string tar_extract_cmd =
                "tar -xzf \"" + target_cache_file.string() + "\" -C \"" +
                global_extract_dir.string() + "\" --strip-components=1";

            int tar_ret = std::system(tar_extract_cmd.c_str());
            if (tar_ret != 0) {
                fs::remove_all(global_extract_dir, ec);
                return false;
            }

            if (!import_package_to_cas(global_extract_dir, package_name, target_version)) {
                return false;
            }

            fs::remove_all(global_extract_dir, ec);
        }
    }

    {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (fs::exists(project_node_modules, ec) || fs::is_symlink(project_node_modules, ec)) {
            fs::remove_all(project_node_modules, ec);
        }

        bool ok = materialize_from_cas(package_name, target_version, project_node_modules);
        if (!ok) {
            std::cerr << "[Lynx ERROR]: Failed to materialize " << package_name << " from CAS\n";
            return false;
        }

        generate_bin_shims(project_node_modules, package_name);
        std::cout << "[Lynx]: Done! " << package_name << "@" << target_version << " (CAS hardlink)" << "\n" << std::flush;

        installed_summary_packages.push_back(package_name + "@" + target_version);
        pending_lifecycle_packages.push_back({project_node_modules, package_name});
    }

    g_lockfile.add_package(package_name, target_version, tarball_url, integrity, dep_map);

    if (!dep_map.empty()) {
        std::vector<std::string> child_deps;
        for (const auto& [dep_name, dep_ver] : dep_map) {
            child_deps.push_back(dep_name + "@" + dep_ver);
        }
        install_packages_parallel(child_deps);
    }

    return true;
}

void PackageInstaller::install_packages_parallel(const std::vector<std::string>& targets) {
    if (targets.empty()) return;

    std::vector<std::future<bool>> jobs;
    jobs.reserve(targets.size());

    for (const auto& t : targets) {
        while ((int)jobs.size() >= LYNX_MAX_PARALLEL) {
            bool progressed = false;
            for (auto it = jobs.begin(); it != jobs.end();) {
                if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    try { it->get(); } catch (...) {}
                    it = jobs.erase(it);
                    progressed = true;
                } else {
                    ++it;
                }
            }
            if (!progressed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        jobs.push_back(std::async(std::launch::async, &PackageInstaller::install_single_package, this, t));
    }

    for (auto& j : jobs) {
        try { j.get(); } catch (...) {}
    }
}

void PackageInstaller::run_all_pending_lifecycles() {
    if (pending_lifecycle_packages.empty()) return;

    std::cout << "\n[Lynx]: Running lifecycle scripts for installed packages...\n";
    for (const auto& [pkg_path, pkg_name] : pending_lifecycle_packages) {
        run_lifecycle_scripts(pkg_path, pkg_name, false);
    }
    pending_lifecycle_packages.clear();
}

void PackageInstaller::print_summary() {
    std::cout << "\n--------------------------------------------------\n";
    std::cout << "[Lynx Summary]:\n";

    if (!installed_summary_packages.empty()) {
        std::cout << "  Installed (" << installed_summary_packages.size() << "):\n";
        for (const auto& pkg : installed_summary_packages) {
            std::cout << "    + " << pkg << "\n";
        }
    }

    if (!skipped_packages.empty()) {
        std::cout << "  Skipped (already up to date) (" << skipped_packages.size() << "):\n";
        for (const auto& pkg : skipped_packages) {
            std::cout << "    - " << pkg << " (skipped)\n";
        }
    }

    if (installed_summary_packages.empty() && skipped_packages.empty()) {
        std::cout << "  Nothing to install or skip.\n";
    }
    std::cout << "--------------------------------------------------\n";
}