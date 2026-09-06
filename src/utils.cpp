#include "utils.h"
#include "json.hpp"
#include "picosha2.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>

using json = nlohmann::json;

std::set<std::string> installed_packages;
std::mutex install_mutex;

fs::path get_lynx_cache_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        fs::path p = fs::path(xdg) / "lynx";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        fs::path p = fs::path(home) / ".cache" / "lynx";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
    if (const char* user_profile = std::getenv("USERPROFILE"); user_profile && *user_profile) {
        fs::path p = fs::path(user_profile) / ".lynx";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
    fs::path p = fs::current_path() / ".lynx_cache";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::string sanitize_filename(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == '@' || c == ':' || c == '*') {
            c = '_';
        }
    }
    return name;
}

void generate_bin_shims(const fs::path& package_path, const std::string& package_name) {
    fs::path pkg_json_path = package_path / "package.json";
    if (!fs::exists(pkg_json_path)) return;

    std::ifstream file(pkg_json_path);
    json pkg_json;
    try {
        file >> pkg_json;
        file.close();
    } catch (...) {
        if (file.is_open()) file.close();
        return;
    }

    if (!pkg_json.contains("bin")) return;

    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";
    std::error_code ec;
    fs::create_directories(bin_dir, ec);

    auto create_shim = [&](const std::string& bin_name, const std::string& target_rel_path) {
        {
            fs::path cmd_path = bin_dir / (bin_name + ".cmd");
            std::ofstream cmd_file(cmd_path);
            if (cmd_file.is_open()) {
                std::string current_nm = (fs::current_path() / "node_modules").string();
                cmd_file << "@SETLOCAL\n";
                cmd_file << "@IF NOT DEFINED NODE_PATH (\n";
                cmd_file << "  @SET \"NODE_PATH=" << current_nm << "\\" << package_name
                         << "\\node_modules;" << current_nm << "\"\n";
                cmd_file << ") ELSE (\n";
                cmd_file << "  @SET \"NODE_PATH=" << current_nm << "\\" << package_name
                         << "\\node_modules;" << current_nm << ";%NODE_PATH%\"\n";
                cmd_file << ")\n";
                cmd_file << "@IF EXIST \"%~dp0\\node.exe\" (\n";
                cmd_file << "  \"%~dp0\\node.exe\"  \"%~dp0\\..\\" << package_name << "\\"
                         << target_rel_path << "\" %*\n";
                cmd_file << ") ELSE (\n";
                cmd_file << "  @SET PATHEXT=%PATHEXT:;.JS;=;%\n";
                cmd_file << "  node  \"%~dp0\\..\\" << package_name << "\\" << target_rel_path
                         << "\" %*\n";
                cmd_file << ")\n";
                cmd_file.close();
            }
        }

#ifndef _WIN32
        {
            fs::path sh_path = bin_dir / bin_name;
            std::ofstream sh_file(sh_path);
            if (sh_file.is_open()) {
                std::string current_nm = (fs::current_path() / "node_modules").string();
                sh_file << "#!/bin/sh\n";
                sh_file << "basedir=$(dirname \"$(realpath \"$0\" 2>/dev/null || echo \"$0\")\")\n";
                sh_file << "export NODE_PATH=\"" << current_nm << "/" << package_name
                        << "/node_modules:" << current_nm << "${NODE_PATH:+:$NODE_PATH}\"\n";
                sh_file << "exec node \"$basedir/../" << package_name << "/" << target_rel_path
                        << "\" \"$@\"\n";
                sh_file.close();

                fs::permissions(sh_path,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                    fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::add, ec);
            }
        }
#endif
    };

    if (pkg_json["bin"].is_string()) {
        std::string bin_name = package_name;
        size_t slash_pos = bin_name.find_last_of("/\\");
        if (slash_pos != std::string::npos) bin_name = bin_name.substr(slash_pos + 1);
        create_shim(bin_name, pkg_json["bin"].get<std::string>());
    } else if (pkg_json["bin"].is_object()) {
        for (auto& [bin_name, target_path] : pkg_json["bin"].items()) {
            create_shim(bin_name, target_path.get<std::string>());
        }
    }
}

bool run_lifecycle_scripts(const fs::path& package_path, const std::string& package_name, bool is_root) {
    fs::path pkg_json_path = package_path / "package.json";
    if (!fs::exists(pkg_json_path)) return true;

    std::ifstream file(pkg_json_path);
    json pkg_json;
    try {
        file >> pkg_json;
        file.close();
    } catch (...) {
        return true;
    }

    if (!pkg_json.contains("scripts") || !pkg_json["scripts"].is_object()) {
        return true;
    }

    std::vector<std::string> lifecycle;
    if (is_root) {
        lifecycle = {"preinstall", "install", "postinstall", "prepare"};
    } else {
        lifecycle = {"install", "postinstall"};
    }

    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";
    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";

    bool all_ok = true;

    for (const auto& script_name : lifecycle) {
        if (!pkg_json["scripts"].contains(script_name)) continue;

        std::string script_cmd = pkg_json["scripts"][script_name].get<std::string>();
        if (script_cmd.empty()) continue;

        {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cout << "[Lynx]: Running " << script_name << " for " << package_name << "...\n" << std::flush;
        }

#ifdef _WIN32
        std::string path_for_child = bin_dir.string() + ";" + old_path;
        _putenv_s("PATH", path_for_child.c_str());

        std::string full_cmd = "cmd /d /s /c \"cd /d \"" + package_path.string() +
                               "\" && set \"PATH=" + path_for_child + "\" && " + script_cmd + "\"";
        int ret = std::system(full_cmd.c_str());
#else
        std::string path_for_child = bin_dir.string() + ":" + old_path;
        setenv("PATH", path_for_child.c_str(), 1);

        std::string full_cmd = "cd \"" + package_path.string() + "\" && " + script_cmd;
        int ret = std::system(full_cmd.c_str());
#endif

        if (ret != 0) {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cerr << "[Lynx ERROR]: " << script_name << " script failed for "
                      << package_name << " (exit code " << ret << ")\n";
            all_ok = false;
        } else {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cout << "[Lynx]: " << script_name << " finished for " << package_name << "\n" << std::flush;
        }
    }

#ifdef _WIN32
    _putenv_s("PATH", old_path.c_str());
#else
    setenv("PATH", old_path.c_str(), 1);
#endif

    return all_ok;
}

