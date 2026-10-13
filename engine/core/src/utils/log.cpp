#include "engine/utils/log.hpp"

#include <chrono>

// --- State ---------------------------------------------------------------------
// Static and zero-initialized, so a message from a static initializer or
// after every allocator is gone still works.

namespace {

struct CategoryState {
    bool enabled = true;
    LogLevel min_level = LOG_INFO;
};

struct RegisteredName {
    char text[LOG_CATEGORY_NAME_CAPACITY];
};

struct Sink {
    LogSinkFn fn = nullptr;
    void* user_data = nullptr;
    LogLevel min_level = LOG_DEBUG;
    u32 generation = 0; // bumped on add and remove; odd while occupied
};

const char* const BUILTIN_NAMES[LOG_CATEGORY::BUILTIN_COUNT] = {
    "ecs", "memory", "asset", "scene", "scheduler", "gpu", "shader", "render", "engine", "window", "luau", "editor",
};

const char* const LEVEL_NAMES[LOG_LEVEL_COUNT] = {"debug", "info", "warning", "error"};

CategoryState g_categories[LOG_MAX_CATEGORIES];
RegisteredName g_names[LOG_MAX_CATEGORIES - LOG_CATEGORY::BUILTIN_COUNT];
u16 g_category_count = LOG_CATEGORY::BUILTIN_COUNT;

Sink g_sinks[LOG_MAX_SINKS];
LogLevel g_stderr_level = LOG_INFO;

// True while sinks run, so a message written by a sink goes to stderr only.
bool g_dispatching = false;
bool g_clock_started = false;
std::chrono::steady_clock::time_point g_clock_origin;

f64 seconds_now() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!g_clock_started) {
        g_clock_started = true;
        g_clock_origin = now;
    }
    return std::chrono::duration<f64>(now - g_clock_origin).count();
}

void print_stderr(const LogRecord& record) {
    fprintf(stderr, "[%s] ", LOG::category_name(record.category));
    if (record.level != LOG_INFO) {
        fprintf(stderr, "%s: ", LOG::level_name(record.level));
    }
    if (record.file != nullptr) {
        if (record.line != 0) {
            fprintf(stderr, "%s:%u: ", record.file, record.line);
        } else {
            fprintf(stderr, "%s: ", record.file);
        }
    }
    fputs(record.text, stderr);
    fputc('\n', stderr);
}

void dispatch(const LogLevel level, LogCategory category, const char* file, const u32 line, const char* text) {
    if (!category.is_valid() || category.id >= g_category_count) {
        category = LOG_CATEGORY::ENGINE;
    }
    LogRecord record;
    record.level = level;
    record.category = category;
    record.text = text;
    record.file = file;
    record.line = line;
    record.time = seconds_now();

    if (level >= g_stderr_level) {
        print_stderr(record);
    }
    if (g_dispatching) {
        return;
    }
    g_dispatching = true;
    for (Sink& sink : g_sinks) {
        if (sink.fn != nullptr && level >= sink.min_level) {
            sink.fn(record, sink.user_data);
        }
    }
    g_dispatching = false;
}

} // namespace

// --- Writing -------------------------------------------------------------------

void LOG::vwrite(const LogLevel level, const LogCategory category, const char* file, const u32 line, const char* format, va_list args) {
    if (!enabled(category, level)) {
        return;
    }
    char text[LOG_FORMAT_CAPACITY];
    vsnprintf(text, sizeof(text), format, args);
    dispatch(level, category, file, line, text);
}

void LOG::write(const LogLevel level, const LogCategory category, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vwrite(level, category, nullptr, 0, format, args);
    va_end(args);
}

void LOG::write_at(const LogLevel level, const LogCategory category, const char* file, const u32 line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vwrite(level, category, file, line, format, args);
    va_end(args);
}

void LOG::write_text(const LogLevel level, const LogCategory category, const char* text) {
    write_text_at(level, category, nullptr, 0, text);
}

void LOG::write_text_at(const LogLevel level, const LogCategory category, const char* file, const u32 line, const char* text) {
    if (!enabled(category, level)) {
        return;
    }
    dispatch(level, category, file, line, text != nullptr ? text : "");
}

#define LOG_LEVEL_FUNCTION(name, level)                                                  \
    void LOG::name(const LogCategory category, const char* format, ...) {                \
        va_list args;                                                                    \
        va_start(args, format);                                                          \
        vwrite(level, category, nullptr, 0, format, args);                               \
        va_end(args);                                                                    \
    }

LOG_LEVEL_FUNCTION(debug, LOG_DEBUG)
LOG_LEVEL_FUNCTION(info, LOG_INFO)
LOG_LEVEL_FUNCTION(warning, LOG_WARNING)
LOG_LEVEL_FUNCTION(error, LOG_ERROR)

#undef LOG_LEVEL_FUNCTION

// --- Categories ----------------------------------------------------------------

bool LOG::enabled(const LogCategory category, const LogLevel level) {
    if (!category.is_valid() || category.id >= g_category_count) {
        // An unknown category is reported as ENGINE, under ENGINE's settings.
        return enabled(LOG_CATEGORY::ENGINE, level);
    }
    const CategoryState& state = g_categories[category.id];
    return state.enabled && level >= state.min_level;
}

void LOG::set_enabled(const LogCategory category, const bool enabled) {
    if (category.is_valid() && category.id < g_category_count) {
        g_categories[category.id].enabled = enabled;
    }
}

bool LOG::is_enabled(const LogCategory category) {
    return category.is_valid() && category.id < g_category_count && g_categories[category.id].enabled;
}

