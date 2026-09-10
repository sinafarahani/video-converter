#include "conv/job.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "conv/gpu.hpp"
#include "conv/messages.hpp"
#include "conv/util.hpp"

namespace conv {
namespace {

namespace fs    = std::filesystem;
using     clock = std::chrono::steady_clock;

// Every output audio stream is encoded at this rate. The original hardcoded
// 128 kbps for a single AAC track; keeping the same figure means output quality
// is unchanged for the common one-track case. A stereo pair joined into one
// stream counts as one stream here, so it does not cost twice.
constexpr std::uint64_t kAudioBitsPerSecond = 128'000;

// MP4 muxing overhead is roughly half a percent. Reserving 2% keeps the result
// under the requested size rather than a hair over it, which is the direction
// that matters when a user asks for "at most 1 GB".
constexpr double kContainerOverheadFraction = 0.02;

// Below this the picture is unwatchable and there is no point pretending the
// target is reachable.
constexpr std::uint64_t kMinimumVideoBitrate = 100'000;

#ifdef _WIN32
constexpr const char* kNullSink = "NUL";
#else
constexpr const char* kNullSink = "/dev/null";
#endif

// Liveness thresholds for a long encode.
constexpr auto kHeartbeatLog   = std::chrono::minutes(10);  // run log line
constexpr auto kHeartbeatUi    = std::chrono::minutes(30);  // UI log line
constexpr auto kStallLog       = std::chrono::minutes(5);   // first warning in the run log
constexpr auto kStallUi        = std::chrono::minutes(15);  // warning in the UI
constexpr auto kStallUiRepeat  = std::chrono::minutes(30);

// A short random hex token, used to give temp files and two-pass stats files a
// name that cannot collide between concurrent runs.
std::string random_token() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return fmt::format("{:012x}", rng() & 0xFFFFFFFFFFFFull);
}

// x264 and x265 leave several files behind for a two-pass run: the stats file,
// an mbtree (x264) or cutree (x265) companion, and .temp variants if the
// process is killed mid-write. The Java version passed no -passlogfile at all,
// so all of this landed in the working directory and stayed there.
//
// `prefix` is built from an ASCII token, so appending these suffixes as narrow
// strings is safe -- no user-supplied filename ever reaches this path.
void remove_pass_logs(const fs::path& prefix) {
    std::error_code ec;
    for (const char* suffix : {"-0.log", "-0.log.mbtree", "-0.log.cutree",
                               "-0.log.temp", "-0.log.mbtree.temp", "-0.log.cutree.temp",
                               ".log", ".log.mbtree", ".log.cutree"}) {
        fs::path p = prefix;
        p += suffix;
        fs::remove(p, ec);
    }
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

std::uint64_t file_size_or_zero(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0ull : n;
}

// The command line as it will be run, for the log. Quoting makes the output
// copy-pasteable into a shell, which is what someone debugging a stuck encode
// from the log actually needs.
std::string command_for_log(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(' ');
        const bool plain = args[i].find_first_of(" \t\"'") == std::string::npos && !args[i].empty();
        if (plain) {
            out += args[i];
        } else {
            out.push_back('"');
            for (const char c : args[i]) {
                if (c == '"') out += "\\\"";
                else          out.push_back(c);
            }
            out.push_back('"');
        }
    }
    return out;
}

std::string human_duration(double seconds) {
    if (seconds < 0) return "?";
    const auto s = static_cast<long long>(seconds);
    if (s < 60)   return fmt::format("{}s", s);
    if (s < 3600) return fmt::format("{}m{:02d}s", s / 60, s % 60);
    return fmt::format("{}h{:02d}m", s / 3600, (s % 3600) / 60);
}

// ---------------------------------------------------------------------------
// Temp-file journal
// ---------------------------------------------------------------------------
//
// Every temp output this engine creates is recorded here before ffmpeg starts
// writing it, and removed once it has been renamed into place or deleted. If
// the application dies mid-encode -- killed from Task Manager after appearing
// hung, a crash, a power cut -- the next start sweeps whatever is still listed.
// Without this, a multi-gigabyte half-written file sits in the user's video
// folder indefinitely, which is exactly what happened the first time the app
// was terminated during a stalled encode.
//
// Only paths we created are ever listed, and they all carry the _tmp<token>
// marker, so the sweep cannot touch a user's own file.
class TempJournal {
public:
    explicit TempJournal(fs::path dir) : file_(std::move(dir) / "pending-temp.json") {}

    void add(const fs::path& p) {
        std::lock_guard lock(mutex_);
        auto list = load();
        const std::string s = path_to_utf8(p);
        if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
        save(list);
    }

    void remove(const fs::path& p) {
        std::lock_guard lock(mutex_);
        auto list = load();
        const std::string s = path_to_utf8(p);
        list.erase(std::remove(list.begin(), list.end(), s), list.end());
        save(list);
    }

    // Deletes every listed file that still exists. Returns what it removed.
    std::vector<std::string> sweep() {
        std::lock_guard lock(mutex_);
        std::vector<std::string> removed;
        for (const auto& s : load()) {
            const fs::path p = path_from_utf8(s);
            // Belt and braces: only our own naming pattern.
            if (s.find("_tmp") == std::string::npos) continue;
            std::error_code ec;
            if (fs::exists(p, ec) && fs::remove(p, ec)) removed.push_back(s);
        }
        save({});
        return removed;
    }

private:
    std::vector<std::string> load() const {
        std::ifstream in(file_);
        if (!in) return {};
        try {
            nlohmann::json j;
            in >> j;
            if (j.is_array()) return j.get<std::vector<std::string>>();
        } catch (...) {
        }
        return {};
    }

    void save(const std::vector<std::string>& list) const {
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
        std::ofstream out(file_, std::ios::trunc);
        if (out) out << nlohmann::json(list).dump();
    }

