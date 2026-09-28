#include "Database.h"

#include <sqlite3.h>

#include <cstdlib>

namespace fh {

const Row::Value* Row::find(std::string_view column) const
{
    if (!m_columns) {
        return nullptr;
    }
    for (std::size_t i = 0; i < m_columns->size(); ++i) {
        if (iequals((*m_columns)[i], column)) {
            return &m_values[i];
        }
    }
    return nullptr;
}

bool Row::has(std::string_view column) const
{
    return find(column) != nullptr;
}

bool Row::isNull(std::string_view column) const
{
    const Value* v = find(column);
    return !v || v->type == Value::Type::Null;
}

double Row::real(std::string_view column, double fallback) const
{
    const Value* v = find(column);
    if (!v) {
        return fallback;
    }
    switch (v->type) {
    case Value::Type::Integer:
        return static_cast<double>(v->integer);
    case Value::Type::Real:
        return v->real;
    case Value::Type::Text: {
        char* end = nullptr;
        const double parsed = std::strtod(v->text.c_str(), &end);
        return end != v->text.c_str() ? parsed : fallback;
    }
    case Value::Type::Null:
        break;
    }
    return fallback;
}

std::int64_t Row::integer(std::string_view column, std::int64_t fallback) const
{
    const Value* v = find(column);
    if (!v) {
        return fallback;
    }
    switch (v->type) {
    case Value::Type::Integer:
        return v->integer;
    case Value::Type::Real:
        return static_cast<std::int64_t>(v->real);
    case Value::Type::Text: {
        char* end = nullptr;
        const long long parsed = std::strtoll(v->text.c_str(), &end, 10);
        return end != v->text.c_str() ? parsed : fallback;
    }
    case Value::Type::Null:
        break;
    }
    return fallback;
}

std::string Row::text(std::string_view column, std::string_view fallback) const
{
    const Value* v = find(column);
    if (!v) {
        return std::string(fallback);
    }
    switch (v->type) {
    case Value::Type::Integer:
        return std::to_string(v->integer);
    case Value::Type::Real:
        return std::to_string(v->real);
    case Value::Type::Text:
        return v->text;
    case Value::Type::Null:
        break;
    }
    return std::string(fallback);
}

Database::Database(Bytes image)
    : m_image(std::move(image))
{
    if (sqlite3_open_v2(":memory:", &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        const std::string reason = m_db ? sqlite3_errmsg(m_db) : "out of memory";
        sqlite3_close(m_db);
        throw DataError("gamedb: {}", reason);
    }
    // SQLite may keep a pointer into the image for the connection's life,
    // which is why the image is a member rather than a copy.
    const int rc = sqlite3_deserialize(m_db, "main", m_image.data(), static_cast<sqlite3_int64>(m_image.size()),
        static_cast<sqlite3_int64>(m_image.size()), SQLITE_DESERIALIZE_READONLY);
    if (rc != SQLITE_OK) {
        const std::string reason = sqlite3_errmsg(m_db);
        sqlite3_close(m_db);
        throw DataError("gamedb: cannot load the image: {}", reason);
    }
}

Database::~Database()
{
    sqlite3_close(m_db);
}

std::vector<Row> Database::query(std::string_view sql, const std::vector<std::string>& args) const
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.data(), static_cast<int>(sql.size()), &statement, nullptr) != SQLITE_OK) {
        throw DataError("gamedb: {}: {}", sqlite3_errmsg(m_db), sql);
    }
    const std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> guard(statement, sqlite3_finalize);
    for (std::size_t i = 0; i < args.size(); ++i) {
        sqlite3_bind_text(statement, static_cast<int>(i + 1), args[i].c_str(), static_cast<int>(args[i].size()),
            SQLITE_TRANSIENT);
    }
    const int columnCount = sqlite3_column_count(statement);
    auto columns = std::make_shared<std::vector<std::string>>();
    for (int c = 0; c < columnCount; ++c) {
        columns->emplace_back(sqlite3_column_name(statement, c));
    }
    std::vector<Row> rows;
    while (true) {
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw DataError("gamedb: {}: {}", sqlite3_errmsg(m_db), sql);
        }
        Row row;
        row.m_columns = columns;
        row.m_values.resize(static_cast<std::size_t>(columnCount));
        for (int c = 0; c < columnCount; ++c) {
            Row::Value& value = row.m_values[static_cast<std::size_t>(c)];
            switch (sqlite3_column_type(statement, c)) {
            case SQLITE_INTEGER:
                value.type = Row::Value::Type::Integer;
                value.integer = sqlite3_column_int64(statement, c);
                break;
            case SQLITE_FLOAT:
                value.type = Row::Value::Type::Real;
                value.real = sqlite3_column_double(statement, c);
                break;
            case SQLITE_TEXT:
            case SQLITE_BLOB: {
                value.type = Row::Value::Type::Text;
                const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, c));
                value.text.assign(bytes ? bytes : "", static_cast<std::size_t>(sqlite3_column_bytes(statement, c)));
                break;
            }
            default:
                break;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<Row> Database::queryOne(std::string_view sql, const std::vector<std::string>& args) const
{
    std::vector<Row> rows = query(sql, args);
    if (rows.empty()) {
        return std::nullopt;
    }
    return std::move(rows.front());
}

bool Database::hasTable(std::string_view table) const
{
    return queryOne("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? COLLATE NOCASE",
               {std::string(table)})
        .has_value();
}

} // namespace fh
