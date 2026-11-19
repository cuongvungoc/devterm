# SRS — Đặc tả yêu cầu phần mềm DevTerm

| Thuộc tính | Giá trị |
| --- | --- |
| Sản phẩm | DevTerm — ứng dụng terminal desktop native trên Linux |
| Phiên bản sản phẩm tham chiếu | 0.3.0 trong `meson.build` |
| Phiên bản tài liệu | 1.0 |
| Ngày lập | 13/11/2026 |
| Baseline source | `8612de6` — `[DT-11] feat: Add review host key when connect ssh session` |
| Trạng thái | Đặc tả theo hành vi hiện có; các giới hạn và hướng mở rộng được ghi riêng |
| Tài liệu thiết kế | [SDD.md](SDD.md) |

## Mục lục

1. [Mục đích và cách đọc](#1-mục-đích-và-cách-đọc)
2. [Tổng quan sản phẩm](#2-tổng-quan-sản-phẩm)
3. [Phạm vi và môi trường](#3-phạm-vi-và-môi-trường)
4. [Yêu cầu chức năng](#4-yêu-cầu-chức-năng)
5. [Các luồng sử dụng chính](#5-các-luồng-sử-dụng-chính)
6. [Dữ liệu và quy tắc lưu trữ](#6-dữ-liệu-và-quy-tắc-lưu-trữ)
7. [Yêu cầu phi chức năng](#7-yêu-cầu-phi-chức-năng)
8. [Xử lý lỗi và bảo toàn dữ liệu](#8-xử-lý-lỗi-và-bảo-toàn-dữ-liệu)
9. [Nghiệm thu và truy vết](#9-nghiệm-thu-và-truy-vết)
10. [Giới hạn và vấn đề cần theo dõi](#10-giới-hạn-và-vấn-đề-cần-theo-dõi)
11. [Quản lý thay đổi](#11-quản-lý-thay-đổi)

## 1. Mục đích và cách đọc

Tài liệu xác định phạm vi, hành vi và điều kiện nghiệm thu DevTerm cho chủ dự án, đội phát triển và QA. Đây là cơ sở để bảo trì và đánh giá thay đổi, không phải cam kết mọi tình huống đã được kiểm thử trên mọi hệ điều hành.

Các yêu cầu `FR-*` mô tả chức năng; `NFR-*` mô tả chất lượng và ràng buộc vận hành. Từ **phải** biểu thị hành vi bắt buộc trong baseline này. Các mục ngoài phạm vi không được hiểu là yêu cầu đã triển khai.

Nguồn đối chiếu: [AGENTS.md](../AGENTS.md), yêu cầu phát triển đã được chủ dự án chấp thuận, [README](../README.md), [meson.build](../meson.build), source trong `src/`, test trong `tests/` và tài liệu `packaging/`. Các mở rộng Sessions, Draft và quản lý tab phản ánh những yêu cầu được chấp thuận sau giới hạn MVP ban đầu của `AGENTS.md`.

## 2. Tổng quan sản phẩm

### 2.1. Bài toán

Người dùng cần thao tác nhiều shell hoặc thiết bị từ một cửa sổ, chia vùng làm việc, dùng lại lệnh thường xuyên, soạn nháp văn bản và lấy output để phân tích. Khi kết nối SSH/Telnet/Serial, việc nhập lại thông tin và đặt password trong các nút lệnh gây bất tiện và dễ lộ thông tin.

DevTerm cung cấp terminal tương tác thật, nhiều tab, split pane và Quick Commands gửi input vào đúng phiên đang dùng. Session profiles lưu metadata kết nối; password được nhập từng lần hoặc ghi nhớ qua keyring hệ thống.

### 2.2. Người dùng và hệ thống liên quan

| Tác nhân | Mục đích/trách nhiệm |
| --- | --- |
| Người dùng Linux | Dùng shell, quản lý tab/pane, Quick Commands, Draft và output |
| Kỹ sư phát triển/vận hành mạng | Kết nối SSH, Telnet, Serial; tái kết nối và kiểm tra host key |
| Người quản trị máy Linux | Cài ứng dụng, dependency, cấu hình quyền thiết bị và dịch vụ keyring |
| Người bảo trì DevTerm | Build, kiểm thử, đóng gói, cập nhật dependency và runtime riêng |
| Shell/chương trình foreground | Nhận input, xuất output qua PTY; quyết định ý nghĩa của input |
| Secret Service | Lưu/lấy/xóa password theo yêu cầu của người dùng |
| Máy/thiết bị đích | Xác thực và cung cấp phiên remote hoặc console |

### 2.3. Thuật ngữ

| Thuật ngữ | Ý nghĩa |
| --- | --- |
| Tab | Workspace có cây terminal split và một Draft riêng |
| Pane | Một terminal VTE có PTY và tiến trình shell/helper riêng |
| Active pane | Terminal được chọn gần nhất trong tab; vẫn được nhớ khi toolbar/dialog lấy focus |
| Profile | Metadata để tạo kết nối SSH, Telnet hoặc Serial; không phải phiên đang chạy |
| Session | Một lần chạy shell hoặc kết nối; reconnect tạo phiên remote mới |
| Scrollback | Lịch sử còn được VTE giữ; không phải log toàn bộ vòng đời pane |
| Alternate screen | Buffer thay thế mà các chương trình như `vim`/`top` có thể sử dụng |
| Draft | Editor văn bản tự do, dùng chung cho các pane trong một tab |
| Endpoint | Đích kết nối gồm giao thức, host/port, username hoặc đường dẫn Serial |
| Host key | Khóa nhận dạng SSH server, khác với password hoặc khóa đăng nhập của người dùng |

## 3. Phạm vi và môi trường

### 3.1. Trong phạm vi

- Terminal local tương tác, nhiều tab, split nhiều cấp, focus và quản lý tiến trình.
- Quick Commands tùy biến, lưu cấu hình, menu chuột phải và sắp xếp bằng kéo thả.
- Search trong từng pane, scrollbar, copy và xuất toàn bộ buffer còn giữ.
- Draft riêng theo tab, chỉ copy văn bản; giữ trong RAM và xác nhận khi bỏ nháp.
- Session profiles SSH/Telnet/Serial, tự đăng nhập có giới hạn, keyring và reconnect tại pane hiện tại.
- Xác nhận host key lần đầu và luồng review/thay host key đã thay đổi có kiểm soát.
- Giao diện sáng/tối, phím tắt và đóng gói `.deb` theo hệ điều hành/CPU đích.

### 3.2. Ngoài phạm vi

- Khôi phục tab, split, Draft, output hoặc kết nối sau khi thoát/crash.
- Broadcast input; chạy hoặc Send to Pane trực tiếp từ Draft.
- Ghi log liên tục, giữ vô hạn lịch sử, xuất màu/HTML hoặc khôi phục output đã mất.
- Plugin, cloud sync, SFTP/file manager, kho password riêng của DevTerm.
- Tự động OTP/MFA, SSH keyboard-interactive, nhiều jump host hoặc chuỗi đăng nhập `enable` nhiều bước.
- Gói Linux phổ quát AppImage/Flatpak/Snap, Windows/macOS.
- Danh sách tìm kiếm tab và hiển thị trạng thái kết nối nâng cao.

### 3.3. Môi trường và dependency

| Thành phần | Ràng buộc |
| --- | --- |
| Nền tảng | Linux desktop có X11 hoặc Wayland và PTY |
| Công nghệ | C11, GTK4 ≥ 4.10, VTE dành cho GTK4 ≥ 0.70; không trộn GTK3 |
| Thư viện | JSON-GLib ≥ 1.6, GLib/GIO, PCRE2, libsecret ≥ 0.20, libutil |
| Build source | Meson, Ninja, compiler C và development packages tương ứng |
| Kết nối | `ssh`, `telnet`, `picocom` khi sử dụng giao thức tương ứng |
| Review SSH host key | `ssh-keygen`, `ssh-keyscan` và cấu hình OpenSSH có thể đọc |
| Ghi nhớ password | Secret Service đang hoạt động trong phiên D-Bus người dùng |
| Serial | Tài khoản của tiến trình có quyền mở thiết bị, cấu hình cổng được thiết bị hỗ trợ |
| Gói cài mục tiêu | Ubuntu 22.04/24.04 × amd64/arm64; biến thể Mint 22.1 Xia amd64, base Noble |

Các mục tiêu đóng gói là ma trận phát hành, không có nghĩa một file `.deb` chạy trên mọi Linux. Máy đích cần Internet để APT tải dependency. Một gói cài phải khớp hệ điều hành và kiến trúc được khai báo.

## 4. Yêu cầu chức năng

### 4.1. Terminal và vòng đời tiến trình

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-TERM-01 | Mỗi pane phải dùng VTE và PTY thật, có tiến trình riêng. | Hai pane chạy shell độc lập; `cd` ở một pane không đổi thư mục shell pane kia; chương trình tương tác nhận input/resize. |
| FR-TERM-02 | Shell local phải ưu tiên `$SHELL` là đường dẫn tuyệt đối tới file thực thi hợp lệ, fallback `/bin/sh`. | `$SHELL` thiếu/không hợp lệ vẫn mở được shell fallback; lỗi spawn được hiển thị. |
| FR-TERM-03 | Phải hỗ trợ Unicode, màu ANSI, selection, clipboard và scrollback. | Nhập/xuất Unicode, dùng Ctrl+Shift+C/V; giữ tối đa 10.000 dòng scrollback cộng màn hình hiện tại. |
| FR-TERM-04 | Spawn, nhận output và quản lý child phải không chờ đồng bộ làm treo UI; shell exit giữ output. | Có thể thao tác pane khác khi chạy lệnh lâu; đóng khi spawn đang chờ không crash; child exit để lại pane xem được. |

### 4.2. Tab và split layout

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-TAB-01 | New Terminal/Ctrl+Shift+T phải tạo và chọn tab mới có một shell local. | Mỗi lần bấm có thêm tab; tab không kế thừa kết nối SSH hoặc biến môi trường export của shell cũ. |
| FR-TAB-02 | Tab/pane local mới phải ưu tiên thư mục local của active pane nếu xác định được, fallback home. | `cd` rồi tạo tab/split mở ở thư mục đó khi đọc được; không dùng đường dẫn remote làm cwd local. |
| FR-TAB-03 | Mỗi tab phải giữ riêng split, output, pane được chọn và Draft trong RAM. | Chuyển tab bằng click hoặc Ctrl+PageUp/PageDown phục hồi đúng workspace/focus; các child tiếp tục chạy. Phím chuyển tab vòng ở hai đầu. |
| FR-TAB-04 | Menu tab phải có Rename Tab, Move Left, Move Right và Close Tab. | Rename chỉ đổi nhãn tab, không đổi profile; tên được trim, dài 1–128 ký tự Unicode, không có ký tự điều khiển hoặc phân cách dòng/đoạn. Cho phép trùng tên. |
| FR-TAB-05 | Phải đổi thứ tự tab bằng giữ chuột trái/kéo thả và menu; tab không bị tạo lại. | ID, PTY, Draft và pane đang chọn giữ nguyên; Esc/thả ngoài thanh tab khôi phục thứ tự; nút × không kích hoạt kéo. |
| FR-TAB-06 | Đóng tab phải xác nhận nếu còn child đang chạy/khởi động hoặc Draft không rỗng. | Cancel giữ mọi dữ liệu; chấp nhận kết thúc child và bỏ Draft; tab cuối đóng đưa cửa sổ về trạng thái có thể New Terminal. |
| FR-SPLIT-01 | Split Right/Down phải chia pane đang chọn trong tab hiện tại, cho phép lồng nhiều cấp. | Split phải hai lần trên pane mới cho tỷ lệ xấp xỉ 50/25/25; không ép các pane về mép. Pane mới là shell local độc lập, kể cả trong tab remote. |
| FR-SPLIT-02 | Divider phải kéo được; khi split/đóng pane phải giữ bố cục các nhánh ngoài hợp lý. | Split nhánh con không cân lại toàn cây; resize giữ tỷ lệ trong giới hạn kích thước cho phép. |
| FR-SPLIT-03 | Click terminal/header/scrollbar phải chọn đúng pane và có dấu hiệu trực quan. | Viền/nhãn Selected chỉ ra pane đích; toolbar/dialog không làm mất pane được nhớ. |
| FR-SPLIT-04 | Đóng pane phải kết thúc child, thu gọn cây và chọn pane còn lại. | Đóng pane trong tab nhiều pane thực hiện ngay, giữ Draft; đóng pane cuối đi qua quy trình đóng tab. |

### 4.3. Quick Commands

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-QC-01 | Người dùng phải tạo/sửa command có ID ổn định, tên và command bắt buộc, tùy chọn Send Enter. | Từ chối chuỗi trống/chỉ whitespace; giữ Unicode, dấu nháy, operators và nội dung command; tooltip xem trước command. |
| FR-QC-02 | Click command phải gửi nguyên văn vào active pane trong tab hiện tại; Send Enter thêm `\r`. | `cd` tác động shell hiện tại; false chỉ điền lệnh; SSH remote nhận input tại remote; không tạo shell phụ hoặc broadcast. |
| FR-QC-03 | Phải giữ đích khi toolbar lấy focus, trả focus về pane sau gửi, báo lỗi khi pane không nhận input. | Không gửi vào pane khác; không tự Ctrl+C, xóa dòng hoặc ngắt foreground; pane exit/reconnect được xử lý an toàn. |
| FR-QC-04 | Chuột phải command phải có Edit/Delete; xóa từ mọi nơi phải hỏi xác nhận. | Đúng ID dù danh sách đổi thứ tự; Cancel là lựa chọn mặc định và giữ nguyên command. |
| FR-QC-05 | Manage phải có Move Up/Down; toolbar hỗ trợ kéo thả sắp xếp. | Giữ scroll/focus tại hàng thao tác; thứ tự toolbar và Manage đồng bộ; chỉ drop hợp lệ mới lưu; hủy kéo không gửi input. |
| FR-QC-06 | Phải lưu/khôi phục thứ tự và nội dung atomic trong `commands.json` version 1. | Khởi động không chạy command; file lỗi được giữ nguyên và khóa chỉnh sửa; lỗi lưu không để UI hiển thị như đã lưu thành công. Khi rỗng có hướng dẫn và Add Command. |

Input Quick Commands đến **chương trình foreground**, không nhất thiết là shell. Nếu đang dùng `vim` hoặc `top`, chương trình đó có thể nhận chuỗi. Quick Commands là cấu hình plaintext có tooltip, không phải nơi lưu password.

### 4.4. Scrollbar và tìm kiếm

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-SCROLL-01 | Mỗi pane phải có scrollbar dọc luôn hiện bên phải, trạng thái riêng. | Khi chưa có lịch sử vẫn giữ chiều rộng; kéo chọn đúng pane và không gửi input vào child; pane exit vẫn cuộn được. |
| FR-SCROLL-02 | Khi đọc lịch sử, output mới phải giữ vị trí trong phạm vi buffer; gõ terminal trở về cuối. | Pane khác không đổi vị trí; gõ trong Search/Draft không bị xử lý như gõ vào terminal; ở cuối tiếp tục theo dõi output. |
| FR-SEARCH-01 | Search/Ctrl+Shift+F phải tìm literal Unicode trong buffer pane đang chọn. | Ký tự `.`, `*`, `[` không được hiểu là regex; query/options riêng theo pane, giữ khi ẩn/đổi tab; Ctrl+F dành cho child. |
| FR-SEARCH-02 | Phải có Match case, Find whole words only, tìm tiến/lùi và quay vòng. | Enter tìm tiếp, Shift+Enter tìm trước; Not found được thông báo; `cat` whole-word bỏ qua `bobcat`, `cat2`, `cat_name`. |
| FR-SEARCH-03 | Search phải không gửi query hoặc phím điều hướng vào PTY. | Esc/× đóng Search, trả focus về pane; đóng pane khi debounce đang chờ không crash. |

Whole-word coi chữ cái, dấu kết hợp Unicode, chữ số và `_` là thành phần từ. Với VTE 0.76, duyệt kết quả thực tế theo dòng logic; nhiều lần xuất hiện trên cùng một dòng không được duyệt riêng. Alternate screen chỉ cung cấp buffer mà VTE đang hiển thị, không phải lịch sử nội bộ của `vim`/`top`.

### 4.5. Copy và export nội dung pane

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-TEXT-01 | Copy All Text phải lấy màn hình và toàn bộ scrollback còn giữ của active pane. | UTF-8, LF, không màu/ANSI/tiêu đề, nối dòng soft-wrap; không phụ thuộc selection hay vị trí cuộn; dùng được sau child exit. |
| FR-TEXT-02 | Export Text phải chụp cùng nội dung ngay khi bấm, sau đó cho chọn file local. | Output mới/đổi tab/đóng pane không đổi bản chụp; gợi ý tên `devterm-YYYYMMDD-HHMMSS.txt`; hỏi ghi đè; Cancel không tạo file. |
| FR-TEXT-03 | Ghi export phải chạy nền và thay file atomic, có báo lỗi và hủy. | Trước commit, lỗi/hủy giữ file đích cũ và dọn file tạm; file mới có quyền 0600; đóng cửa sổ hủy việc còn chờ. |

Chỉ lấy buffer còn tồn tại, không khôi phục output đã bị loại/ghi đè. Alternate screen không được ghép với lịch sử shell đang ẩn. Bản chụp có thể chứa thông tin nhạy cảm do người dùng hoặc chương trình đã hiển thị; đây không phải bộ lọc dữ liệu.

### 4.6. Draft

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-DRAFT-01 | Mỗi tab phải có một Draft dùng chung cho các pane, hiển thị dưới cây terminal. | Mặc định ẩn; lần đầu mở khoảng 30% chiều cao workspace; divider kéo được; tab khác có Draft riêng. |
| FR-DRAFT-02 | Draft phải hỗ trợ văn bản tự do Unicode nhiều dòng, selection, clipboard, Undo/Redo, bọc dòng và cuộn. | Enter xuống dòng; Ctrl+Z Undo, Ctrl+Shift+Z Redo; soạn thảo không gửi input tới PTY; không có Send/Execute. |
| FR-DRAFT-03 | Copy phải lấy selection nếu có, ngược lại toàn bộ; Clear phải xác nhận và Undo được. | Giữ whitespace/xuống dòng; Copy không xóa hoặc đánh dấu đã lưu; Clear là một thao tác Undo; rỗng khóa Copy/Clear. |
| FR-DRAFT-04 | Nội dung phải chỉ nằm trong RAM; ẩn/đổi tab giữ trạng thái; đóng có nháp phải xác nhận. | Giữ caret, selection, scroll, undo và divider; whitespace cũng tính là nháp; nháp ẩn/đã Copy/child exit vẫn được cảnh báo. Đóng cửa sổ liệt kê các tab còn nháp. |

Xác nhận đóng chỉ áp dụng cho thao tác đóng bình thường. Crash, kill cưỡng bức hoặc mất điện không thể bảo toàn Draft. Khi không có Draft, đóng cả cửa sổ hiện không thêm hộp thoại chỉ vì còn child sống; không đồng nhất với quy tắc đóng từng tab.

### 4.7. Session profiles và kết nối

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-SESSION-01 | Sessions phải có Add/Edit/Duplicate/Delete và Connect/double-click mở tab mới. | Lưu UUID và metadata vào `sessions.json` version 1; không tự connect khi khởi động. Duplicate có UUID mới và không sao chép password đã nhớ. |
| FR-SESSION-02 | SSH phải hỗ trợ host, port, username, timeout, password hoặc key/agent. | Password che ký tự và gửi tối đa một lần trong luồng tự động; host mới phải được xác nhận; host key thay đổi bị từ chối trước password. |
| FR-SESSION-03 | Telnet phải hỗ trợ host/port và tự đăng nhập theo prompt cấu hình. | Có chế độ tương tác thủ công; khi tự động xử lý được prompt chia gói/ANSI; giao diện thông báo Telnet không mã hóa. |
| FR-SESSION-04 | Serial phải hỗ trợ device, baud, data bits, parity, stop bits, flow control và Send initial Enter. | Dùng client thực, giữ khóa cổng; không tự nâng quyền; lỗi quyền/thiết bị không tồn tại được giữ để chẩn đoán. |
| FR-SESSION-05 | Telnet/Serial auto-login phải có username/password/ready prompt theo profile, timeout 5–120 giây. | Username prompt rỗng cho thiết bị chỉ hỏi password; prompt lặp/timeout kết thúc, không lặp gửi password; Esc/Ctrl+C hủy. Input khác bị bỏ qua và transcript xác thực không vào VTE, kể cả thiết bị echo password. |
| FR-SESSION-06 | Disconnect phải dừng client; Reconnect Current Pane phải dùng lại chính pane hiện tại. | Giữ tab, VTE, split, Draft và buffer còn giữ; lấy credential trước khi dừng client cũ; Cancel giữ kết nối cũ; không chạy chồng hai client; đổi tab không đổi đích. |
| FR-SESSION-07 | Reconnect phải dùng bản chụp profile của pane, xử lý lỗi và hủy đúng vòng đời. | Sửa/xóa profile không đổi pane đã mở; đóng pane/app hủy yêu cầu; thất bại giữ output, không auto-retry; không hứa phục hồi chương trình/cwd remote. |
| FR-SESSION-08 | Remember password phải dùng Secret Service, gắn UUID và endpoint, không fallback plaintext. | Đổi tên giữ liên kết; đổi endpoint không dùng password cũ; keyring lỗi cho phép nhập một lần; xóa profile yêu cầu xóa credential liên quan. File có trường `password` bị từ chối. |

Serial cho phép cấu hình baud 50–4.000.000, data bits 5–8, stop bits 1–2, parity `none/even/odd`, flow `none/hard/soft`. Thiết bị/driver/client vẫn có thể từ chối một cấu hình nằm trong khoảng hợp lệ này. Device phải là đường dẫn tuyệt đối dưới `/dev/`; identity file SSH nếu có phải là đường dẫn tuyệt đối.

### 4.8. SSH host key thay đổi

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-HOST-01 | Pane phải có Review Host Key khi helper xác nhận client OpenSSH thất bại vì host key thay đổi. | Output remote giả cảnh báo không kích hoạt review; không dựa vào việc quét text hiển thị trong VTE; lỗi giữ nguyên pane. |
| FR-HOST-02 | Review phải hiển thị endpoint, file hiệu lực, fingerprint cũ/mới, loại khóa và backup dự kiến. | Chuẩn bị chạy nền, có thể hủy; Copy Fingerprint không đóng dialog; Cancel/Esc trước xác nhận không sửa file. |
| FR-HOST-03 | Replace Key & Reconnect chỉ được phép sau xác nhận fingerprint đã kiểm chứng độc lập. | Cancel mặc định; không bỏ kiểm tra host key. `ssh-keyscan` chỉ lấy khóa ứng viên, phải khớp fingerprint OpenSSH đã báo; không tự chứng minh server đáng tin. |
| FR-HOST-04 | Thay key phải có backup riêng, chống ghi đè thay đổi đồng thời và chỉ tác động endpoint xác nhận. | Backup toàn file quyền 0600, tên duy nhất; giữ entry/alias khác, hỗ trợ hash; file đổi trong lúc review bị từ chối; sau lưu gọi reconnect đúng pane qua luồng credential thông thường. |
| FR-HOST-05 | Trường hợp không xác định an toàn phải giữ file và báo xử lý thủ công. | Kết nối qua ProxyJump/ProxyCommand/HostKeyAlias; file hệ thống/symlink/không sở hữu/không ghi được/lớn hơn 8 MiB; entry wildcard, CA, revoked hoặc phức tạp không được tự thay. |

`known_hosts` là dữ liệu dùng chung với ứng dụng SSH khác. Backup không tự xóa. Sau khi đã chấp nhận và lưu key, hủy password hoặc reconnect lỗi không hoàn tác key đã tin cậy; nếu commit thất bại sau bước backup, backup có thể vẫn tồn tại.

### 4.9. Giao diện chung

| ID | Yêu cầu | Điều kiện nghiệm thu |
| --- | --- | --- |
| FR-UI-01 | Giao diện phải có header New Terminal/Sessions, toolbar workspace, thanh tab, Quick Commands và trạng thái. | Pane actions khóa khi workspace rỗng; số tab/pane phản ánh tab hiện tại; command tự xuống hàng/cuộn khi không đủ chỗ. |
| FR-UI-02 | Phải có sáng/tối và danh sách phím tắt; theme đồng bộ trong cùng tiến trình. | Menu ứng dụng chuyển Dark appearance; terminal vẫn tối; lựa chọn theme hiện chỉ giữ trong phiên, không lưu xuống đĩa. |

## 5. Các luồng sử dụng chính

### UC-01 — Chạy command trong pane được chọn

1. Người dùng mở tab và split thành ít nhất hai pane.
2. Chọn pane B rồi click một Quick Command trên toolbar.
3. Ứng dụng giữ B làm đích dù focus đi qua nút; gửi command và `\r` nếu bật Send Enter, sau đó focus B.
4. Chương trình foreground tại B nhận input; pane A không nhận gì.

Ngoại lệ: không có pane/child đã exit/đang reconnect thì báo trạng thái và không chuyển sang gửi tại pane khác. Nếu B đang có lệnh nhập dở hoặc chạy `vim`, DevTerm không tự sửa ngữ cảnh trước khi gửi.

### UC-02 — Sắp xếp và sửa Quick Commands

1. Chuột phải command cần sửa, chọn Edit, cập nhật rồi Save.
2. Giữ chuột trái kéo command tới vị trí mới, thả hợp lệ để lưu thứ tự.
3. Mở Manage, Move Up/Down hoặc Delete tại hàng ở sâu trong danh sách.
4. UI giữ vùng đang xem và focus phù hợp; Delete yêu cầu xác nhận.

Ngoại lệ: Esc/drop ngoài hủy reorder; lỗi ghi hoàn nguyên thay đổi chưa lưu và báo lỗi. File JSON lỗi khóa chỉnh sửa, giữ file cho người dùng sửa/di chuyển rồi mở lại ứng dụng.

### UC-03 — Soạn Draft và đóng tab

1. Mở Draft dưới tab hiện tại, nhập ghi chú/lệnh nhiều dòng.
2. Copy selection hoặc toàn bộ rồi tự paste vào nơi mong muốn.
3. Ẩn Draft, chuyển tab và quay lại: nội dung cùng trạng thái editor còn nguyên.
4. Đóng tab: xác nhận bỏ nháp và kết thúc các child nếu còn sống.

Cancel không thay đổi tab. Copy không bỏ nghĩa vụ xác nhận nháp chưa xóa. Clear được xác nhận và Undo được, nhưng đóng tab đã chấp nhận không Undo được.

### UC-04 — Kết nối và reconnect

1. Chọn profile trong Sessions, Connect; lấy password từ keyring hoặc hỏi người dùng khi cần.
2. Mở tab mới; thực hiện kiểm tra host key SSH hoặc nhận diện prompt Telnet/Serial trước khi cho tương tác.
3. Khi cần, chọn Reconnect Current Pane.
4. Chuẩn bị credential trước, dừng client cũ, chờ kết thúc rồi spawn client mới trong cùng VTE.
5. Tab, split, Draft và scrollback được giữ; người dùng tiếp tục trên phiên remote mới.

Cancel trước bước dừng giữ kết nối cũ. Nếu client cũ chưa kết thúc đúng hạn thì không spawn chồng. Thiếu quyền Serial cần xử lý quyền tài khoản ngoài ứng dụng, không bằng chạy toàn bộ DevTerm dưới root.

### UC-05 — Review SSH host key

1. OpenSSH từ chối do key thay đổi; chưa gửi password, pane xuất hiện Review Host Key.
2. Người dùng mở review, đối chiếu endpoint/file/fingerprint cũ và mới.
3. Xác minh fingerprint qua console thiết bị hoặc quản trị viên, tick xác nhận.
4. Replace Key & Reconnect tạo backup, kiểm tra file chưa đổi, thay đúng entry và reconnect chính pane nguồn.

Cancel trước xác nhận không đổi file. File/candidate thay đổi hoặc cấu hình phức tạp dừng quy trình, không bỏ strict checking. Reconnect sau đó vẫn phải qua xác thực và kiểm tra key bình thường.

### UC-06 — Xuất output

1. Chọn pane cần phân tích, bấm Export Text.
2. Ứng dụng chụp buffer lúc bấm; người dùng chọn đường dẫn local và quyết định ghi đè nếu có.
3. Worker ghi file tạm và commit atomic; UI báo kết quả.

Output mới không được thêm vào snapshot đang chờ. Hủy hoặc lỗi trước commit giữ file đích cũ. Thao tác đã commit không thể hoàn tác bằng yêu cầu hủy đến muộn.

## 6. Dữ liệu và quy tắc lưu trữ

| Dữ liệu | Vị trí | Thời gian tồn tại | Quy tắc |
| --- | --- | --- | --- |
| Quick Commands | `$XDG_CONFIG_HOME/devterm/commands.json` | Qua các lần chạy | Version 1, ID duy nhất, array giữ thứ tự; plaintext |
| Session metadata | `$XDG_CONFIG_HOME/devterm/sessions.json` | Qua các lần chạy | Version 1, UUID duy nhất; không có password |
| Password ghi nhớ | Secret Service của tài khoản | Theo keyring/consent người dùng | Gắn profile + endpoint; không đưa vào JSON |
| Tab/title/order/split/active pane | RAM | Đến khi đóng tab/app | Không tự khôi phục |
| Draft, search, scrollback | RAM/VTE của tab hoặc pane | Đến khi giải phóng; scrollback có giới hạn | Không tự lưu ra file |
| Export | File local người dùng chọn | Đến khi người dùng xóa | Snapshot UTF-8, không màu |
| SSH host keys | File hiệu lực do OpenSSH xác định | Theo cấu hình SSH | Chỉ sửa trong luồng xác nhận; backup cạnh file |
| Theme | Trạng thái UI trong tiến trình | Phiên chạy | Không có setting persistence |

Nếu `XDG_CONFIG_HOME` không thiết lập hoặc rỗng, hai JSON fallback về `~/.config/devterm/`. File chưa tồn tại tương ứng danh sách rỗng. JSON sai cú pháp/schema/version/ID phải được giữ nguyên và báo lỗi; lỗi Quick Commands không được ngăn terminal local hoạt động, lỗi Sessions không được ngăn Quick Commands hoặc terminal.

Không có cơ chế merge/khóa liên tiến trình cho sửa JSON; atomic replace tránh file dở dang nhưng không giải quyết hai bên đồng thời lưu. Schema đầy đủ và ví dụ nằm trong SDD.

## 7. Yêu cầu phi chức năng

| ID | Yêu cầu | Cách đánh giá |
| --- | --- | --- |
| NFR-01 | UI phải giữ khả năng tương tác khi spawn, chạy client, ghi export, lấy credential hoặc quét host key. | Chạy output dài đồng thời đổi tab/split; các công việc chờ I/O dài phải dùng async/worker/helper. Không đặt SLA độ trễ chưa đo. |
| NFR-02 | Phải quản lý callback và child an toàn khi đóng/hủy giữa chừng. | Test đóng khi spawn/password/search/export/host-key đang chờ; không crash, use-after-free hoặc client trùng do reconnect. |
| NFR-03 | Lỗi lưu trước commit phải không làm hỏng file cũ; cấu hình lỗi không bị âm thầm ghi đè. | Test lỗi quyền, JSON sai, cancel export và conflict known_hosts; UI thông báo trạng thái thật. Atomic không đồng nghĩa bảo đảm mọi trường hợp mất điện. |
| NFR-04 | Password tự động phải không xuất hiện trong argv, environment, JSON, file tạm plaintext hoặc buffer VTE. | Kiểm tra launch descriptor, IPC askpass, echo từ server giả và copy/export; tránh đưa secret vào log/core dump của frontend/helper. |
| NFR-05 | SSH phải giữ kiểm tra danh tính host; thay trust phải có hành động xác nhận cụ thể. | Từ chối key thay đổi trước password; không dùng StrictHostKeyChecking=no; kiểm tra kênh thông báo riêng và consent review. |
| NFR-06 | Phải hoạt động với quyền tài khoản desktop thông thường, không yêu cầu root cho terminal/session. | Gói cài có thể cần sudo/APT; runtime app không tự chmod thiết bị/nâng quyền. Quyền Serial là điều kiện hệ điều hành. |
| NFR-07 | Nội dung command, terminal, Draft, copy/export và tên tab phải hỗ trợ UTF-8. | Round-trip Unicode, dấu kết hợp, dấu nháy, whitespace và operators; validation không phá chuỗi hợp lệ. |
| NFR-08 | Giao diện phải dùng được khi thay đổi kích thước, có tooltip và thao tác bàn phím cho chức năng chính. | Xem label ellipsis/tooltip, vùng cuộn, focus, phím tắt; đánh giá riêng trên Wayland/Orca trước khi cam kết hỗ trợ hoàn chỉnh. |
| NFR-09 | Thiết kế phải phân tách UI, terminal/layout, model/config, credential và helper kết nối. | Review ranh giới module, ownership và test độc lập; không mô phỏng terminal bằng TextView. |
| NFR-10 | Artifact phát hành phải đúng OS/CPU, đi kèm kết quả test/checksum và không chứa dữ liệu máy phát triển. | Build/test trên môi trường mục tiêu, cài container/VM sạch; không đưa password, keyring, user config, Git metadata vào gói. |

Keyring không ngăn ứng dụng khác của cùng người dùng trong phiên đã mở khóa truy cập dữ liệu theo chính sách desktop. Không cam kết chống root/same-UID inspection tuyệt đối. Telnet không có tính bí mật trên mạng; bảo vệ password tại máy local không thay đổi đặc tính này.

## 8. Xử lý lỗi và bảo toàn dữ liệu

| Tình huống | Hành vi bắt buộc |
| --- | --- |
| Shell/client không spawn được hoặc exit | Hiển thị lỗi/trạng thái, giữ pane và output xem được |
| Không có active pane hợp lệ | Khóa action thích hợp hoặc báo rõ; không tự chọn pane khác để gửi command |
| JSON không hợp lệ | Giữ nguyên file, khóa editor của tính năng đó, cho dùng terminal local |
| Lưu cấu hình thất bại | Báo lỗi, không chấp nhận trạng thái UI như đã lưu thành công |
| Keyring bị khóa/không có | Báo lỗi và có đường nhập password một lần; không fallback file |
| Auto-login không nhận prompt/timeout/rejected | Dừng, không lặp password; không đưa transcript chứa secret vào VTE |
| `/dev/ttyUSB0: Permission denied` | Giữ diagnostic; người dùng kiểm tra quyền/group hiệu lực của tiến trình và đăng nhập lại khi cần |
| Host key thay đổi | Từ chối xác thực, cho review có kiểm chứng hoặc hướng dẫn xử lý thủ công |
| known_hosts đổi khi review | Từ chối commit; yêu cầu lấy lại trạng thái thay vì ghi đè phiên bản mới |
| Đóng tab/app còn Draft | Cancel mặc định trong hộp thoại xác nhận; chấp nhận mới bỏ dữ liệu |
| Export lỗi/hủy trước commit | File đích cũ giữ nguyên, file tạm được dọn khi có thể |

## 9. Nghiệm thu và truy vết

Ma trận module/test chi tiết nằm trong [SDD, mục 13](SDD.md#13-kiểm-thử-và-truy-vết-yêu-cầu). Những luồng sau là checklist nghiệm thu; không phải tuyên bố đã chạy lại khi lập tài liệu:

1. Mở shell, chạy lệnh thường và `vim`/`top`/`ping`; kiểm tra màu, Unicode, resize và UI đáp ứng.
2. Tạo nhiều tab, rename Unicode, reorder bằng menu/kéo; Esc/drop ngoài hủy, không gửi ký tự vào terminal.
3. Split liên tiếp cả hai hướng, kéo divider rồi split/resize/close; giữ hình học các nhánh khác.
4. CRUD/reorder Quick Commands, giữ scroll Manage; mở lại xác nhận thứ tự/ID; lỗi save và JSON sai không mất dữ liệu.
5. Chọn pane khác nhau rồi gửi `cd`, dấu nháy, pipe, Unicode, Send Enter false; đúng phiên và không broadcast.
6. Kéo scrollbar khi output chạy; output giữ vị trí khi đọc lịch sử, gõ terminal về cuối.
7. Search case/whole-word/literal, tiến/lùi/wrap, isolation và đóng giữa debounce; query không lọt vào child.
8. Copy/Export đúng buffer/snapshot, alternate screen, Unicode; thử Cancel, overwrite, lỗi quyền và pane exit.
9. Draft copy selection/all, Clear/Undo, ẩn/đổi tab, xác nhận đóng; không có đường thực thi nội dung editor.
10. SSH password/key/agent, host mới Cancel/Trust, password sai; kiểm tra keyring có/không, endpoint đổi và profile duplicate/delete.
11. Telnet/Serial prompt chia gói/ANSI/password echo, timeout/cancel, Serial quyền thiếu; không lộ secret trong buffer/export.
12. Reconnect đúng pane giữa nhiều tab, Cancel trước khi dừng, client không kết thúc, lỗi retry và đóng khi đang chờ.
13. Host-key review cũ/mới, Copy/Cancel, checkbox, backup, hash/alias, conflict, cấu hình bị từ chối và reconnect giữ workspace.
14. Build `.deb` cho từng tổ hợp cần phát hành; kiểm checksum, dependency, launcher/helper, upgrade/remove giữ JSON và thử máy desktop đích.

Repository hiện khai báo **14 nhóm Meson**, tổng **80 test** nếu đủ dependency/display theo danh sách test và README tại baseline. Một số nhóm có thể skip khi thiếu GUI/client/fixture; phải đọc log, không chỉ dựa vào trạng thái nhóm. Báo cáo đóng gói cũ ghi 46 test của artifact 0.1.0, không chứng minh artifact 0.3.0 đã nghiệm thu. Tài liệu này không chạy lại test và không tạo gói mới.

## 10. Giới hạn và vấn đề cần theo dõi

| Nội dung | Tình trạng / tác động |
| --- | --- |
| Buffer hữu hạn | Không dùng Copy/Export như bản ghi đầy đủ từ lúc mở pane; alternate screen và clear-screen có thể thay đổi dữ liệu còn lấy được |
| Tìm kiếm VTE | Điều hướng theo dòng logic trên VTE 0.76; chưa có UI regex hoặc bộ đếm mọi occurrence |
| RAM-only | Mất Draft/workspace khi crash hoặc kết thúc cưỡng bức; không có phục hồi phiên |
| Xử lý secret | Không tự phát hiện/xóa password người dùng đã lưu trong Quick Commands; input thủ công vẫn có thể bị echo |
| Auto-login | Chỉ hỗ trợ luồng được nêu; prompt riêng phải đủ chính xác; không phải workflow engine |
| Platform validation | Kiểm thử được ghi nhận chủ yếu trên X11/Xvfb; Wayland, Orca, GPU/Serial vật lý và ARM thật cần nghiệm thu riêng |
| Packaging Mint | `build-mint-deb.py` tại baseline còn kiểm report đúng 9 nhóm; Meson hiện có 14. Cần đồng bộ điều kiện này trước khi dùng report build mới để tái đóng gói Mint; đây là tồn đọng, chưa sửa trong tài liệu này |
| Artifact cũ | Các ví dụ version 0.1.0 trong tài liệu packaging và artifact cũ không phản ánh tự động code 0.3.0 |

Các cải tiến có thể xem xét ở phiên bản sau: tìm kiếm tab, trạng thái kết nối, sắp xếp lại menu/toolbar, distribution format phổ quát và lưu Draft có chủ đích. Chúng chưa thuộc yêu cầu triển khai của baseline này.

## 11. Quản lý thay đổi

Mỗi thay đổi hành vi cần nêu ID yêu cầu chịu ảnh hưởng, cập nhật tiêu chí nghiệm thu, SDD và test/checklist liên quan. Thay đổi định dạng JSON phải có quyết định version/migration rõ ràng; không diễn giải dữ liệu cũ bằng schema mới rồi ghi đè. Thay đổi trust/credential phải xét cả luồng hủy, endpoint đích, file dùng chung và callback sau khi pane đóng.
