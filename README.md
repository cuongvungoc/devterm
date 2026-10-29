# DevTerm

DevTerm là terminal desktop native cho Linux, viết bằng C, GTK4 và VTE dành cho GTK4. Mỗi tab có bố cục split riêng; mỗi pane có PTY và shell riêng. Quick Commands gửi nguyên chuỗi command vào pane đang chọn trong tab hiện tại, kể cả khi pane đó đang ở phiên SSH.

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

- **New Terminal** hoặc **Ctrl+Shift+T** tạo và chuyển sang tab mới với một shell local. Tên tab lần lượt là `Terminal 1`, `Terminal 2`… Thanh tab cuộn được khi có nhiều tab.
- Tab mới kế thừa thư mục local của pane đang chọn nếu xác định được, fallback về home. DevTerm đọc thư mục của tiến trình shell local qua `/proc`; đường dẫn remote do SSH báo về không được dùng để khởi động shell local. Tab mới không kế thừa kết nối SSH hoặc biến môi trường được export trong shell của tab cũ.
- **Split Right** chia pane đang chọn thành trái/phải; **Split Down** chia trên/dưới, trong tab hiện tại. Pane mới cũng ưu tiên thư mục local của pane đang chọn. Có thể chia tiếp bất kỳ pane nào, kéo đường phân cách để đổi kích thước. Click vào terminal hoặc tiêu đề pane để chọn; viền xanh đánh dấu pane hiện tại.
- **Ctrl+PageUp** / **Ctrl+PageDown** chuyển tab trước/sau, vòng lại ở hai đầu. Mỗi tab ghi nhớ pane được chọn gần nhất và trả focus về pane đó khi quay lại; các shell ở tab khác tiếp tục chạy.
- **Close Pane** đóng pane đang chọn và thu gọn bố cục. Nếu đó là pane cuối, thao tác sẽ đóng tab. Nút **×** trên tab hoặc **Ctrl+Shift+W** đóng toàn bộ tab. Khi tab còn shell đang chạy hoặc khởi động, ứng dụng hỏi xác nhận; chọn **Cancel** để giữ tab, **Close Tab** để kết thúc tất cả pane trong tab đó. Đóng pane riêng trong tab có nhiều pane vẫn thực hiện ngay.
- Đóng tab cuối giữ cửa sổ ở trạng thái trống với nút **New Terminal**. Shell tự thoát sẽ giữ pane cùng output để xem lại; đóng tab không còn shell sống sẽ không hỏi xác nhận. Tab và bố cục không được khôi phục sau khi thoát ứng dụng.
- **Ctrl+Shift+C** sao chép vùng chọn, **Ctrl+Shift+V** dán từ clipboard. VTE xử lý màu ANSI, scrollback và kích thước PTY.
- **Add Command** tạo nút đầu tiên. **Manage…** cho phép thêm, sửa, xóa và đổi thứ tự bằng nút mũi tên. Tooltip của mỗi nút hiển thị command.
- Quick Commands dùng chung cho tất cả tab. Bấm nút sẽ gửi nội dung vào pane được chọn gần nhất trong tab đang hiển thị, rồi trả focus về pane đó; tab bị ẩn không nhận input. **Send Enter** gửi thêm `\r`; khi tắt, command chỉ được điền và chờ người dùng nhấn Enter. Input đi vào chương trình foreground hiện tại: nếu pane đang SSH, lệnh đến phiên remote; nếu đang mở `vim` hoặc `top`, input có thể được chương trình đó xử lý. DevTerm không xóa dòng đang gõ và không ngắt chương trình đang chạy.

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

`meson test` có hai nhóm kiểm tra:

- `commands`: 4 test cho round-trip Unicode, dấu nháy, shell operators, thứ tự, `send_enter`, đường dẫn XDG, dữ liệu lỗi và lỗi ghi file vẫn giữ nguyên nội dung cũ.
- `layout`: 2 test tích hợp với shell/PTY thật cho bố cục độc lập, kế thừa thư mục có dấu cách, bỏ qua thông tin thư mục remote, fallback sau shell exit và giải phóng pane khi spawn chưa hoàn tất. Nhóm này tự bỏ qua nếu không có display; cần X11 hoặc Wayland để chạy đầy đủ.

Build và cả hai nhóm test đã qua. Dependency được giải nén tạm từ gói Ubuntu 24.04 vào `/tmp` vì máy kiểm thử yêu cầu mật khẩu sudo. Kiểm tra giao diện trên X11/Xvfb đã xác nhận: nút New Terminal và phím tắt tạo tab riêng; tab mới kế thừa thư mục; split nhiều cấp; chuyển tab khôi phục đúng pane; Quick Commands chỉ gửi vào pane đang chọn trong tab hiện tại; Cancel giữ nguyên shell; Close Tab kết thúc các shell của tab đó; đóng pane cuối đưa về trạng thái trống; tạo lại tab ở home; đóng tab đã exit không hỏi xác nhận; đóng cửa sổ bằng yêu cầu chuẩn `WM_DELETE_WINDOW` thoát bình thường. Các kiểm tra cấu hình và Quick Commands của phiên bản trước cũng đã xác nhận thêm/sửa/xóa/đổi thứ tự, `send_enter=false`, shell exit và giữ nguyên file JSON lỗi.

Checklist kiểm tra thủ công trên hệ thống cài dependency bình thường:

1. Mở ứng dụng, nhập lệnh shell, chạy `vim`, `top` và xác nhận màu ANSI, scrollback, copy/paste.
2. Split cả hai hướng nhiều lần, kéo divider, chọn và đóng các pane theo nhiều thứ tự.
3. Thêm, sửa, xóa, đổi vị trí nút; mở lại ứng dụng để xác nhận thứ tự, ID và nội dung được giữ.
4. Chọn lần lượt hai pane rồi bấm cùng nút; chỉ pane được chọn nhận input.
5. Kiểm tra `send_enter=false`, dấu nháy, pipe, Unicode và `cd` trong shell hiện tại.
6. SSH vào một máy thử và bấm nút ở remote prompt; xác nhận lệnh chạy trên remote.
7. Thử `exit`, file cấu hình lỗi, thư mục cấu hình không ghi được và lệnh lâu như `ping`; giao diện vẫn phản hồi.
8. Tạo nhiều tab bằng nút và phím tắt, split khác nhau ở từng tab, đổi tab bằng click và phím tắt; xác nhận bố cục, thư mục, output và active pane được giữ riêng.
9. Đóng tab bằng nút × và phím tắt, thử cả Cancel và Close Tab; đóng pane cuối và tab cuối, rồi tạo lại. Kiểm tra các shell ở tab khác vẫn chạy.

Chưa kiểm tra SSH vì không có target; chưa chạy `vim`, `top`, `ping` trong phiên kiểm thử tự động. Tab đã được kiểm tra trên X11; cần xác nhận thêm tương tác trên Wayland. Lỗi GDK từng ghi nhận khi kiểm thử đóng cửa sổ xuất phát từ việc hủy trực tiếp X surface bằng `xdotool windowclose`; kiểm tra lại bằng yêu cầu đóng chuẩn của window manager đã qua.
