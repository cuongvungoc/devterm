# DevTerm

DevTerm là terminal desktop native cho Linux, viết bằng C, GTK4 và VTE dành cho GTK4. Mỗi pane có PTY và shell riêng. Quick Commands gửi nguyên chuỗi command vào pane đang chọn, kể cả khi pane đó đang ở phiên SSH.

## Cài đặt và chạy

Trên Ubuntu 24.04:

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config libgtk-4-dev libvte-2.91-gtk4-dev libjson-glib-dev
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/devterm
```

Phiên bản đã kiểm tra: GTK 4.14.5, VTE GTK4 0.76.0, JSON-GLib 1.8.0, Meson 1.3.2. Cần phiên đồ họa X11 hoặc Wayland để mở cửa sổ.

## Sử dụng

- **Split Right** chia pane đang chọn thành trái/phải; **Split Down** chia trên/dưới. Có thể chia tiếp bất kỳ pane nào, kéo đường phân cách để đổi kích thước. Click vào terminal hoặc tiêu đề pane để chọn; viền xanh đánh dấu pane hiện tại.
- **Close Pane** đóng pane đang chọn và thu gọn bố cục. Sau khi đóng pane cuối, dùng **New Terminal** để tạo lại.
- **Ctrl+Shift+C** sao chép vùng chọn, **Ctrl+Shift+V** dán từ clipboard. VTE xử lý màu ANSI, scrollback và kích thước PTY.
- **Add Command** tạo nút đầu tiên. **Manage…** cho phép thêm, sửa, xóa và đổi thứ tự bằng nút mũi tên. Tooltip của mỗi nút hiển thị command.
- Bấm Quick Command sẽ gửi nội dung vào pane được chọn gần nhất, rồi trả focus về pane đó. **Send Enter** gửi thêm `\r`; khi tắt, command chỉ được điền và chờ người dùng nhấn Enter. Input đi vào chương trình foreground hiện tại: nếu pane đang SSH, lệnh đến phiên remote; nếu đang mở `vim` hoặc `top`, input có thể được chương trình đó xử lý. DevTerm không xóa dòng đang gõ và không ngắt chương trình đang chạy.

Nếu shell đã thoát hoặc chưa khởi động được, DevTerm báo lỗi trên pane hoặc thanh trạng thái và không gửi Quick Command vào pane đó.

## Cấu hình

File nằm tại `$XDG_CONFIG_HOME/devterm/commands.json`. Khi `XDG_CONFIG_HOME` không có hoặc rỗng, đường dẫn là `~/.config/devterm/commands.json`. File được ghi theo cách thay thế atomic và giữ thứ tự nút. Mỗi ID được tạo một lần và giữ nguyên khi sửa hoặc đổi vị trí.

```json
{
  "version": 1,
  "commands": [
    {
      "id": "unique-stable-id",
      "name": "Check internet",
      "command": "ping 8.8.8.8",
      "send_enter": true
    }
  ]
}
```

DevTerm không tự chạy command khi đọc file. Nếu JSON lỗi, phiên bản không hỗ trợ, thiếu trường bắt buộc hoặc ID trùng, terminal vẫn mở; thông báo lỗi xuất hiện và việc sửa Quick Commands bị khóa để file lỗi không bị ghi đè. Hãy sửa hoặc di chuyển file lỗi rồi mở lại ứng dụng. Nếu ghi file thất bại, thanh trạng thái sẽ báo lỗi.

## Kiểm thử

`meson test` kiểm tra round-trip Unicode, dấu nháy, shell operators, thứ tự, `send_enter`, đường dẫn XDG, dữ liệu lỗi và lỗi ghi file vẫn giữ nguyên nội dung cũ. Trên máy kiểm thử, dependency chưa được cài vào hệ thống do `sudo` yêu cầu mật khẩu; các gói Ubuntu 24.04 được giải nén tạm vào `/tmp` để build và chạy. Build và test cấu hình đã qua. Qua X11 và VTE thực, đã xác nhận shell nhận lệnh qua PTY; split phải rồi split xuống tạo ba shell; đóng hết pane và tạo lại; giao diện thêm/sửa/xóa/đổi thứ tự lưu đúng JSON; Quick Command chạy trong pane được chọn, `cd /tmp` giữ trạng thái riêng của pane, đổi pane chỉ đổi đích nhận lệnh; `send_enter=false` chờ Enter; shell exit được báo rõ và không crash khi bấm command. Với cấu hình JSON lỗi, shell vẫn chạy, nút quản lý bị khóa và file lỗi giữ nguyên.

Checklist kiểm tra thủ công trên hệ thống cài dependency bình thường:

1. Mở ứng dụng, nhập lệnh shell, chạy `vim`, `top` và xác nhận màu ANSI, scrollback, copy/paste.
2. Split cả hai hướng nhiều lần, kéo divider, chọn và đóng các pane theo nhiều thứ tự.
3. Thêm, sửa, xóa, đổi vị trí nút; mở lại ứng dụng để xác nhận thứ tự, ID và nội dung được giữ.
4. Chọn lần lượt hai pane rồi bấm cùng nút; chỉ pane được chọn nhận input.
5. Kiểm tra `send_enter=false`, dấu nháy, pipe, Unicode và `cd` trong shell hiện tại.
6. SSH vào một máy thử và bấm nút ở remote prompt; xác nhận lệnh chạy trên remote.
7. Thử `exit`, file cấu hình lỗi, thư mục cấu hình không ghi được và lệnh lâu như `ping`; giao diện vẫn phản hồi.

Chưa kiểm tra SSH vì không có target; chưa chạy `vim`, `top`, `ping` trong phiên kiểm thử tự động. Khi đóng cửa sổ qua window manager trong môi trường thư viện giải nén tạm, GDK/EGL báo assertion; ứng dụng GTK tối giản chạy cùng bộ thư viện cũng gặp lỗi tương tự. Cần xác nhận thao tác thoát ứng dụng trên bộ thư viện được cài bình thường.
