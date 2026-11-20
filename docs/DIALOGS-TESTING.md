# Kiểm thử dialog danh sách — 20/11/2026

GTK 4.14.5, VTE GTK4 0.76.0, JSON-GLib 1.8.0; Meson/Ninja. Không thêm dependency runtime. Dữ liệu kiểm tra có 30 session, 31 nút Quick Commands và Sequence 30 bước; dùng thư mục cấu hình tạm, không sửa dữ liệu người dùng.

```sh
meson compile -C build
GDK_BACKEND=x11 GTK_IM_MODULE=simple meson test -C build --print-errorlogs
# Chạy riêng kiểm tra bố cục trên display đã chuẩn bị:
GDK_BACKEND=x11 meson test -C build dialog-layout --print-errorlogs
# Font lớn hơn và theme sáng:
DEVTERM_DIALOG_LARGE_FONT=1 DEVTERM_DIALOG_LIGHT=1 GDK_BACKEND=x11 \
  meson test -C build dialog-layout --print-errorlogs
```

`dialog-layout` đo hình học trên GTK thật: số hàng nằm trọn trong vùng cuộn, danh sách chiếm ít nhất 55% chiều cao dialog, cửa sổ không vượt màn hình, nút cuối vẫn ở trong cửa sổ. Kiểm tra resize Sessions về bố cục hai dòng mà giữ lựa chọn; đóng/mở khôi phục đúng kích thước, không hụt dần vì viền CSD. Sequence thêm hàng cuối và di chuyển bước phải giữ focus, reveal đúng hàng; Cancel giữ nguyên 30 bước đã lưu. Các popover thông tin mở/đóng được và không thực thi lệnh.

Ba ca hồi quy về frame (`opening-frames`, `editor-frames`, `shortcut-opening-frames`) theo dõi `GdkFrameClock::after-paint` ngay từ frame đầu, so sánh kích thước cửa sổ và bounds của các hàng/control qua các frame:

- Mở Sessions 4 lần với kích thước rộng/hẹp đã nhớ; không có frame đầu hai dòng rồi thu về một dòng. Kéo qua hai ngưỡng của vùng nhãn 660/700px và khoảng giữa từ cả hai phía; row và lựa chọn giữ nguyên.
- Mở Add, Edit Command và Edit Sequence; mỗi lượt đổi Type 8 lần, giữ kích thước cửa sổ và vị trí Type/Name/Save. Nội dung Unicode ở cả hai trang còn nguyên. Thu nhỏ trong Command rồi đổi sang Sequence không ép cửa sổ lớn lên; mở lại dùng chung kích thước đã chọn, không hụt dần vì CSD; Cancel giữ cấu hình gốc.
- Mở lại Keyboard Shortcuts sau resize về 900/880/920/760px; tên và metadata giữ nguyên bố cục qua các frame đầu. Kéo vùng nhãn qua hai ngưỡng 400/440px giữ bố cục trong khoảng giữa.
- Với `DEVTERM_DIALOG_TEST_WM=1`, so sánh hình học native qua `xdotool` trước/sau đổi Type, gồm tọa độ X/Y. Kiểm tra maximize/restore và Esc đóng trợ giúp vẫn được giữ.

Editor dùng chung mặc định **1040×760** cho cả hai loại, giới hạn theo monitor. Vùng Add Step và trợ giúp chia sẻ một hàng cố định để giữ diện tích danh sách trên màn hình nhỏ. Bộ nhớ kích thước đợi configure event ổn định 150 ms, bỏ kích thước maximized/fullscreen và hủy timer ngay khi đóng dialog; timer không giữ reference làm chậm destroy. Unmap nhớ kích thước native cuối cùng nếu người dùng đóng trước khi timer hoàn tất, bỏ qua maximize/fullscreen.

Build `meson compile -C build` thành công. Lượt cuối qua **19/19 nhóm, 114 ca test**, không fail, skip hoặc timeout; gồm đủ 15 ca kết nối loopback. Lượt hồi quy mới được ghi ở `build/meson-logs/high-full-final.txt` và `high-full-final.json`; lượt màn hình nhỏ/window manager ở `high-small-wm-fixed.txt`. Dependency thử nghiệm `asyncssh` nằm trong thư mục tạm; runtime ứng dụng không đổi.

Số hàng đầy đủ đo được (đang ở đầu danh sách):

| Môi trường | Sessions | Manage Quick Commands | Sequence | Shortcuts |
| --- | ---: | ---: | ---: | ---: |
| Xvfb 1920×1080, font mặc định, theme tối | 14 | 13 | 11 | 14 |
| Xvfb/Openbox 1366×768, Sans 13, theme sáng | 10 | 12 | 8 | 11 |

Lượt mới với Openbox riêng trên Xvfb 1366×768 qua cả 4 ca dialog, xác nhận tọa độ/kích thước native không đổi qua Type, đồng thời nút Maximize/Restore thật sự đổi trạng thái cửa sổ; Esc đóng popover trợ giúp và giữ dialog đang mở (cả Shortcuts có bộ bắt phím riêng). Không thay window manager hoặc cài gói vào desktop của người dùng. Có thể chạy nhánh này với `DEVTERM_DIALOG_TEST_WM=1` khi display đã có window manager và `xdotool`.

Đã xem ảnh GTK thực tế ở cả hai theme; ảnh chụp cục bộ có tiền tố `build/high-priority-large-*` và `build/high-priority-small-light-*`. Có thể tạo lại bằng biến `DEVTERM_DIALOG_CAPTURE=/absolute/path/prefix` khi chạy test. Ảnh không cần để build/chạy ứng dụng.

Giới hạn: chưa chạy trực tiếp trên Wayland hoặc Orca, chưa xác minh mọi tổ hợp monitor/scale/font hệ thống. Số hàng thay đổi theo font, scale và kích thước người dùng chọn. Kích thước chỉ nhớ trong RAM theo loại dialog; không ghi vị trí hoặc kích thước ra file. Search mới cho Sessions/Manage là bước phát triển sau, không thuộc thay đổi bố cục này.

Kiểm tra thủ công trên desktop thật:

1. Mở/đóng Sessions nhiều lần ở 1920×1080; quan sát các hàng ngay khi cửa sổ xuất hiện. Chọn một session rồi kéo hẹp/rộng: lựa chọn không mất, bố cục không đổi qua lại khi kéo gần ranh giới.
2. Mở Add/Edit Quick Command, nhập dữ liệu ở cả Command và Sequence, đổi Type liên tục. Type/Name, hướng dẫn, Save/Cancel và khung cửa sổ giữ vị trí; dữ liệu đang nhập không mất.
3. Kéo nhỏ editor khi đang ở Command rồi đổi Sequence; kiểm tra các nút còn thấy được. Đóng/mở lại Add hoặc Edit phải dùng cùng kích thước đã chọn. Cancel không ghi dữ liệu.
4. Kiểm tra thêm trên GNOME/Wayland và nhiều monitor/fractional scale; các môi trường này chưa được xác minh trong lượt tự động.

Các sửa lỗi đóng session, preference và định danh pane được mô tả tại [UI-PRIORITY-TESTING.md](UI-PRIORITY-TESTING.md).
