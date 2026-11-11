# DevTerm cho Linux Mint 22.1 Xia amd64

Gói này dành cho máy có `ID=linuxmint`, `VERSION_ID=22.1`, `UBUNTU_CODENAME=noble` trong `/etc/os-release` và `dpkg --print-architecture` trả về `amd64`, đúng thông tin máy được cung cấp. Các bản Mint khác và LMDE chưa nằm trong phạm vi gói này.

## Cài đặt

Mở terminal tại thư mục chứa file rồi chạy:

```sh
sudo apt update
sudo apt install ./devterm_0.1.0-1~linuxmint22.1_amd64.deb
devterm
```

Có thể mở DevTerm từ menu ứng dụng. APT tải các dependency qua Internet. Không cần Meson, compiler hoặc Docker trên máy đích. Không cần thay đổi `/etc/os-release` của máy. Gỡ bằng `sudo apt remove devterm`; cấu hình trong thư mục người dùng được giữ nguyên.

## Tạo lại gói

```sh
# Nếu chưa có bản Noble đã build/test:
python3 packaging/build-debs.py --ubuntu 24.04 --arch amd64
# Tạo biến thể Mint từ artifact và báo cáo build tương ứng:
python3 packaging/build-mint-deb.py dist/devterm_0.1.0-1~ubuntu24.04_amd64.deb
python3 packaging/verify-debs.py dist/devterm_0.1.0-1~linuxmint22.1_amd64.deb
```

Gói Mint giữ nguyên binary, launcher, icon, dependency và các file ứng dụng của bản Noble đã qua 46 test. Chỉ cập nhật version, điều kiện cài đặt và thêm tài liệu này. `.build.json` ghi checksum gói nguồn và từng file được giữ nguyên; kết quả test binary được kế thừa từ bản nguồn, không phải một lần chạy lại 46 test trên Mint. Gói và checksum nằm trong `dist/`.

Kiểm tra bản cài dùng container tối giản với kho Xia và gói `base-files/xia` được APT xác thực của Linux Mint trên nền Noble. GUI chạy trên X11/Xvfb; chưa thay thế kiểm tra trực tiếp phiên Cinnamon, keyring, Wayland hoặc thiết bị Serial trên máy người dùng. Kết quả cụ thể nằm trong `.install.json` và [báo cáo kiểm thử](TESTING.md).
