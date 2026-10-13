#pragma once

#include "engine/defines.hpp"

// The one place every message goes: engine, apps and scripts write through
// LOG::, and whoever wants to see the messages adds a sink (stderr is built
// in; the editor's Output panel and a log file are sinks too). A message has
// a level, a category, the text, and optionally the file and line it is
// about (a script, a scene file), kept apart from the text so a panel can
// jump there.
//
// Categories are small ids: the engine's own are the constants in
// LOG_CATEGORY::, and anything else (a plugin, a project, a script)
// registers its own by name with LOG::register_category, which hands back
// the same id for the same name. Each category can be switched off or
// given a minimum level (the verbosity), and LOG::write checks that first,
// before formatting and before any sink runs, so a muted category costs a
// table lookup. Sinks have their own minimum level on top, which only
// decides where a message goes, never whether it is produced.
//
// Single-threaded, like the rest of the engine. The state is static and
// needs no init: a message before main or after the Engine is gone still
// reaches stderr. A sink that writes a message while it handles one gets
// the nested message on stderr only, so a sink can never recurse into
// itself.

enum LogLevel : u32 {
    LOG_DEBUG = 0, // off by default: the chatty detail a category shows when asked
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3,
};
constexpr u32 LOG_LEVEL_COUNT = 4;
// As a minimum level: nothing passes.
constexpr LogLevel LOG_LEVEL_NONE = static_cast<LogLevel>(LOG_LEVEL_COUNT);

constexpr u16 LOG_INVALID_CATEGORY = 0xFFFF;

struct LogCategory {
    u16 id = LOG_INVALID_CATEGORY;

    constexpr bool is_valid() const { return this->id != LOG_INVALID_CATEGORY; }
    constexpr bool operator==(const LogCategory other) const { return this->id == other.id; }
    constexpr bool operator!=(const LogCategory other) const { return this->id != other.id; }
};

// The engine's categories. Their names are the lowercase of these.
namespace LOG_CATEGORY {

constexpr LogCategory ECS{0};
constexpr LogCategory MEMORY{1};
constexpr LogCategory ASSET{2};
constexpr LogCategory SCENE{3};
constexpr LogCategory SCHEDULER{4};
constexpr LogCategory GPU{5};
constexpr LogCategory SHADER{6};
constexpr LogCategory RENDER{7};
constexpr LogCategory ENGINE{8};
constexpr LogCategory WINDOW{9};
constexpr LogCategory LUAU{10};
constexpr LogCategory EDITOR{11};
constexpr u16 BUILTIN_COUNT = 12;

} // namespace LOG_CATEGORY

// Registered categories, built-ins included.
constexpr u16 LOG_MAX_CATEGORIES = 64;
constexpr usz LOG_CATEGORY_NAME_CAPACITY = 32;
constexpr usz LOG_MAX_SINKS = 8;
// A formatted message is cut at this many bytes; write_text has no limit.
constexpr usz LOG_FORMAT_CAPACITY = 2048;

struct LogRecord {
    LogLevel level = LOG_INFO;
    LogCategory category;
    // Valid during the sink call only; may hold newlines (a shader log, a
    // script traceback).
    const char* text = "";
    // The file and line the message is about, nullptr / 0 when none.
    const char* file = nullptr;
    u32 line = 0;
    // Seconds since the first message of the process.
    f64 time = 0;
};

using LogSinkFn = void (*)(const LogRecord& record, void* user_data);

struct LogSinkHandle {
    u32 index = 0;
    u32 generation = 0;

    bool is_valid() const { return this->generation != 0; }
};

namespace LOG {

// --- Writing -------------------------------------------------------------------

void write(LogLevel level, LogCategory category, const char* format, ...);
void write_at(LogLevel level, LogCategory category, const char* file, u32 line, const char* format, ...);
void vwrite(LogLevel level, LogCategory category, const char* file, u32 line, const char* format, va_list args);
// Preformatted text of any length, delivered as is.
void write_text(LogLevel level, LogCategory category, const char* text);
void write_text_at(LogLevel level, LogCategory category, const char* file, u32 line, const char* text);

void debug(LogCategory category, const char* format, ...);
void info(LogCategory category, const char* format, ...);
void warning(LogCategory category, const char* format, ...);
void error(LogCategory category, const char* format, ...);

// --- Categories ----------------------------------------------------------------

// Would a message of this level in this category be produced?
bool enabled(LogCategory category, LogLevel level);
void set_enabled(LogCategory category, bool enabled);
bool is_enabled(LogCategory category);
// The lowest level the category produces; LOG_INFO at start, LOG_DEBUG is
// the verbose setting, LOG_LEVEL_NONE silences it like set_enabled(false).
void set_min_level(LogCategory category, LogLevel level);
LogLevel min_level(LogCategory category);

// The category of this name, registered now if it was not. Names are cut
// at LOG_CATEGORY_NAME_CAPACITY - 1 bytes. Invalid (and an error logged)
// once LOG_MAX_CATEGORIES are registered.
LogCategory register_category(const char* name);
// Invalid when no category has the name.
LogCategory find_category(const char* name);
// "?" for an invalid or unregistered id.
const char* category_name(LogCategory category);
// Built-ins plus registered ones; ids are 0 .. count - 1.
u16 category_count();

const char* level_name(LogLevel level);

// --- Sinks ---------------------------------------------------------------------

// `min_level` is the sink's own floor on top of the categories'. Invalid
// (and an error logged) once LOG_MAX_SINKS are added.
LogSinkHandle add_sink(LogSinkFn fn, void* user_data, LogLevel min_level = LOG_DEBUG);
// A stale or invalid handle is ignored.
void remove_sink(LogSinkHandle handle);
void set_sink_min_level(LogSinkHandle handle, LogLevel level);

// The built-in stderr sink prints `[category] level: file:line: text`;
// LOG_INFO at start, LOG_LEVEL_NONE silences it.
void set_stderr_level(LogLevel level);
LogLevel stderr_level();

// Back to the state at process start: every sink removed, registered
// categories forgotten, every category enabled at LOG_INFO, stderr at
// LOG_INFO. For tests.
void reset();

} // namespace LOG

// Skips the argument evaluation too when the message would not be produced,
// for hot paths; the plain functions check the same thing after the call.
#define LOG_IF(level, category, ...)                                                     \
    do {                                                                                 \
        if (LOG::enabled((category), (level))) {                                         \
            LOG::write((level), (category), __VA_ARGS__);                                \
        }                                                                                \
    } while (0)
