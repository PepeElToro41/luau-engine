#include "support/test_support.hpp"

#include "engine/utils/log.hpp"

#include <cstring>
#include <string>
#include <vector>

// The log is process-global, so every test starts and ends with LOG::reset()
// and keeps stderr quiet while it runs.

namespace {

struct Captured {
    LogLevel level;
    LogCategory category;
    std::string text;
    std::string file;
    u32 line;
    bool had_file;
};

struct Capture {
    std::vector<Captured> records;

    static void sink(const LogRecord& record, void* user_data) {
        Capture* self = static_cast<Capture*>(user_data);
        self->records.push_back({record.level, record.category, record.text, record.file != nullptr ? record.file : "", record.line,
                                 record.file != nullptr});
    }
};

void quiet() {
    LOG::reset();
    LOG::set_stderr_level(LOG_LEVEL_NONE);
}

} // namespace

TEST_CASE("utils/log: built-in categories have their names and ids") {
    quiet();
    CHECK(LOG::category_count() == LOG_CATEGORY::BUILTIN_COUNT);
    CHECK(strcmp(LOG::category_name(LOG_CATEGORY::ECS), "ecs") == 0);
    CHECK(strcmp(LOG::category_name(LOG_CATEGORY::RENDER), "render") == 0);
    CHECK(strcmp(LOG::category_name(LOG_CATEGORY::LUAU), "luau") == 0);
    CHECK(strcmp(LOG::category_name(LOG_CATEGORY::EDITOR), "editor") == 0);
    CHECK(strcmp(LOG::category_name(LogCategory{}), "?") == 0);
    CHECK(strcmp(LOG::category_name(LogCategory{LOG_CATEGORY::BUILTIN_COUNT}), "?") == 0);
    CHECK(LOG::find_category("gpu") == LOG_CATEGORY::GPU);
    CHECK_FALSE(LOG::find_category("nothing").is_valid());
    CHECK(strcmp(LOG::level_name(LOG_DEBUG), "debug") == 0);
    CHECK(strcmp(LOG::level_name(LOG_ERROR), "error") == 0);
    CHECK(strcmp(LOG::level_name(LOG_LEVEL_NONE), "?") == 0);
    LOG::reset();
}

TEST_CASE("utils/log: register_category hands out one id per name") {
    quiet();
    const LogCategory physics = LOG::register_category("physics");
    REQUIRE(physics.is_valid());
    CHECK(physics.id == LOG_CATEGORY::BUILTIN_COUNT);
    CHECK(LOG::category_count() == LOG_CATEGORY::BUILTIN_COUNT + 1);
    CHECK(strcmp(LOG::category_name(physics), "physics") == 0);
    CHECK(LOG::register_category("physics") == physics);
    CHECK(LOG::find_category("physics") == physics);
    CHECK(LOG::register_category("gpu") == LOG_CATEGORY::GPU);

    const LogCategory audio = LOG::register_category("audio");
    CHECK(audio.id == physics.id + 1);
    CHECK(LOG::is_enabled(audio));
    CHECK(LOG::min_level(audio) == LOG_INFO);

    SUBCASE("long names are cut and still match themselves") {
        std::string name(LOG_CATEGORY_NAME_CAPACITY + 10, 'x');
        const LogCategory cut = LOG::register_category(name.c_str());
        REQUIRE(cut.is_valid());
        CHECK(strlen(LOG::category_name(cut)) == LOG_CATEGORY_NAME_CAPACITY - 1);
        CHECK(LOG::register_category(name.c_str()) == cut);
    }

    SUBCASE("an empty name is refused") {
        CHECK_FALSE(LOG::register_category("").is_valid());
        CHECK_FALSE(LOG::register_category(nullptr).is_valid());
    }

    SUBCASE("the table fills up") {
        for (u16 i = LOG::category_count(); i < LOG_MAX_CATEGORIES; ++i) {
            char name[16];
            snprintf(name, sizeof(name), "c%u", i);
            CHECK(LOG::register_category(name).is_valid());
        }
        CHECK(LOG::category_count() == LOG_MAX_CATEGORIES);
        CHECK_FALSE(LOG::register_category("one_more").is_valid());
    }

    SUBCASE("reset forgets them") {
        LOG::reset();
        CHECK(LOG::category_count() == LOG_CATEGORY::BUILTIN_COUNT);
        CHECK_FALSE(LOG::find_category("physics").is_valid());
    }
    LOG::reset();
}