    fs::path           file_;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// Input planning helpers
// ---------------------------------------------------------------------------

// Absolute, lexically normal, native separators, no trailing separator -- so
// "D:\Day1\" typed by hand and "D:\Day1" from the picker compare and name
// alike. Idempotent, and it never touches the disk beyond the current folder.
fs::path normalize_input(const fs::path& p) {
    if (p.empty()) return {};
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    a = a.lexically_normal();
    a.make_preferred();
    if (!a.has_filename() && a.has_relative_path()) a = a.parent_path();
    return a;
}

// Resolves links, 8.3 short names, subst drives and on-disk casing, so two
// spellings of one file yield one key. Used only for keys, never shown.
fs::path canonical_or_self(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return (ec || c.empty()) ? p : c;
}

// path_key with exactly one trailing separator, for "is under this folder"
// prefix tests that must not match D:\Day10 when asking about D:\Day1.
std::string dir_key(const fs::path& dir) {
    std::string k = path_key(dir);
    const char sep = static_cast<char>(fs::path::preferred_separator);
    if (k.empty() || k.back() != sep) k.push_back(sep);
    return k;
}

// The subfolder a folder input gets in the output when there are several
// inputs. A drive or file-system root has no name of its own.
std::string folder_label(const fs::path& folder) {
    std::string name = path_to_utf8(folder.filename());
    if (!name.empty()) return name;
#ifdef _WIN32
    for (const char c : path_to_utf8(folder.root_name())) {
        if (std::isalnum(static_cast<unsigned char>(c))) name.push_back(c);  // "D:" -> "D"
    }
#endif
    return name.empty() ? std::string("root") : name;
}

// `file`'s place under `base`, keeping its structure relative to `root`.
// lexically_relative, unlike fs::relative, never touches the disk and so
// cannot be led outside `base` by a junction; anything that does not sit
// under `root` falls back to its bare file name.
fs::path target_under(const fs::path& base, const fs::path& root, const fs::path& file) {
    fs::path rel = file.lexically_relative(root);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == ".." || rel == ".") rel = file.filename();
    return base / rel;
}

}  // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::vector<fs::path> collect_files(const fs::path& dir, std::vector<fs::path>* unreadable) {
    std::vector<fs::path> out;

    // An explicit stack of folders rather than recursive_directory_iterator:
    // that one's increment() fails for good at the first folder it cannot
    // open -- a broken junction, a path too long, no permission -- and every
    // file after it in the walk was silently lost.
    std::vector<fs::path> pending{dir};
    while (!pending.empty()) {
        const fs::path current = std::move(pending.back());
        pending.pop_back();

        std::error_code ec;
        fs::directory_iterator it(current, ec);
        const fs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code sec;
            // Links to folders (symlinks, junctions) are not followed, as
            // before: they can loop, or lead outside the folder picked.
            if (it->symlink_status(sec).type() == fs::file_type::directory) {
                pending.push_back(it->path());
            } else if (it->is_regular_file(sec)) {
                out.push_back(it->path());
            }
        }
        // What the listing gave before failing is kept.
        if (ec && unreadable) unreadable->push_back(current);
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path output_target(const Settings& s, const fs::path& root, const fs::path& file) {
    if (s.output_mode == OutputMode::Replace) return file;
    return target_under(s.output_dir, root, file);
}

fs::path build_output_path(const Settings& s,
                           const fs::path& input_root,
                           const fs::path& input_file) {
    const fs::path target = output_target(s, input_root, input_file);
    if (s.output_mode == OutputMode::CopyTo && target.has_parent_path()) {
        std::error_code mec;
        fs::create_directories(target.parent_path(), mec);
    }
    return target;
}

