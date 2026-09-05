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
std::string get_current_os() {
#if defined(_WIN32) || defined(_WIN64)
    return "win32";
#elif defined(__APPLE__) || defined(__MACH__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unknown";
#endif
}

std::string get_current_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "ia32";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

// Kiem tra xem package meta/json co phu hop voi OS & Arch hien tai khong
bool is_platform_supported(const json& pkg_meta) {
    std::string current_os = get_current_os();
    std::string current_arch = get_current_arch();

    // 1. Kiem tra truong "os" trong package.json / metadata
    if (pkg_meta.contains("os") && pkg_meta["os"].is_array()) {
        bool os_match = false;
        bool negated_os = false;

        for (const auto& os_item : pkg_meta["os"]) {
            std::string os_str = os_item.get<std::string>();
            if (os_str.rfind("!", 0) == 0) { // Negation (vi du: "!win32")
                if (os_str.substr(1) == current_os) {
                    return false; // Bi loai tru truc tiep
                }
            } else {
                negated_os = true;
                if (os_str == current_os) {
                    os_match = true;
                }
            }
        }
        if (negated_os && !os_match) return false;
    }

    // 2. Kiem tra truong "cpu" trong package.json / metadata
    if (pkg_meta.contains("cpu") && pkg_meta["cpu"].is_array()) {
        bool cpu_match = false;
        bool negated_cpu = false;

        for (const auto& cpu_item : pkg_meta["cpu"]) {
            std::string cpu_str = cpu_item.get<std::string>();
            if (cpu_str.rfind("!", 0) == 0) { // Negation (vi du: "!x64")
                if (cpu_str.substr(1) == current_arch) {
                    return false; // Bi loai tru truc tiep
                }
            } else {
                negated_cpu = true;
                if (cpu_str == current_arch) {
                    cpu_match = true;
                }
            }
        }
        if (negated_cpu && !cpu_match) return false;
    }

    return true;
}
bool is_package_name_compatible(const std::string& pkg_name) {
    std::string os = get_current_os();
    std::string arch = get_current_arch();

    const std::vector<std::string> known_oses = {
        "win32", "darwin", "linux", "freebsd", "openbsd", "netbsd", "aix", "solaris", "sunos", "android", "openharmony"
    };

    const std::vector<std::string> known_arches = {
        "x64", "arm64", "ia32", "arm", "ppc64", "s390x", "riscv64", "loong64", "x86_64"
    };

    // 1. Kiểm tra OS
    std::string matched_os = "";
    for (const auto& o : known_oses) {
        if (pkg_name.find(o) != std::string::npos) {
            matched_os = o;
            break;
        }
    }
    if (!matched_os.empty() && matched_os != os) {
        return false;
    }

    // 2. Kiểm tra Arch
    std::string matched_arch = "";
    for (const auto& a : known_arches) {
        if (pkg_name.find(a) != std::string::npos) {
            matched_arch = a;
            break;
        }
    }
    if (!matched_arch.empty()) {
        bool arch_matches = (matched_arch == arch) ||
                             (arch == "x64" && matched_arch == "x86_64");
        if (!arch_matches) return false;
    }

    // 3. Lọc bỏ gói musl trên các hệ thống Linux glibc (Ubuntu, Debian, Fedora,...)
    if (os == "linux") {
        if (pkg_name.find("musl") != std::string::npos) {
#ifndef __musl__
            return false;
#endif
        }
    }

    return true;
}