std::string sha256_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    std::vector<unsigned char> hash(picosha2::k_digest_size);
    picosha2::hash256(file, hash.begin(), hash.end());
    return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

fs::path add_to_cas(const fs::path& source_file) {
    std::string hash = sha256_file(source_file);
    if (hash.empty()) return {};

    std::string dir2 = hash.substr(0, 2);
    fs::path store_path = get_lynx_cache_dir() / "store" / dir2 / hash;

    std::error_code ec;
    if (!fs::exists(store_path, ec)) {
        fs::create_directories(store_path.parent_path(), ec);
        fs::copy_file(source_file, store_path, fs::copy_options::overwrite_existing, ec);
        if (ec) return {};

        fs::permissions(store_path,
            fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
            fs::perm_options::replace, ec);
    }
    return store_path;
}

bool import_package_to_cas(const fs::path& extracted_dir,
                           const std::string& pkg_name,
                           const std::string& version) {
    std::vector<CasFile> files;
    std::error_code ec;

    for (auto& entry : fs::recursive_directory_iterator(extracted_dir,
            fs::directory_options::skip_permission_denied, ec)) {
        if (ec) return false;
        if (!entry.is_regular_file()) continue;

        fs::path rel = fs::relative(entry.path(), extracted_dir, ec);
        if (ec) continue;

        fs::path store_path = add_to_cas(entry.path());
        if (store_path.empty()) {
            std::cerr << "[Lynx ERROR]: Failed to add to CAS: " << entry.path() << "\n";
            return false;
        }

        CasFile cf;
        cf.relative_path = rel.generic_string();
        cf.hash = store_path.filename().string();
        files.push_back(cf);
    }

    fs::path index_dir = get_lynx_cache_dir() / "index";
    fs::create_directories(index_dir, ec);

    std::string index_name = sanitize_filename(pkg_name) + "@" + version + ".json";
    fs::path index_path = index_dir / index_name;

    json j;
    j["name"] = pkg_name;
    j["version"] = version;
    j["files"] = json::array();

    for (const auto& f : files) {
        j["files"].push_back({
            {"path", f.relative_path},
            {"hash", f.hash}
        });
    }

    std::ofstream out(index_path);
    if (!out) return false;
    out << j.dump(2);
    return true;
}

bool is_package_in_cas(const std::string& pkg_name, const std::string& version) {
    std::string index_name = sanitize_filename(pkg_name) + "@" + version + ".json";
    fs::path index_path = get_lynx_cache_dir() / "index" / index_name;
    std::error_code ec;
    return fs::exists(index_path, ec);
}

bool materialize_from_cas(const std::string& pkg_name,
                          const std::string& version,
                          const fs::path& target_dir) {
    std::string index_name = sanitize_filename(pkg_name) + "@" + version + ".json";
    fs::path index_path = get_lynx_cache_dir() / "index" / index_name;

    std::error_code ec;
    if (!fs::exists(index_path, ec)) {
        std::cerr << "[Lynx ERROR]: CAS index not found for " << pkg_name << "@" << version << "\n";
        return false;
    }

    json j;
    {
        std::ifstream in(index_path);
        if (!in) return false;
        in >> j;
    }

    if (fs::exists(target_dir, ec)) {
        fs::remove_all(target_dir, ec);
    }
    fs::create_directories(target_dir, ec);

    for (const auto& item : j["files"]) {
        std::string rel = item["path"].get<std::string>();
        std::string hash = item["hash"].get<std::string>();

        fs::path store_file = get_lynx_cache_dir() / "store" / hash.substr(0, 2) / hash;
        fs::path dest = target_dir / rel;

        fs::create_directories(dest.parent_path(), ec);

        if (fs::exists(dest, ec)) {
            fs::permissions(dest, fs::perms::owner_write, fs::perm_options::add, ec);
            fs::remove(dest, ec);
        }

        ec.clear();
        fs::create_hard_link(store_file, dest, ec);

        if (ec) {
            ec.clear();
            fs::copy_file(store_file, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "[Lynx ERROR]: Cannot link/copy " << rel << " (" << ec.message() << ")\n";
                return false;
            }
        }

        fs::permissions(dest, fs::perms::owner_write, fs::perm_options::add, ec);
    }
    return true;
}

bool hardlink_directory(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return false;

    fs::create_directories(to, ec);
    if (ec) return false;

    for (auto& entry : fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) return false;

        auto rel = fs::relative(entry.path(), from, ec);
        if (ec) return false;

        fs::path dst = to / rel;

        if (entry.is_directory()) {
            fs::create_directories(dst, ec);
        } else if (entry.is_regular_file()) {
            if (fs::exists(dst, ec)) fs::remove(dst, ec);
            fs::create_hard_link(entry.path(), dst, ec);
            if (ec) return false;
        }
    }
    return true;
}

bool link_or_copy_directory(const fs::path& from, const fs::path& to) {
    if (hardlink_directory(from, to)) return true;

    std::error_code ec;
    if (fs::exists(to, ec)) fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}