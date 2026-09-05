#include "utils.h"
#include "json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

std::set<std::string> installed_packages;
std::mutex install_mutex;

fs::path get_lynx_cache_dir() {
    const char* home_dir = std::getenv("USERPROFILE"); // Windows
    if (!home_dir) {
        home_dir = std::getenv("HOME"); // Linux / macOS
    }

    if (!home_dir) return fs::current_path() / ".lynx_cache";
    
    fs::path cache_path = fs::path(home_dir) / ".lynx";
    std::error_code ec;
    if (!fs::exists(cache_path, ec)) {
        fs::create_directories(cache_path, ec);
    }
    return cache_path;
}

std::string sanitize_filename(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == '@') {
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
#ifdef _WIN32
        // Windows (.cmd shim)
        fs::path cmd_path = bin_dir / (bin_name + ".cmd");
        std::ofstream cmd_file(cmd_path);
        if (cmd_file.is_open()) {
            std::string current_nm = (fs::current_path() / "node_modules").string();
            cmd_file << "@SETLOCAL\n";
            cmd_file << "@IF NOT DEFINED NODE_PATH (\n";
            cmd_file << "  @SET \"NODE_PATH=" << current_nm << "\\" << package_name << "\\node_modules;" << current_nm << "\"\n";
            cmd_file << ") ELSE (\n";
            cmd_file << "  @SET \"NODE_PATH=" << current_nm << "\\" << package_name << "\\node_modules;" << current_nm << ";%NODE_PATH%\"\n";
            cmd_file << ")\n";
            cmd_file << "@IF EXIST \"%~dp0\\node.exe\" (\n";
            cmd_file << "  \"%~dp0\\node.exe\"  \"%~dp0\\..\\" << package_name << "\\" << target_rel_path << "\" %*\n";
            cmd_file << ") ELSE (\n";
            cmd_file << "  @SET PATHEXT=%PATHEXT:;.JS;=;%\n";
            cmd_file << "  node  \"%~dp0\\..\\" << package_name << "\\" << target_rel_path << "\" %*\n";
            cmd_file << ")\n";
            cmd_file.close();
        }
#else
        // POSIX / Linux / macOS (Shell script + chmod +x)
        fs::path sh_path = bin_dir / bin_name;
        std::ofstream sh_file(sh_path);
        if (sh_file.is_open()) {
            std::string current_nm = (fs::current_path() / "node_modules").string();
            sh_file << "#!/bin/sh\n";
            sh_file << "basedir=$(dirname \"$(echo \"$0\" | sed -e 's,\\\\,/,g')\")\n";
            sh_file << "export NODE_PATH=\"" << current_nm << "/" << package_name << "/node_modules:" << current_nm << ":$NODE_PATH\"\n";
            sh_file << "exec node \"$basedir/../" << package_name << "/" << target_rel_path << "\" \"$@\"\n";
            sh_file.close();

            // Cấp quyền thực thi rwxr-xr-x (chmod +x)
            fs::permissions(sh_path,
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec |
                fs::perms::owner_read | fs::perms::owner_write |
                fs::perms::group_read | fs::perms::others_read,
                fs::perm_options::add, ec);
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
