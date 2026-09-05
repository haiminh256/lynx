#include "utils.h"
#include "json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>

using json = nlohmann::json;

std::set<std::string> installed_packages;
std::mutex install_mutex;

fs::path get_lynx_cache_dir() {
    // Cross-platform cache
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
        // Windows .cmd
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
        // Unix shell script
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

// ====================== LIFECYCLE SCRIPTS ======================
bool run_lifecycle_scripts(const fs::path& package_path, const std::string& package_name) {
    fs::path pkg_json_path = package_path / "package.json";
    if (!fs::exists(pkg_json_path)) return true;

    std::ifstream file(pkg_json_path);
    json pkg_json;
    try {
        file >> pkg_json;
        file.close();
    } catch (...) {
        return true; // không có scripts hợp lệ thì bỏ qua
    }

    if (!pkg_json.contains("scripts") || !pkg_json["scripts"].is_object()) {
        return true;
    }

    // Thứ tự chuẩn của npm
    const std::vector<std::string> lifecycle = {
        "preinstall",
        "install",
        "postinstall"
        // có thể thêm "prepare" nếu muốn
    };

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

        // Chạy trong thư mục của package
        std::string full_cmd = "cmd /d /s /c \"cd /d \"" + package_path.string() +
                               "\" && set \"PATH=" + path_for_child + "\" && " + script_cmd + "\"";
        int ret = std::system(full_cmd.c_str());
#else
        std::string path_for_child = bin_dir.string() + ":" + old_path;
        setenv("PATH", path_for_child.c_str(), 1);

        // Dùng sh -c để chạy đúng môi trường + cwd
        std::string full_cmd = "cd \"" + package_path.string() + "\" && " + script_cmd;
        int ret = std::system(full_cmd.c_str());
#endif

        if (ret != 0) {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cerr << "[Lynx ERROR]: " << script_name << " script failed for "
                      << package_name << " (exit code " << ret << ")\n";
            all_ok = false;
            // Có thể break sớm nếu muốn fail-fast
            // break;
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