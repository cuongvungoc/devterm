# Kiểm thử UI ưu tiên cao — 20/11/2026

Phạm vi gồm ba điểm từ lần rà soát UI/UX: Keyboard Shortcuts đổi bố cục sau khi mở, quy tắc xác nhận đóng không nhất quán và tên pane bị thông báo trạng thái thay thế. Không thêm Search/CRUD mới, thay đổi Sequence thành workflow hoặc đổi cơ chế gửi input.

Môi trường: GTK 4.14.5, VTE GTK4 0.76.0, JSON-GLib 1.8.0, Meson/Ninja. Dùng Xvfb và cấu hình tạm; không sửa cấu hình/keyring/session của người dùng.

```sh
meson compile -C build
GDK_BACKEND=x11 GTK_IM_MODULE=simple meson test -C build --print-errorlogs
# Kiểm tra bố cục với font lớn và window manager trên display riêng:
DEVTERM_DIALOG_LARGE_FONT=1 DEVTERM_DIALOG_LIGHT=1 DEVTERM_DIALOG_TEST_WM=1 \
  GDK_BACKEND=x11 GTK_IM_MODULE=simple \
  meson test -C build dialog-layout --timeout-multiplier 2 --print-errorlogs
```

Các kiểm tra hồi quy:

- `dialog-layout`: thêm `shortcut-opening-frames`, mở lại cửa sổ sau resize ở 900/880/920/760px, theo dõi bounds từng frame bằng `GdkFrameClock::after-paint`; kiểm tra cả hai phía của khoảng ngưỡng 400/440px vùng nhãn. Các ca Sessions, editor, mật độ hàng, focus/cuộn và maximize/restore được giữ. Size memory lưu native bounds, loại maximize/fullscreen và ghi nhận lần cuối khi unmap nếu người dùng đóng trước khi timer 150ms hoàn tất.
- `tabs`: năm ca tích hợp mới cho Cancel/default button, không tạo chồng xác nhận, đóng đúng pane đã yêu cầu sau khi đổi lựa chọn, pane đích biến mất trong lúc chờ, bảo vệ Draft nếu sibling đóng khiến pane đích thành pane cuối, đóng cửa sổ có session sống và đóng nhanh khi đã kết thúc. Kiểm tra preference đồng bộ giữa cửa sổ, tải lại từ file, đóng khi tắt xác nhận và giữ cảnh báo Draft ẩn. Sequence gửi qua PTY thật: title giữ nguyên, footer ghi nguồn pane, không đổi pane/focus nhận input.
- `shortcuts-ui`: phím Close Pane mở xác nhận, giữ phím không tạo thêm dialog, Esc hủy và nút Close Pane xác nhận. Các kiểm tra raw PTY vẫn kiểm tra không lọt phím giao diện vào child.
- `reconnect`: giữ title Unicode và endpoint dễ đọc qua ba lần reconnect; client running không được hiểu là remote đã xác thực. Xác thực và prompt vẫn nằm trong output terminal.
- `preferences`: bốn ca cho mặc định bật, round-trip true/false, file mới 0600, đường dẫn Unicode/XDG, JSON/schema/version lỗi được giữ nguyên, thay đổi file ngoài ứng dụng và lỗi ghi.

Tùy chọn **Confirm Before Closing Sessions** nằm trong menu ứng dụng, lưu tại `$XDG_CONFIG_HOME/devterm/preferences.json` (fallback `~/.config/devterm/preferences.json`):

```json
{"version": 1, "confirm_close_sessions": true}
```

Khi file không hợp lệ, mặc định bật xác nhận và giữ nguyên file. Ghi lỗi không thay model hoặc trạng thái checkbox. Tắt tùy chọn chỉ bỏ cảnh báo session; Draft luôn được xét riêng. Không thay đổi hành vi ngắt client của Disconnect hoặc Reconnect.

Build `meson compile -C build` thành công. Lượt cuối qua **114 ca trong 19/19 nhóm**, không fail, skip hoặc timeout; gồm đủ 15 ca kết nối loopback. Kết quả hồi quy được ghi tại `build/meson-logs/high-full-final.txt` và `high-full-final.json`. Lượt font lớn/window manager tại `high-small-wm-fixed.txt`. Lượt riêng trên Xvfb/Openbox 1366×768, Sans 13 và theme sáng qua cả 4 ca dialog và 10 ca reconnect. Đã xem ảnh render thực tế của Shortcuts và header pane; ảnh dialog có tiền tố `build/high-priority-large-*` và `build/high-priority-small-light-*`.

Kiểm tra bổ sung bằng binary `build/devterm` trên Openbox/Xvfb với dữ liệu tạm: bấm nút Sequence bằng chuột ở Pane 2, chuyển sang Pane 1 trong lúc delay, xác nhận hai marker được ghi từ chuỗi và title/nguồn footer giữ đúng Pane 2. Phím Close Pane mở xác nhận; Esc hủy, bấm chuột Close Pane loại đúng Pane 1 và giữ Pane 2 cùng output. Alt+F4 mở xác nhận đóng cửa sổ còn session, Esc giữ ứng dụng. Ảnh kiểm tra trong `build/high-native-sequence-other-pane.png`, `high-native-sequence-complete.png`, `high-native-after-close-pane.png`. Các thao tác thử nghiệm chỉ dùng local shell và file trong thư mục tạm.

Kiểm tra thủ công:

1. Thu hẹp Shortcuts gần 900px, đóng/mở nhiều lần; hàng không đổi một dòng/hai dòng sau khi đã hiện. Kéo rộng/hẹp thực sự vẫn đổi bố cục.
2. Đóng pane/tab/cửa sổ còn session bằng menu, nút đóng và phím tắt. Cancel/Esc giữ nguyên session; xác nhận đóng đúng đối tượng. Pane đã kết thúc không hỏi nếu không mất Draft.
3. Tắt tùy chọn, mở cửa sổ mới hoặc chạy lại ứng dụng: lựa chọn được giữ. Draft ẩn vẫn cần xác nhận khi đóng tab/cửa sổ.
4. Chạy Sequence có delay rồi chọn pane khác; tên hai pane giữ nguyên, tiến độ nằm riêng, footer chỉ rõ nguồn. Kiểm tra tooltip tên/endpoint và lỗi dài khi pane hẹp.

Giới hạn: chưa xác minh trực tiếp trên GNOME/Wayland, fractional scale hoặc Orca. Client liveness là trạng thái tiến trình PTY, không chứng minh remote đã sẵn sàng nhận lệnh hoặc command đã hoàn thành. ID pane giữ trong tiến trình, không phải session/layout persistence.
