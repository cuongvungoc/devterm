# Kiểm thử gói Ubuntu và Linux Mint

Ngày thực hiện: 06/11/2026. Máy build Ubuntu 24.04 amd64; executable ARM64 chạy bằng QEMU/binfmt. Container kiểm tra bản cài không chứa compiler hay SDK GTK của builder. GUI chạy bằng tài khoản thường trên X11/Xvfb.

| Hệ điều hành | CPU | Bộ test ứng dụng | Kiểm tra gói đã cài |
| --- | --- | --- | --- |
| Ubuntu 22.04 | amd64 | 46/46, không skip | Đạt |
| Ubuntu 22.04 | arm64 | 46/46, không skip | Đạt |
| Ubuntu 24.04 | amd64 | 46/46, không skip | Đạt |
| Ubuntu 24.04 | arm64 | 46/46, không skip | Đạt |

46 test thuộc 9 nhóm: commands (4), layout (6), search (4), contents (4), export (5), sessions (5), session-ui (3), connections (12), draft (3). Log đầy đủ nằm cạnh từng artifact trong `dist/*.tests.txt`; `.build.json` ghi package dependency, toolchain, source runtime và SHA-256.

Kiểm tra bản cài qua `packaging/verify-debs.py` bao gồm:

- APT cài artifact vào Ubuntu sạch và giải quyết dependency; chạy `--version` cho cả app và helper, kiểm tra thư viện ELF và checksum file đã cài.
- Mở launcher `/usr/bin/devterm`, nhập lệnh vào PTY thật, split phải/xuống, mở tab, soạn Draft không thực thi nội dung và mở/hủy hộp thoại Export.
- Mở Sessions và sử dụng helper đã cài để gọi SSH client giả phục vụ kiểm tra. Client không kết nối ra mạng; bài này kiểm tra đường tìm helper và môi trường chương trình con.
- Shell và client nạp GLib hệ thống, không nhận đường dẫn thư viện/schema riêng của DevTerm. Đây là kiểm tra quan trọng cho runtime riêng của Ubuntu 22.04.
- Thử đường từ chối OS marker không khớp, nâng package lên revision kiểm thử, gỡ và cài lại, xác nhận JSON cấu hình người dùng vẫn giữ nguyên. Các thay đổi OS marker chỉ xảy ra trong container dùng một lần.

Kết quả máy đọc được nằm trong `dist/*.install.json`, kèm checksum đúng artifact được cài. `dist/SHA256SUMS` dùng để kiểm tra file trước khi chuyển máy.

## Giới hạn và nghiệm thu trên máy đích

- Chưa thử trên phần cứng ARM64 thật, GPU hoặc phiên Wayland thật. QEMU/Xvfb xác minh executable và luồng ứng dụng, không xác minh hiệu năng đồ họa trên thiết bị.
- Test connections dùng SSH/Telnet server loopback và PTY giả cho Serial. Chưa thử router hay cổng USB-Serial vật lý của người dùng.
- Keyring được kiểm tra trong môi trường D-Bus/test service; vẫn cần xác minh mở khóa Secret Service trong phiên desktop thực tế.
- Bài nâng cấp dùng cùng payload với revision cao hơn để kiểm tra vòng đời dpkg và giữ cấu hình; chưa có bản phát hành DevTerm cũ độc lập để kiểm tra migration giữa các phiên bản.

Trên máy đích: cài đúng gói bằng APT; mở từ menu, thử resize/split/scrollbar, Quick Commands, Search, Draft và lưu Export; thử SSH password/host-key/keyring, Telnet và Serial với thiết bị của mình; kiểm tra theme/font và clipboard với desktop đang dùng. Lưu công việc/nháp trước khi đóng ứng dụng để nâng cấp.

## Linux Mint 22.1 Xia amd64 — 11/11/2026

Artifact: `devterm_0.1.0-1~linuxmint22.1_amd64.deb`. Binary và các file ứng dụng được giữ nguyên từ gói Noble amd64 đã qua 46 test ở trên; chỉ thay control/version/preinst và thêm tài liệu Mint. Đây là kết quả test binary kế thừa, không phải chạy lại 46 test trên Mint.

Đã kiểm tra độc lập gói Mint trong container tối giản nền Noble có kho Xia được APT xác thực, dùng gói `base-files` chính thức `13ubuntu10mint22.1.0+xia`. `/etc/os-release` do gói này cung cấp trả về `ID=linuxmint`, `VERSION_ID=22.1`, `VERSION_CODENAME=xia`, `UBUNTU_CODENAME=noble`, khớp máy được cung cấp; không dùng file nhận diện giả để vượt điều kiện cài đặt.

- Đạt: APT cài mới, ELF không thiếu thư viện, checksum file đã cài, app/helper `--version`.
- Đạt: launcher đã cài, lệnh trong PTY, split phải/xuống, tab mới, Draft không thực thi, mở/hủy Export và helper gọi client thử nghiệm, không truyền runtime riêng vào chương trình con; app thoát bình thường.
- Đạt: từ chối sai version, sai distro, sai Ubuntu base và thiếu Ubuntu base. Các bài từ chối tạm thay OS marker trong container, rồi khôi phục trước khi tiếp tục.
- Đạt: nâng revision thử nghiệm, gỡ/cài lại và bảo toàn cấu hình người dùng.

Provenance và kết quả nằm trong `dist/devterm_0.1.0-1~linuxmint22.1_amd64.build.json` và `.install.json`. Chưa kiểm tra trực tiếp desktop, keyring và phần cứng của máy Mint người dùng; các giới hạn Wayland/Serial ở trên vẫn áp dụng. Xem [hướng dẫn Mint](MINT.md) để cài và tạo lại gói.