InputPlan plan_inputs(const Settings& s) {
    InputPlan plan;
    const bool copy = s.output_mode == OutputMode::CopyTo;

    // The output folder, canonical, for the "inside an input folder" test and
    // for comparing planned outputs with planned inputs without a stat each.
    const fs::path out        = copy ? normalize_input(s.output_dir) : fs::path{};
    const fs::path canon_out  = copy ? canonical_or_self(out) : fs::path{};
    const std::string out_dir = copy ? dir_key(canon_out) : std::string{};

    // A file found inside a folder input, keyed once.
    struct Child {
        fs::path    path;
        fs::path    canon;         // derived lexically from the folder's canonical path
        std::string key;
        bool        media = false; // audio/video by extension, and not an engine temp file
        bool        video = false;
    };
    struct Folder {
        fs::path           path;   // as given (normalised)
        std::string        key;    // canonical
        std::string        dir;    // dir_key of the canonical path
        std::vector<Child> children;
    };
    struct Loose {
        fs::path    path;
        fs::path    canon;
        std::string key;
        bool        video = false;
    };
    // A usable input, in input order: a folder (index into `folders`) or a
    // loose file.
    struct Entry {
        int   folder = -1;
        Loose loose;
    };
    struct Item {
        PlannedFile f;
        fs::path    canon;       // canonical input, the base of its keys
        int         folder = -1; // index into `folders`; -1 for a loose file
    };
    std::vector<Folder> folders;                        // distinct folder inputs, in input order
    std::vector<Entry>  entries;
    std::vector<Item>   items;
    std::unordered_map<std::string, size_t> item_by_key;  // canonical input key -> index in items
    std::unordered_map<std::string, bool>   top_level;    // distinct usable inputs -> is a folder
    std::unordered_set<std::string>         seen_other;   // folder files already counted as ignored/excluded
    std::unordered_set<std::string>         reported;     // missing / not-media paths already listed

    // "is strictly inside" for two dir_key()s.
    const auto strictly_inside = [](const std::string& inner, const std::string& outer) {
        return inner != outer && inner.starts_with(outer);
    };

    // 1. What each input is. A folder is walked once, however often it is
    //    given; a second mention only counts its files as duplicates below.
    for (const fs::path& raw : s.inputs) {
        if (raw.empty()) continue;
        const fs::path p = normalize_input(raw);

        std::error_code ec;
        const fs::file_status st = fs::status(p, ec);
        if (!fs::exists(st)) {
            if (reported.insert(path_key(p)).second) plan.missing.push_back(p);
            continue;
        }
        ++plan.found_inputs;

        if (fs::is_directory(st)) {
            const fs::path    canon = canonical_or_self(p);
            const std::string key   = path_key(canon);
            top_level.emplace(key, true);

            int folder = -1;
            for (size_t i = 0; i < folders.size(); ++i) {
                if (folders[i].key == key) folder = static_cast<int>(i);
            }
            if (folder < 0) {
                folder = static_cast<int>(folders.size());
                Folder f{p, key, dir_key(canon), {}};
                for (fs::path& child : collect_files(p, &plan.unreadable_folders)) {
                    // Keyed off the folder's canonical path lexically: one
                    // canonicalisation per input, not one per file.
                    Child c;
                    c.canon = canon / child.lexically_relative(p);
                    c.key   = path_key(c.canon);
                    const std::string ext = lower_extension(child);
                    c.video = is_video_extension(ext);
                    c.media = (c.video || is_audio_extension(ext)) && !is_temp_sibling(child);
                    c.path  = std::move(child);
                    f.children.push_back(std::move(c));
                }
                folders.push_back(std::move(f));
            }
            entries.push_back({folder, {}});
            continue;
        }

        if (fs::is_regular_file(st)) {
            const std::string ext   = lower_extension(p);
            const bool        video = is_video_extension(ext);
            // An engine temp file named directly is no more an input than one
            // found in a folder.
            if ((video || is_audio_extension(ext)) && !is_temp_sibling(p)) {
                Loose l;
                l.path  = p;
                l.video = video;
                // A link is keyed by where it sits, as the same link reached
                // through a folder walk is; weakly_canonical would key it by its
                // target, and put its output checks in the target's folder.
                std::error_code lec;
                l.canon = fs::is_symlink(fs::symlink_status(p, lec))
                              ? canonical_or_self(p.parent_path()) / p.filename()
                              : canonical_or_self(p);
                l.key = path_key(l.canon);
                top_level.emplace(l.key, false);
                entries.push_back({-1, std::move(l)});
                continue;
            }
        }
        if (reported.insert(path_key(p)).second) plan.not_media.push_back(p);
    }

    // 2. Layout, CopyTo only. One folder on its own keeps the original layout
    //    (its contents straight into the output). Otherwise every folder gets a
    //    subfolder named after it -- "Day1 (2)" for a second folder of the same
    //    name -- and loose files go straight into the output. Missing and
    //    non-media inputs contribute nothing, so they do not change the layout.
    //    A folder with no media of its own -- none at all, or only files an
    //    earlier folder already has (a folder picked inside another) -- takes
    //    no name, so it cannot push a real one to "Day1 (2)".
    const bool single_folder = top_level.size() == 1 && top_level.begin()->second;
    const bool subfolders    = copy && !single_folder;

    std::vector<std::string>        labels(folders.size());  // empty: no subfolder
    std::unordered_set<std::string> used_labels;
    const auto assign_label = [&](size_t i) {
        const std::string name  = folder_label(folders[i].path);
        std::string       label = name;
        for (int n = 2; !used_labels.insert(path_key(path_from_utf8(label))).second; ++n) {
            label = fmt::format("{} ({})", name, n);
        }
        labels[i] = std::move(label);
    };
    if (subfolders) {
        std::unordered_set<std::string> claimed;
        for (size_t i = 0; i < folders.size(); ++i) {
            const bool skip_out = strictly_inside(out_dir, folders[i].dir);
            bool       owns     = false;
            for (const Child& c : folders[i].children) {
                if (!c.media || (skip_out && c.key.starts_with(out_dir))) continue;
                if (claimed.insert(c.key).second) owns = true;
            }
            if (owns) assign_label(i);
        }
    }

    // Where each folder's outputs go. With the output folder equal to an input
    // folder (or above one), out/<name>/ lies inside that input, and a second
    // run would read the first run's results from it and write them again one
    // level deeper, run after run. Never read from these either.
    std::vector<std::string> zones;
    for (size_t i = 0; i < folders.size(); ++i) {
        if (!labels[i].empty()) zones.push_back(dir_key(canon_out / path_from_utf8(labels[i])));
    }

    const auto add = [&](const fs::path& input, const fs::path& canon, std::string key, bool video,
                         int folder) {
        const auto [it, inserted] = item_by_key.try_emplace(std::move(key), items.size());
        if (!inserted) {
            ++plan.duplicates;
            // A file picked on its own that also sits in a picked folder keeps
            // its place in the order but takes the folder's layout.
            Item& first = items[it->second];
            if (first.folder < 0 && folder >= 0) {
                first.folder  = folder;
                first.f.input = input;
                first.f.root  = folders[static_cast<size_t>(folder)].path;
            }
            return;
        }
        Item item;
        item.f.input = input;
        item.f.root  = folder >= 0 ? folders[static_cast<size_t>(folder)].path : input.parent_path();
        item.f.video = video;
        item.canon   = canon;
        item.folder  = folder;
        items.push_back(std::move(item));
    };

    // 3. The files, in input order, each once.
    for (const Entry& e : entries) {
        if (e.folder < 0) {
            add(e.loose.path, e.loose.canon, e.loose.key, e.loose.video, -1);
            continue;
        }
        const Folder& f = folders[static_cast<size_t>(e.folder)];

        // Edge case: an output folder strictly inside this input folder must
        // never be read from, or a second run would convert the first run's
        // results -- nor may any per-folder subfolder of the output that lies
        // inside it (`zones`). An output folder equal to this one is left to
        // the collision rule below, which keeps outputs off inputs.
        std::vector<const std::string*> skip;
        if (copy) {
            if (strictly_inside(out_dir, f.dir)) skip.push_back(&out_dir);
            for (const std::string& z : zones) {
                if (strictly_inside(z, f.dir)) skip.push_back(&z);
            }
        }

        for (const Child& c : f.children) {
            const bool excluded = std::any_of(skip.begin(), skip.end(), [&](const std::string* z) {
                return c.key.starts_with(*z);
            });
            if (excluded) {
                if (seen_other.insert(c.key).second) ++plan.excluded_output_subtree;
                continue;
            }
            if (!c.media) {
                if (seen_other.insert(c.key).second) ++plan.ignored_in_folders;
                continue;
            }
            add(c.path, c.canon, c.key, c.video, e.folder);
        }
    }

    // A folder whose files all looked like an earlier folder's -- until that
    // folder's copies were skipped as output above -- has files after all,
    // and still gets a name.
    std::vector<fs::path> folder_base(folders.size(), out);
    if (subfolders) {
        for (const Item& item : items) {
            if (item.folder >= 0 && labels[static_cast<size_t>(item.folder)].empty()) {
                assign_label(static_cast<size_t>(item.folder));
            }
        }
        for (size_t i = 0; i < folders.size(); ++i) {
            if (!labels[i].empty()) folder_base[i] = out / path_from_utf8(labels[i]);
        }
    }

    // 4. Output names, decided in plan order. A name is taken when an earlier
    // file already got it, when it is an input of this run (an output never
    // overwrites an input; covers the output folder being an input folder --
    // and in Copy mode that includes the file's own input, since writing there
    // would replace the original that the mode promises to keep), or -- in
    // Replace mode -- when some unrelated file already sits there: converting
    // clip.avi must not delete the user's own clip.mp4. In CopyTo mode such an
    // unrelated file is left to existing_policy.
    std::unordered_set<std::string>      taken_outputs;
    std::unordered_map<std::string, int> wanted;  // output key before any rename -> files wanting it
    std::vector<std::string>             wanted_key(items.size());
    plan.files.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        Item& item = items[i];

        fs::path target = !copy             ? item.f.input
                          : item.folder >= 0 ? target_under(folder_base[static_cast<size_t>(item.folder)],
                                                            item.f.root, item.f.input)
                                             : out / item.f.input.filename();
        target = with_extension(target, item.f.video ? ".mp4" : ".mp3");

        // Keys for candidates are derived from an already canonical base, so
        // they compare with the input keys without touching the disk.
        const auto key_of = [&](const fs::path& candidate) {
            return copy ? path_key(canon_out / candidate.lexically_relative(out))
                        : path_key(item.canon.parent_path() / candidate.filename());
        };
        const auto taken = [&](const fs::path& candidate) {
            const std::string key = key_of(candidate);
            if (taken_outputs.count(key)) return true;
            if (const auto in = item_by_key.find(key);
                in != item_by_key.end() && (copy || in->second != i)) {
                return true;
            }
            if (!copy) {
                std::error_code ec;
                if (fs::exists(candidate, ec) && !same_path(candidate, item.f.input)) return true;
            }
            return false;
        };

        wanted_key[i] = key_of(target);
        ++wanted[wanted_key[i]];

        const std::string stem = path_to_utf8(target.stem());
        const std::string ext  = path_to_utf8(target.extension());
        fs::path candidate = target;
        for (int n = 2; taken(candidate); ++n) {
            candidate = target.parent_path() / path_from_utf8(fmt::format("{} ({}){}", stem, n, ext));
            item.f.renamed = true;
        }
        taken_outputs.insert(key_of(candidate));
        item.f.output = std::move(candidate);
        plan.files.push_back(std::move(item.f));
    }
    for (size_t i = 0; i < plan.files.size(); ++i) {
        plan.files[i].name_clash = wanted[wanted_key[i]] > 1;
    }
    return plan;
}

