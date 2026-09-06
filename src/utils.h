#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <set>
#include <mutex>

namespace fs = std::filesystem;

extern std::set<std::string> installed_packages;
extern std::mutex install_mutex;

constexpr int LYNX_MAX_PARALLEL = 30;

fs::path get_lynx_cache_dir();
std::string sanitize_filename(std::string name);
void generate_bin_shims(const fs::path& package_path, const std::string& package_name);

bool run_lifecycle_scripts(const fs::path& package_path, const std::string& package_name, bool is_root = false);

struct CasFile {
    std::string relative_path;
    std::string hash;
};

std::string sha256_file(const fs::path& path);

fs::path add_to_cas(const fs::path& source_file);

bool import_package_to_cas(const fs::path& extracted_dir,
                           const std::string& pkg_name,
                           const std::string& version);

bool is_package_in_cas(const std::string& pkg_name, const std::string& version);

bool materialize_from_cas(const std::string& pkg_name,
                          const std::string& version,
                          const fs::path& target_dir);

bool hardlink_directory(const fs::path& from, const fs::path& to);
bool link_or_copy_directory(const fs::path& from, const fs::path& to);