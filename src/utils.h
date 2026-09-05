#pragma once

#include <filesystem>
#include <string>
#include <set>
#include <mutex>

namespace fs = std::filesystem;

extern std::set<std::string> installed_packages;
extern std::mutex install_mutex;

constexpr int LYNX_MAX_PARALLEL = 4;

fs::path get_lynx_cache_dir();
std::string sanitize_filename(std::string name);
void generate_bin_shims(const fs::path& package_path, const std::string& package_name);

bool run_lifecycle_scripts(const fs::path& package_path, const std::string& package_name);
bool hardlink_directory(const fs::path& from, const fs::path& to);

bool link_or_copy_directory(const fs::path& from, const fs::path& to);