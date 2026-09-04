#include "install.h"
#include "utils.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

static std::atomic<uint64_t> g_temp_counter{0};

static std::string make_unique_temp(const std::string& package_name) {
    uint64_t n = g_temp_counter.fetch_add(1);
    std::ostringstream oss;
    oss << "lynx_meta_" << sanitize_filename(package_name) << "_"
        << n << "_"
        << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << ".json";
    return oss.str();
}

static void safe_remove(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        fs::remove(p, ec); // không throw
    }
}

bool install_single_package(const std::string& raw_input) {
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

    while (!requested_version.empty() &&
           (requested_version.front() == '^' ||
            requested_version.front() == '~' ||
            requested_version.front() == '=')) {
        requested_version = requested_version.substr(1);
    }

    if (requested_version == "*" ||
        requested_version == "latest" ||
        requested_version.find("workspace") != std::string::npos) {
        requested_version = "";
    }

    std::string cache_key = package_name + (requested_version.empty() ? "" : "@" + requested_version);

    {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (installed_packages.count(cache_key)) return true;
        installed_packages.insert(cache_key);
    }

    std::string temp_file = make_unique_temp(package_name);
    std::string url = "https://registry.npmjs.org/" + package_name;

    safe_remove(temp_file);

    std::string curl_command =
        "curl -s -L -H \"Accept: application/vnd.npm.install-v1+json\" \"" + url +
        "\" -o \"" + temp_file + "\"";

    {
        std::lock_guard<std::mutex> lock(install_mutex);
    }
    std::system(curl_command.c_str());

    std::error_code ec;
    if (!fs::exists(temp_file, ec) || fs::file_size(temp_file, ec) == 0) {
        std::lock_guard<std::mutex> lock(install_mutex);
        std::cerr << "[Lynx ERROR]: Network error or package " << package_name << " does not exist!\n";
        safe_remove(temp_file);
        return false;
    }

    json parsed_data;
    try {
        {
            std::ifstream file(temp_file);
            if (!file.is_open()) {
                std::lock_guard<std::mutex> lock(install_mutex);
                std::cerr << "[Lynx ERROR]: Cannot open metadata for " << package_name << "\n";
                safe_remove(temp_file);
                return false;
            }
            file >> parsed_data;
        }
        safe_remove(temp_file);

        std::string target_version = requested_version;
        if (target_version.empty()) {
            if (parsed_data.contains("dist-tags") && parsed_data["dist-tags"].contains("latest")) {
                target_version = parsed_data["dist-tags"]["latest"].get<std::string>();
            } else {
                std::lock_guard<std::mutex> lock(install_mutex);
                std::cerr << "[Lynx ERROR]: Could not find latest version for " << package_name << "\n";
                return false;
            }
        }

        if (!parsed_data.contains("versions") || !parsed_data["versions"].contains(target_version)) {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cerr << "[Lynx ERROR]: Version " << target_version
                      << " not found for package " << package_name << "!\n";
            return false;
        }

        json current_version_meta = parsed_data["versions"][target_version];
        if (!current_version_meta.contains("dist") || !current_version_meta["dist"].contains("tarball")) {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cerr << "[Lynx ERROR]: No tarball for " << package_name << "@" << target_version << "\n";
            return false;
        }
        std::string tarball_url = current_version_meta["dist"]["tarball"].get<std::string>();

        {
            std::lock_guard<std::mutex> lock(install_mutex);
        }

        std::string archive_name = sanitize_filename(package_name) + "-" + target_version + ".tgz";
        fs::path cache_dir = get_lynx_cache_dir();
        fs::path target_cache_file = cache_dir / archive_name;
        fs::path global_extract_dir =
            cache_dir / "extracted" / (sanitize_filename(package_name) + "_" + target_version);

        if (!fs::exists(global_extract_dir, ec)) {
            if (!fs::exists(target_cache_file, ec) || fs::file_size(target_cache_file, ec) == 0) {
                {
                    std::lock_guard<std::mutex> lock(install_mutex);
                    std::cout << "[Lynx]: Fetching " << package_name << "...\n" << std::flush;
                }
                fs::path tmp_tgz = cache_dir / (archive_name + ".part." + std::to_string(g_temp_counter.fetch_add(1)));
                std::string curl_download_cmd =
                    "curl -s -L \"" + tarball_url + "\" -o \"" + tmp_tgz.string() + "\"";
                std::system(curl_download_cmd.c_str());

                if (fs::exists(tmp_tgz, ec) && fs::file_size(tmp_tgz, ec) > 0) {
                    std::error_code rename_ec;
                    fs::rename(tmp_tgz, target_cache_file, rename_ec);
                    if (rename_ec) {
                        safe_remove(tmp_tgz);
                    }
                } else {
                    safe_remove(tmp_tgz);
                    std::lock_guard<std::mutex> lock(install_mutex);
                    std::cerr << "[Lynx ERROR]: Download failed for " << package_name << "\n";
                    return false;
                }
            }

            {
                std::lock_guard<std::mutex> lock(install_mutex);
                if (!fs::exists(global_extract_dir, ec)) {
                    std::cout << "[Lynx]: Extracting " << package_name << "...\n" << std::flush;
                    fs::create_directories(global_extract_dir, ec);
                    std::string tar_extract_cmd =
                        "tar -xf \"" + target_cache_file.string() + "\" -C \"" +
                        global_extract_dir.string() + "\" --strip-components=1";
                    std::system(tar_extract_cmd.c_str());
                } else {

                }
            }
        } else {
            std::lock_guard<std::mutex> lock(install_mutex);
        }

        fs::path project_node_modules = fs::current_path() / "node_modules" / package_name;

        {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::error_code copy_ec;
            if (fs::exists(project_node_modules, copy_ec) || fs::is_symlink(project_node_modules, copy_ec)) {
                fs::remove_all(project_node_modules, copy_ec);
            }
            fs::create_directories(project_node_modules.parent_path(), copy_ec);

            fs::copy(global_extract_dir, project_node_modules,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);

            if (copy_ec) {
                std::cerr << "[Lynx ERROR]: Copy failed for " << package_name << "! "
                          << copy_ec.message() << "\n";
                return false;
            }

            generate_bin_shims(project_node_modules, package_name);
            std::cout << "[Lynx]: Done! " << package_name << "@" << target_version << "\n" << std::flush;
        }

        std::vector<std::string> child_deps;

        if (current_version_meta.contains("dependencies") &&
            !current_version_meta["dependencies"].empty()) {
            for (auto& [dep_name, dep_ver] : current_version_meta["dependencies"].items()) {
                child_deps.push_back(dep_name + "@" + dep_ver.get<std::string>());
            }
        }

        if (current_version_meta.contains("optionalDependencies") &&
            !current_version_meta["optionalDependencies"].empty()) {
            for (auto& [opt_name, opt_ver] : current_version_meta["optionalDependencies"].items()) {
                child_deps.push_back(opt_name + "@" + opt_ver.get<std::string>());
            }
        }

        if (!child_deps.empty()) {
            install_packages_parallel(child_deps);
        }

        return true;

    } catch (const json::exception& e) {
        safe_remove(temp_file);
        std::lock_guard<std::mutex> lock(install_mutex);
        std::cerr << "[Lynx ERROR]: JSON parse failed for " << package_name << ": " << e.what() << "\n";
        return false;
    } catch (const fs::filesystem_error& e) {
        safe_remove(temp_file);
        std::lock_guard<std::mutex> lock(install_mutex);
        std::cerr << "[Lynx ERROR]: Filesystem error for " << package_name << ": " << e.what() << "\n";
        return false;
    } catch (const std::exception& e) {
        safe_remove(temp_file);
        std::lock_guard<std::mutex> lock(install_mutex);
        std::cerr << "[Lynx ERROR]: Failed for " << package_name << ": " << e.what() << "\n";
        return false;
    }
}

