#include "create.h"
#include "install.h"
#include "utils.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Chuyển "vite@latest" → "create-vite@latest"
// Nếu đã là "create-xxx" thì giữ nguyên
static std::string resolve_create_package(const std::string& raw) {
    std::string name = raw;
    std::string version;

    size_t at_pos = raw.find('@');
    if (at_pos == 0) {
        // scoped: @scope/pkg@version — hiếm với create
        at_pos = raw.find('@', 1);
    }
    if (at_pos != std::string::npos && at_pos > 0) {
        name = raw.substr(0, at_pos);
        version = raw.substr(at_pos); // giữ cả @version
    }

    // Nếu chưa bắt đầu bằng create- thì thêm prefix
    if (name.rfind("create-", 0) != 0) {
        name = "create-" + name;
    }

    return name + version;
}

// Lấy đường dẫn bin chính của package vừa cài
static bool get_package_bin(const fs::path& package_dir,
                            const std::string& package_name,
                            std::string& out_bin_path) {
    fs::path pkg_json_path = package_dir / "package.json";
    if (!fs::exists(pkg_json_path)) return false;

    std::ifstream file(pkg_json_path);
    json pkg_json;
    try {
        file >> pkg_json;
        file.close();
    } catch (...) {
        if (file.is_open()) file.close();
        return false;
    }

    if (!pkg_json.contains("bin")) return false;

    std::string rel;
    if (pkg_json["bin"].is_string()) {
        rel = pkg_json["bin"].get<std::string>();
    } else if (pkg_json["bin"].is_object()) {
        // Ưu tiên bin trùng tên package, không thì lấy cái đầu
        std::string short_name = package_name;
        size_t slash = short_name.find_last_of("/\\");
        if (slash != std::string::npos) short_name = short_name.substr(slash + 1);

        if (pkg_json["bin"].contains(short_name)) {
            rel = pkg_json["bin"][short_name].get<std::string>();
        } else {
            // lấy entry đầu tiên
            for (auto& [k, v] : pkg_json["bin"].items()) {
                rel = v.get<std::string>();
                break;
            }
        }
    }

    if (rel.empty()) return false;

    out_bin_path = (package_dir / rel).string();
    return fs::exists(out_bin_path);
}

int cmd_create(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "[Lynx ERROR]: Usage: lynx create <template>[@version] [args...]\n"
                  << "  Example: lynx create vite@latest\n"
                  << "  Example: lynx create next-app\n";
        return 1;
    }

    std::string raw_input = argv[2];
    std::string create_pkg = resolve_create_package(raw_input);

    // Tách tên package (không version) để tìm folder
    std::string package_name = create_pkg;
    size_t at_pos = create_pkg.find('@');
    if (at_pos == 0) at_pos = create_pkg.find('@', 1);
    if (at_pos != std::string::npos && at_pos > 0) {
        package_name = create_pkg.substr(0, at_pos);
    }

    std::cout << "[Lynx]: Creating project with " << create_pkg << "...\n\n";

    // Cài package create-* (vào node_modules + cache)
    if (!install_single_package(create_pkg)) {
        std::cerr << "[Lynx ERROR]: Failed to install " << create_pkg << "\n";
        return 1;
    }

    fs::path package_dir = fs::current_path() / "node_modules" / package_name;
    if (!fs::exists(package_dir)) {
        std::cerr << "[Lynx ERROR]: Package directory not found: " << package_dir << "\n";
        return 1;
    }

    std::string bin_path;
    if (!get_package_bin(package_dir, package_name, bin_path)) {
        std::cerr << "[Lynx ERROR]: No bin entry found in " << package_name << "\n";
        return 1;
    }

    // Ghép lệnh: node <bin> [các arg còn lại]
    std::string cmd = "node \"" + bin_path + "\"";
    for (int i = 3; i < argc; ++i) {
        cmd += " ";
        cmd += argv[i];
    }

    // Đảm bảo node_modules/.bin có trong PATH
    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";
    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";
#ifdef _WIN32
    std::string new_path = bin_dir.string() + ";" + old_path;
    _putenv_s("PATH", new_path.c_str());
#else
    std::string new_path = bin_dir.string() + ":" + old_path;
    setenv("PATH", new_path.c_str(), 1);
#endif

    std::cout << "[Lynx]: Running: " << cmd << "\n\n";
    std::cout.flush();

#ifdef _WIN32
    std::string full_cmd = "cmd /c \"" + cmd + "\"";
    int result = std::system(full_cmd.c_str());
#else
    int result = std::system(cmd.c_str());
#endif

    if (result != 0) {
        std::cerr << "[Lynx ERROR]: create exited with code " << result << "\n";
        return result != 0 ? result : 1;
    }

    return 0;
}