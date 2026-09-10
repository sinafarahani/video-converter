// Every string that reaches the user's log pane, in one place.
//
// These are byte-for-byte the strings the Java version emitted, including the
// missing space in "عملیات برای فایل<path>" -- reproducing them exactly means
// old and new builds produce comparable logs. Fixing the spacing is a one-line
// change here whenever that is wanted.
//
// The engine calls these; it never embeds a literal.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace conv::msg {

// -- validation ------------------------------------------------------------
inline constexpr std::string_view kInputDirEmpty   = "فیلد پوشه ورودی نمیتواند خالی باشد";
inline constexpr std::string_view kInputDirMissing = "پوشه ورودی پیدا نشد.";
inline constexpr std::string_view kInvalidSize     = "حداکثر اندازه خروجی نامعتبر است.";
inline constexpr std::string_view kOutputDirEmpty  = "فیلد پوشه خروجی نمیتواند خالی باشد";
inline constexpr std::string_view kNoFilesFound    = "هیچ فایلی در پوشه ورودی پیدا نشد.";

// -- lifecycle -------------------------------------------------------------
inline constexpr std::string_view kStarting  = "در حال شروع عملیات لطفا صبر کنید...";
inline constexpr std::string_view kFinished  = "عملیات به پایان رسید.";
inline constexpr std::string_view kCancelled = "عملیات لغو شد.";

// -- per file --------------------------------------------------------------
std::string file_started(std::string_view path);                   // عملیات برای فایل<p> شروع شد.
std::string file_failed(std::string_view path);                    // عملیات برای فایل<p> ناموفق بود.
std::string file_failed_code(std::string_view path, int code);     // ... کد: <n>
std::string file_saved(std::string_view path);                     // فایل: <p>با موفقیت ذخیره شد.

// -- file operation errors -------------------------------------------------
std::string temp_delete_failed(std::string_view path);   // خطا در حذف فایل موقت: <p>
std::string replace_failed(std::string_view path);       // خطا در هنگام جایگزین کردن فایل: <p>
std::string original_delete_failed(std::string_view path);  // خطا در حذف فایل اصلی: <p>
std::string rename_failed(std::string_view path);        // خطا در تغییر نام فایل موقت به فایل اصلی: <p>

// -- new in this version ---------------------------------------------------
// These have no Java counterpart; they report the behaviour added in the
// rewrite. Flagged here so it is obvious what is new.
std::string already_small_enough(std::string_view path, std::uint64_t size, std::uint64_t target);
std::string ffmpeg_missing();
std::string using_encoder(std::string_view device, std::string_view encoder, double speedup);
std::string audio_selected(int kept, int total);
std::string audio_stereo_pair(int left, int right);

// Why the software encoder was chosen. Each is a complete sentence.
std::string sw_no_gpu();                                              // nothing usable found
std::string sw_forced();                                              // the hidden override
std::string sw_encoder_unavailable(std::string_view encoder);         // ffmpeg lacks it
std::string sw_gpu_encoder_failed(std::string_view device,
                                  std::string_view encoder);          // present but will not run
std::string sw_gpu_too_slow(std::string_view device, double speedup, double required);

// Watchdog: ffmpeg has been silent for a while but is still running.
std::string stall_warning(int minutes);
// Heartbeat for the UI log during a long encode.
std::string still_working(std::string_view phase, double percent, double fps, double speed);

// Phase labels for the progress line.
std::string_view phase_label(std::string_view phase);

}  // namespace conv::msg
