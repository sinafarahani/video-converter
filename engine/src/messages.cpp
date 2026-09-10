#include "conv/messages.hpp"

#include <fmt/format.h>

#include "conv/util.hpp"

namespace conv::msg {

// The odd spacing in these three ("فایل<path>", "<path>با") is inherited from
// the Java original and reproduced on purpose.
std::string file_started(std::string_view path) {
    return fmt::format("عملیات برای فایل{} شروع شد.", path);
}

std::string file_failed(std::string_view path) {
    return fmt::format("عملیات برای فایل{} ناموفق بود.", path);
}

std::string file_failed_code(std::string_view path, int code) {
    return fmt::format("عملیات برای فایل{} ناموفق بود.کد: {}", path, code);
}

std::string file_saved(std::string_view path) {
    return fmt::format("فایل: {}با موفقیت ذخیره شد.", path);
}

std::string temp_delete_failed(std::string_view path) {
    return fmt::format("خطا در حذف فایل موقت: {}", path);
}

std::string replace_failed(std::string_view path) {
    return fmt::format("خطا در هنگام جایگزین کردن فایل: {}", path);
}

std::string original_delete_failed(std::string_view path) {
    return fmt::format("خطا در حذف فایل اصلی: {}", path);
}

std::string rename_failed(std::string_view path) {
    return fmt::format("خطا در تغییر نام فایل موقت به فایل اصلی: {}", path);
}

// --- new in this version ---------------------------------------------------

std::string already_small_enough(std::string_view path, std::uint64_t size, std::uint64_t target) {
    return fmt::format("فایل {} از قبل کوچکتر از اندازه هدف است ({} < {})، بدون فشرده‌سازی منتقل شد.",
                       path, format_bytes(size), format_bytes(target));
}

std::string ffmpeg_missing() {
    return "ffmpeg پیدا نشد. لطفا برنامه را دوباره نصب کنید.";
}

std::string using_encoder(std::string_view device, std::string_view encoder, double speedup) {
    return fmt::format("پردازنده گرافیکی {} استفاده می‌شود ({}، {:.1f} برابر سریع‌تر).",
                       device, encoder, speedup);
}

std::string audio_selected(int kept, int total) {
    return fmt::format("{} مسیر صوتی از {} مسیر دارای صدا تشخیص داده شد.", kept, total);
}

std::string audio_stereo_pair(int left, int right) {
    return fmt::format("مسیرهای صوتی {} و {} به عنوان یک جفت استریو (چپ/راست) ترکیب شدند.",
                       left + 1, right + 1);
}

// --- why the CPU is doing the work --------------------------------------------

std::string sw_no_gpu() {
    return "پردازنده گرافیکی مناسبی پیدا نشد؛ پردازش با پردازنده مرکزی انجام می‌شود.";
}

std::string sw_forced() {
    return "پردازش با پردازنده مرکزی انجام می‌شود (تنظیم دستی).";
}

std::string sw_encoder_unavailable(std::string_view encoder) {
    return fmt::format("رمزگذار {} در این نسخه ffmpeg موجود نیست؛ پردازش با پردازنده مرکزی انجام می‌شود.",
                       encoder);
}

std::string sw_gpu_encoder_failed(std::string_view device, std::string_view encoder) {
    return fmt::format(
        "پردازنده گرافیکی {} پیدا شد ولی رمزگذار {} اجرا نشد؛ احتمالاً درایور کارت گرافیک قدیمی است "
        "و باید به‌روزرسانی شود. جزئیات خطا در فایل لاگ ثبت شد. پردازش با پردازنده مرکزی انجام می‌شود.",
        device, encoder);
}

std::string sw_gpu_too_slow(std::string_view device, double speedup, double required) {
    return fmt::format(
        "پردازنده گرافیکی {} فقط {:.1f} برابر سریع‌تر از پردازنده مرکزی بود (حداقل {:.0f} برابر لازم است)؛ "
        "پردازش با پردازنده مرکزی انجام می‌شود.",
        device, speedup, required);
}

// --- liveness ---------------------------------------------------------------------

std::string stall_warning(int minutes) {
    return fmt::format(
        "هشدار: ffmpeg به مدت {} دقیقه پیشرفتی گزارش نکرده است، ولی فرایند هنوز در حال اجراست. "
        "اگر این پیام تکرار شد، احتمالاً مشکلی در خواندن فایل ورودی یا نوشتن خروجی وجود دارد.",
        minutes);
}

std::string still_working(std::string_view phase, double percent, double fps, double speed) {
    return fmt::format("در حال پردازش ({}): {:.1f}٪ — {:.1f} فریم بر ثانیه — سرعت {:.3f}x",
                       phase_label(phase), percent, fps, speed);
}

std::string_view phase_label(std::string_view phase) {
    if (phase == "analyze") return "تحلیل صدا";
    if (phase == "pass1")   return "مرحله ۱ از ۲";
    if (phase == "pass2")   return "مرحله ۲ از ۲";
    if (phase == "encode")  return "رمزگذاری";
    if (phase == "remux")   return "انتقال بدون فشرده‌سازی";
    if (phase == "audio")   return "تبدیل صدا";
    if (phase == "probe")   return "بررسی فایل";
    return phase;
}

}  // namespace conv::msg
