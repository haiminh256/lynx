# 🚀 Lynx CLI

**Lynx** là một công cụ quản lý gói (package manager) siêu nhẹ, hiệu năng cao dành cho các dự án web, được viết bằng **C++17**. Lynx hỗ trợ tự động tải, giải nén và quản lý dependencies từ registry npm với tốc độ tối ưu và khả năng quản lý lockfile chính xác.

---

## 🛠️ Tính năng nổi bật

- **Tốc độ cực nhanh**: Biên dịch bằng C++17 giúp giảm thiểu overhead so với các package manager truyền thống.
- **Hỗ trợ đa nền tảng**: Biên dịch và chạy mượt mà trên **Windows**, **macOS**, và **Linux**.
- **Quản lý lockfile**: Tự động tạo và cập nhật lockfile để đảm bảo môi trường cài đặt đồng nhất.
- **Xử lý bất đồng bộ & đa luồng**: Tối ưu hóa thời gian tải dependencies.

---

## 📋 Yêu cầu hệ thống

Để biên dịch Lynx từ mã nguồn, bạn cần chuẩn bị:

- **Trình biên dịch C++**: Hỗ trợ C++17 trở lên (`g++`, `clang++`, hoặc `MSVC`).
- **Nền tảng Windows**: Khuyên dùng **MinGW-w64** (với `mingw32-make`).
- **Nền tảng Linux/macOS**: Thư viện `pthread` và `make`.

---

## 🔧 Hướng dẫn cài đặt & Biên dịch

### 1. Clone repository

```bash
git clone https://github.com/haiminh256/lynx.git
cd lynx
make