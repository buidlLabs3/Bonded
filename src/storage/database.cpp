#include "storage/database.h"

#include <sqlite3.h>

#include <memory>

namespace bonded {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw DomainError(sqlite3_errmsg(database));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_{nullptr};
};

void bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
    if (sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw DomainError("cannot bind database text value");
    }
}

std::string columnText(sqlite3_stmt* statement, int index)
{
    const auto* value = sqlite3_column_text(statement, index);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

} // namespace

Database::Database(const std::filesystem::path& path)
{
    if (sqlite3_open_v2(path.string().c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                                         SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string error = db_ == nullptr ? "cannot open database" : sqlite3_errmsg(db_);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw DomainError(error);
    }
    execute("PRAGMA foreign_keys = ON");
    execute("PRAGMA journal_mode = WAL");
    execute("PRAGMA synchronous = FULL");
    sqlite3_busy_timeout(db_, 5000);
}

Database::~Database()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Database::execute(const std::string& sql) const
{
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? "database execution failed" : error;
        sqlite3_free(error);
        throw DomainError(message);
    }
}

void Database::migrate()
{
    execute("BEGIN IMMEDIATE");
    try {
        execute("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)");
        execute("INSERT INTO schema_version(version) SELECT 1 WHERE NOT EXISTS "
                "(SELECT 1 FROM schema_version)");
        execute("CREATE TABLE IF NOT EXISTS policies("
                "inbox_id TEXT NOT NULL, version INTEGER NOT NULL, document TEXT NOT NULL, "
                "PRIMARY KEY(inbox_id, version))");
        execute("CREATE TABLE IF NOT EXISTS messages("
                "id TEXT PRIMARY KEY, idempotency_key TEXT NOT NULL UNIQUE, document TEXT NOT NULL, "
                "revision INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS outbox("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, topic TEXT NOT NULL, payload TEXT NOT NULL, "
                "attempts INTEGER NOT NULL DEFAULT 0, acknowledged INTEGER NOT NULL DEFAULT 0)");
        execute("CREATE TABLE IF NOT EXISTS processed_events("
                "event_id TEXT PRIMARY KEY, processed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
        execute("COMMIT");
    } catch (...) {
        execute("ROLLBACK");
        throw;
    }
}

void Database::savePolicy(const InboxPolicy& policy)
{
    Statement statement(db_, "INSERT INTO policies(inbox_id, version, document) VALUES(?, ?, ?)");
    bindText(statement.get(), 1, policy.inbox_id);
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(policy.version));
    bindText(statement.get(), 3, Json(policy).dump());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

std::optional<InboxPolicy> Database::latestPolicy(const std::string& inbox_id) const
{
    Statement statement(db_, "SELECT document FROM policies WHERE inbox_id = ? "
                             "ORDER BY version DESC LIMIT 1");
    bindText(statement.get(), 1, inbox_id);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        return Json::parse(columnText(statement.get(), 0)).get<InboxPolicy>();
    }
    return std::nullopt;
}

bool Database::createMessage(const MessageRecord& message, const std::string& idempotency_key)
{
    Statement statement(db_, "INSERT OR IGNORE INTO messages(id, idempotency_key, document, revision) "
                             "VALUES(?, ?, ?, ?)");
    bindText(statement.get(), 1, message.id);
    bindText(statement.get(), 2, idempotency_key);
    bindText(statement.get(), 3, Json(message).dump());
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(message.revision));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    return sqlite3_changes(db_) == 1;
}

std::optional<MessageRecord> Database::message(const std::string& message_id) const
{
    Statement statement(db_, "SELECT document FROM messages WHERE id = ?");
    bindText(statement.get(), 1, message_id);
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        return Json::parse(columnText(statement.get(), 0)).get<MessageRecord>();
    }
    return std::nullopt;
}

void Database::updateMessage(const MessageRecord& message, std::uint64_t previous_revision)
{
    Statement statement(db_, "UPDATE messages SET document = ?, revision = ? "
                             "WHERE id = ? AND revision = ?");
    bindText(statement.get(), 1, Json(message).dump());
    sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(message.revision));
    bindText(statement.get(), 3, message.id);
    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(previous_revision));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) != 1) {
        throw DomainError("message update revision conflict");
    }
}

void Database::enqueue(const std::string& topic, const std::string& payload)
{
    Statement statement(db_, "INSERT INTO outbox(topic, payload) VALUES(?, ?)");
    bindText(statement.get(), 1, topic);
    bindText(statement.get(), 2, payload);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

std::vector<OutboxRecord> Database::pendingOutbox(std::size_t limit) const
{
    Statement statement(db_, "SELECT id, topic, payload, attempts FROM outbox "
                             "WHERE acknowledged = 0 ORDER BY id LIMIT ?");
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(limit));
    std::vector<OutboxRecord> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({sqlite3_column_int64(statement.get(), 0), columnText(statement.get(), 1),
                          columnText(statement.get(), 2),
                          static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 3))});
    }
    return result;
}

void Database::acknowledgeOutbox(std::int64_t id)
{
    Statement statement(db_, "UPDATE outbox SET acknowledged = 1 WHERE id = ?");
    sqlite3_bind_int64(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

void Database::recordProcessedEvent(const std::string& event_id)
{
    Statement statement(db_, "INSERT OR IGNORE INTO processed_events(event_id) VALUES(?)");
    bindText(statement.get(), 1, event_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw DomainError(sqlite3_errmsg(db_));
    }
}

bool Database::hasProcessedEvent(const std::string& event_id) const
{
    Statement statement(db_, "SELECT 1 FROM processed_events WHERE event_id = ?");
    bindText(statement.get(), 1, event_id);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

} // namespace bonded