BitrateBudget compute_budget(std::uint64_t target_bytes,
                             double duration_seconds,
                             int audio_track_count) {
    BitrateBudget b;
    b.audio_track_count = std::max(0, audio_track_count);
    b.audio_bps_each    = kAudioBitsPerSecond;

    if (duration_seconds <= 0.0 || target_bytes == 0) return b;

    const double usable = static_cast<double>(target_bytes) * (1.0 - kContainerOverheadFraction);
    const double total_bps = usable * 8.0 / duration_seconds;
    const double audio_bps =
        static_cast<double>(kAudioBitsPerSecond) * static_cast<double>(b.audio_track_count);

    const double video_bps = total_bps - audio_bps;
    if (video_bps < static_cast<double>(kMinimumVideoBitrate)) {
        // Even at the floor the audio alone overruns the target. The original
        // clamped video to 2000 bps here and carried on, which produced a file
        // several times the requested size while reporting success.
        b.video_bps = kMinimumVideoBitrate;
        b.feasible  = false;
        return b;
    }

    b.video_bps = static_cast<std::uint64_t>(video_bps);
    b.feasible  = true;
    return b;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct JobRunner::Impl {
    ToolPaths     tools;
    RunLog&       run_log;
    fs::path      cache_dir;
    TempJournal   journal;

    Settings      settings;
    JobCallbacks  cb;
    EncoderChoice encoder;

    std::atomic_bool cancel{false};
    std::atomic_bool running{false};
    std::thread      worker;

    // The process currently encoding, so cancel() can stop it, plus the temp
    // file it is writing so cancel() can remove it.
    std::mutex   active_mutex;
    Process*     active_process = nullptr;
    fs::path     active_temp;

    int file_count = 0;
    int file_index = 0;

    // Per-file progress state, for the richer Progress report.
    Progress          progress;
    clock::time_point file_started{};

    Impl(ToolPaths t, RunLog& l, fs::path cd)
        : tools(std::move(t)), run_log(l), cache_dir(std::move(cd)), journal(cache_dir) {}

    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }

    void ui(std::string_view text) {
        if (cb.log) cb.log(text);
    }
    void file_log(std::string_view text) { run_log.write(text); }

    // -- progress ---------------------------------------------------------------

    void begin_file(const fs::path& current, double duration) {
        progress              = Progress{};
        progress.file_index   = file_index;
        progress.file_count   = file_count;
        progress.current_file = path_to_utf8(current);
        progress.duration     = duration;
        file_started          = clock::now();
    }

    void set_phase(std::string phase, int pass, int pass_count) {
        progress.phase      = std::move(phase);
        progress.pass       = pass;
        progress.pass_count = pass_count;
        progress.fps        = 0.0;
        progress.speed      = 0.0;
        progress.out_time   = 0.0;
        progress.eta_seconds = -1.0;
        emit_progress(progress.file_fraction);
    }

    void emit_progress(double within_file) {
        if (!cb.progress || file_count <= 0) return;
        progress.file_fraction = std::clamp(within_file, 0.0, 1.0);
        progress.fraction = std::clamp(
            (file_index + progress.file_fraction) / static_cast<double>(file_count), 0.0, 1.0);
        progress.elapsed_seconds = static_cast<double>(
            std::chrono::duration_cast<std::chrono::seconds>(clock::now() - file_started).count());
        cb.progress(progress);
    }

    // Runs one ffmpeg invocation, forwarding progress. `base` and `span` place
    // this invocation inside the current file's slice of the progress bar.
    //
    // Also the liveness layer: a heartbeat in the log while things are moving,
    // and an escalating warning when ffmpeg stops reporting while still alive.
    // The first field deployment of this app sat for days on a CPU encode that
    // showed 0% the whole time; whether or not it was actually stuck was
    // unknowable from the outside. It should never be unknowable again.
    std::optional<int> run_ffmpeg(std::vector<std::string> args,
                                  double base,
                                  double span,
                                  int remaining_passes_after_this) {
        Process proc;
        {
            std::lock_guard lock(active_mutex);
            active_process = &proc;
        }

        file_log(fmt::format("exec: {}", command_for_log(args)));
        const auto started = clock::now();

        auto last_output    = started;   // anything on stdout
        auto last_heartbeat = started;
        auto last_ui_beat   = started;
        auto last_stall_ui  = clock::time_point{};
        bool stall_logged   = false;

        const double duration = progress.duration;

        const auto on_stdout = [&](std::string_view line) {
            last_output = clock::now();
            if (stall_logged) {
                file_log("ffmpeg is reporting progress again");
                stall_logged = false;
            }

            const auto p = parse_progress_line(line);
            switch (p.kind) {
                case ProgressLine::Kind::Fps:
                    if (p.value >= 0) progress.fps = p.value;
                    break;
                case ProgressLine::Kind::Speed:
                    if (p.value >= 0) progress.speed = p.value;
                    break;
                case ProgressLine::Kind::OutTime:
                    if (p.value >= 0) progress.out_time = p.value;
                    break;
                case ProgressLine::Kind::End:
                    // The block ends here; update the bar and the ETA once
                    // per block rather than per field.
                    break;
                default:
                    return;
            }
            if (p.kind != ProgressLine::Kind::End && p.kind != ProgressLine::Kind::OutTime) return;
            if (duration <= 0.0) return;

            const double frac = std::clamp(progress.out_time / duration, 0.0, 1.0);
            if (progress.speed > 0.0) {
                const double remaining_here = (duration - progress.out_time) / progress.speed;
                const double remaining_more = remaining_passes_after_this * (duration / progress.speed);
                progress.eta_seconds = remaining_here + remaining_more;
            }
            emit_progress(base + span * frac);

            const auto now = clock::now();
            if (now - last_heartbeat >= kHeartbeatLog) {
                last_heartbeat = now;
                file_log(fmt::format("  ... {} {:.1f}% fps={:.2f} speed={:.4f}x out_time={} eta={}",
                                     progress.phase, progress.file_fraction * 100.0, progress.fps,
                                     progress.speed, human_duration(progress.out_time),
                                     human_duration(progress.eta_seconds)));
            }
            if (now - last_ui_beat >= kHeartbeatUi) {
                last_ui_beat = now;
                ui(msg::still_working(progress.phase, progress.file_fraction * 100.0, progress.fps,
                                      progress.speed));
            }
        };

        const auto on_stderr = [&](std::string_view line) {
            if (!line.empty()) file_log(fmt::format("[ffmpeg] {}", line));
        };

        // Called ~every 150 ms while ffmpeg is alive and silent.
        const auto on_idle = [&]() {
            const auto now    = clock::now();
            const auto silent = now - last_output;

            if (!stall_logged && silent >= kStallLog) {
                stall_logged = true;
                file_log(fmt::format(
                    "WARNING: no progress output from ffmpeg for {} minutes; process still running "
                    "(phase {}, last out_time {})",
                    std::chrono::duration_cast<std::chrono::minutes>(silent).count(), progress.phase,
                    human_duration(progress.out_time)));
            }
            if (silent >= kStallUi) {
                const bool due = (last_stall_ui == clock::time_point{}) ||
                                 (now - last_stall_ui >= kStallUiRepeat);
                if (due) {
                    last_stall_ui = now;
                    ui(msg::stall_warning(
                        static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(silent).count())));
                }
            }
        };

        const auto code = proc.run(args, on_stdout, on_stderr, &cancel, on_idle);

        {
            std::lock_guard lock(active_mutex);
            active_process = nullptr;
        }

        const auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(clock::now() - started).count();
        if (code) {
            file_log(fmt::format("exit {} after {}", *code, human_duration(static_cast<double>(secs))));
        } else {
            const char* why = "unknown";
            switch (proc.outcome()) {
                case Process::Outcome::NotStarted: why = "could not start"; break;
                case Process::Outcome::Cancelled:  why = "cancelled by user"; break;
                case Process::Outcome::Stopped:    why = "stopped"; break;
                case Process::Outcome::Failed:     why = "I/O failure"; break;
                default: break;
            }
            file_log(fmt::format("ffmpeg did not complete: {} after {}{}", why,
                                 human_duration(static_cast<double>(secs)),
                                 proc.error().empty() ? "" : " -- " + proc.error()));
        }
        return code;
    }

    void set_temp(const fs::path& p) {
        {
            std::lock_guard lock(active_mutex);
            active_temp = p;
        }
        journal.add(p);
    }
    void clear_temp() {
        fs::path p;
        {
            std::lock_guard lock(active_mutex);
            p = active_temp;
            active_temp.clear();
        }
        if (!p.empty()) journal.remove(p);
    }

    // Output must exist, be non-empty, and be readable as media. Same check the
    // original made, but via the probe we already have.
    bool output_valid(const fs::path& p) {
        if (!file_exists(p) || file_size_or_zero(p) == 0) return false;
        const auto info = probe_media(tools, p);
        return info.valid;
    }

    // Moves `temp` into place, honouring Replace vs CopyTo, and reports.
    //
    // This is where the original's most damaging bug lived: in Replace mode it
    // compared the input path against an output path whose extension had
    // already been rewritten to .mp4, decided they were "different files", and
    // so wrote clip.mp4 while leaving the original clip.avi sitting next to it.
    // Only inputs already named .mp4 were genuinely replaced.
    // -- the disk's last word on output names ----------------------------------
    //
    // plan_inputs() decides names from path keys. Where the keys fold two names
    // differently from the file system -- a non-ASCII letter's case on macOS,
    // Unicode normalisation, a hard link -- two "different" names are one file,
    // and replacing it would delete an input or a result of this very run.
    // File identities catch that.
    std::vector<fs::path>              run_inputs;  // this run's inputs, set by run()
    std::optional<std::vector<FileId>> input_ids;   // their identities, read on first need
    std::vector<FileId>                produced;    // every output this run has written

    // True when `p` is a file this run wrote, or one of its inputs.
    bool belongs_to_run(const fs::path& p) {
        const auto id = file_id(p);
        if (!id) return false;
        if (std::find(produced.begin(), produced.end(), *id) != produced.end()) return true;
        if (!input_ids) {
            input_ids.emplace();
            input_ids->reserve(run_inputs.size());
            for (const auto& in : run_inputs) {
                if (const auto iid = file_id(in)) input_ids->push_back(*iid);
            }
        }
        return std::find(input_ids->begin(), input_ids->end(), *id) != input_ids->end();
    }

    // The first "<stem> (n)<ext>" next to `p` that nothing occupies.
    static fs::path free_sibling(const fs::path& p) {
        const std::string stem = path_to_utf8(p.stem());
        const std::string ext  = path_to_utf8(p.extension());
        fs::path candidate = p;
        for (int n = 2; n < 100000; ++n) {
            candidate = p.parent_path() / path_from_utf8(fmt::format("{} ({}){}", stem, n, ext));
            std::error_code ec;
            if (!fs::exists(candidate, ec) && !ec) break;
        }
        return candidate;
    }

    bool finalize(const fs::path& input, fs::path output, const fs::path& temp) {
        std::error_code ec;
        const bool replace_mode = settings.output_mode == OutputMode::Replace;
        bool       in_place     = same_path(input, output);

        // Behind the plan: in Copy mode the original is never the file that
        // gets replaced; an output never replaces an input or an earlier
        // result of this run; and in Replace mode it never replaces anything
        // -- the plan saw that name free, so whatever sits there now is new.
        // Such an output takes the next free " (n)" name instead.
        const bool collides = in_place ? !replace_mode
                                       : file_exists(output) && (replace_mode || belongs_to_run(output));
        if (collides) {
            output   = free_sibling(output);
            in_place = false;
            ui(msg::output_renamed(path_to_utf8(input), path_to_utf8(output)));
            file_log(fmt::format("Output name taken on disk, saving as: {}", path_to_utf8(output)));
        }

        if (!in_place) {
            if (file_exists(output)) {
                fs::remove(output, ec);
                if (ec || file_exists(output)) {
                    ui(msg::replace_failed(path_to_utf8(output)));
                    file_log(fmt::format("Failed to replace file: {}", path_to_utf8(output)));
                    return false;
                }
            }
            fs::rename(temp, output, ec);
            if (ec) {
                // Falls back to copy+delete when temp and output are on
                // different volumes, which rename cannot cross.
                std::error_code cec;
                fs::copy_file(temp, output, fs::copy_options::overwrite_existing, cec);
                if (cec) {
                    ui(msg::rename_failed(path_to_utf8(output)));
                    file_log(fmt::format("Failed to rename into place: {}", path_to_utf8(output)));
                    return false;
                }
                fs::remove(temp, cec);
            }

            // The fix: in Replace mode the source file is meant to be gone.
            if (settings.output_mode == OutputMode::Replace && file_exists(input)) {
                std::error_code rec;
                fs::remove(input, rec);
                if (rec) {
                    file_log(fmt::format("Converted but could not remove original: {}",
                                         path_to_utf8(input)));
                }
            }
        } else {
            if (file_exists(input)) {
                fs::remove(input, ec);
                if (ec || file_exists(input)) {
                    ui(msg::original_delete_failed(path_to_utf8(input)));
                    file_log(fmt::format("Failed to delete file: {}", path_to_utf8(input)));
                    return false;
                }
            }
            fs::rename(temp, input, ec);
            if (ec) {
                ui(msg::rename_failed(path_to_utf8(input)));
                file_log(fmt::format("Failed to rename file: {}", path_to_utf8(input)));
                return false;
            }
        }

        if (const auto id = file_id(output)) produced.push_back(*id);

        const auto shown = path_to_utf8(output);
        ui(msg::file_saved(shown));
        file_log(fmt::format("SUCCESS: {}", shown));
        return true;
    }

    void discard_temp(const fs::path& temp) {
        if (!file_exists(temp)) return;
        std::error_code ec;
        fs::remove(temp, ec);
        if (ec) {
            ui(msg::temp_delete_failed(path_to_utf8(temp)));
            file_log(fmt::format("Failed to delete temp file: {}", path_to_utf8(temp)));
        }
    }

    // Logs the audio decision and reports the stereo pairs to the user.
    void report_audio(const MediaInfo& info) {
        if (info.audio_streams.size() <= 1) return;

        const int kept = static_cast<int>(std::count_if(
            info.audio_streams.begin(), info.audio_streams.end(),
            [](const AudioStreamInfo& a) { return a.selected; }));
        ui(msg::audio_selected(kept, static_cast<int>(info.audio_streams.size())));

        for (const auto& a : info.audio_streams) {
            std::string note;
            if (a.is_pair_leader())        note = fmt::format(" (L of pair with {})", a.stereo_partner);
            else if (a.is_pair_follower()) note = fmt::format(" (R of pair with {})", a.stereo_partner);
            file_log(fmt::format("  audio {}: {} ch {}, mean {:.1f} dB, peak {:.1f} dB -> {}{}",
                                 a.audio_index, a.channels, a.codec_name, a.mean_db, a.peak_db,
                                 a.selected ? "KEEP" : "drop", note));
        }
        for (const auto& a : info.audio_streams) {
            if (a.is_pair_leader()) ui(msg::audio_stereo_pair(a.audio_index, a.stereo_partner));
        }
    }

    // -- the converters ----------------------------------------------------------

    FileResult convert_video(const fs::path& input, fs::path output);
    FileResult convert_audio(const fs::path& input, fs::path output);
    FileResult remux_video(const fs::path& input, fs::path output, const MediaInfo& info);

    void run();
};

