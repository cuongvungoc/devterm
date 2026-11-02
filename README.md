# DevTerm

DevTerm là terminal desktop native cho Linux, viết bằng C, GTK4 và VTE dành cho GTK4. Mỗi tab có bố cục split riêng; mỗi pane có PTY và shell riêng. Quick Commands gửi nguyên chuỗi command vào pane đang chọn trong tab hiện tại, kể cả khi pane đó đang ở phiên SSH.

## Cài đặt và chạy

Trên Ubuntu 24.04:

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config libgtk-4-dev libvte-2.91-gtk4-dev libjson-glib-dev libpcre2-dev libsecret-1-dev openssh-client inetutils-telnet picocom gnome-keyring
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/devterm
```

Phiên bản đã kiểm tra: GTK 4.14.5, VTE GTK4 0.76.0, JSON-GLib 1.8.0, libsecret 0.21.4, OpenSSH 9.6p1, picocom 3.1, Meson 1.3.2. Cần phiên đồ họa X11 hoặc Wayland để mở cửa sổ. Ghi nhớ password cần dịch vụ Secret Service đang hoạt động trong phiên D-Bus của người dùng, ví dụ GNOME Keyring. Thiếu dịch vụ này vẫn có thể nhập password cho từng lần kết nối.

Build tạo hai executable `devterm` và `devterm-connect`; giữ chúng cùng thư mục khi chạy hoặc phân phối. `meson install -C build` cài cả hai. `ssh`, `telnet` và `picocom` chỉ cần khi sử dụng giao thức tương ứng.

Nếu thư mục `build` được tạo trước đây với dependency giải nén trong `/tmp`, sau khi cài dependency bằng apt cần xóa cache đường dẫn cũ và cấu hình lại:

```sh
meson setup --reconfigure --clearcache build
meson compile -C build
```

## Sử dụng

- **New Terminal** hoặc **Ctrl+Shift+T** tạo và chuyển sang tab mới với một shell local. Tên tab lần lượt là `Terminal 1`, `Terminal 2`… Thanh tab cuộn được khi có nhiều tab.
- Tab mới kế thừa thư mục local của pane đang chọn nếu xác định được, fallback về home. DevTerm đọc thư mục của tiến trình shell local qua `/proc`; đường dẫn remote do SSH báo về không được dùng để khởi động shell local. Tab mới không kế thừa kết nối SSH hoặc biến môi trường được export trong shell của tab cũ.
- **Split Right** chia đôi pane đang chọn thành trái/phải; **Split Down** chia đôi trên/dưới, trong tab hiện tại. Các divider bên ngoài giữ nguyên vị trí khi split hoặc đóng pane; khi resize cửa sổ, tỷ lệ đã chọn được giữ trong giới hạn kích thước hiển thị. Ví dụ split phải hai lần liên tiếp tạo tỷ lệ khoảng 50% / 25% / 25%. Pane mới cũng ưu tiên thư mục local của pane đang chọn. Có thể chia tiếp bất kỳ pane nào, kéo đường phân cách để đổi kích thước. Click vào terminal hoặc tiêu đề pane để chọn; viền xanh đánh dấu pane hiện tại.
- **Ctrl+PageUp** / **Ctrl+PageDown** chuyển tab trước/sau, vòng lại ở hai đầu. Mỗi tab ghi nhớ pane được chọn gần nhất và trả focus về pane đó khi quay lại; các shell ở tab khác tiếp tục chạy.
- **Close Pane** đóng pane đang chọn và thu gọn bố cục. Nếu đó là pane cuối, thao tác sẽ đóng tab. Nút **×** trên tab hoặc **Ctrl+Shift+W** đóng toàn bộ tab. Khi tab còn shell đang chạy hoặc khởi động, ứng dụng hỏi xác nhận; chọn **Cancel** để giữ tab, **Close Tab** để kết thúc tất cả pane trong tab đó. Đóng pane riêng trong tab có nhiều pane vẫn thực hiện ngay.
- Đóng tab cuối giữ cửa sổ ở trạng thái trống với nút **New Terminal**. Shell tự thoát sẽ giữ pane cùng output để xem lại; đóng tab không còn shell sống sẽ không hỏi xác nhận. Tab và bố cục không được khôi phục sau khi thoát ứng dụng.
- **Ctrl+Shift+C** sao chép vùng chọn, **Ctrl+Shift+V** dán từ clipboard. VTE xử lý màu ANSI, scrollback và kích thước PTY.
- Mỗi pane có **scrollbar dọc luôn hiện bên phải**, dùng để kéo nhanh trong buffer còn giữ. Khi chưa có lịch sử để cuộn, scrollbar vẫn chiếm cùng chiều rộng và không kéo được. Kéo scrollbar chọn pane tương ứng và giữ focus nhập liệu ở terminal; các pane/tab khác có vị trí cuộn riêng. Đang đọc lịch sử thì output mới giữ vị trí, trong giới hạn buffer; kéo về cuối sẽ tiếp tục theo dõi output. Gõ phím vào terminal đưa màn hình về cuối; gõ trong ô Search vẫn dành cho tìm kiếm. Pane đã shell exit/ngắt kết nối vẫn cuộn xem được. Với màn hình thay thế như `vim`/`top`, scrollbar không truy cập lịch sử nội bộ của chương trình đó.
- **Copy All Text** sao chép toàn bộ văn bản còn lưu trong pane đang chọn, gồm màn hình và scrollback, vào clipboard. Vùng chọn và vị trí cuộn hiện tại được giữ nguyên; **Ctrl+Shift+C** vẫn chỉ sao chép vùng chọn.
- **Export Text…** chụp nội dung pane ngay khi bấm, rồi mở hộp thoại Save để chọn tên và vị trí lưu. Tên gợi ý là `devterm-YYYYMMDD-HHMMSS.txt`; hộp thoại hỏi xác nhận khi chọn ghi đè. Hủy hộp thoại không tạo file. Export vẫn dùng bản chụp ban đầu nếu có output mới, đổi tab hoặc đóng pane trong khi thao tác đang chờ.
- Copy All Text và Export Text lấy cùng định dạng văn bản UTF-8, xuống dòng LF, không màu/ANSI, không thêm tiêu đề. Dòng dài bị bọc theo chiều rộng terminal được nối lại thành dòng logic. Nội dung gồm prompt, input được hiển thị và output; không gửi input vào shell, không dừng chương trình đang chạy. Pane đã shell exit vẫn copy/xuất được.
- Phạm vi chỉ là buffer còn giữ (tối đa 10.000 dòng scrollback cộng màn hình hiện tại), không khôi phục nội dung đã bị loại khỏi buffer hoặc bị chương trình ghi đè. Khi đang ở màn hình thay thế như `vim`/`top`, bản chụp lấy buffer màn hình đó, không gộp lịch sử shell đang ẩn. Đây là thao tác chụp một lần, không ghi log liên tục.
- Export hỗ trợ đường dẫn file local, ghi file bất đồng bộ qua file tạm cùng thư mục rồi đổi tên atomic. Lỗi ghi không làm hỏng file đích cũ; file mới có quyền `0600`. VTE trích xuất vào bộ nhớ trên luồng GTK trước khi mở Save, nên buffer rất lớn có thể gây chờ ngắn; ghi đĩa không chạy trên luồng GTK. Đóng cửa sổ hủy thao tác còn chờ và dọn file tạm; thao tác đã hoàn tất lưu không được hoàn tác.
- **Search** hoặc **Ctrl+Shift+F** mở thanh tìm kiếm trong pane đang chọn. Tìm trong nội dung hiện tại và scrollback còn được giữ (tối đa 10.000 dòng). Mặc định tìm văn bản thường, không phân biệt hoa/thường; bật **Match case** để phân biệt. Các ký tự như `.`, `*`, `[` được tìm nguyên văn, không phải regex.
- Search tự tìm khi thay đổi nội dung. Dùng **Enter** / nút mũi tên xuống để tìm tiếp, **Shift+Enter** / mũi tên lên để tìm ngược; kết quả được đánh dấu và tự cuộn đến vị trí tìm thấy, quay vòng khi đến cuối. **Not found** nghĩa là không tìm thấy. **Esc** hoặc nút × trên thanh search đóng thanh và trả focus về terminal. Enter vẫn chạy lệnh bình thường nếu focus đang ở terminal.
- **Find whole words only** chỉ tìm khi hai bên nội dung cần tìm không liền với chữ cái, dấu Unicode, chữ số hoặc `_`. Ví dụ, tìm `cat` sẽ khớp `(cat)` nhưng bỏ qua `bobcat`, `cat2` và `cat_name`. Mặc định tắt; có thể dùng cùng **Match case**.
- Mỗi pane giữ riêng nội dung search và hai tùy chọn Match case / Find whole words only trong phiên hiện tại, kể cả khi đổi tab hoặc đóng/mở lại thanh search. Search vẫn dùng được với output còn lưu sau khi shell exit. Search không gửi input vào shell hoặc chương trình foreground; **Ctrl+F** tiếp tục dành cho chương trình trong terminal.
- **Add Command** tạo nút đầu tiên. **Manage…** cho phép thêm, sửa, xóa và đổi thứ tự bằng nút mũi tên. Tooltip của mỗi nút hiển thị command.
- Quick Commands dùng chung cho tất cả tab. Bấm nút sẽ gửi nội dung vào pane được chọn gần nhất trong tab đang hiển thị, rồi trả focus về pane đó; tab bị ẩn không nhận input. **Send Enter** gửi thêm `\r`; khi tắt, command chỉ được điền và chờ người dùng nhấn Enter. Input đi vào chương trình foreground hiện tại: nếu pane đang SSH, lệnh đến phiên remote; nếu đang mở `vim` hoặc `top`, input có thể được chương trình đó xử lý. DevTerm không xóa dòng đang gõ và không ngắt chương trình đang chạy.

Nếu shell đã thoát hoặc chưa khởi động được, DevTerm báo lỗi trên pane hoặc thanh trạng thái và không gửi Quick Command vào pane đó.

Giới hạn của search trên VTE 0.76: điều hướng trước/sau chỉ chọn một lần xuất hiện trên mỗi dòng logic (một dòng có thể được bọc thành nhiều dòng hiển thị). Nhiều lần xuất hiện trên cùng một dòng không được duyệt riêng. Đây là giới hạn của [bộ tìm kiếm VTE 0.76](https://github.com/GNOME/vte/blob/0.76.0/src/vte.cc#L11157).

## Sessions: SSH, Telnet và Serial

Bấm **Sessions…** trên toolbar để quản lý profile bằng **Add**, **Edit**, **Duplicate**, **Delete**. Chọn profile rồi bấm **Connect**, hoặc double-click profile, để mở một tab mới mang tên profile. Không tự kết nối khi khởi động. Split trong tab remote vẫn tạo shell local độc lập; Quick Commands, Search, Copy All Text và Export Text tiếp tục áp dụng cho pane đang chọn.

- **SSH**: nhập host, port (mặc định 22), username, timeout và chọn xác thực bằng password hoặc key/agent. Password được nhập trong ô che ký tự; **Remember in keyring** là tùy chọn. Key/agent dùng OpenSSH và đường dẫn private key tùy chọn; passphrase của key nên được mở khóa trong `ssh-agent` trước. Lần đầu gặp host, DevTerm hiển thị câu hỏi và fingerprint do OpenSSH cung cấp; kiểm tra fingerprint rồi chọn **Trust Host Key**. Cancel dừng kết nối. Host key thay đổi bị OpenSSH từ chối; ứng dụng không tự bỏ qua kiểm tra hay xóa `known_hosts`. Password chỉ được gửi một lần cho mỗi lần Connect.
- **Telnet**: nhập host, port (mặc định 23), username nếu cần và bật **Automatic login** để dùng các prompt của profile. Telnet truyền dữ liệu và password không mã hóa trên mạng; chỉ dùng trong môi trường phù hợp. Client chạy ở chế độ 8 bit, tắt local escape và bỏ qua `.telnetrc` để tránh mất byte Unicode hoặc cấu hình trace ngoài ý muốn.
- **Serial**: chọn đường dẫn thiết bị, ví dụ `/dev/serial/by-id/...`, baud rate, data bits, parity, stop bits và flow control. Tài khoản cần quyền mở cổng, thường qua nhóm `dialout` trên Ubuntu. Có thể bật Automatic login và **Send initial Enter** khi thiết bị cần Enter để hiện login prompt. DevTerm dùng picocom, giữ cơ chế khóa cổng của client và không yêu cầu chạy ứng dụng bằng root.

Với Telnet/Serial, cấu hình regex riêng cho **Username prompt**, **Password prompt** và **Ready prompt**. Có thể để trống Username prompt nếu thiết bị chỉ hỏi password. Ví dụ username `(?i)(login|username)\s*:\s*$`, password `(?i)password\s*:\s*$`, ready `router#\s*$`. Mỗi biểu thức phải khớp phần cuối output; chọn Ready prompt đủ đặc trưng cho thiết bị để tránh nhận nhầm banner là đăng nhập thành công. Engine xử lý prompt bị chia thành nhiều gói và bỏ ANSI khi so khớp.

Trong lúc tự đăng nhập Telnet/Serial, ký tự gõ và Quick Commands được bỏ qua; **Esc/Ctrl+C** hủy. Output của giai đoạn xác thực được giữ ngoài VTE và loại bỏ sau đăng nhập, kể cả khi thiết bị echo lại password, để không lọt vào scrollback, copy hoặc export. Sau thông báo đăng nhập thành công có thể nhấn Enter để hiện lại prompt. Gặp prompt không khớp sẽ chờ đến timeout (5–120 giây); gặp lời nhắc xác thực lặp lại, bị từ chối hoặc mất kết nối thì dừng, không tự thử lại password. Khi tắt Automatic login, terminal tương tác thủ công bình thường.

**Disconnect Current Pane** trong Sessions dừng kết nối của pane remote đang chọn, giữ output để xem lại. **Reconnect Current Pane** dừng kết nối cũ rồi mở tab mới theo bản chụp profile của pane đó; có thể yêu cầu nhập lại password. Hủy hộp thoại password không khôi phục kết nối đã dừng. Đóng pane/tab hoặc ứng dụng sẽ kết thúc client tương ứng.

Profile được lưu atomic tại `$XDG_CONFIG_HOME/devterm/sessions.json`, fallback `~/.config/devterm/sessions.json`, với cấu trúc `{"version":1,"sessions":[...]}`. Mỗi profile có UUID ổn định, tên, loại giao thức (`0`: SSH, `1`: Telnet, `2`: Serial), địa chỉ/tham số cổng và các tùy chọn đăng nhập. File chỉ chứa metadata và cờ ghi nhớ; không chứa password. JSON lỗi hoặc version không hỗ trợ được báo rõ và khóa sửa profile, giữ nguyên file lỗi; terminal local vẫn dùng được.

Ví dụ một profile SSH password (các trường của giao thức khác vẫn được lưu để đổi giao thức trong editor):

```json
{
  "version": 1,
  "sessions": [{
    "id": "572bc287-c861-4323-b0bd-a6c9f1a93af8",
    "name": "Router Lab", "type": 0,
    "host": "192.0.2.1", "port": 22, "username": "admin", "identity": "",
    "device": "", "baud": 115200, "data_bits": 8, "stop_bits": 1,
    "parity": "none", "flow": "none", "timeout": 30,
    "password_auth": true, "remember": false,
    "auto_login": false, "send_initial_enter": false,
    "login_prompt": "(?i)(login|username)\\s*:\\s*$",
    "password_prompt": "(?i)password\\s*:\\s*$",
    "ready_prompt": "router#\\s*$"
  }]
}
```

Password ghi nhớ được lưu bằng libsecret/Secret Service, gắn với UUID và endpoint của profile. Đổi tên giữ nguyên liên kết; đổi endpoint không tái sử dụng password cũ; Duplicate tạo UUID mới và không sao chép credential. Delete yêu cầu xóa credential của profile. Nếu keyring bị khóa/không khả dụng, ứng dụng thông báo lỗi và cho phép nhập password một lần, không fallback sang file plaintext. Keyring bảo vệ dữ liệu lưu trên đĩa; không thay thế việc khóa phiên Linux khi rời máy, và ứng dụng khác trong cùng phiên người dùng đã mở khóa có thể truy cập Secret Service.

Password được truyền qua descriptor RAM và IPC riêng cho SSH askpass, không đặt trong command line, biến môi trường, file cấu hình hay file tạm trên đĩa. Luồng xử lý tránh đưa password vào thông báo lỗi; frontend và helper tắt core dump cho bộ nhớ của chính chúng. Quick Commands cũ không tự chuyển đổi: hãy xóa thủ công các nút đã chứa password. Nội dung được gõ/gửi thủ công sau đăng nhập vẫn tuân theo chế độ echo của chương trình/thiết bị đang chạy.

Giới hạn phiên bản này: SSH password dùng phương thức `password`, chưa hỗ trợ tự động OTP/MFA, `keyboard-interactive`, password cho nhiều jump host hoặc đổi password bắt buộc. Telnet/Serial tự động hỗ trợ username/password rồi ready prompt, chưa có chuỗi bước `enable`, nhiều password hoặc script tùy ý. Không khôi phục kết nối/tab đang chạy sau khi thoát. Các luồng đặc biệt vẫn có thể thực hiện thủ công trong terminal local.

## Cấu hình Quick Commands

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

`meson test` có tám nhóm kiểm tra (43 test khi có đủ dependency và display):

- `commands`: 4 test cho round-trip Unicode, dấu nháy, shell operators, thứ tự, `send_enter`, đường dẫn XDG, dữ liệu lỗi và lỗi ghi file vẫn giữ nguyên nội dung cũ.
- `layout`: 6 test tích hợp với shell/PTY thật cho bố cục độc lập, kế thừa thư mục có dấu cách, bỏ qua thông tin thư mục remote, fallback sau shell exit và giải phóng pane khi spawn chưa hoàn tất. Các test hình học dùng cửa sổ được hiển thị thật để đo chia đôi khi split phải/xuống liên tiếp, split khác hướng, chọn lại pane cũ, giữ divider đã chỉnh khi resize/đóng pane, split nhiều lần trước khi bố trí và đóng split đang chờ khi cửa sổ bị ẩn. Nhóm này tự bỏ qua nếu không có display; cần X11 hoặc Wayland để chạy đầy đủ.
- `search`: 4 test với VTE thật cho Unicode, ký tự regex được tìm nguyên văn, scrollback, Match case, Find whole words only (gồm chữ số, `_`, dấu Unicode dạng tổ hợp và kết hợp Match case), trước/sau/quay vòng, trạng thái độc lập giữa pane, search sau shell exit, pane bị ẩn rồi hiển thị lại và đóng pane khi search đang chờ. Nhóm này cũng tự bỏ qua nếu không có display.
- `contents`: 4 test với VTE thật cho clipboard khớp snapshot, Unicode, bỏ ANSI, lấy scrollback khi đang cuộn ở đầu, giữ vùng chọn/vị trí cuộn, loại nội dung đã vượt giới hạn buffer, dòng dài sau resize, buffer độc lập giữa pane, snapshot không đổi khi có output mới/đóng pane, màn hình thay thế và shell exit. Ca scrollbar kiểm tra vị trí bên phải, luôn hiện/kích thước ổn định, giữ vị trí khi có output, theo dõi output ở cuối, đồng bộ Search, độc lập giữa pane và cuộn sau shell exit. Cần display để chạy nhóm này.
- `export`: 5 test cho ghi UTF-8 và tên file Unicode, thay file atomic (kiểm tra qua hard link), quyền file, hủy trước khi lưu, lỗi lúc commit, thư mục không ghi được và từ chối đường dẫn không phải local. Nhóm này không cần GUI; test quyền ghi tự bỏ qua nếu chạy bằng root.
- `sessions`: 5 test cho round-trip Unicode/thứ tự, dữ liệu lỗi và giữ file cũ, liên kết credential theo endpoint, descriptor RAM đã seal và state machine prompt phân mảnh/ANSI/password-only/từ chối đăng nhập.
- `session-ui`: 3 test GTK cho thêm/nhân bản/xóa profile, nhập password, ghi nhớ từ hộp thoại Connect, lưu/lấy/xóa credential qua libsecret, hủy request khi đóng ứng dụng và giữ file cấu hình lỗi. Test chạy D-Bus và GNOME Keyring riêng với thư mục tạm, không dùng keyring thật của người dùng. Cần display và `gnome-keyring-daemon`.
- `connections`: 12 test với client thật và server SSH/Telnet loopback hoặc cổng Serial giả lập bằng PTY: tự đăng nhập, tương tác, resize, initial Enter, lỗi cổng, echo password không lọt vào output, sai password không retry, prompt lạ/timeout/hủy, host key thay đổi, hủy/chấp nhận host mới và từ chối tiến trình lạ đọc password qua IPC. Cần OpenSSH, Telnet, picocom; SSH server thử nghiệm cần Python `asyncssh`; thao tác hộp thoại host key cần X11 và `xdotool`. Các phần thiếu dependency được báo skip.

Để chạy đầy đủ test kết nối, có thể tạo virtualenv thử nghiệm rồi thêm site-packages vào `PYTHONPATH` cho Python chạy Meson:

```sh
sudo apt install python3-venv xdotool xvfb
python3 -m venv /tmp/devterm-test-venv
/tmp/devterm-test-venv/bin/pip install asyncssh
PYTHONPATH="$(/tmp/devterm-test-venv/bin/python -c 'import sysconfig; print(sysconfig.get_path("purelib"))')" xvfb-run -a meson test -C build --print-errorlogs
```

Ở phiên bản Sessions trước, build và đủ 42 test trong tám nhóm đã qua trên X11/Xvfb, không có test bị skip. Dependency khi đó được giải nén tạm từ gói Ubuntu 24.04 vào `/tmp`. Kiểm tra giao diện trên X11/Xvfb đã xác nhận: nút New Terminal và phím tắt tạo tab riêng; tab mới kế thừa thư mục; split nhiều cấp; chuyển tab khôi phục đúng pane; Quick Commands chỉ gửi vào pane đang chọn trong tab hiện tại; Cancel giữ nguyên shell; Close Tab kết thúc các shell của tab đó; đóng pane cuối đưa về trạng thái trống; tạo lại tab ở home; đóng tab đã exit không hỏi xác nhận; đóng cửa sổ bằng yêu cầu chuẩn `WM_DELETE_WINDOW` thoát bình thường. Các kiểm tra cấu hình và Quick Commands của phiên bản trước cũng đã xác nhận thêm/sửa/xóa/đổi thứ tự, `send_enter=false`, shell exit và giữ nguyên file JSON lỗi.

Phiên bản scrollbar đã build bằng dependency cài trực tiếp trên hệ thống và qua đủ 14 test liên quan thuộc `layout`, `search`, `contents`, không có skip. Kiểm tra bổ sung trên X11/Xvfb bằng chuột/phím thật với chương trình foreground đọc PTY raw xác nhận: kéo scrollbar chọn đúng pane và giữ focus terminal; kéo không gửi input vào child; gõ phím trở về cuối; Quick Commands chỉ gửi vào pane vừa kéo; đóng pane trong lúc kéo không gây lỗi. Chưa kiểm tra thao tác scrollbar trên Wayland.

Lỗi split liên tiếp ép pane về mép đã được tái hiện bằng test kích thước trước khi sửa. Sau khi sửa, cả 14 test đều qua trên X11/Xvfb; thao tác trên ứng dụng bằng nút toolbar và kéo chuột thật đã xác nhận split phải hai lần, kéo divider ngoài rồi split xuống hai lần vẫn giữ vị trí divider đã kéo. Các test hình học cũng kiểm tra resize, đóng pane và mở rộng nhánh split còn lại.

Kiểm tra giao diện search trên X11/Xvfb đã xác nhận phím mở search, query riêng giữa pane/tab, active pane cập nhật khi click vào ô search, Quick Commands vẫn gửi đúng pane và đóng pane an toàn khi search còn đang chờ. Một chương trình foreground đọc PTY ở raw mode xác nhận nội dung query, Enter, Shift+Enter và Esc không lọt vào child; sau khi đóng search, ký tự gõ tiếp đi vào đúng terminal.

Copy/xuất nội dung đã được kiểm tra trên X11/Xvfb với clipboard và hộp thoại Save thực tế: đúng pane, UTF-8, giữ bản chụp trong khi output mới tiếp tục xuất hiện, lưu file và xác nhận ghi đè. Chương trình foreground đọc PTY ở raw mode xác nhận Copy All Text, mở/hủy Save không gửi input vào child và focus trở lại terminal sau khi hủy. Đo trích xuất khoảng 811 KB tại giới hạn scrollback mất khoảng 1,4 ms trên môi trường kiểm thử; đây không phải cam kết thời gian cho mọi kích thước buffer. Với hộp thoại Save modal, GTK có thể chặn yêu cầu đóng cửa sổ cha; nhánh đóng cửa sổ thực sự cũng đã được kiểm tra dưới debugger để xác nhận hủy hộp thoại và thoát bình thường.

Checklist kiểm tra thủ công trên hệ thống cài dependency bình thường:

1. Mở ứng dụng, nhập lệnh shell, chạy `vim`, `top` và xác nhận màu ANSI, scrollback, copy/paste.
2. Split cả hai hướng nhiều lần: pane đang chọn được chia đôi, các pane bên ngoài giữ kích thước. Kéo divider sang tỷ lệ khác rồi split, resize cửa sổ và đóng pane; xác nhận tỷ lệ được giữ. Thử click nhanh liên tiếp và split sau khi đổi tab.
3. Thêm, sửa, xóa, đổi vị trí nút; mở lại ứng dụng để xác nhận thứ tự, ID và nội dung được giữ.
4. Chọn lần lượt hai pane rồi bấm cùng nút; chỉ pane được chọn nhận input.
5. Kiểm tra `send_enter=false`, dấu nháy, pipe, Unicode và `cd` trong shell hiện tại.
6. SSH vào một máy thử và bấm nút ở remote prompt; xác nhận lệnh chạy trên remote.
7. Thử `exit`, file cấu hình lỗi, thư mục cấu hình không ghi được và lệnh lâu như `ping`; giao diện vẫn phản hồi.
8. Tạo nhiều tab bằng nút và phím tắt, split khác nhau ở từng tab, đổi tab bằng click và phím tắt; xác nhận bố cục, thư mục, output và active pane được giữ riêng.
9. Đóng tab bằng nút × và phím tắt, thử cả Cancel và Close Tab; đóng pane cuối và tab cuối, rồi tạo lại. Kiểm tra các shell ở tab khác vẫn chạy.
10. Mở Search bằng nút và phím tắt trong các pane khác nhau; tìm Unicode, ký tự đặc biệt và nội dung trong scrollback; thử Match case, Find whole words only, Enter, Shift+Enter, Esc và đổi tab khi đang nhập query. Xác nhận query và các tùy chọn của từng pane được giữ riêng và Quick Commands vẫn gửi đúng pane khi ô search lấy focus.
11. Dùng Copy All Text và Export Text ở các pane/tab khác nhau, cả khi shell đã exit; kiểm tra UTF-8, scrollback, dòng dài và nội dung chỉ thuộc pane đang chọn. Thử Cancel, ghi đè, lỗi quyền ghi, output tiếp tục chạy trong lúc mở Save và đóng cửa sổ khi Save còn mở.
12. Thêm profile SSH/Telnet/Serial cho thiết bị thử nghiệm; kiểm tra prompt thực tế, username/password, timeout và initial Enter. Lưu password vào keyring, khởi động lại rồi Connect; sửa endpoint và kiểm tra không dùng lại password cũ. Thử sai password, keyring bị khóa, host key mới/thay đổi, cổng Serial bận/mất quyền và tháo thiết bị. Dùng Disconnect/Reconnect rồi đóng tab để xác nhận client dừng và output còn giữ.
13. Tạo output dài ở nhiều pane, kéo scrollbar của pane chưa được chọn và bấm Quick Command; xác nhận đúng pane nhận input. Kéo lên khi output vẫn chạy, gõ phím để về cuối, thử kéo về cuối để tiếp tục theo dõi. Đổi tab, mở Search, split/resize/đóng pane và shell exit; xác nhận scrollbar luôn nằm bên phải, không phủ chữ và mỗi pane cuộn độc lập.

Kiểm tra Sessions trên ứng dụng thật với X11/Xvfb và Telnet loopback đã xác nhận Connect mở tab mới, password không xuất hiện trong buffer Copy All Text, input remote hoạt động, split tạo shell local riêng và đóng pane remote dừng kết nối. Keyring và SSH dùng dịch vụ/server thử nghiệm riêng; Serial chỉ dùng PTY giả lập. Chưa kiểm tra router/server production, thiết bị Serial vật lý hoặc tương tác trên Wayland; chưa chạy `vim`, `top`, `ping` trong phiên kiểm thử tự động. Lỗi GDK từng ghi nhận khi kiểm thử đóng cửa sổ xuất phát từ việc hủy trực tiếp X surface bằng `xdotool windowclose`; kiểm tra lại bằng yêu cầu đóng chuẩn của window manager đã qua.
