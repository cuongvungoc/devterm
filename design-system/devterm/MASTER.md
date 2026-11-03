# DevTerm — quy chuẩn giao diện desktop

Áp dụng cho ứng dụng native C + GTK4 + VTE, không áp dụng bố cục landing page hay bộ component web. Đây là bản chọn lọc từ skill `ui-ux-pro-max`, đối chiếu với ứng dụng và API GTK4 thực tế.

## Hướng thiết kế

- Workspace tập trung vào terminal; phong cách tối giản, nền slate, điểm nhấn teal. Nút New Terminal là hành động chính.
- Header chứa New Terminal, Sessions, tên ứng dụng và menu ứng dụng. Toolbar bên dưới nhóm Split Right/Down và Search/Draft/menu pane.
- Tab có icon, tên, trạng thái chọn và nút đóng. Tên dài được rút gọn, có tooltip đầy đủ; thanh tab cuộn được.
- Pane được chọn có cả viền teal và nhãn Selected. Output vẫn là VTE/PTY thực sự. Terminal dùng nền tối ở cả hai chế độ giao diện.
- Draft nằm dưới các terminal trong tab, không chứa action gửi lệnh. Giữ nguyên cơ chế RAM, focus và xác nhận mất nháp.
- Menu pane chứa Copy All Text, Export Text, Close Pane và Close Tab. Action không phù hợp bị vô hiệu hóa khi workspace trống; Export bị khóa trong lúc có thao tác export đang chờ.
- Quick Commands có Add Command/Manage cố định phía trên, các nút bên dưới xuống hàng trước khi rút gọn tên dài. Vùng nút có chiều cao giới hạn và cuộn dọc khi cần; thứ tự vẫn theo cấu hình.
- Footer ghi trạng thái hiện tại và tổng tab/số terminal pane của tab đang xem. Thông báo dài có tooltip.
- Sessions hiển thị loại giao thức, tên và endpoint thành các cấp riêng; trạng thái trống hướng dẫn tạo profile.

## Màu

Nguồn triển khai: `src/ui-style.c` (semantic tokens), `data/style.css` (component), `src/pane.c` (VTE palette). Không thêm font tải từ mạng hoặc thư viện giao diện khác.

| Vai trò | Tối | Sáng |
| --- | --- | --- |
| Nền workspace | `#111722` | `#F2F5F9` |
| Surface/control | `#1A2331` | `#FFFFFF` |
| Chữ chính | `#E4EBF3` | `#172033` |
| Chữ phụ | `#A0AEC1` | `#526176` |
| Viền | `#364356` | `#CFD8E3` |
| Accent | `#5DD6C0` | `#0F766E` |
| Chữ trên accent | `#10231F` | `#FFFFFF` |
| Lỗi/thao tác xóa | `#FDA4AF` | `#B42332` |

Terminal: nền `#101723`, chữ `#E2E8F0`, con trỏ `#5DD6C0`; bảng ANSI 16 màu khai báo qua API VTE. Nhãn Selected dùng `#9BE3D8` trên `#203E40`.

Các cặp chữ được tính bằng công thức relative luminance sRGB: chữ chính/surface tối **13,15:1**, sáng **16,27:1**; chữ phụ/surface tối **7,01:1**, sáng **6,31:1**; nút primary tối **9,24:1**, sáng **5,47:1**; Selected **7,88:1**; chữ terminal mặc định **14,57:1**. Màu do chương trình trong terminal tự chọn có thể có độ tương phản khác.

## Typography, kích thước và tương tác

- Giữ font giao diện của GTK và font monospace của terminal/editor; không ép cỡ chữ theo chuẩn mobile. Tiêu đề dùng trọng lượng 600–700; nội dung thường 400.
- Khoảng cách chính 8/12/16/24 px; nhóm nút dùng 6–8 px. Bo góc control 8 px, khung 8–10 px. Không thêm blur hay hiệu ứng chuyển động riêng.
- Cửa sổ mặc định 1200×800, tối thiểu 760×480 ở mức chữ mặc định. Cho phép resize, nhiều tab và split tùy ý; khi chia quá nhiều, người dùng cần mở rộng cửa sổ hoặc kéo divider để có vùng đọc đủ lớn.
- Dùng GtkHeaderBar, GtkNotebook, GtkPaned, GtkFlowBox, GMenu/GAction và dialog native. Icon symbolic cùng họ, nhãn truy cập cho nút chỉ có icon, icon trang trí được ẩn khỏi accessibility tree.
- Không dùng màu làm tín hiệu duy nhất. Giữ outline focus, tooltip và phím tắt hiện có; có mục Keyboard Shortcuts trong menu ứng dụng.
- Theme tối là mặc định. Menu Dark appearance đổi sáng/tối cho các cửa sổ trong tiến trình hiện tại, cửa sổ mới dùng cùng lựa chọn; chưa lưu ra đĩa. Provider thuộc GtkApplication để đóng một cửa sổ không gỡ theme khỏi cửa sổ còn lại.
- Không thêm animation; chuyển động và hành vi control theo GTK/cài đặt của phiên chạy ứng dụng.

## Cách dùng skill và đối chiếu

Đã tra `--design-system` với truy vấn `developer terminal desktop productivity` và `native desktop developer tool minimal`, dùng density 8/variance 3. Chỉ chọn hướng tối giản, phân cấp chức năng và palette phù hợp công cụ kỹ thuật; bỏ các kết quả landing/FAQ không phù hợp terminal.

Tra UX cho toolbar/progressive disclosure và thử lại với truy vấn hẹp hơn không cho kết quả khớp đã xác minh; skill cũng không có stack GTK. Bố cục toolbar/menu trong bản này là quyết định thiết kế dựa trên tính năng DevTerm và control native, không phải một guideline GTK được tìm thấy trong dataset. Không lưu đầu ra tra cứu chưa được kiểm chứng.

API được kiểm tra với GTK 4.14.5/VTE 0.76 trong môi trường build và tài liệu chính thức: [GtkHeaderBar](https://docs.gtk.org/gtk4/class.HeaderBar.html), [GtkPopoverMenu](https://docs.gtk.org/gtk4/class.PopoverMenu.html), [GTK CSS](https://docs.gtk.org/gtk4/css-overview.html). CSS nhúng vào executable bằng GResource để chạy độc lập với thư mục làm việc.

## Xác minh

- Build Meson/Ninja; chạy toàn bộ 46 test trong 9 nhóm với display X11/Xvfb, không skip.
- Kiểm tra ứng dụng thật ở 1200×800 và 800×650, cả hai theme: split nhiều cấp, Draft, Quick Commands xuống hàng, tab, menu và dialog.
- Kiểm tra bố cục 800×650 với font GTK Sans 13; kiểm tra đồng bộ theme giữa cửa sổ đang mở/cửa sổ mới, đóng cửa sổ đầu tiên, trạng thái action khi đóng/mở lại tab cuối và vòng đời hộp thoại còn chờ.
- Dùng foreground reader trên PTY raw kiểm tra Draft, Search, Copy và hủy Export không gửi ký tự ngoài ý muốn; Quick Commands vẫn đến terminal được chọn gần nhất.
- Chưa kiểm tra bằng Orca hoặc trực tiếp trên Wayland. Test desktop không thay thế thử nghiệm với mọi theme hệ thống, mức phóng chữ hoặc thiết bị phần cứng.

Ảnh chụp kiểm tra cục bộ nằm trong `build/ui-preview-dark.png`, `build/ui-preview-light.png` và `build/ui-preview-compact.png`; không phải mockup, không thuộc source bắt buộc để build.