// ---------------------------------------------------------------------------

FileResult JobRunner::Impl::remux_video(const fs::path& input, fs::path output,
                                        const MediaInfo& info) {
    output = with_extension(output, ".mp4");
    const fs::path temp = make_temp_sibling(output, ".mp4");
    set_temp(temp);
    set_phase("remux", 0, 0);

    std::vector<std::string> args{path_to_utf8(tools.ffmpeg), "-nostdin", "-y", "-v", "error",
                                  "-i", path_to_utf8(input)};

    if (info.has_video) {
        args.push_back("-map");
        args.push_back("0:v:0");
    }
    // Stream copy cannot join two mono tracks into one stereo stream, so a
    // detected pair is carried as two mono streams here. It is a remux: the
    // whole point is not touching the data.
    for (const auto& a : info.audio_streams) {
        if (!a.selected) continue;
        args.push_back("-map");
        args.push_back(fmt::format("0:a:{}", a.audio_index));
    }

    args.insert(args.end(), {"-c", "copy"});
    args.push_back(path_to_utf8(temp));
    args.insert(args.end(), {"-progress", "pipe:1", "-nostats", "-hide_banner"});

    const auto code = run_ffmpeg(args, 0.0, 1.0, 0);
    if (!code) {
        discard_temp(temp);
        clear_temp();
        return FileResult::Cancelled;
    }
    if (*code != 0 || !output_valid(temp)) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Remux failed: {}", path_to_utf8(input)));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }

    const bool ok = finalize(input, output, temp);
    clear_temp();
    return ok ? FileResult::Remuxed : FileResult::Failed;
}

