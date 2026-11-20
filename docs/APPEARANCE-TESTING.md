# Kiểm thử lưu giao diện sáng/tối — 20/11/2026

Menu **☰ → Dark appearance** lưu lựa chọn vào `preferences.json` version 1, cùng tùy chọn xác nhận đóng session:

```json
{"version": 1, "confirm_close_sessions": true, "appearance": "light"}
```

Đường dẫn: `$XDG_CONFIG_HOME/devterm/preferences.json`, fallback `~/.config/devterm/preferences.json` nếu biến môi trường thiếu/rỗng. `appearance` nhận `light`/`dark`; file cũ không có trường này dùng Dark và không bị ghi lại chỉ vì khởi động ứng dụng. Save giữ cả hai tùy chọn, dùng ghi atomic/durable hiện có. File không hợp lệ được giữ nguyên; lỗi lưu hiện thông báo và giữ theme/checkbox đang dùng. Terminal vẫn dùng palette tối, chưa có chế độ tự theo hệ thống.

Kiểm tra tự động:

```sh
meson compile -C build
GDK_BACKEND=x11 GTK_IM_MODULE=simple meson test -C build preferences appearance --print-errorlogs
```

- `preferences`: 5 ca, gồm round-trip Light/Dark, thay từng tùy chọn mà không mất tùy chọn còn lại, tương thích file cũ, quyền file 0600, JSON/appearance sai được giữ nguyên, lỗi ghi và đường dẫn XDG/Unicode.
- `appearance`: 3 ca tích hợp qua code ứng dụng thật. Gọi menu action và kiểm tra CSS/GtkSettings/checkbox của hai cửa sổ đang mở và cửa sổ thứ ba mở sau khi đổi theme. Tiến trình con mới kiểm tra lựa chọn tại `map` và `after-paint` đầu cho Light/Dark, file chưa có, file cũ và file lỗi. Làm hỏng cấu hình trong lúc chạy rồi đổi theme phải hiện alert và giữ nguyên theme ở cả hai cửa sổ.

Kiểm tra thủ công trên desktop:

1. Bỏ chọn **Dark appearance**, đóng và mở lại DevTerm: giao diện sáng từ đầu, checkbox không bật lại.
2. Chọn lại Dark, đóng/mở lại: giao diện tối được giữ.
3. Đổi tùy chọn xác nhận đóng session rồi chạy lại: cả hai lựa chọn được giữ đúng.
4. Với nhiều cửa sổ cùng tiến trình, đổi theme từ một cửa sổ: mọi cửa sổ hiện tại và cửa sổ mới dùng cùng theme.

Môi trường kiểm tra tự động: GTK 4.14.5, VTE GTK4 0.76.0, JSON-GLib 1.8.0, Xvfb/Openbox 1920×1080, cấu hình tạm. Chưa xác minh trực tiếp trên GNOME/Wayland hoặc fractional scaling.

Kết quả: build thành công; bộ hồi quy qua **118 ca trong 20/20 nhóm**, không fail, skip hoặc timeout, bao gồm đủ 15 ca kết nối loopback. Log: `build/meson-logs/appearance-full.txt` và `appearance-full.json`. `git diff --check` sạch.
