Bạn là senior software engineer chuyên phát triển ứng dụng desktop Linux, C, GTK và PTY.

Hãy triển khai một ứng dụng terminal Linux tên tạm là DevTerm, hỗ trợ split pane và thanh nút lệnh tùy biến theo phong cách Quick Commands của Xshell.

Tôi cần ứng dụng chạy thực tế, không chỉ mockup, bản thiết kế hoặc hướng dẫn.

## 1. Khảo sát và lựa chọn công nghệ

Trước khi sửa code:

- Đọc AGENTS.md và khảo sát cấu trúc repository, công nghệ, build system và dependency hiện có.
- Nếu repository đã có ứng dụng terminal, tích hợp tính năng vào kiến trúc hiện tại.
- Nếu repository trống, dùng C + GTK4 + VTE tương ứng GTK4, Meson/Ninja và JSON-GLib.
- Kiểm tra phiên bản dependency và API thực tế trong môi trường. Không trộn GTK3 với GTK4 hoặc dùng API dựa trên suy đoán.
- Giữ thay đổi trong phạm vi yêu cầu, không ghi đè công việc không liên quan.

Đây là ứng dụng desktop native trên Linux.

## 2. Terminal tương tác thực sự

Mỗi terminal pane phải:

- Sử dụng terminal widget và PTY thực sự.
- Có tiến trình shell riêng; ưu tiên shell của người dùng, fallback `/bin/sh`.
- Hỗ trợ nhập lệnh, hiển thị output, ANSI colors, scrollback, copy/paste và resize.
- Chạy được các chương trình tương tác như `vim`, `top`, `ssh`.
- Không block UI khi spawn shell, chạy command hoặc nhận nhiều output.
- Xử lý lỗi spawn và shell exit rõ ràng.

Không mô phỏng terminal bằng text view kết hợp đọc stdout của subprocess.

## 3. Split pane

Cho phép:

- Split trái/phải.
- Split trên/dưới.
- Tiếp tục split một pane bất kỳ để tạo bố cục nhiều pane.
- Kéo divider để thay đổi kích thước.
- Click để chọn pane, có dấu hiệu trực quan cho pane đang được chọn.
- Đóng pane và thu gọn cây bố cục hợp lý.

Có thể dùng các GtkPaned lồng nhau. Tách trạng thái bố cục và vòng đời terminal đủ rõ để dễ bảo trì.

Xử lý resize terminal/PTY, focus, shell exit và đóng pane đúng cách. Nếu pane cuối cùng bị đóng, hiển thị trạng thái cho phép tạo terminal mới.

## 4. Thanh nút lệnh tùy biến — tính năng chính

“Check internet” với command `ping 8.8.8.8` chỉ là ví dụ.

Không hardcode chức năng riêng cho từng nút. Người dùng phải tự tạo các nút phục vụ mục đích bất kỳ.

Cung cấp giao diện:

- Thêm nút.
- Sửa nút.
- Xóa nút.
- Đổi thứ tự bằng thao tác đơn giản, chẳng hạn Move Left/Move Right.
- Lưu cấu hình và tự khôi phục khi mở lại ứng dụng.

Mỗi nút có:

- ID ổn định.
- Tên hiển thị, bắt buộc.
- Command, bắt buộc.
- Tùy chọn “Send Enter”: gửi Enter ngay sau command hoặc chỉ điền command.

Command hỗ trợ Unicode, dấu nháy, đường dẫn và shell operators như `|`, `&&`, `>` bằng cách gửi nguyên nội dung vào terminal session.

Hiển thị command trong tooltip để người dùng xem trước.

Nếu chưa có nút nào, hiển thị hướng dẫn ngắn và nút “Add Command”.

## 5. Ngữ nghĩa khi bấm nút

Nút phải gửi command vào terminal pane được chọn gần nhất.

Ví dụ:

- Pane đang ở shell local: command chạy trong shell local đó.
- Pane đang SSH vào router và ở remote shell prompt: command chạy trên router.
- Command `cd` phải thay đổi thư mục của chính shell trong pane.

Không dùng `system()`, `popen()` hoặc một shell phụ độc lập để thay thế cơ chế này.

Dùng API gửi input vào child của terminal widget, ví dụ `vte_terminal_feed_child()` khi phù hợp với phiên bản VTE đang dùng.

