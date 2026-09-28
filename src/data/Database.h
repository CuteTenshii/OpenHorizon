#pragma once

#include "base/Bytes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace fh {

/// One result row, with values looked up by column name. Names match without
/// regard to case, as the game's own queries are written inconsistently.
class Row {
public:
    bool has(std::string_view column) const;
    bool isNull(std::string_view column) const;
    /// The value converted to the requested type; a missing or NULL column
    /// gives `fallback`.
    double real(std::string_view column, double fallback = 0.0) const;
    float f(std::string_view column, float fallback = 0.0F) const
    {
        return static_cast<float>(real(column, fallback));
    }
    std::int64_t integer(std::string_view column, std::int64_t fallback = 0) const;
    int i(std::string_view column, int fallback = 0) const
    {
        return static_cast<int>(integer(column, fallback));
    }
    std::string text(std::string_view column, std::string_view fallback = {}) const;

private:
    friend class Database;
    struct Value {
        enum class Type : std::uint8_t { Null, Integer, Real, Text } type = Type::Null;
        std::int64_t integer = 0;
        double real = 0.0;
        std::string text;
    };
    const Value* find(std::string_view column) const;

    std::shared_ptr<const std::vector<std::string>> m_columns;
    std::vector<Value> m_values;
};

/// Read-only access to the game database (`media/db/gamedb.slt`, SQLite).
/// The file is copied into memory, so it can come from an archive or an
/// overlay like any other game file.
class Database {
public:
    /// Opens a database image held in memory. Throws DataError if SQLite
    /// rejects it.
    explicit Database(Bytes image);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Runs `sql` with `?` placeholders bound to `args` in order. Throws
    /// DataError on SQL errors.
    std::vector<Row> query(std::string_view sql, const std::vector<std::string>& args = {}) const;
    /// The first row, or nothing.
    std::optional<Row> queryOne(std::string_view sql, const std::vector<std::string>& args = {}) const;

    bool hasTable(std::string_view table) const;

private:
    sqlite3* m_db = nullptr;
    Bytes m_image;
};

} // namespace fh