FileResult JobRunner::Impl::convert_video(const fs::path& input, fs::path output) {
    ui(msg::file_started(path_to_utf8(input)));
    begin_file(input, 0.0);
    set_phase("probe", 0, 0);

    MediaInfo info = probe_media(tools, input);
    if (!info.valid) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Could not read media: {}", path_to_utf8(input)));
        return FileResult::Failed;
    }
    progress.duration = info.duration_seconds;
    file_log(fmt::format("{}: {:.1f}s, {}x{} {}, {} audio stream(s), {}", path_to_utf8(input),
                         info.duration_seconds, info.width, info.height, info.video_codec_name,
                         info.audio_streams.size(), format_bytes(file_size_or_zero(input))));

    // Which microphone was actually live.
    if (!info.audio_streams.empty()) {
        set_phase("analyze", 0, 0);
        const auto t0 = clock::now();
        analyze_audio(tools, input, info, AudioDetectionConfig{}, &cancel);
        if (cancelled()) return FileResult::Cancelled;
        file_log(fmt::format("audio analysis took {}",
                             human_duration(static_cast<double>(
                                 std::chrono::duration_cast<std::chrono::seconds>(clock::now() - t0)
                                     .count()))));
        report_audio(info);
    }

    const int output_audio = count_output_audio_streams(info);

    // Already small enough? Re-encoding would only make it bigger.
    if (settings.size_mode == SizeMode::Manual) {
        const std::uint64_t current = file_size_or_zero(input);
        if (current > 0 && current <= settings.max_size_bytes) {
            ui(msg::already_small_enough(path_to_utf8(input), current, settings.max_size_bytes));
            file_log(fmt::format("Already within target, remuxing: {}", path_to_utf8(input)));
            return remux_video(input, std::move(output), info);
        }
    }

    output = with_extension(output, ".mp4");
    const fs::path temp = make_temp_sibling(output, ".mp4");
    set_temp(temp);

    const bool two_pass = uses_two_pass(encoder) && settings.size_mode == SizeMode::Manual;

    // Two-pass stats go next to the temp output rather than into %TEMP%.
    // x265's cutree for a two-hour 1080p source runs to gigabytes; the output
    // drive is the one place guaranteed to have room for that, whereas a small
    // system drive fills, x265 fails to write, and the encode dies a day in.
    // ASCII token only, so no Persian ever reaches the narrow-string suffixes.
    const fs::path pass_log =
        temp.parent_path() / path_from_utf8(fmt::format("converter-stats-{}", random_token()));

    BitrateBudget budget;
    if (settings.size_mode == SizeMode::Manual) {
        budget = compute_budget(settings.max_size_bytes, info.duration_seconds, output_audio);
        file_log(fmt::format("budget: target {}, video {} kbps, {} audio stream(s) at {} kbps{}",
                             format_bytes(settings.max_size_bytes), budget.video_bps / 1000,
                             output_audio, budget.audio_bps_each / 1000,
                             budget.feasible ? "" : " -- TARGET UNREACHABLE, encoding at minimum"));
    }

    // Assembles one ffmpeg command line for the given pass.
    const auto build = [&](int pass, const fs::path& dest) {
        std::vector<std::string> a{path_to_utf8(tools.ffmpeg), "-nostdin", "-y", "-v", "error",
                                   "-i", path_to_utf8(input)};

        const bool want_audio = (pass != 1) && output_audio > 0;

        // Stereo pairs are joined with a filter; everything else maps directly.
        std::string filter;
        std::vector<std::string> audio_maps;
        if (want_audio) {
            int pair_no = 0;
            for (const auto& s : info.audio_streams) {
                if (!s.selected || s.is_pair_follower()) continue;
                if (s.is_pair_leader()) {
                    const std::string label = fmt::format("[pair{}]", pair_no++);
                    filter += fmt::format("[0:a:{}][0:a:{}]join=inputs=2:channel_layout=stereo{};",
                                          s.audio_index, s.stereo_partner, label);
                    audio_maps.push_back(label);
                } else {
                    audio_maps.push_back(fmt::format("0:a:{}", s.audio_index));
                }
            }
            if (!filter.empty()) filter.pop_back();  // trailing ';'
        }

        if (!filter.empty()) {
            a.push_back("-filter_complex");
            a.push_back(filter);
        }

        a.push_back("-map");
        a.push_back("0:v:0");
        for (const auto& m : audio_maps) {
            a.push_back("-map");
            a.push_back(m);
        }

        a.push_back("-c:v");
        a.push_back(encoder.ffmpeg_encoder);

        const std::string preset = preset_arg(encoder, settings.speed);
        if (!preset.empty()) {
            a.push_back("-preset");
            a.push_back(preset);
        }

        if (settings.size_mode == SizeMode::Manual) {
            const auto br = bitrate_args(encoder, budget.video_bps);
            a.insert(a.end(), br.begin(), br.end());
        } else {
            const auto q = quality_args(encoder, kOutputCodec, settings.level);
            a.insert(a.end(), q.begin(), q.end());
        }

        const auto compat = compatibility_args(encoder, kOutputCodec);
        a.insert(a.end(), compat.begin(), compat.end());

        if (two_pass) {
            a.push_back("-pass");
            a.push_back(std::to_string(pass));
            a.push_back("-passlogfile");
            a.push_back(path_to_utf8(pass_log));
        }

        if (pass == 1) {
            a.push_back("-an");
            a.push_back("-f");
            a.push_back("null");
            a.push_back(kNullSink);
        } else {
            if (want_audio) {
                a.push_back("-c:a");
                a.push_back("aac");
                a.push_back("-b:a");
                a.push_back(std::to_string(budget.audio_bps_each ? budget.audio_bps_each
                                                                 : kAudioBitsPerSecond));
            } else {
                a.push_back("-an");
            }
            a.push_back(path_to_utf8(dest));
        }

        a.push_back("-progress");
        a.push_back("pipe:1");
        a.push_back("-nostats");
        a.push_back("-hide_banner");
        return a;
    };

    // Pass 1 fills the first half of this file's slice, pass 2 the second --
    // matching the original's progress behaviour. A single-pass hardware encode
    // fills the whole slice.
    if (two_pass) {
        set_phase("pass1", 1, 2);
        const auto code = run_ffmpeg(build(1, {}), 0.0, 0.5, 1);
        if (!code) {
            discard_temp(temp);
            remove_pass_logs(pass_log);
            clear_temp();
            return FileResult::Cancelled;
        }
        if (*code != 0) {
            ui(msg::file_failed_code(path_to_utf8(input), *code));
            file_log(fmt::format("Error processing file: {} (pass 1, exit {})",
                                 path_to_utf8(input), *code));
            remove_pass_logs(pass_log);
            clear_temp();
            return FileResult::Failed;
        }
    }

    const double base = two_pass ? 0.5 : 0.0;
    const double span = two_pass ? 0.5 : 1.0;
    set_phase(two_pass ? "pass2" : "encode", two_pass ? 2 : 0, two_pass ? 2 : 0);
    const auto code = run_ffmpeg(build(2, temp), base, span, 0);

    remove_pass_logs(pass_log);

    if (!code) {
        discard_temp(temp);
        clear_temp();
        return FileResult::Cancelled;
    }
    if (*code != 0) {
        ui(msg::file_failed_code(path_to_utf8(input), *code));
        file_log(fmt::format("Error processing file: {} (exit {})", path_to_utf8(input), *code));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }
    if (!output_valid(temp)) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Output failed validation: {}", path_to_utf8(input)));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }

    // Report how close we landed; the only way to know the rate control is
    // behaving on real footage rather than synthetic tests.
    if (settings.size_mode == SizeMode::Manual) {
        const auto actual = file_size_or_zero(temp);
        const double delta = settings.max_size_bytes
                                 ? (static_cast<double>(actual) -
                                    static_cast<double>(settings.max_size_bytes)) *
                                       100.0 / static_cast<double>(settings.max_size_bytes)
                                 : 0.0;
        file_log(fmt::format("Size: {} (target {}, {:+.1f}%)", format_bytes(actual),
                             format_bytes(settings.max_size_bytes), delta));
    }

    const bool ok = finalize(input, output, temp);
    clear_temp();
    return ok ? FileResult::Converted : FileResult::Failed;
}