TEST_CASE("utils/log: a sink receives the formatted record") {
    quiet();
    Capture capture;
    const LogSinkHandle handle = LOG::add_sink(&Capture::sink, &capture);
    REQUIRE(handle.is_valid());

    LOG::info(LOG_CATEGORY::ASSET, "loaded %d of %s", 3, "things");
    LOG::write_at(LOG_ERROR, LOG_CATEGORY::SCENE, "demo.scene", 40, "bad %s", "line");
    LOG::write_text(LOG_WARNING, LOG_CATEGORY::SHADER, "100% raw");

    REQUIRE(capture.records.size() == 3);
    CHECK(capture.records[0].level == LOG_INFO);
    CHECK(capture.records[0].category == LOG_CATEGORY::ASSET);
    CHECK(capture.records[0].text == "loaded 3 of things");
    CHECK_FALSE(capture.records[0].had_file);
    CHECK(capture.records[0].line == 0);

    CHECK(capture.records[1].level == LOG_ERROR);
    CHECK(capture.records[1].file == "demo.scene");
    CHECK(capture.records[1].line == 40);
    CHECK(capture.records[1].text == "bad line");

    CHECK(capture.records[2].level == LOG_WARNING);
    CHECK(capture.records[2].text == "100% raw");

    SUBCASE("write_text is not cut, write is") {
        const std::string longer(LOG_FORMAT_CAPACITY * 2, 'a');
        LOG::write_text(LOG_INFO, LOG_CATEGORY::LUAU, longer.c_str());
        LOG::write(LOG_INFO, LOG_CATEGORY::LUAU, "%s", longer.c_str());
        REQUIRE(capture.records.size() == 5);
        CHECK(capture.records[3].text.size() == longer.size());
        CHECK(capture.records[4].text.size() == LOG_FORMAT_CAPACITY - 1);
    }

    SUBCASE("an unknown category is reported as engine") {
        LOG::info(LogCategory{}, "lost");
        LOG::info(LogCategory{LOG_MAX_CATEGORIES + 5}, "lost too");
        REQUIRE(capture.records.size() == 5);
        CHECK(capture.records[3].category == LOG_CATEGORY::ENGINE);
        CHECK(capture.records[4].category == LOG_CATEGORY::ENGINE);
    }

    SUBCASE("removing the sink stops delivery and stale handles are ignored") {
        LOG::remove_sink(handle);
        LOG::info(LOG_CATEGORY::ASSET, "gone");
        CHECK(capture.records.size() == 3);
        LOG::remove_sink(handle);
        LOG::set_sink_min_level(handle, LOG_ERROR);
        // The slot can be reused; the old handle does not reach the new sink.
        Capture other;
        const LogSinkHandle fresh = LOG::add_sink(&Capture::sink, &other);
        CHECK(fresh.index == handle.index);
        CHECK(fresh.generation != handle.generation);
        LOG::remove_sink(handle);
        LOG::info(LOG_CATEGORY::ASSET, "still here");
        CHECK(other.records.size() == 1);
    }
    LOG::reset();
}

TEST_CASE("utils/log: categories can be muted or made verbose before anything is formatted") {
    quiet();
    Capture capture;
    LOG::add_sink(&Capture::sink, &capture);

    CHECK_FALSE(LOG::enabled(LOG_CATEGORY::ECS, LOG_DEBUG));
    CHECK(LOG::enabled(LOG_CATEGORY::ECS, LOG_INFO));
    LOG::debug(LOG_CATEGORY::ECS, "hidden");
    CHECK(capture.records.empty());

    LOG::set_min_level(LOG_CATEGORY::ECS, LOG_DEBUG);
    CHECK(LOG::enabled(LOG_CATEGORY::ECS, LOG_DEBUG));
    LOG::debug(LOG_CATEGORY::ECS, "shown");
    CHECK(capture.records.size() == 1);

    LOG::set_min_level(LOG_CATEGORY::ECS, LOG_ERROR);
    LOG::warning(LOG_CATEGORY::ECS, "hidden");
    LOG::error(LOG_CATEGORY::ECS, "shown");
    CHECK(capture.records.size() == 2);

    LOG::set_enabled(LOG_CATEGORY::ECS, false);
    CHECK_FALSE(LOG::is_enabled(LOG_CATEGORY::ECS));
    CHECK_FALSE(LOG::enabled(LOG_CATEGORY::ECS, LOG_ERROR));
    LOG::error(LOG_CATEGORY::ECS, "hidden");
    LOG::error(LOG_CATEGORY::GPU, "other categories are untouched");
    CHECK(capture.records.size() == 3);

    LOG::set_min_level(LOG_CATEGORY::GPU, LOG_LEVEL_NONE);
    LOG::error(LOG_CATEGORY::GPU, "hidden");
    CHECK(capture.records.size() == 3);

    int evaluated = 0;
    LOG_IF(LOG_DEBUG, LOG_CATEGORY::ASSET, "%d", ++evaluated);
    CHECK(evaluated == 0);
    LOG_IF(LOG_INFO, LOG_CATEGORY::ASSET, "%d", ++evaluated);
    CHECK(evaluated == 1);
    CHECK(capture.records.size() == 4);

    // Settings on an invalid category are ignored.
    LOG::set_enabled(LogCategory{}, false);
    LOG::set_min_level(LogCategory{}, LOG_ERROR);
    CHECK(LOG::min_level(LogCategory{}) == LOG_LEVEL_NONE);
    LOG::info(LogCategory{}, "as engine");
    CHECK(capture.records.size() == 5);
    LOG::reset();
}

