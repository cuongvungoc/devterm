# Tài liệu DevTerm

Bộ tài liệu mô tả yêu cầu và thiết kế của **DevTerm 0.3.0**, đối chiếu source tại commit `8612de6` ngày **13/11/2026**.

| Tài liệu | Mục đích | Người đọc chính |
| --- | --- | --- |
| [SRS — Software Requirements Specification](SRS.md) | Phạm vi sản phẩm, yêu cầu chức năng/phi chức năng, quy tắc nghiệp vụ và tiêu chí nghiệm thu | Product owner, BA, developer, QA |
| [SDD — Software Design Description](SDD.md) | Kiến trúc, module, dữ liệu, luồng xử lý, vòng đời tiến trình và thiết kế kiểm thử | Developer, reviewer, QA, người bảo trì |

SRS trả lời **DevTerm phải làm gì**; SDD giải thích **source hiện tại thực hiện các yêu cầu đó như thế nào**. Mã yêu cầu `FR-*`, `NFR-*` trong SRS được ánh xạ sang module và nhóm test ở cuối SDD. Hai tài liệu ghi nhận các tính năng đã phát triển, bao gồm những mở rộng được chủ dự án chấp thuận sau MVP ban đầu trong `AGENTS.md`.

Thông tin vận hành liên quan:

- [Kiểm thử Sequence](SEQUENCES-TESTING.md): các ca mới và kết quả hồi quy cho chuỗi lệnh từng hàng.
- [README ứng dụng](../README.md): cài dependency, build, chạy và sử dụng.
- [Đóng gói `.deb`](../packaging/README.md): build bằng Docker, dependency và runtime riêng.
- [Linux Mint](../packaging/MINT.md): biến thể Mint 22.1 Xia amd64.
- [Báo cáo kiểm thử đóng gói](../packaging/TESTING.md): bằng chứng của các artifact được nêu trong báo cáo; không tự áp dụng cho bản source mới.
- [Quy chuẩn giao diện](../design-system/devterm/MASTER.md): màu sắc và thành phần giao diện.

Khi thay đổi hành vi, cập nhật yêu cầu trong SRS, thiết kế tương ứng trong SDD và ma trận truy vết. Việc tạo bộ tài liệu này chỉ đối chiếu source và tài liệu hiện có; không phải một lần build, chạy toàn bộ test hay nghiệm thu lại gói cài đặt.