void LOG::set_min_level(const LogCategory category, const LogLevel level) {
    if (category.is_valid() && category.id < g_category_count) {
        g_categories[category.id].min_level = level;
    }
}

LogLevel LOG::min_level(const LogCategory category) {
    if (category.is_valid() && category.id < g_category_count) {
        return g_categories[category.id].min_level;
    }
    return LOG_LEVEL_NONE;
}

LogCategory LOG::find_category(const char* name) {
    if (name == nullptr) {
        return LogCategory{};
    }
    for (u16 i = 0; i < g_category_count; ++i) {
        if (strcmp(category_name(LogCategory{i}), name) == 0) {
            return LogCategory{i};
        }
    }
    return LogCategory{};
}

LogCategory LOG::register_category(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        error(LOG_CATEGORY::ENGINE, "log: a category needs a name");
        return LogCategory{};
    }
    char cut[LOG_CATEGORY_NAME_CAPACITY];
    snprintf(cut, sizeof(cut), "%s", name);
    const LogCategory existing = find_category(cut);
    if (existing.is_valid()) {
        return existing;
    }
    if (g_category_count >= LOG_MAX_CATEGORIES) {
        error(LOG_CATEGORY::ENGINE, "log: cannot register category '%s': all %u are taken", cut, LOG_MAX_CATEGORIES);
        return LogCategory{};
    }
    const u16 id = g_category_count++;
    memcpy(g_names[id - LOG_CATEGORY::BUILTIN_COUNT].text, cut, sizeof(cut));
    g_categories[id] = CategoryState{};
    return LogCategory{id};
}

const char* LOG::category_name(const LogCategory category) {
    if (!category.is_valid() || category.id >= g_category_count) {
        return "?";
    }
    if (category.id < LOG_CATEGORY::BUILTIN_COUNT) {
        return BUILTIN_NAMES[category.id];
    }
    return g_names[category.id - LOG_CATEGORY::BUILTIN_COUNT].text;
}

u16 LOG::category_count() {
    return g_category_count;
}

const char* LOG::level_name(const LogLevel level) {
    return level < LOG_LEVEL_COUNT ? LEVEL_NAMES[level] : "?";
}

// --- Sinks ---------------------------------------------------------------------

LogSinkHandle LOG::add_sink(const LogSinkFn fn, void* user_data, const LogLevel min_level) {
    if (fn == nullptr) {
        error(LOG_CATEGORY::ENGINE, "log: a sink needs a function");
        return LogSinkHandle{};
    }
    for (u32 i = 0; i < LOG_MAX_SINKS; ++i) {
        Sink& sink = g_sinks[i];
        if (sink.fn == nullptr) {
            sink.fn = fn;
            sink.user_data = user_data;
            sink.min_level = min_level;
            sink.generation += 1;
            return LogSinkHandle{i, sink.generation};
        }
    }
    error(LOG_CATEGORY::ENGINE, "log: cannot add a sink: all %llu slots are taken", static_cast<unsigned long long>(LOG_MAX_SINKS));
    return LogSinkHandle{};
}

static Sink* sink_of(const LogSinkHandle handle) {
    if (!handle.is_valid() || handle.index >= LOG_MAX_SINKS) {
        return nullptr;
    }
    Sink& sink = g_sinks[handle.index];
    if (sink.fn == nullptr || sink.generation != handle.generation) {
        return nullptr;
    }
    return &sink;
}

void LOG::remove_sink(const LogSinkHandle handle) {
    if (Sink* sink = sink_of(handle)) {
        sink->fn = nullptr;
        sink->user_data = nullptr;
        sink->generation += 1;
    }
}

void LOG::set_sink_min_level(const LogSinkHandle handle, const LogLevel level) {
    if (Sink* sink = sink_of(handle)) {
        sink->min_level = level;
    }
}

void LOG::set_stderr_level(const LogLevel level) {
    g_stderr_level = level;
}

LogLevel LOG::stderr_level() {
    return g_stderr_level;
}

void LOG::reset() {
    for (Sink& sink : g_sinks) {
        if (sink.fn != nullptr) {
            sink.generation += 1;
        }
        sink.fn = nullptr;
        sink.user_data = nullptr;
        sink.min_level = LOG_DEBUG;
    }
    for (CategoryState& state : g_categories) {
        state = CategoryState{};
    }
    g_category_count = LOG_CATEGORY::BUILTIN_COUNT;
    g_stderr_level = LOG_INFO;
    g_dispatching = false;
}

// --- Assertions ----------------------------------------------------------------
// Declared in defines.hpp. The message goes to stderr itself and then
// through the sinks, so a log file holds the last thing that happened.

void ENGINE_ASSERTS::fail(const char* condition, const char* file, const int line, const char* format, ...) {
    char text[LOG_FORMAT_CAPACITY];
    const int head = snprintf(text, sizeof(text), "assertion failed: %s\n  ", condition);
    va_list args;
    va_start(args, format);
    if (head > 0 && static_cast<usz>(head) < sizeof(text)) {
        vsnprintf(text + head, sizeof(text) - static_cast<usz>(head), format, args);
    }
    va_end(args);
    fprintf(stderr, "%s:%d: %s\n", file, line, text);
    const LogLevel kept = g_stderr_level;
    g_stderr_level = LOG_LEVEL_NONE; // printed above already
    dispatch(LOG_ERROR, LOG_CATEGORY::ENGINE, file, line > 0 ? static_cast<u32>(line) : 0, text);
    g_stderr_level = kept;
    abort();
}
