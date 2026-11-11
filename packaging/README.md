# Gói cài DevTerm cho Ubuntu và Linux Mint

Hỗ trợ Ubuntu **22.04 / 24.04**, mỗi bản có gói **amd64 / arm64** riêng. Cần desktop X11 hoặc Wayland và Internet để APT tải dependency. Bốn gói là những bản build riêng, không phải đổi nhãn kiến trúc của cùng một binary.

Có thêm biến thể **Linux Mint 22.1 Xia amd64** dùng lại payload của bản Noble đã kiểm thử, với version và điều kiện cài đặt riêng. Xem [cách cài và tạo lại gói Mint](MINT.md).

## Cài đặt

Kiểm tra `VERSION_ID` trong `/etc/os-release` và `dpkg --print-architecture`, rồi chọn đúng file trong `dist/`:

| Hệ điều hành | CPU | File phiên bản 0.1.0, revision 1 |
| --- | --- | --- |
| 22.04 | Intel/AMD 64 bit (`amd64`) | `devterm_0.1.0-1~ubuntu22.04_amd64.deb` |
| 22.04 | ARM 64 bit (`arm64`) | `devterm_0.1.0-1~ubuntu22.04_arm64.deb` |
| 24.04 | Intel/AMD 64 bit (`amd64`) | `devterm_0.1.0-1~ubuntu24.04_amd64.deb` |
| 24.04 | ARM 64 bit (`arm64`) | `devterm_0.1.0-1~ubuntu24.04_arm64.deb` |
| Linux Mint 22.1 Xia | Intel/AMD 64 bit (`amd64`) | `devterm_0.1.0-1~linuxmint22.1_amd64.deb` |

```sh
# Ví dụ Ubuntu 24.04, CPU Intel/AMD 64 bit
sudo apt install ./devterm_0.1.0-1~ubuntu24.04_amd64.deb
devterm
devterm --version
```

Có thể mở DevTerm từ menu ứng dụng. CPU ARM64 chọn file có hậu tố `_arm64.deb`. Gói Ubuntu sẽ từ chối cài trên bản Ubuntu không khớp. Gói Mint chỉ chấp nhận `linuxmint`, version `22.1`, base `noble`; chưa mở rộng sang Mint khác hoặc LMDE.

`apt install` một file `.deb` có phiên bản mới để nâng cấp; `sudo apt remove devterm` để gỡ. Các script cài/gỡ không sửa hoặc xóa `~/.config/devterm/`. Password thuộc Secret Service của người dùng, không nằm trong gói hay JSON. Đăng nhập desktop và mở khóa keyring để ghi nhớ password; thiếu dịch vụ vẫn dùng password một lần. Serial cần quyền thiết bị của tài khoản, thường là nhóm `dialout`; ứng dụng không chạy bằng root.

Sau khi nâng cấp, đóng/mở lại DevTerm sau khi đã lưu công việc và copy nháp cần giữ để sử dụng executable mới.

## Build cả bốn gói

Máy build cần Docker hoạt động, Python 3 và đủ dung lượng đĩa. Các compiler, Meson và dependency nằm trong container. Lần build đầu tải các package Ubuntu và tool build; nguồn của runtime riêng được kiểm tra hash và lưu trong `dist/sources/`.

```sh
python3 packaging/build-debs.py
# Hoặc chọn một tổ hợp:
python3 packaging/build-debs.py --ubuntu 24.04 --arch amd64
```

Mặc định dùng tối đa 2 job để hạn chế RAM. Dùng `--jobs N`, `--output /path/to/dist`, `--revision N` để điều chỉnh. Gói mang version trong `meson.build` cộng revision Ubuntu. Mỗi gói có `.tests.txt`, `.build.json`; `SHA256SUMS` chứa checksum các gói. Script dừng khi test lỗi hoặc có skip.

Build khác kiến trúc host cần QEMU/binfmt cho CPU đích; ví dụ build ARM64 trên x86_64 cần QEMU ARM64, hoặc có thể tạo riêng gói ARM64 trên máy ARM64. Script kiểm tra thực thi kiến trúc trước khi build; không tự thay đổi binfmt của host. Hướng dẫn Docker chính thức: <https://docs.docker.com/build/building/multi-platform/>. QEMU hữu ích cho build/test tự động nhưng không thay thế thử GUI/GPU và Serial trên phần cứng ARM thật.

Script mặc định gỡ các image builder mang tên `devterm-builder:<ubuntu>-<arch>` sau mỗi tổ hợp để tiết kiệm đĩa; không prune image/container khác. Dùng `--keep-images` để giữ builder. Source mount read-only, container không được mount Docker socket. Container chạy root để build/stage package; bộ test chạy bằng tài khoản thường trong container.