FileResult JobRunner::Impl::convert_audio(const fs::path& input, fs::path output) {
    ui(msg::file_started(path_to_utf8(input)));
    begin_file(input, 0.0);
    set_phase("probe", 0, 0);

    const MediaInfo info = probe_media(tools, input);
    if (!info.valid || info.audio_streams.empty()) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Could not read audio: {}", path_to_utf8(input)));
        return FileResult::Failed;
    }
    progress.duration = info.duration_seconds;

    output = with_extension(output, ".mp3");
    const fs::path temp = make_temp_sibling(output, ".mp3");
    set_temp(temp);
    set_phase("audio", 0, 0);

    const bool already_mp3 = info.audio_streams.front().codec_name == "mp3";

    std::vector<std::string> args{path_to_utf8(tools.ffmpeg), "-nostdin", "-y", "-v", "error",
                                  "-i", path_to_utf8(input), "-vn"};
    if (already_mp3) {
        args.insert(args.end(), {"-c:a", "copy"});
    } else {
        args.insert(args.end(), {"-c:a", "libmp3lame", "-q:a", "0"});
    }
    args.push_back(path_to_utf8(temp));
    args.insert(args.end(), {"-progress", "pipe:1", "-nostats", "-hide_banner"});

    const auto code = run_ffmpeg(args, 0.0, 1.0, 0);
    if (!code) {
        discard_temp(temp);
        clear_temp();
        return FileResult::Cancelled;
    }
    if (*code != 0) {
        ui(msg::file_failed_code(path_to_utf8(input), *code));
        file_log(fmt::format("Error processing file: {} (exit {})", path_to_utf8(input), *code));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }
    if (!output_valid(temp)) {
        ui(msg::file_failed(path_to_utf8(input)));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }

    const bool ok = finalize(input, output, temp);
    clear_temp();
    return ok ? FileResult::Converted : FileResult::Failed;
}

// ---------------------------------------------------------------------------