void install_packages_parallel(const std::vector<std::string>& targets) {
    if (targets.empty()) return;

    std::vector<std::future<bool>> jobs;
    jobs.reserve(targets.size());

    for (const auto& t : targets) {
        while ((int)jobs.size() >= LYNX_MAX_PARALLEL) {
            bool progressed = false;
            for (auto it = jobs.begin(); it != jobs.end();) {
                if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    try {
                        it->get();
                    } catch (...) {
                    }
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

        jobs.push_back(std::async(std::launch::async, install_single_package, t));
    }

    for (auto& j : jobs) {
        try {
            j.get();
        } catch (...) {
        }
    }
}

int cmd_install(int argc, char* argv[]) {
    if (argc == 2) {
        std::string pkg_json_path = "package.json";
        if (!fs::exists(pkg_json_path)) {
            std::cerr << "[Lynx ERROR]: No package.json found in the current directory!\n";
            return 1;
        }

        std::ifstream file(pkg_json_path);
        json pkg_json;
        try {
            file >> pkg_json;
            file.close();
        } catch (...) {
            std::cerr << "[Lynx ERROR]: Failed to parse package.json. Invalid JSON format!\n";
            if (file.is_open()) file.close();
            return 1;
        }

        std::cout << "[Lynx]: Found package.json. Scanning dependencies...\n";
        std::cout << "[Lynx]: Parallel install (max " << LYNX_MAX_PARALLEL << " jobs)\n\n";

        std::vector<std::string> all_targets;

        if (pkg_json.contains("dependencies") && !pkg_json["dependencies"].empty()) {
            for (auto& [name, version] : pkg_json["dependencies"].items()) {
                all_targets.push_back(name + "@" + version.get<std::string>());
            }
        }

        if (pkg_json.contains("devDependencies") && !pkg_json["devDependencies"].empty()) {
            for (auto& [name, version] : pkg_json["devDependencies"].items()) {
                all_targets.push_back(name + "@" + version.get<std::string>());
            }
        }
        if (pkg_json.contains("peerDependencies") && !pkg_json["peerDependencies"].empty()) {
            for (auto& [name, version] : pkg_json["peerDependencies"].items()) {
                all_targets.push_back(name + "@" + version.get<std::string>());
            }
        }

        if (pkg_json.contains("optionalDependencies") &&
            !pkg_json["optionalDependencies"].empty()) {
            for (auto& [name, version] : pkg_json["optionalDependencies"].items()) {
                all_targets.push_back(name + "@" + version.get<std::string>());
            }
        }

        if (all_targets.empty()) {
            std::cout << "[Lynx]: No dependencies found to install.\n";
        } else {
            std::cout << "[Lynx]: Installing " << all_targets.size() << " top-level packages...\n\n";
            install_packages_parallel(all_targets);
            std::cout << "\n[Lynx]: All packages installed.\n";
        }
    } else if (argc >= 3) {
        std::vector<std::string> targets;
        for (int i = 2; i < argc; ++i) {
            targets.push_back(argv[i]);
        }
        if (targets.size() == 1) {
            install_single_package(targets[0]);
        } else {
            std::cout << "[Lynx]: Installing " << targets.size()
                      << " packages in parallel...\n\n";
            install_packages_parallel(targets);
        }
    }

    return 0;
}