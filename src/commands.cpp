#include "commands.h"
#include "installer.h"
#include "lockfile.h"
#include "utils.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = std::filesystem;

int InstallCommand::execute(const std::vector<std::string>& args) {
    PackageInstaller installer;

    if (args.empty()) {
        std::string pkg_json_path = "package.json";
        if (!fs::exists(pkg_json_path)) {
            std::cerr << "[Lynx ERROR]: No package.json found in current directory!\n";
            return 1;
        }

        std::ifstream file(pkg_json_path);
        json pkg_json;
        try {
            file >> pkg_json;
            file.close();
        } catch (...) {
            std::cerr << "[Lynx ERROR]: Invalid package.json format!\n";
            return 1;
        }

        std::cout << "[Lynx]: Found package.json. Scanning dependencies...\n";
        std::vector<std::string> all_targets;

        const std::vector<std::string> dep_keys = {
            "dependencies", "devDependencies", "peerDependencies", "optionalDependencies"
        };

        for (const auto& key : dep_keys) {
            if (pkg_json.contains(key) && !pkg_json[key].empty()) {
                for (auto& [name, version] : pkg_json[key].items()) {
                    all_targets.push_back(name + "@" + version.get<std::string>());
                }
            }
        }

        if (all_targets.empty()) {
            std::cout << "[Lynx]: No dependencies found to install.\n";
        } else {
            std::cout << "[Lynx]: Installing " << all_targets.size() << " packages...\n\n";
            installer.install_packages_parallel(all_targets);
            std::cout << "\n[Lynx]: All packages installed.\n";
        }
    } else if (args.size() == 1) {
        installer.install_single_package(args[0]);
    } else {
        installer.install_packages_parallel(args);
    }

    g_lockfile.save();
    std::cout << "[Lynx]: Updated lynx-lock.json\n";
    return 0;
}

int UninstallCommand::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "[Lynx ERROR]: Please specify a package to uninstall!\n";
        return 1;
    }

    std::string target_package = args[0];
    fs::path target_path = fs::current_path() / "node_modules" / target_package;

    if (fs::exists(target_path) || fs::is_symlink(target_path)) {
        std::cout << "[Lynx]: Removing " << target_package << "...\n";
        fs::remove_all(target_path);

        g_lockfile.remove_package(target_package);
        g_lockfile.save();

        std::cout << "[Lynx]: Successfully uninstalled " << target_package << ".\n";
    } else {
        std::cerr << "[Lynx ERROR]: Package " << target_package << " is not installed.\n";
        return 1;
    }
    return 0;
}

int RunCommand::execute(const std::vector<std::string>& args) {
    std::string pkg_json_path = "package.json";
    if (!fs::exists(pkg_json_path)) {
        std::cerr << "[Lynx ERROR]: No package.json found!\n";
        return 1;
    }

    std::ifstream file(pkg_json_path);
    json pkg_json;
    try {
        file >> pkg_json;
    } catch (...) {
        std::cerr << "[Lynx ERROR]: Failed to parse package.json!\n";
        return 1;
    }

    if (args.empty()) {
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

    std::string script_name = args[0];
    if (!pkg_json.contains("scripts") || !pkg_json["scripts"].contains(script_name)) {
        std::cerr << "[Lynx ERROR]: Script \"" << script_name << "\" not found!\n";
        return 1;
    }

    std::string script_cmd = pkg_json["scripts"][script_name].get<std::string>();
    for (size_t i = 1; i < args.size(); ++i) {
        script_cmd += " " + args[i];
    }

    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";
    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";

#ifdef _WIN32
    std::string path_for_child = bin_dir.string() + ";" + old_path;
    _putenv_s("PATH", path_for_child.c_str());
    std::string full_cmd = "cmd /d /s /c \"set \"PATH=" + path_for_child + "\" && " + script_cmd + "\"";
    return std::system(full_cmd.c_str());
#else
    std::string path_for_child = bin_dir.string() + ":" + old_path;
    setenv("PATH", path_for_child.c_str(), 1);
    return std::system(script_cmd.c_str());
#endif
}

int CreateCommand::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "[Lynx ERROR]: Usage: lynx create <template>[@version]\n";
        return 1;
    }

    std::string raw_input = args[0];
    std::string pkg_name = raw_input;
    std::string version = "";

    size_t at_pos = raw_input.find('@');
    if (at_pos == 0) {
        at_pos = raw_input.find('@', 1);
    }
    if (at_pos != std::string::npos && at_pos > 0) {
        pkg_name = raw_input.substr(0, at_pos);
        version = raw_input.substr(at_pos);
    }

    if (pkg_name.rfind("create-", 0) != 0) {
        pkg_name = "create-" + pkg_name;
    }

    std::string full_install_pkg = pkg_name + version;

    PackageInstaller installer;
    std::cout << "[Lynx]: Installing creator package " << full_install_pkg << "...\n";
    if (!installer.install_single_package(full_install_pkg)) {
        return 1;
    }

    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";

    std::string bin_name = pkg_name; 

    std::string run_cmd = bin_name;
    for (size_t i = 1; i < args.size(); ++i) {
        run_cmd += " " + args[i];
    }

    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";

#ifdef _WIN32
    std::string path_for_child = bin_dir.string() + ";" + old_path;
    _putenv_s("PATH", path_for_child.c_str());
    std::string full_cmd = "cmd /d /s /c \"set \"PATH=" + path_for_child + "\" && " + run_cmd + "\"";
    return std::system(full_cmd.c_str());
#else
    std::string path_for_child = bin_dir.string() + ":" + old_path;
    setenv("PATH", path_for_child.c_str(), 1);
    return std::system(run_cmd.c_str());
#endif
}

std::unique_ptr<ICommand> CommandFactory::create_command(const std::string& name) {
    if (name == "install") return std::make_unique<InstallCommand>();
    if (name == "uninstall") return std::make_unique<UninstallCommand>();
    if (name == "run") return std::make_unique<RunCommand>();
    if (name == "create") return std::make_unique<CreateCommand>();
    return nullptr;
}