Các tag `devterm-builder:*` và `devterm-verify:*` dành riêng cho quy trình này. Không chạy đồng thời hai lệnh build cùng tổ hợp/đường dẫn output: chúng dùng chung cache runtime và tên artifact.

Nếu build lỗi, image builder được giữ để sửa/chạy lại. Runtime 22.04 được cache theo CPU trong `dist/.runtime-cache/`, có fingerprint source lock, option và package toolchain; có thể xóa cache để build lại hoàn toàn. Cache chỉ phục vụ build, không cần chuyển sang máy đích.

## Runtime và tương thích

- Ubuntu 24.04 dùng GTK4/VTE và thư viện hệ thống. Dependency ELF được sinh bằng `dpkg-shlibdeps`; SSH, Telnet, picocom, icon và MIME được khai báo riêng.
- Ubuntu 22.04 có GTK cũ hơn yêu cầu của ứng dụng. Gói này build GLib, Wayland, GTK4 và VTE từ **source Ubuntu Noble có patch Ubuntu**, liên kết với ABI hệ thống Jammy và cài riêng tại `/usr/lib/devterm/runtime`.
- ELF RUNPATH chỉ trỏ đến runtime riêng của binary/thư viện. Launcher không export `LD_LIBRARY_PATH`, `PATH` riêng hay biến GTK vào shell của người dùng. Terminal tiếp tục chạy shell và chương trình của máy.
- Runtime riêng sử dụng thư mục module GIO của Ubuntu qua symlink, gồm backend dconf; module desktop tiếp tục do APT cập nhật. Không chép cấu hình desktop hay module của máy phát triển vào gói.
- Build 22.04 bật `-Dbundled_runtime=true`. Frontend/helper nạp schema GTK riêng trước khi khởi tạo giao diện, rồi khôi phục biến môi trường ngay lập tức. Schema được cache trong tiến trình UI; shell/client không nhận đường dẫn schema riêng.
- Hai executable đặt cùng `/usr/lib/devterm/` để cơ chế tìm `devterm-connect` và SSH askpass vẫn hoạt động; `/usr/bin/devterm` chỉ gọi executable chính.
- Runtime Jammy có X11/Wayland, Cairo/OpenGL; không build các module video, in ấn hoặc Vulkan vì DevTerm không dùng chúng.
- Runtime riêng không được APT nâng cấp độc lập. Khi upstream/Ubuntu có sửa lỗi, người bảo trì phải cập nhật lock, build, kiểm thử và phát hành gói DevTerm mới. `runtime-sources.json` ghi snapshot và checksum từng archive; build không tự lấy phiên bản khác.

## Nguồn và thông tin phân phối

`dist/sources/` giữ nguyên `.dsc`, orig tarball và patch Debian/Ubuntu đã dùng. `dpkg-source` kiểm tra archive và áp dụng patch; hash trong lock được lấy từ source index Ubuntu đã được APT xác thực. Thông tin copyright của từng thành phần nằm trong `/usr/share/doc/devterm/runtime-copyright/` và source archive tương ứng.

Repository chưa khai báo license riêng cho DevTerm. Gói dùng cho việc triển khai của chủ dự án; trước khi phát hành công khai cần chủ dự án quyết định license và thông tin maintainer. Không đưa cấu hình cá nhân, password, keyring, Git metadata hoặc cache skill vào artifact.

## Kiểm tra bản cài

Ngoài 46 test hiện có trong builder, cần cài artifact vào container/VM sạch tương ứng để kiểm tra dependency, ELF, shell/PTY, helper, cài lại/nâng cấp và gỡ cài đặt. Kiểm tra GUI thật trên X11/Wayland, keyring desktop và thiết bị Serial thật là bước nghiệm thu trên máy đích. Xem [kết quả thực hiện và giới hạn](TESTING.md).

```sh
python3 packaging/verify-debs.py dist/devterm_*.deb
```

Lệnh này chỉ cài/gỡ trong container dùng một lần, không cài DevTerm vào máy phát triển. Nó dùng APT cài gói trên image Ubuntu sạch, chạy launcher đã cài với shell/PTY thật trên Xvfb, split hai hướng và mở tab. Một chương trình host được thực thi trong từng pane sẽ nạp GLib hệ thống và ghi lại môi trường để phát hiện runtime riêng bị truyền sang shell. Kiểm tra tiếp đường từ chối OS marker sai, nâng revision thật qua dpkg, gỡ/cài lại và bảo toàn JSON người dùng. Kết quả nằm trong `.install.json` cạnh gói tương ứng; timeout test ARM64 được nới cho QEMU, không thay đổi timeout đăng nhập của ứng dụng.
