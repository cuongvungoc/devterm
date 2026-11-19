# Kiểm tra phím tắt tùy chỉnh

Môi trường triển khai: GTK 4.14.5, VTE 0.76.0, JSON-GLib 1.8.0, Meson/Ninja; kiểm thử bàn phím qua X11 và xdotool, VTE/PTY thật. Không có dependency runtime mới.

```sh
meson compile -C build
GDK_BACKEND=x11 meson test -C build --print-errorlogs
# Hoặc display cô lập khi có xvfb:
xvfb-run -a -s "-screen 0 1600x1000x24" env GDK_BACKEND=x11 meson test -C build --print-errorlogs
```

`shortcuts` không cần display. `shortcuts-ui` cần backend X11 và `xdotool`; thiếu điều kiện trả mã 77 để Meson ghi nhận skip. Không chạy đồng thời công cụ GUI khác thay đổi focus trên cùng display trong lúc test. Các test dùng thư mục cấu hình tạm, không sửa cấu hình của người dùng.

Các nhánh kiểm thử tự động mới:

- Round-trip overrides, bỏ gán bằng null, mặc định khi thiếu file, XDG rỗng, ID duy nhất, đổi chỗ hai phím không phụ thuộc thứ tự JSON.
- JSON/schema/version/action/phím không hợp lệ; phím trùng; dữ liệu lỗi được giữ nguyên; lỗi ghi không làm thay đổi file nguồn.
- Phím thật tạo split cả hai hướng, chuyển pane/tab, đổi tên, bật Draft, đóng pane và chặn key auto-repeat.
- Đổi phím qua dialog thật: Apply, gỡ phím cũ trên pane cũ/mới, cửa sổ mới và application mới nạp lại file; tooltip cập nhật.
- Xung đột/Reassign, Remove, Reset riêng lẻ (kể cả khi mặc định đã bị action khác chiếm), Reset All, Cancel, tìm kiếm và đóng cửa sổ khi dialog còn mở.
- Copy All/Copy vùng chọn và clipboard; đổi pane rồi Copy All khi toolbar giữ focus chỉ lấy nội dung pane đích; Draft vẫn paste qua GTK.
- File hỏng: không được Apply cho đến khi chủ động khôi phục; backup giữ đúng nội dung; lỗi ghi qua dialog giữ nguyên bộ phím đang dùng; file bị đổi từ ngoài được bảo toàn.
- Phím Reconnect/Disconnect đi đúng pane sau khi toolbar lấy focus, giữ tab/split/output, không ngắt shell local. Cancel credential giữ kết nối; đóng pane khi đang chờ không crash. Nhánh này dùng client SSH giả lập qua helper/PTY thật; kiểm thử giao thức với client/server thật nằm trong nhóm `connections` hiện có.
- Foreground reader ở chế độ PTY raw xác nhận Copy/Search không lọt byte xuống child, phím Copy và Next Tab đã bỏ gán được trả về terminal, Paste tùy chỉnh gửi đúng Unicode.

Kết quả: `meson compile -C build` thành công; toàn bộ **16/16 nhóm test** qua trên display Xvfb riêng, không fail, skip hoặc timeout. Hai nhóm mới gồm 5 test cấu hình và 7 test tích hợp GUI. Lượt thử trên XWayland của desktop gặp frame không cập nhật (cả test bố cục có sẵn cũng lỗi); kết quả nghiệm thu được lấy từ lượt Xvfb cô lập, không thay đổi chế độ render của ứng dụng.

Checklist xác minh bổ sung trên máy người dùng:

1. Reconnect/Disconnect trên SSH/Telnet thật và Serial vật lý; hủy nhập mật khẩu, host-key review, ngắt mạng rồi kết nối lại.
2. Thử bộ phím mặc định và phím tự gán với vim/top/ssh; phím đã gán thuộc DevTerm, phím khác tiếp tục vào terminal.
3. Chọn phím theo layout bàn phím đang dùng, đặc biệt Shift+dấu câu, AltGr, đổi layout và desktop global shortcuts.
4. Tùy chỉnh, Apply, đóng ứng dụng rồi mở lại; thử Remove, Reset riêng từng action, Reset All và Cancel.
5. Thử trên Wayland, các mức phóng chữ/theme và trình đọc màn hình Orca.

Chưa xác minh tương tác trực tiếp trên Wayland, mọi layout/AltGr, Orca hoặc thiết bị SSH/Serial vật lý. Ảnh chụp GTK thực tế của dialog được lưu cục bộ tại `build/shortcuts-preview-dark.png` và `build/shortcuts-preview-light.png` (không thuộc source bắt buộc để build).
