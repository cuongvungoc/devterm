# Kiểm thử Quick Command Sequences

Môi trường: GTK 4.14.5, VTE GTK4 0.76.0, JSON-GLib 1.8.0, Meson/Ninja; GUI trên X11/Xvfb 1600×1000. Không thêm dependency runtime.

```sh
meson compile -C build
GDK_BACKEND=x11 GTK_IM_MODULE=simple meson test -C build --print-errorlogs
```

Cần display cho GTK/VTE, `xdotool` cho các ca bàn phím và host-key dialog, OpenSSH/Telnet/picocom cho client thật và Python `asyncssh` cho SSH server loopback. Xem README để chuẩn bị môi trường Python thử nghiệm. Các test dùng thư mục cấu hình tạm; không sửa Quick Commands của người dùng. Các bài test GUI cần chạy tuần tự trên cùng display.

Kết quả ngày 19/11/2026: build thành công; lượt hồi quy **17/17 nhóm Meson** thành công, không fail/timeout. Log chi tiết của lượt đầu có 9 ca SSH skip vì thiếu Python `asyncssh`. Sau khi bổ sung thư viện vào thư mục thử nghiệm tạm và khởi động lại display Xvfb đã hết phiên, đã chạy lại riêng `connections`: **15/15 ca qua, không skip**. Nhóm `commands` cũng qua lại sau khi thêm ca lỗi backup/missing-kind. Tổng cộng **101 ca test đã được chạy qua**, kết hợp các lượt này, không tính lặp.

Log cục bộ: `build/meson-logs/testlog.txt` (toàn bộ nhóm), `sequence-connections.txt` (15 ca client/server), `sequence-config.txt` (cấu hình và backup). Không cài dependency Python vào môi trường runtime của ứng dụng.

Các nhánh mới:

| Nhóm | Xác minh |
| --- | --- |
| `commands` | Trộn Command/Sequence, Unicode/dấu nháy/operators/khoảng trắng và thứ tự; last-delay chuẩn hóa 0; copy sâu; v1 đọc không ghi file, giữ ID và Send Enter; backup đúng byte; lỗi backup giữ file v1; JSON/schema/delay/kind sai bị từ chối và không ghi đè |
| `quick-ui` | Tạo Sequence từ dialog thật; chặn hàng trống; thêm/lên/xuống/xóa; command và delay đi cùng nhau; hàng cuối thành —; Save/reload; Cancel giữ định nghĩa; đổi Sequence thành Command giữ ID; thao tác editor không thực thi lệnh |
| `sequence` | Shell/PTY thật nhận Unicode, quote, pipe/operator text đúng thứ tự; delay 250/350 ms có đo thời gian; không chờ delay cuối; timer UI vẫn chạy; xóa/sửa định nghĩa không đổi lượt đang gửi; không lấy focus ở lượt sau; báo bận cùng pane, pane khác độc lập; `cd` đổi chính shell; delay 0; exit/close/shutdown bỏ hàng chưa gửi |
| `reconnect` | Qua helper/VTE/PTY thật với client fixture: hủy chuỗi ngay khi chuẩn bị reconnect, Cancel password không tiếp tục, Disconnect/Reconnect không chuyển hàng còn lại sang child mới |
| `tabs` | Qua callback Quick Commands thật của cửa sổ: chuyển tab và focus Draft trong delay không đổi pane đích hoặc lấy focus; đóng app khi còn lượt chờ an toàn |

Đã xem dialog GTK thực tế với ba hàng, delay khác nhau và hàng cuối — ở cả theme tối/sáng. Ảnh cục bộ tại `build/sequence-preview-dark.png` và `build/sequence-preview-light.png`, không cần để build hoặc chạy ứng dụng.

Giới hạn: delay không xác nhận lệnh đã xong hoặc thành công; foreground program nhận input như khi người dùng gõ. Bàn phím/paste thủ công có thể xen vào chuỗi. Lệnh đã gửi không bị ngắt hay hoàn tác khi hủy. Kiểm thử reconnect dùng client xác định; các bài giao thức dùng server loopback/Serial PTY. Chưa xác minh trên Wayland, Orca, router/server production hoặc Serial vật lý.

Checklist thủ công:

1. Add Command → Type Sequence; nhập các hàng và delay; lên/xuống/xóa, thử xóa hết rồi thêm lại; Save/Cancel; mở lại ứng dụng và xem tooltip.
2. Chạy chuỗi trong shell local và SSH/Telnet/Serial thử nghiệm; kiểm tra `cd`, Unicode, operators, delay 0 và delay dài. Lệnh báo lỗi không làm chuỗi tự dừng; lệnh đang chạy có thể nhận input của hàng sau.
3. Chuyển pane/tab/Draft khi đang chờ: đích giữ nguyên, focus không bị kéo lại. Bấm lại báo bận, không tự chạy thêm lượt; pane khác vẫn nhận Quick Commands.
4. Trong delay, thử exit, đóng pane/tab/app, Disconnect và Reconnect, kể cả Cancel password. Hàng chưa gửi phải bị bỏ; lượt chạy mới chỉ bắt đầu khi chủ động bấm nút.
5. Dùng cấu hình v1, mở ứng dụng không đổi file; sửa/lưu tạo v2 và backup v1 đúng byte. Thử thư mục không ghi được và JSON lỗi, xác nhận không mất cấu hình/model đang dùng.
