#include "run.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <cstdio>

using json = nlohmann::json;
namespace fs = std::filesystem;

static std::string find_node_executable() {
#ifdef _WIN32
    FILE* pipe = _popen("where node 2>nul", "r");
#else
    FILE* pipe = popen("command -v node 2>/dev/null", "r");
#endif
    if (!pipe) return "node";

    std::string result;
    char buf[512];
    if (fgets(buf, sizeof(buf), pipe)) {
        result = buf;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    if (result.empty()) return "node";
    return result;
}

static std::string get_node_dir(const std::string& node_path) {
    try {
        fs::path p(node_path);
        if (p.has_parent_path()) return p.parent_path().string();
    } catch (...) {
    }
    return "";
}

int cmd_run(int argc, char* argv[]) {
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
        std::cerr << "[Lynx ERROR]: Failed to parse package.json!\n";
        if (file.is_open()) file.close();
        return 1;
    }

    if (argc < 3) {
        if (!pkg_json.contains("scripts") || pkg_json["scripts"].empty()) {
            std::cout << "[Lynx]: No scripts found in package.json.\n";
            return 0;
        }
        std::cout << "[Lynx]: Available scripts:\n\n";
        for (auto& [name, cmd] : pkg_json["scripts"].items()) {
            std::cout << "  " << name << "\n    " << cmd.get<std::string>() << "\n\n";
        }
        return 0;
    }

    std::string script_name = argv[2];

    if (!pkg_json.contains("scripts") || !pkg_json["scripts"].contains(script_name)) {
        std::cerr << "[Lynx ERROR]: Script \"" << script_name << "\" not found in package.json!\n";
        return 1;
    }

    std::string script_cmd = pkg_json["scripts"][script_name].get<std::string>();

    for (int i = 3; i < argc; ++i) {
        script_cmd += " ";
        script_cmd += argv[i];
    }

    std::string node_exe = find_node_executable();
    std::string node_dir = get_node_dir(node_exe);
    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";

    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";

#ifdef _WIN32
    std::string path_for_child = bin_dir.string() + ";" + node_dir + ";" + old_path;

    _putenv_s("PATH", path_for_child.c_str());

    if (pkg_json.contains("name")) {
        _putenv_s("npm_package_name", pkg_json["name"].get<std::string>().c_str());
    }
    _putenv_s("npm_lifecycle_event", script_name.c_str());

    std::cout << "[Lynx]: Running \"" << script_name << "\": " << script_cmd << "\n";
    std::cout << "[Lynx]: Using node at: " << node_exe << "\n\n" << std::flush;

    // Set PATH ngay trong lệnh — chắc chắn cmd con nhận được
    std::string full_cmd =
        "cmd /d /s /c \"set \"PATH=" + path_for_child + "\" && " + script_cmd + "\"";

    int result = std::system(full_cmd.c_str());
#else
    std::string path_for_child = bin_dir.string() + ":" + node_dir + ":" + old_path;
    setenv("PATH", path_for_child.c_str(), 1);

    if (pkg_json.contains("name")) {
        setenv("npm_package_name", pkg_json["name"].get<std::string>().c_str(), 1);
    }
    setenv("npm_lifecycle_event", script_name.c_str(), 1);

    std::cout << "[Lynx]: Running \"" << script_name << "\": " << script_cmd << "\n";

    int result = std::system(script_cmd.c_str());
#endif

    if (result != 0) {
        std::cerr << "[Lynx ERROR]: Script \"" << script_name << "\" exited with code " << result << "\n";
        return result != 0 ? result : 1;
    }

    return 0;
}