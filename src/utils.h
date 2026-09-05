#pragma once

#include <filesystem>
#include <string>
#include <set>
#include <mutex>

namespace fs = std::filesystem;

// Tránh cài trùng package trong 1 phiên (thread-safe)
extern std::set<std::string> installed_packages;
extern std::mutex install_mutex;

// Số job tải song song tối đa
constexpr int LYNX_MAX_PARALLEL = 4;

fs::path get_lynx_cache_dir();
std::string sanitize_filename(std::string name);
void generate_bin_shims(const fs::path& package_path, const std::string& package_name);

// Chạy lifecycle scripts của 1 package (preinstall / install / postinstall)
// Trả về true nếu tất cả script chạy thành công (hoặc không có script)
bool run_lifecycle_scripts(const fs::path& package_path, const std::string& package_name);