TEST_CASE("utils/log: a sink's own minimum level only filters that sink") {
    quiet();
    Capture everything;
    Capture problems;
    const LogSinkHandle all = LOG::add_sink(&Capture::sink, &everything);
    LOG::add_sink(&Capture::sink, &problems, LOG_WARNING);

    LOG::info(LOG_CATEGORY::ENGINE, "info");
    LOG::warning(LOG_CATEGORY::ENGINE, "warning");
    LOG::error(LOG_CATEGORY::ENGINE, "error");
    CHECK(everything.records.size() == 3);
    CHECK(problems.records.size() == 2);
    CHECK(problems.records[0].level == LOG_WARNING);

    LOG::set_sink_min_level(all, LOG_ERROR);
    LOG::info(LOG_CATEGORY::ENGINE, "info");
    LOG::error(LOG_CATEGORY::ENGINE, "error");
    CHECK(everything.records.size() == 4);
    CHECK(problems.records.size() == 3);

    SUBCASE("the sink table fills up") {
        Capture more[LOG_MAX_SINKS];
        usz added = 0;
        for (Capture& capture : more) {
            if (LOG::add_sink(&Capture::sink, &capture).is_valid()) {
                added += 1;
            }
        }
        CHECK(added == LOG_MAX_SINKS - 2);
        CHECK_FALSE(LOG::add_sink(nullptr, nullptr).is_valid());
    }

    SUBCASE("the stderr level is a setting of its own") {
        CHECK(LOG::stderr_level() == LOG_LEVEL_NONE);
        LOG::set_stderr_level(LOG_WARNING);
        CHECK(LOG::stderr_level() == LOG_WARNING);
        LOG::reset();
        CHECK(LOG::stderr_level() == LOG_INFO);
    }
    LOG::reset();
}

namespace {

struct Reentrant {
    int calls = 0;

    static void sink(const LogRecord& record, void* user_data) {
        Reentrant* self = static_cast<Reentrant*>(user_data);
        self->calls += 1;
        if (self->calls == 1) {
            LOG::error(record.category, "from inside the sink");
        }
    }
};

} // namespace

TEST_CASE("utils/log: a message written by a sink does not reach the sinks again") {
    quiet();
    Reentrant reentrant;
    Capture capture;
    LOG::add_sink(&Reentrant::sink, &reentrant);
    LOG::add_sink(&Capture::sink, &capture);
    LOG::info(LOG_CATEGORY::ENGINE, "outer");
    CHECK(reentrant.calls == 1);
    REQUIRE(capture.records.size() == 1);
    CHECK(capture.records[0].text == "outer");
    LOG::reset();
}

TEST_CASE("utils/log: the record's time advances") {
    quiet();
    struct Times {
        std::vector<f64> values;
        static void sink(const LogRecord& record, void* user_data) {
            static_cast<Times*>(user_data)->values.push_back(record.time);
        }
    } times;
    LOG::add_sink(&Times::sink, &times);
    LOG::info(LOG_CATEGORY::ENGINE, "first");
    LOG::info(LOG_CATEGORY::ENGINE, "second");
    REQUIRE(times.values.size() == 2);
    CHECK(times.values[0] >= 0.0);
    CHECK(times.values[1] >= times.values[0]);
    LOG::reset();
}