Nếu `send_enter = true`, gửi thêm carriage return `\r`; nếu false, chỉ gửi command.

Các chi tiết bắt buộc:

- Ghi nhớ pane được chọn dù toolbar hoặc dialog lấy keyboard focus.
- Sau khi gửi command, trả focus về đúng pane.
- Nếu pane đã đóng hoặc shell đã exit, xử lý an toàn và thông báo rõ.
- Không tự gửi Ctrl+C, xóa dòng đang gõ hoặc ngắt chương trình foreground.
- Không giả định input luôn đến shell: nếu đang mở `vim`, `top` hoặc chương trình khác, input có thể đến chương trình đó. Giải thích ngắn hành vi này trong giao diện cấu hình hoặc README.
- Không tự thêm cơ chế broadcast đến mọi pane.

## 6. Lưu cấu hình

Theo chuẩn XDG:

- Dùng `$XDG_CONFIG_HOME/devterm/commands.json`.
- Nếu XDG_CONFIG_HOME không được thiết lập hoặc rỗng, fallback `~/.config/devterm/commands.json`.
- Có trường version cho định dạng cấu hình.
- Giữ nguyên thứ tự các nút.

Ví dụ cấu trúc:

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

Ghi file theo cách atomic để tránh file bị ghi dở. Khi cấu hình sai JSON hoặc không hợp lệ, ứng dụng không được crash hoặc âm thầm ghi đè file lỗi. Báo lỗi dễ hiểu và vẫn cho phép sử dụng terminal.

Không tự chạy command khi khởi động ứng dụng hoặc khi đọc cấu hình.

## 7. Kiến trúc và chất lượng

Tách các phần hợp lý:

- Khởi tạo ứng dụng và cửa sổ.
- Terminal pane và vòng đời child process.
- Quản lý split layout và active pane.
- Model, cấu hình và thực thi Quick Commands.
- Dialog thêm/sửa command.

Ưu tiên code rõ ràng, ít abstraction không cần thiết.

Chú ý ownership và reference của GObject, lifetime của signal callback, lỗi spawn bất đồng bộ và tình huống pane bị đóng khi callback chưa hoàn tất.

Không thêm các tính năng ngoài MVP như SSH profile manager, serial terminal, plugin system, cloud sync hoặc session persistence.

## 8. Kiểm thử và tiêu chí nghiệm thu

Build dự án và chạy các kiểm tra có thể thực hiện trong môi trường.

Viết test phù hợp cho phần cấu hình: round-trip Unicode, bảo toàn thứ tự và xử lý dữ liệu lỗi. Tránh test chỉ lặp lại implementation.

Kiểm tra thực tế hoặc cung cấp checklist kiểm tra thủ công cho:

1. Mở ứng dụng và dùng shell tương tác.
2. Split nhiều lần theo cả hai hướng, resize và đóng pane.
3. Tạo, sửa, xóa, đổi thứ tự các nút.
4. Khởi động lại và xác nhận cấu hình được giữ nguyên.
5. Bấm nút sau khi đổi active pane: command chỉ đi vào đúng pane.
6. `send_enter = false` không tự thực thi command.
7. Command có dấu nháy, pipe và Unicode được gửi nguyên vẹn.
8. Command `cd` tác động đến shell hiện tại.
9. Command được gửi vào remote shell khi pane đang SSH.
10. Shell exit, cấu hình lỗi hoặc lỗi ghi file không làm ứng dụng crash.
11. Chạy lệnh lâu như `ping` không làm treo UI.

Nếu thiếu GUI, dependency hoặc SSH target, nêu rõ phần nào chưa xác minh. Không tuyên bố đã kiểm tra hành vi chưa thực sự chạy.

## 9. Kết quả bàn giao

Hoàn thành source code và README gồm:

- Dependency và lệnh cài đặt phù hợp với bản Linux được hỗ trợ.
- Lệnh build và chạy.
- Cách split pane và quản lý Quick Commands.
- Đường dẫn và định dạng cấu hình.
- Hành vi gửi input vào terminal session.
- Kết quả kiểm thử và giới hạn còn lại.

Hãy bắt đầu bằng việc khảo sát repository, trình bày ngắn hướng triển khai, sau đó thực hiện đến khi có phiên bản MVP chạy được. Tự quyết định các chi tiết UI nhỏ trong phạm vi yêu cầu.