void JobRunner::Impl::run() {
    RunSummary summary;
    ui(msg::kStarting);

    produced.clear();
    input_ids.reset();
    run_inputs.clear();

    for (const auto& in : settings.inputs) file_log(fmt::format("Input: {}", path_to_utf8(in)));

    // Existence checks and folder walks happen here, on the worker thread, and
    // before the encoder is chosen: no benchmark when there is nothing to do.
    const InputPlan plan = plan_inputs(settings);
    run_inputs.reserve(plan.files.size());
    for (const auto& f : plan.files) run_inputs.push_back(f.input);

    // A folder the walk could not list would otherwise vanish from the run
    // without a word.
    if (!plan.unreadable_folders.empty()) {
        ui(msg::folders_unreadable(static_cast<int>(plan.unreadable_folders.size())));
        for (const auto& d : plan.unreadable_folders) {
            file_log(fmt::format("Folder could not be read, its files are not processed: {}",
                                 path_to_utf8(d)));
        }
    }

    // Files that are neither audio nor video stay exactly where they are and
    // are not copied into the output tree. Inside folders that is silent, as
    // it always was; a file the user named directly gets a line saying so.
    for (const auto& p : plan.missing) {
        ui(msg::input_missing(path_to_utf8(p)));
        file_log(fmt::format("Input not found: {}", path_to_utf8(p)));
    }
    for (const auto& p : plan.not_media) {
        ui(msg::input_not_media(path_to_utf8(p)));
        file_log(fmt::format("Not audio/video, left alone: {}", path_to_utf8(p)));
    }
    summary.missing           = static_cast<int>(plan.missing.size());
    summary.skipped_not_media = static_cast<int>(plan.not_media.size());
    if (plan.duplicates) {
        file_log(fmt::format("{} file(s) reached more than once, processed once", plan.duplicates));
    }
    if (plan.ignored_in_folders) {
        file_log(fmt::format("{} non-media file(s) in the input folders left alone",
                             plan.ignored_in_folders));
    }
    if (plan.excluded_output_subtree) {
        file_log(fmt::format("{} file(s) inside the output folder not read as input",
                             plan.excluded_output_subtree));
    }

    if (plan.files.empty()) {
        // Counted per input, not per distinct path: `missing` lists a path
        // given twice only once.
        const bool all_missing = !plan.missing.empty() && plan.found_inputs == 0;
        ui(all_missing ? msg::kInputsMissing : msg::kNoMediaFound);
        file_log(all_missing ? "No input exists." : "No audio or video files in the inputs.");
        running = false;
        if (cb.finished) cb.finished(summary);
        return;
    }

    // Pick the encoder once. The benchmark is cached, so this is instant after
    // the first run on a given machine.
    encoder = choose_encoder(tools, cache_dir, 2.0, &cancel, settings.force_software_encoder);
    ui(encoder.reason);
    file_log(fmt::format("Encoder: {} ({}) -- {}", encoder.ffmpeg_encoder, encoder.device_name,
                         encoder.detail));

    // Media files only, so "i / N" counts what is actually converted.
    file_count = static_cast<int>(plan.files.size());
    file_log(fmt::format("{} media file(s) from {} input(s)", file_count, settings.inputs.size()));

    for (int i = 0; i < file_count; ++i) {
        if (cancelled()) {
            summary.cancelled = true;
            break;
        }
        file_index = i;

        const PlannedFile& f = plan.files[static_cast<size_t>(i)];

        // A file this run wrote or reads is not "existing" in the policy's
        // sense (names the plan's keys told apart that are one file on
        // disk): finalize() gives such an output a free name instead.
        if (settings.output_mode == OutputMode::CopyTo &&
            settings.existing_policy == ExistingFilePolicy::Skip && file_exists(f.output) &&
            !belongs_to_run(f.output)) {
            if (f.name_clash) {
                // Several inputs share this output name, and the order decides
                // who gets which; the existing file may be another input's.
                ui(msg::skipped_name_clash(path_to_utf8(f.input), path_to_utf8(f.output)));
                file_log(fmt::format("Skipped, output exists and several inputs share its name: {} -> {}",
                                     path_to_utf8(f.input), path_to_utf8(f.output)));
            } else {
                file_log(fmt::format("Skipped existing file: {}", path_to_utf8(f.output)));
            }
            ++summary.skipped_exists;
            begin_file(f.input, 0.0);
            emit_progress(1.0);
            continue;
        }

        // Created only now, so a cancelled run leaves no empty folders behind
        // for the files it never reached.
        if (settings.output_mode == OutputMode::CopyTo && f.output.has_parent_path()) {
            std::error_code mec;
            fs::create_directories(f.output.parent_path(), mec);
        }
        if (f.renamed) {
            ui(msg::output_renamed(path_to_utf8(f.input), path_to_utf8(f.output)));
            file_log(fmt::format("Output name taken, saving as: {}", path_to_utf8(f.output)));
        }

        const FileResult r = f.video ? convert_video(f.input, f.output)
                                     : convert_audio(f.input, f.output);
        switch (r) {
            case FileResult::Converted:     ++summary.converted; break;
            case FileResult::Remuxed:       ++summary.remuxed; ++summary.skipped_small; break;
            case FileResult::SkippedExists: ++summary.skipped_exists; break;
            case FileResult::Failed:        ++summary.failed; break;
            case FileResult::Cancelled:     summary.cancelled = true; break;
            default: break;
        }
        if (r == FileResult::Cancelled) break;

        emit_progress(1.0);
    }

    if (summary.cancelled) {
        ui(msg::kCancelled);
        file_log("Process cancelled by user.");
    } else {
        ui(msg::kFinished);
        file_log("All tasks completed.");
    }
    file_log(fmt::format("Summary: {} converted, {} remuxed, {} skipped, {} failed, "
                         "{} not media, {} missing",
                         summary.converted, summary.remuxed, summary.skipped_exists,
                         summary.failed, summary.skipped_not_media, summary.missing));

    running = false;
    if (cb.finished) cb.finished(summary);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

JobRunner::JobRunner(ToolPaths tools, RunLog& run_log, fs::path cache_dir)
    : impl_(std::make_unique<Impl>(std::move(tools), run_log, std::move(cache_dir))) {
    // Anything a previous, abruptly terminated run left behind.
    for (const auto& removed : impl_->journal.sweep()) {
        impl_->run_log.write("Removed orphaned temp file from a previous run: " + removed);
    }
}

JobRunner::~JobRunner() {
    cancel();
    join();
}

bool JobRunner::running() const {
    return impl_->running.load(std::memory_order_relaxed);
}

bool JobRunner::start(Settings settings, JobCallbacks callbacks) {
    if (running()) return false;
    join();  // reap a previous finished thread

    impl_->settings = std::move(settings);
    impl_->cb       = std::move(callbacks);
    // Paths arrive with whatever separators the caller used; normalising here
    // keeps every later path -- and every log line -- consistent. Inputs are
    // also made absolute. Whether they exist is left to the worker thread:
    // this runs on the UI thread, and walking a large folder here would freeze
    // the window.
    auto& inputs = impl_->settings.inputs;
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
                                [](const fs::path& p) { return p.empty(); }),
                 inputs.end());
    for (auto& p : inputs) p = normalize_input(p);
    impl_->settings.output_dir = impl_->settings.output_dir.lexically_normal().make_preferred();
    impl_->cancel.store(false, std::memory_order_relaxed);

    // Validation, in the same order as the original.
    if (inputs.empty()) {
        impl_->ui(msg::kInputsEmpty);
        return false;
    }
    if (!impl_->tools.valid()) {
        impl_->ui(msg::ffmpeg_missing());
        return false;
    }
    if (impl_->settings.size_mode == SizeMode::Manual && impl_->settings.max_size_bytes < 1) {
        impl_->ui(msg::kInvalidSize);
        return false;
    }
    if (impl_->settings.output_mode == OutputMode::CopyTo &&
        impl_->settings.output_dir.empty()) {
        impl_->ui(msg::kOutputDirEmpty);
        return false;
    }

    impl_->running.store(true, std::memory_order_relaxed);
    impl_->run_log.write("Compression process started.");
    impl_->worker = std::thread([p = impl_.get()] { p->run(); });
    return true;
}

void JobRunner::cancel() {
    impl_->cancel.store(true, std::memory_order_relaxed);

    std::lock_guard lock(impl_->active_mutex);
    if (impl_->active_process) impl_->active_process->request_stop();
}

void JobRunner::join() {
    if (impl_->worker.joinable()) impl_->worker.join();
}

}  // namespace conv
