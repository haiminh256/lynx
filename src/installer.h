#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

struct TempFileCleaner {
    fs::path filepath;
    ~TempFileCleaner() {
        std::error_code ec;
        if (!filepath.empty() && fs::exists(filepath, ec)) {
            fs::remove(filepath, ec);
        }
    }
};

class PackageInstaller {
public:
    PackageInstaller() = default;

    bool install_single_package(const std::string& raw_input);
    void install_packages_parallel(const std::vector<std::string>& targets);

private:
    std::string make_unique_temp(const std::string& package_name);
    void safe_remove(const fs::path& p);
};