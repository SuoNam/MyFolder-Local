#include "httplib.h"
#include "json.hpp"
#include <drogon/drogon.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using Rows = std::vector<json>;

static constexpr uint64_t DEFAULT_QUOTA = 5ULL * 1024 * 1024 * 1024;
static constexpr size_t MAX_JSON = 64 * 1024;
static constexpr size_t CHUNK = 256 * 1024;
static constexpr int RESET_WINDOW = 30 * 60;
static const std::array<std::string, 4> PERMISSIONS = {"upload", "download", "modify", "delete"};

struct ApiError : std::runtime_error {
    int status;
    ApiError(int code, const std::string& message) : std::runtime_error(message), status(code) {}
};

[[noreturn]] static void fail(int code, const std::string& message) { throw ApiError(code, message); }
static int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }
static std::string env(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value && *value ? std::string(value) : fallback;
}
static fs::path path_from_utf8(const std::string& value) { return fs::u8path(value); }
static std::string path_string(const fs::path& value) { return value.u8string(); }
static std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

static std::string random_bytes(size_t count) {
    std::string value(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(value.data()), static_cast<int>(count)) != 1) {
        throw std::runtime_error("系统随机数不可用");
    }
    return value;
}
static std::string hex_bytes(const unsigned char* data, size_t count) {
    static constexpr char table[] = "0123456789abcdef";
    std::string value;
    value.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        value.push_back(table[data[i] >> 4]);
        value.push_back(table[data[i] & 15]);
    }
    return value;
}
static std::string random_hex(size_t count) {
    auto value = random_bytes(count);
    return hex_bytes(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}
static std::string sha256(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    return hex_bytes(digest, sizeof digest);
}
static std::string b64(const std::string& raw) {
    std::string encoded(4 * ((raw.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                    reinterpret_cast<const unsigned char*>(raw.data()), static_cast<int>(raw.size()));
    return encoded;
}
static std::string unb64(const std::string& encoded) {
    if (encoded.size() % 4) return {};
    std::string raw(encoded.size() * 3 / 4, '\0');
    int count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(raw.data()),
                                reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    if (count < 0) return {};
    size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') ++padding;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
    raw.resize(static_cast<size_t>(count) - padding);
    return raw;
}
static std::string password_hash(const std::string& password) {
    if (password.size() < 6) fail(400, "密码至少需要 6 个字符");
    auto salt = random_bytes(16);
    std::string digest(32, '\0');
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
                          310000, EVP_sha256(), static_cast<int>(digest.size()),
                          reinterpret_cast<unsigned char*>(digest.data())) != 1) {
        throw std::runtime_error("密码计算失败");
    }
    return "pbkdf2:310000:" + b64(salt) + ":" + b64(digest);
}
static bool verify_password(const std::string& password, const std::string& encoded) {
    std::istringstream input(encoded);
    std::string kind, rounds_str, salt_b64, digest_b64;
    if (!std::getline(input, kind, ':') || !std::getline(input, rounds_str, ':') ||
        !std::getline(input, salt_b64, ':') || !std::getline(input, digest_b64, ':') || kind != "pbkdf2") return false;
    auto salt = unb64(salt_b64), expected = unb64(digest_b64);
    if (salt.empty() || expected.size() != 32) return false;
    int rounds = 0;
    try { rounds = std::stoi(rounds_str); } catch (...) { return false; }
    if (rounds < 1 || rounds > 10000000) return false;
    std::string actual(expected.size(), '\0');
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
                          rounds, EVP_sha256(), static_cast<int>(actual.size()),
                          reinterpret_cast<unsigned char*>(actual.data())) != 1) return false;
    return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

class Db {
    sqlite3* db_ = nullptr;
public:
    explicit Db(const fs::path& path) {
        if (sqlite3_open(path_string(path).c_str(), &db_) != SQLITE_OK) {
            std::string message = db_ ? sqlite3_errmsg(db_) : "SQLite 打开失败";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(message);
        }
        sqlite3_busy_timeout(db_, 30000);
        raw("PRAGMA foreign_keys=ON");
    }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db() { if (db_) sqlite3_close(db_); }
    void raw(const std::string& sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    Rows query(const std::string& sql, const json& args = json::array()) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
        try {
            for (size_t i = 0; i < args.size(); ++i) {
                const auto& value = args[i];
                int slot = static_cast<int>(i + 1);
                int result = SQLITE_OK;
                if (value.is_null()) result = sqlite3_bind_null(stmt, slot);
                else if (value.is_boolean()) result = sqlite3_bind_int(stmt, slot, value.get<bool>() ? 1 : 0);
                else if (value.is_number_integer()) result = sqlite3_bind_int64(stmt, slot, value.get<int64_t>());
                else {
                    auto text = value.is_string() ? value.get<std::string>() : value.dump();
                    result = sqlite3_bind_text(stmt, slot, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
                }
                if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
            }
            Rows rows;
            int rc;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                json row = json::object();
                for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
                    const char* column = sqlite3_column_name(stmt, i);
                    switch (sqlite3_column_type(stmt, i)) {
                    case SQLITE_INTEGER: row[column] = sqlite3_column_int64(stmt, i); break;
                    case SQLITE_FLOAT: row[column] = sqlite3_column_double(stmt, i); break;
                    case SQLITE_TEXT: row[column] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)); break;
                    case SQLITE_NULL: row[column] = nullptr; break;
                    default: row[column] = nullptr; break;
                    }
                }
                rows.push_back(std::move(row));
            }
            if (rc != SQLITE_DONE) {
                int code = sqlite3_extended_errcode(db_);
                if ((code & 0xff) == SQLITE_CONSTRAINT) fail(409, "数据已存在或违反约束");
                throw std::runtime_error(sqlite3_errmsg(db_));
            }
            sqlite3_finalize(stmt);
            return rows;
        } catch (...) {
            sqlite3_finalize(stmt);
            throw;
        }
    }
    json one(const std::string& sql, const json& args = json::array()) {
        auto rows = query(sql, args);
        return rows.empty() ? json() : rows.front();
    }
    int64_t id() const { return sqlite3_last_insert_rowid(db_); }
};

static std::string as_string(const json& object, const std::string& key, const std::string& fallback = "") {
    if (!object.is_object() || !object.contains(key) || object[key].is_null()) return fallback;
    if (object[key].is_string()) return object[key].get<std::string>();
    return object[key].dump();
}
static int64_t as_int(const json& object, const std::string& key) {
    if (!object.is_object() || !object.contains(key)) throw std::invalid_argument("缺少数字参数");
    if (object[key].is_number_integer()) return object[key].get<int64_t>();
    if (object[key].is_string()) return std::stoll(object[key].get<std::string>());
    throw std::invalid_argument("数字参数无效");
}
static int64_t query_int(const httplib::Request& req, const std::string& key) {
    if (!req.has_param(key)) throw std::invalid_argument("参数无效");
    return std::stoll(req.get_param_value(key));
}
static json json_body(const httplib::Request& req) {
    if (req.body.size() > MAX_JSON) fail(413, "请求内容过大");
    json body = json::parse(req.body, nullptr, false);
    if (!body.is_object()) fail(400, "JSON 格式错误");
    return body;
}

struct Config {
    fs::path data, files, db, web;
    std::string host;
    int port, workers;
    size_t max_uploads;
    size_t max_request_bytes;
};
static Config config() {
    fs::path data = fs::absolute(path_from_utf8(env("MYFOLDER_DATA", "data")));
    fs::path web = fs::absolute(path_from_utf8(env("MYFOLDER_WEB", "web/dist")));
    return {data, data / "files", data / "myfolder.sqlite3", web,
            env("MYFOLDER_HOST", "0.0.0.0"), std::stoi(env("MYFOLDER_PORT", "8080")),
            std::max(1, std::stoi(env("MYFOLDER_IO_WORKERS", "4"))),
            static_cast<size_t>(std::max(1, std::stoi(env("MYFOLDER_MAX_UPLOADS", "2")))),
            static_cast<size_t>(std::clamp(std::stoull(env("MYFOLDER_MAX_REQUEST_GIB", "5")), 1ULL, 1024ULL) *
                                1024ULL * 1024 * 1024)};
}

static void initialize(const Config& cfg) {
    fs::create_directories(cfg.files);
    Db db(cfg.db);
    db.raw("PRAGMA journal_mode=WAL");
    db.raw(R"SQL(
CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY,username TEXT NOT NULL UNIQUE,password TEXT NOT NULL,role TEXT NOT NULL CHECK(role IN ('admin','user')),created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS sessions (token_hash TEXT PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,expires_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS grants (id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,path TEXT NOT NULL,upload INTEGER NOT NULL,download INTEGER NOT NULL,modify INTEGER NOT NULL,delete_allowed INTEGER NOT NULL,UNIQUE(user_id,path));
CREATE INDEX IF NOT EXISTS grants_user_path ON grants(user_id,path);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);
)SQL");
    auto version = db.one("PRAGMA user_version");
    if (version["user_version"].get<int>() < 1) {
        db.raw("INSERT OR IGNORE INTO grants(user_id,path,upload,download,modify,delete_allowed) SELECT id,'',0,1,0,0 FROM users WHERE role='user'");
        db.raw("PRAGMA user_version=1");
    }
    version = db.one("PRAGMA user_version");
    if (version["user_version"].get<int>() < 2) {
        auto columns = db.query("PRAGMA table_info(users)");
        bool super = false, reset = false;
        for (const auto& row : columns) {
            super |= row["name"] == "is_superadmin";
            reset |= row["name"] == "reset_until";
        }
        if (!super) db.raw("ALTER TABLE users ADD COLUMN is_superadmin INTEGER NOT NULL DEFAULT 0");
        if (!reset) db.raw("ALTER TABLE users ADD COLUMN reset_until INTEGER NOT NULL DEFAULT 0");
        auto first = db.one("SELECT id FROM users WHERE role='admin' ORDER BY id LIMIT 1");
        if (!first.is_null()) db.query("UPDATE users SET is_superadmin=1 WHERE id=?", json::array({first["id"]}));
        db.raw("PRAGMA user_version=2");
    }
    db.raw(R"SQL(
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS password_requests (id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,status TEXT NOT NULL CHECK(status IN ('pending','approved','rejected')),created_at INTEGER NOT NULL,reviewed_at INTEGER,reviewer_id INTEGER REFERENCES users(id) ON DELETE SET NULL);
CREATE TABLE IF NOT EXISTS remote_outgoing (id INTEGER PRIMARY KEY,url TEXT NOT NULL,remote_id TEXT NOT NULL,remote_name TEXT NOT NULL,token TEXT NOT NULL,request_id TEXT NOT NULL,status TEXT NOT NULL,created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS remote_incoming (id TEXT PRIMARY KEY,source_id TEXT NOT NULL,source_name TEXT NOT NULL,token_hash TEXT NOT NULL UNIQUE,status TEXT NOT NULL,created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS remote_grants (id INTEGER PRIMARY KEY,incoming_id TEXT NOT NULL REFERENCES remote_incoming(id) ON DELETE CASCADE,path TEXT NOT NULL,upload INTEGER NOT NULL,download INTEGER NOT NULL,modify INTEGER NOT NULL,delete_allowed INTEGER NOT NULL,UNIQUE(incoming_id,path));
)SQL");
    db.query("INSERT OR IGNORE INTO settings VALUES('server_id',?)", json::array({random_hex(16)}));
    db.raw("INSERT OR IGNORE INTO settings VALUES('server_name','MyFolder LAN')");
    db.query("INSERT OR IGNORE INTO settings VALUES('storage_limit',?)", json::array({std::to_string(DEFAULT_QUOTA)}));
}

static std::string setting(Db& db, const std::string& key) {
    auto row = db.one("SELECT value FROM settings WHERE key=?", json::array({key}));
    if (row.is_null()) throw std::runtime_error("服务器配置缺失");
    return as_string(row, "value");
}
static std::string role_of(const json& user) {
    return as_string(user, "role") == "admin" && user.value("is_superadmin", 0) != 0 ? "superadmin" : as_string(user, "role");
}
static json public_user(const json& user) {
    return {{"id", user["id"]}, {"username", user["username"]}, {"role", role_of(user)},
            {"reset_until", user.value("reset_until", 0)}};
}
static std::string clean_path(const std::string& raw) {
    if (raw.find('\\') != std::string::npos || raw.find('\0') != std::string::npos) fail(400, "无效路径");
    size_t left = raw.find_first_not_of('/');
    if (left == std::string::npos) return "";
    size_t right = raw.find_last_not_of('/');
    std::string value = raw.substr(left, right - left + 1);
    std::istringstream input(value);
    std::string part;
    while (std::getline(input, part, '/')) {
        if (part.empty() || part == "." || part == ".." || part.rfind(".upload-", 0) == 0) fail(400, "无效路径");
        for (unsigned char c : part) if (c < 32) fail(400, "无效路径");
    }
    return value;
}
static std::string parent_of(const std::string& value) {
    auto pos = value.rfind('/');
    return pos == std::string::npos ? "" : value.substr(0, pos);
}
static bool within(const std::string& child, const std::string& parent) {
    return parent.empty() || child == parent || (child.size() > parent.size() &&
           child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/');
}
static fs::path disk_path(const Config& cfg, const std::string& logical) {
    fs::path result = cfg.files;
    std::istringstream input(logical);
    std::string part;
    while (std::getline(input, part, '/')) {
        result /= path_from_utf8(part);
        std::error_code error;
        if (fs::is_symlink(fs::symlink_status(result, error))) fail(400, "符号链接不可访问");
    }
    return result;
}
static Rows grants_for(Db& db, const json& user) {
    if (role_of(user) == "peer") return db.query("SELECT * FROM remote_grants WHERE incoming_id=?", json::array({user["peer_id"]}));
    if (as_string(user, "role") == "admin") return {};
    return db.query("SELECT * FROM grants WHERE user_id=?", json::array({user["id"]}));
}
static std::set<std::string> flags(Db& db, const json& user, const std::string& path, const Rows* given = nullptr) {
    if (as_string(user, "role") == "admin") return {PERMISSIONS.begin(), PERMISSIONS.end()};
    Rows owned;
    if (!given) { owned = grants_for(db, user); given = &owned; }
    std::set<std::string> result;
    for (const auto& grant : *given) {
        if (!within(path, as_string(grant, "path"))) continue;
        for (const auto& key : PERMISSIONS) {
            const auto column = key == "delete" ? "delete_allowed" : key;
            if (grant.value(column, 0) != 0) result.insert(key);
        }
    }
    return result;
}
static bool visible(Db& db, const json& user, const std::string& path, const Rows* given = nullptr) {
    Rows owned;
    if (!given) { owned = grants_for(db, user); given = &owned; }
    if (!flags(db, user, path, given).empty()) return true;
    for (const auto& grant : *given) if (within(as_string(grant, "path"), path)) return true;
    return false;
}
static json permissions_json(const std::set<std::string>& permissions) {
    json result = json::array();
    for (const auto& value : permissions) result.push_back(value);
    return result;
}
static void require_permission(Db& db, const json& user, const std::string& path, const std::string& permission) {
    if (!flags(db, user, path).count(permission)) fail(403, "该路径没有" + permission + "权限");
}
static void require_admin(const json& user) {
    if (as_string(user, "role") != "admin") fail(403, "需要管理员权限");
}
static void require_super(const json& user) {
    if (role_of(user) != "superadmin") fail(403, "需要超级管理员权限");
}
static std::pair<uint64_t, uint64_t> storage_stats(const Config& cfg) {
    uint64_t bytes = 0, count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(cfg.files, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        const auto& entry = *it;
        if (entry.is_symlink(error)) { it.disable_recursion_pending(); continue; }
        if (entry.is_regular_file(error) && entry.path().filename().u8string().rfind(".upload-", 0) != 0) {
            bytes += entry.file_size(error);
            ++count;
        }
    }
    return {bytes, count};
}

static std::string session_cookie(const httplib::Request& req) {
    auto cookie = req.get_header_value("Cookie");
    size_t start = 0;
    while (start < cookie.size()) {
        auto end = cookie.find(';', start);
        auto part = trim(cookie.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (part.rfind("session=", 0) == 0) return part.substr(8);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}
static json identity(Db& db, const httplib::Request& req) {
    auto token = session_cookie(req);
    if (token.empty()) fail(401, "请先登录");
    auto user = db.one("SELECT u.id,u.username,u.role,u.is_superadmin,u.reset_until FROM sessions s "
                       "JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at>?",
                       json::array({sha256(token), now()}));
    if (user.is_null()) fail(401, "登录已失效");
    return user;
}
static json peer_identity(Db& db, const httplib::Request& req) {
    auto header = req.get_header_value("Authorization");
    if (header.rfind("Bearer ", 0) != 0) fail(401, "缺少服务器凭据");
    auto row = db.one("SELECT id,status FROM remote_incoming WHERE token_hash=?",
                      json::array({sha256(header.substr(7))}));
    if (row.is_null()) fail(401, "服务器凭据无效");
    return row;
}
static void response_json(httplib::Response& res, int code, const json& body) {
    res.status = code;
    res.set_content(body.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Content-Type-Options", "nosniff");
}
static std::string logical_arg(const httplib::Request& req) {
    return clean_path(req.has_param("path") ? req.get_param_value("path") : "");
}
static std::string url_encode(const std::string& input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result.push_back(static_cast<char>(c));
        else { result.push_back('%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    return result;
}

class Service {
    Config cfg_;
    std::mutex quota_mutex_;
    std::map<std::string, uint64_t> reservations_;
    std::atomic<size_t> uploads_{0};
public:
    explicit Service(Config cfg) : cfg_(std::move(cfg)) { initialize(cfg_); }
    const Config& cfg() const { return cfg_; }
    void dispatch(const httplib::Request& req, httplib::Response& res);
    void upload(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader);
private:
    void static_file(const httplib::Request& req, httplib::Response& res);
    void list_files(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void download(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void folder_zip(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void upload_file(Db& db, const json& user, const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader);
    void file_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void account_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void admin_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void remote_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void peer_actions(Db& db, const httplib::Request& req, httplib::Response& res);
    void proxy_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res);
    void proxy_upload(Db& db, const json& user, const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader);
};

static std::string lower_ascii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
static int64_t file_modified(const fs::path& path) {
    const auto time = fs::last_write_time(path);
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}
static void serve_file(const fs::path& path, const std::string& mime, httplib::Response& res,
                       const std::string& filename = "", const std::string& cache = "no-store",
                       bool remove_when_done = false) {
    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*file) fail(404, "文件不存在");
    auto size = fs::file_size(path);
    res.set_header("Cache-Control", cache);
    res.set_header("X-Content-Type-Options", "nosniff");
    if (!filename.empty()) res.set_header("Content-Disposition", "attachment; filename*=UTF-8''" + url_encode(filename));
    res.set_header("Accept-Ranges", "bytes");
    res.set_content_provider(static_cast<size_t>(size), mime,
        [file](size_t offset, size_t length, httplib::DataSink& sink) {
            std::array<char, CHUNK> buffer{};
            file->clear();
            file->seekg(static_cast<std::streamoff>(offset));
            if (!*file) return false;
            auto amount = static_cast<std::streamsize>(std::min(length, buffer.size()));
            file->read(buffer.data(), amount);
            auto read = file->gcount();
            return read > 0 && sink.write(buffer.data(), static_cast<size_t>(read));
        },
        [file, path, remove_when_done](bool) {
            file->close();
            if (remove_when_done) { std::error_code error; fs::remove(path, error); }
        });
}

void Service::static_file(const httplib::Request& req, httplib::Response& res) {
    std::string logical;
    try { logical = clean_path(req.path); } catch (...) { fail(404, "页面不存在"); }
    if (logical.empty()) logical = "index.html";
    auto file = cfg_.web / path_from_utf8(logical);
    if (!fs::is_regular_file(file)) fail(404, "页面不存在");
    std::string extension = lower_ascii(file.extension().u8string());
    std::string mime = "application/octet-stream";
    if (extension == ".html") mime = "text/html; charset=utf-8";
    else if (extension == ".js") mime = "text/javascript; charset=utf-8";
    else if (extension == ".css") mime = "text/css; charset=utf-8";
    else if (extension == ".svg") mime = "image/svg+xml";
    else if (extension == ".png") mime = "image/png";
    else if (extension == ".ico") mime = "image/x-icon";
    else if (extension == ".woff2") mime = "font/woff2";
    serve_file(file, mime, res, "", logical.rfind("assets/", 0) == 0 ? "public, max-age=31536000, immutable" : "no-cache");
}

void Service::list_files(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    auto logical = logical_arg(req);
    auto target = disk_path(cfg_, logical);
    if (!fs::is_directory(target)) fail(404, "文件夹不存在");
    auto grants = grants_for(db, user);
    if (!visible(db, user, logical, &grants)) fail(403, "该路径不可见");
    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(target)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.is_directory() != b.is_directory()) return a.is_directory();
        return lower_ascii(a.path().filename().u8string()) < lower_ascii(b.path().filename().u8string());
    });
    json items = json::array();
    for (const auto& entry : entries) {
        auto name = entry.path().filename().u8string();
        if (name.rfind(".upload-", 0) == 0 || entry.is_symlink()) continue;
        auto child = logical.empty() ? name : logical + "/" + name;
        if (!visible(db, user, child, &grants)) continue;
        json size = entry.is_regular_file() ? json(static_cast<int64_t>(entry.file_size())) : json(nullptr);
        items.push_back({{"name", name}, {"path", child}, {"directory", entry.is_directory()},
                         {"size", size}, {"modified", file_modified(entry.path())},
                         {"permissions", permissions_json(flags(db, user, child, &grants))}});
    }
    response_json(res, 200, {{"path", logical}, {"permissions", permissions_json(flags(db, user, logical, &grants))}, {"items", items}});
}

void Service::download(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    auto logical = logical_arg(req);
    require_permission(db, user, logical, "download");
    auto target = disk_path(cfg_, logical);
    if (!fs::is_regular_file(target)) fail(404, "文件不存在");
    serve_file(target, "application/octet-stream", res, target.filename().u8string());
}

static void put16(std::ostream& stream, uint16_t value) {
    stream.put(static_cast<char>(value)); stream.put(static_cast<char>(value >> 8));
}
static void put32(std::ostream& stream, uint32_t value) {
    put16(stream, static_cast<uint16_t>(value)); put16(stream, static_cast<uint16_t>(value >> 16));
}
static void put64(std::ostream& stream, uint64_t value) {
    put32(stream, static_cast<uint32_t>(value)); put32(stream, static_cast<uint32_t>(value >> 32));
}
static uint32_t crc32_file(const fs::path& path) {
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
            values[i] = crc;
        }
        return values;
    }();
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("读取文件失败");
    std::array<char, 64 * 1024> buffer{};
    uint32_t crc = 0xffffffffU;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        for (std::streamsize i = 0; i < input.gcount(); ++i)
            crc = (crc >> 8) ^ table[(crc ^ static_cast<unsigned char>(buffer[static_cast<size_t>(i)])) & 0xff];
    }
    return crc ^ 0xffffffffU;
}
static uint64_t output_offset(std::ostream& stream) {
    return static_cast<uint64_t>(static_cast<std::streamoff>(stream.tellp()));
}
struct ZipEntry { std::string name; uint64_t size, offset; uint32_t crc; bool large; };
static void write_zip(const fs::path& archive_path, const std::vector<fs::path>& files, const fs::path& root) {
    std::ofstream output(archive_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("无法创建 ZIP 文件");
    std::vector<ZipEntry> entries;
    std::array<char, 64 * 1024> buffer{};
    for (const auto& path : files) {
        auto name = path.lexically_relative(root).generic_u8string();
        if (name.size() > 65535) fail(400, "ZIP 文件路径过长");
        uint64_t size = fs::file_size(path), offset = output_offset(output);
        uint32_t crc = crc32_file(path);
        bool large = size >= 0xffffffffULL || offset >= 0xffffffffULL;
        put32(output, 0x04034b50U);
        put16(output, large ? 45 : 20); put16(output, 0x0800); put16(output, 0);
        put16(output, 0); put16(output, 0); put32(output, crc);
        put32(output, size >= 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(size));
        put32(output, size >= 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(size));
        put16(output, static_cast<uint16_t>(name.size()));
        put16(output, size >= 0xffffffffULL ? 20 : 0);
        output.write(name.data(), static_cast<std::streamsize>(name.size()));
        if (size >= 0xffffffffULL) {
            put16(output, 0x0001); put16(output, 16); put64(output, size); put64(output, size);
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("读取文件失败");
        uint64_t remaining = size;
        while (remaining) {
            auto amount = static_cast<std::streamsize>(std::min<uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), amount);
            if (input.gcount() != amount) throw std::runtime_error("生成 ZIP 时源文件变化");
            output.write(buffer.data(), amount);
            remaining -= static_cast<uint64_t>(amount);
        }
        entries.push_back({name, size, offset, crc, large});
    }
    uint64_t directory_start = output_offset(output);
    bool zip64 = false;
    for (const auto& entry : entries) {
        bool size64 = entry.size >= 0xffffffffULL;
        bool offset64 = entry.offset >= 0xffffffffULL;
        zip64 |= entry.large;
        uint16_t extra = static_cast<uint16_t>((size64 ? 16 : 0) + (offset64 ? 8 : 0));
        if (extra) extra += 4;
        put32(output, 0x02014b50U);
        put16(output, entry.large ? 45 : 20); put16(output, entry.large ? 45 : 20);
        put16(output, 0x0800); put16(output, 0);
        put16(output, 0); put16(output, 0); put32(output, entry.crc);
        put32(output, size64 ? 0xffffffffU : static_cast<uint32_t>(entry.size));
        put32(output, size64 ? 0xffffffffU : static_cast<uint32_t>(entry.size));
        put16(output, static_cast<uint16_t>(entry.name.size())); put16(output, extra);
        put16(output, 0); put16(output, 0); put16(output, 0); put32(output, 0);
        put32(output, offset64 ? 0xffffffffU : static_cast<uint32_t>(entry.offset));
        output.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        if (extra) {
            put16(output, 0x0001); put16(output, static_cast<uint16_t>(extra - 4));
            if (size64) { put64(output, entry.size); put64(output, entry.size); }
            if (offset64) put64(output, entry.offset);
        }
    }
    uint64_t directory_size = output_offset(output) - directory_start;
    zip64 |= entries.size() >= 65535 || directory_start >= 0xffffffffULL || directory_size >= 0xffffffffULL;
    if (zip64) {
        uint64_t zip64_offset = output_offset(output);
        put32(output, 0x06064b50U); put64(output, 44);
        put16(output, 45); put16(output, 45); put32(output, 0); put32(output, 0);
        put64(output, entries.size()); put64(output, entries.size());
        put64(output, directory_size); put64(output, directory_start);
        put32(output, 0x07064b50U); put32(output, 0); put64(output, zip64_offset); put32(output, 1);
    }
    put32(output, 0x06054b50U); put16(output, 0); put16(output, 0);
    put16(output, entries.size() >= 65535 ? 0xffff : static_cast<uint16_t>(entries.size()));
    put16(output, entries.size() >= 65535 ? 0xffff : static_cast<uint16_t>(entries.size()));
    put32(output, directory_size >= 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(directory_size));
    put32(output, directory_start >= 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(directory_start));
    put16(output, 0);
    if (!output) throw std::runtime_error("ZIP 文件写入失败");
}

void Service::folder_zip(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    auto logical = logical_arg(req);
    require_permission(db, user, logical, "download");
    auto target = disk_path(cfg_, logical);
    if (!fs::is_directory(target)) fail(404, "文件夹不存在");
    auto grants = grants_for(db, user);
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(target, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        const auto& entry = *it;
        if (entry.is_symlink(error)) { it.disable_recursion_pending(); continue; }
        auto name = entry.path().filename().u8string();
        if (name.rfind(".upload-", 0) == 0) {
            if (entry.is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (entry.is_regular_file(error)) {
            auto relative = entry.path().lexically_relative(cfg_.files).generic_u8string();
            if (flags(db, user, relative, &grants).count("download")) files.push_back(entry.path());
        }
    }
    auto archive = cfg_.data / path_from_utf8(".archive-" + random_hex(12) + ".zip");
    try {
        write_zip(archive, files, target);
        serve_file(archive, "application/zip", res,
                   (target.filename().empty() ? "files" : target.filename().u8string()) + ".zip", "no-store", true);
    } catch (...) {
        fs::remove(archive, error);
        throw;
    }
}

static void replace_file(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("替换目标文件失败");
#else
    fs::rename(temporary, target);
#endif
}

void Service::upload_file(Db& db, const json& user, const httplib::Request& req,
                          httplib::Response& res, const httplib::ContentReader& reader) {
    auto logical = logical_arg(req);
    if (logical.empty()) fail(400, "必须指定文件名");
    auto target = disk_path(cfg_, logical);
    if (fs::is_directory(target)) fail(409, "目标是文件夹");
    require_permission(db, user, fs::exists(target) ? logical : parent_of(logical),
                       fs::exists(target) ? "modify" : "upload");
    if (!fs::is_directory(target.parent_path())) fail(404, "父文件夹不存在");
    if (!req.has_header("Content-Length")) fail(411, "上传需要 Content-Length");
    if (req.has_header("Content-Encoding") && req.get_header_value("Content-Encoding") != "identity")
        fail(415, "上传不支持压缩请求体");
    uint64_t size = req.get_header_value_u64("Content-Length");
    if (uploads_.fetch_add(1) >= cfg_.max_uploads) {
        uploads_.fetch_sub(1);
        fail(503, "上传任务繁忙，请稍后重试");
    }
    struct Slot { std::atomic<size_t>& counter; ~Slot() { counter.fetch_sub(1); } } slot{uploads_};
    {
        std::lock_guard<std::mutex> lock(quota_mutex_);
        if (reservations_.count(logical)) fail(409, "该文件正在上传中");
        auto used = storage_stats(cfg_).first;
        uint64_t old = fs::is_regular_file(target) ? fs::file_size(target) : 0;
        uint64_t pending = 0;
        for (const auto& item : reservations_) pending += item.second;
        uint64_t delta = size > old ? size - old : 0;
        uint64_t limit = std::stoull(setting(db, "storage_limit"));
        if (used > limit || delta > limit - used || pending > limit - used - delta)
            fail(413, "存储空间已达到当前服务器的大小上限");
        reservations_[logical] = delta;
    }
    auto temporary = target.parent_path() / path_from_utf8(".upload-" + random_hex(12));
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("无法创建上传临时文件");
        uint64_t received = 0;
        bool complete = reader([&](const char* data, size_t count) {
            if (count > size - received) return false;
            output.write(data, static_cast<std::streamsize>(count));
            received += count;
            return static_cast<bool>(output);
        });
        output.close();
        if (!complete || received != size) fail(400, "上传内容不完整");
        {
            std::lock_guard<std::mutex> lock(quota_mutex_);
            auto used = storage_stats(cfg_).first;
            uint64_t old = fs::is_regular_file(target) ? fs::file_size(target) : 0;
            uint64_t others = 0;
            for (const auto& item : reservations_) if (item.first != logical) others += item.second;
            uint64_t limit = std::stoull(setting(db, "storage_limit"));
            if (used < old || used - old > limit || size > limit - (used - old) || others > limit - (used - old) - size)
                fail(413, "存储空间已达到当前服务器的大小上限");
            replace_file(temporary, target);
            reservations_.erase(logical);
        }
        response_json(res, 201, {{"ok", true}, {"path", logical}, {"size", size}});
    } catch (...) {
        std::error_code error;
        fs::remove(temporary, error);
        std::lock_guard<std::mutex> lock(quota_mutex_);
        reservations_.erase(logical);
        throw;
    }
}

void Service::file_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/api/list" && req.method == "GET") return list_files(db, user, req, res);
    if (req.path == "/api/download" && req.method == "GET") return download(db, user, req, res);
    if (req.path == "/api/folder.zip" && req.method == "GET") return folder_zip(db, user, req, res);
    if (req.path == "/api/mkdir" && req.method == "POST") {
        auto logical = clean_path(as_string(json_body(req), "path"));
        if (logical.empty()) fail(400, "不能创建根目录");
        require_permission(db, user, parent_of(logical), "upload");
        auto target = disk_path(cfg_, logical);
        if (!fs::is_directory(target.parent_path())) fail(404, "父文件夹不存在");
        if (fs::exists(target)) fail(409, "目标已存在");
        fs::create_directory(target);
        return response_json(res, 201, {{"ok", true}});
    }
    if (req.path == "/api/move" && req.method == "POST") {
        auto body = json_body(req);
        auto source = clean_path(as_string(body, "source")), destination = clean_path(as_string(body, "destination"));
        if (source.empty() || destination.empty() || within(destination, source)) fail(400, "移动路径无效");
        require_permission(db, user, source, "modify");
        require_permission(db, user, parent_of(destination), "upload");
        auto old = disk_path(cfg_, source), target = disk_path(cfg_, destination);
        if (!fs::exists(old) || !fs::is_directory(target.parent_path())) fail(404, "源路径或目标父文件夹不存在");
        if (fs::exists(target)) fail(409, "目标已存在");
        fs::rename(old, target);
        return response_json(res, 200, {{"ok", true}});
    }
    if (req.path == "/api/delete" && req.method == "DELETE") {
        auto logical = logical_arg(req);
        if (logical.empty()) fail(400, "不能删除根目录");
        require_permission(db, user, logical, "delete");
        auto target = disk_path(cfg_, logical);
        if (!fs::exists(target)) fail(404, "路径不存在");
        fs::remove_all(target);
        return response_json(res, 200, {{"ok", true}});
    }
    fail(404, "接口不存在");
}

void Service::account_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/api/me" && req.method == "GET")
        return response_json(res, 200, public_user(user));
    if (req.path == "/api/logout" && req.method == "POST") {
        db.query("DELETE FROM sessions WHERE token_hash=? AND user_id=?",
                 json::array({sha256(session_cookie(req)), user["id"]}));
        response_json(res, 200, {{"ok", true}});
        res.set_header("Set-Cookie", "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        return;
    }
    if (req.path == "/api/password" && req.method == "POST") {
        if (!user.value("reset_until", 0) || user.value("reset_until", 0) <= now())
            fail(403, "仅可在重置获批后的 30 分钟内修改密码");
        auto password = as_string(json_body(req), "password");
        if (password == "123456") fail(400, "新密码不能与临时密码相同");
        db.query("UPDATE users SET password=?,reset_until=0 WHERE id=?",
                 json::array({password_hash(password), user["id"]}));
        return response_json(res, 200, {{"ok", true}});
    }
    fail(404, "接口不存在");
}

static json server_info(Db& db, const Config& cfg) {
    auto [used, count] = storage_stats(cfg);
    return {{"name", setting(db, "server_name")}, {"server_id", setting(db, "server_id")},
            {"storage_path", path_string(cfg.files)},
            {"storage_limit", std::stoull(setting(db, "storage_limit"))},
            {"storage_used", used}, {"file_count", count},
            {"remotes", db.query("SELECT id,remote_name,status FROM remote_outgoing ORDER BY id")}};
}

void Service::admin_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/api/server-info") {
        if (req.method == "GET") return response_json(res, 200, server_info(db, cfg_));
        if (req.method == "POST") {
            require_super(user);
            auto body = json_body(req);
            auto name = trim(as_string(body, "name"));
            int64_t limit;
            try { limit = as_int(body, "storage_limit"); } catch (...) { fail(400, "存储上限无效"); }
            if (name.empty() || name.size() > 80 || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
                fail(400, "服务器名称无效");
            if (limit < 1 || static_cast<uint64_t>(limit) > 1024ULL * 1024 * 1024 * 1024 * 1024)
                fail(400, "存储上限需在 1 字节至 1 PB 之间");
            db.raw("BEGIN IMMEDIATE");
            try {
                db.query("UPDATE settings SET value=? WHERE key='server_name'", json::array({name}));
                db.query("UPDATE settings SET value=? WHERE key='storage_limit'", json::array({std::to_string(limit)}));
                db.raw("COMMIT");
            } catch (...) { db.raw("ROLLBACK"); throw; }
            return response_json(res, 200, server_info(db, cfg_));
        }
    }
    if (req.path == "/api/users") {
        require_admin(user);
        if (req.method == "GET") {
            json users = json::array();
            for (const auto& row : db.query("SELECT id,username,role,is_superadmin,reset_until FROM users ORDER BY id")) {
                if (role_of(user) == "superadmin" || row["role"] == "user" || row["id"] == user["id"])
                    users.push_back(public_user(row));
            }
            return response_json(res, 200, {{"users", users}});
        }
        if (req.method == "POST") {
            auto body = json_body(req);
            auto name = trim(as_string(body, "username"));
            if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '-';
            })) fail(400, "用户名只允许字母、数字、下划线、减号");
            auto role = as_string(body, "role", "user");
            if (role != "admin" && role != "user") fail(400, "角色无效");
            if (role == "admin") require_super(user);
            auto hashed = password_hash(as_string(body, "password"));
            db.raw("BEGIN IMMEDIATE");
            try {
                db.query("INSERT INTO users(username,password,role,created_at) VALUES(?,?,?,?)",
                         json::array({name, hashed, role, now()}));
                if (role == "user") db.query("INSERT INTO grants(user_id,path,upload,download,modify,delete_allowed) VALUES(?,?,?,?,?,?)",
                                              json::array({db.id(), "", 0, 1, 0, 0}));
                db.raw("COMMIT");
            } catch (...) { db.raw("ROLLBACK"); throw; }
            return response_json(res, 201, {{"ok", true}});
        }
        if (req.method == "DELETE") {
            int64_t id;
            try { id = query_int(req, "id"); } catch (...) { fail(400, "用户 ID 无效"); }
            if (id == as_int(user, "id")) fail(400, "不能删除当前账号");
            auto row = db.one("SELECT role,is_superadmin FROM users WHERE id=?", json::array({id}));
            if (row.is_null()) fail(404, "用户不存在");
            if (row["role"] == "admin") {
                require_super(user);
                if (row.value("is_superadmin", 0)) fail(400, "不能删除超级管理员");
            }
            db.query("DELETE FROM users WHERE id=?", json::array({id}));
            return response_json(res, 200, {{"ok", true}});
        }
    }
    if (req.path == "/api/grants") {
        if (req.method == "GET") {
            int64_t id = as_int(user, "id");
            if (as_string(user, "role") == "admin" && req.has_param("user_id")) {
                try { id = query_int(req, "user_id"); } catch (...) { fail(400, "用户 ID 无效"); }
            }
            return response_json(res, 200, {{"grants", db.query(
                "SELECT id,user_id,path,upload,download,modify,delete_allowed FROM grants WHERE user_id=? ORDER BY path",
                json::array({id}))}});
        }
        require_admin(user);
        if (req.method == "POST") {
            auto body = json_body(req);
            int64_t id;
            std::string logical;
            try { id = as_int(body, "user_id"); logical = clean_path(as_string(body, "path")); }
            catch (...) { fail(400, "授权参数无效"); }
            auto recipient = db.one("SELECT role FROM users WHERE id=?", json::array({id}));
            if (recipient.is_null() || recipient["role"] != "user") fail(400, "只能给普通用户授权");
            if (!fs::is_directory(disk_path(cfg_, logical))) fail(404, "授权路径必须是已存在文件夹");
            db.query("INSERT INTO grants(user_id,path,upload,download,modify,delete_allowed) VALUES(?,?,?,?,?,?) "
                     "ON CONFLICT(user_id,path) DO UPDATE SET upload=excluded.upload,download=excluded.download,"
                     "modify=excluded.modify,delete_allowed=excluded.delete_allowed",
                     json::array({id, logical, body.value("upload", false), body.value("download", false),
                                  body.value("modify", false), body.value("delete", false)}));
            return response_json(res, 200, {{"ok", true}});
        }
        if (req.method == "DELETE") {
            int64_t id;
            try { id = query_int(req, "id"); } catch (...) { fail(400, "授权 ID 无效"); }
            db.query("DELETE FROM grants WHERE id=?", json::array({id}));
            return response_json(res, 200, {{"ok", true}});
        }
    }
    if (req.path == "/api/password-requests") {
        require_admin(user);
        if (req.method == "GET") {
            json values = json::array();
            for (auto row : db.query("SELECT r.id,r.status,r.created_at,r.reviewed_at,u.username,u.role,u.is_superadmin "
                                     "FROM password_requests r JOIN users u ON u.id=r.user_id "
                                     "ORDER BY r.created_at DESC,r.id DESC LIMIT 100")) {
                if (row["role"] == "user" || role_of(user) == "superadmin") {
                    row["role"] = role_of(row);
                    row.erase("is_superadmin");
                    values.push_back(std::move(row));
                }
            }
            return response_json(res, 200, {{"requests", values}});
        }
    }
    if (req.path == "/api/password-requests/review" && req.method == "POST") {
        require_admin(user);
        auto body = json_body(req);
        int64_t id;
        try { id = as_int(body, "id"); } catch (...) { fail(400, "申请 ID 无效"); }
        auto decision = as_string(body, "decision");
        if (decision != "approve" && decision != "reject") fail(400, "审核决定无效");
        auto row = db.one("SELECT r.user_id,r.status,u.role,u.is_superadmin FROM password_requests r "
                          "JOIN users u ON u.id=r.user_id WHERE r.id=?", json::array({id}));
        if (row.is_null() || row["status"] != "pending") fail(404, "待审核申请不存在");
        if (row["role"] == "admin" && role_of(user) != "superadmin") fail(403, "管理员申请须由超级管理员审核");
        if (row.value("is_superadmin", 0)) fail(403, "超级管理员不能通过网页重置");
        auto hashed = decision == "approve" ? password_hash("123456") : "";
        db.raw("BEGIN IMMEDIATE");
        try {
            db.query("UPDATE password_requests SET status=?,reviewed_at=?,reviewer_id=? WHERE id=?",
                     json::array({decision == "approve" ? "approved" : "rejected", now(), user["id"], id}));
            if (decision == "approve") {
                db.query("UPDATE users SET password=?,reset_until=? WHERE id=?",
                         json::array({hashed, now() + RESET_WINDOW, row["user_id"]}));
                db.query("DELETE FROM sessions WHERE user_id=?", json::array({row["user_id"]}));
            }
            db.raw("COMMIT");
        } catch (...) { db.raw("ROLLBACK"); throw; }
        return response_json(res, 200, {{"ok", true}});
    }
    fail(404, "接口不存在");
}

static std::string remote_url(const std::string& input) {
    auto url = trim(input);
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) fail(400, "请输入完整的 http(s)://地址:端口，不含路径或账号");
    auto scheme = url.substr(0, scheme_end);
    auto authority = url.substr(scheme_end + 3);
    if ((scheme != "http" && scheme != "https") || authority.empty() ||
        authority.find_first_of("/?#@") != std::string::npos)
        fail(400, "请输入完整的 http(s)://地址:端口，不含路径或账号");
    std::string host;
    int port = scheme == "https" ? 443 : 80;
    if (authority.front() == '[') {
        auto end = authority.find(']');
        if (end == std::string::npos) fail(400, "地址无效");
        host = authority.substr(1, end - 1);
        if (end + 1 < authority.size()) {
            if (authority[end + 1] != ':') fail(400, "端口无效");
            try { port = std::stoi(authority.substr(end + 2)); } catch (...) { fail(400, "端口无效"); }
        }
    } else {
        auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) {
            try { port = std::stoi(authority.substr(colon + 1)); } catch (...) { fail(400, "端口无效"); }
        }
    }
    if (host.empty() || port < 1 || port > 65535) fail(400, "地址或端口无效");
    if (scheme == "http") {
        bool private_ip = host == "::1" || lower_ascii(host).rfind("fd", 0) == 0 ||
                          host.rfind("127.", 0) == 0 || host.rfind("10.", 0) == 0 ||
                          host.rfind("192.168.", 0) == 0 || host.rfind("172.", 0) == 0;
        if (host.rfind("172.", 0) == 0) {
            int second = 0;
            try { second = std::stoi(host.substr(4)); } catch (...) { second = 0; }
            private_ip = second >= 16 && second <= 31;
        }
        if (!private_ip) fail(400, "公网或域名连接必须使用 HTTPS");
    }
    return scheme + "://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

static httplib::Client make_client(const std::string& base) {
    httplib::Client client(base);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(60, 0);
    client.set_write_timeout(60, 0);
    client.set_follow_location(false);
    return client;
}
static std::pair<int, json> peer_call(const std::string& base, const std::string& method,
                                      const std::string& endpoint, const std::string& token = "",
                                      const json* payload = nullptr) {
    auto client = make_client(base);
    httplib::Headers headers{{"Accept", "application/json"}};
    if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
    httplib::Result result = method == "GET" ? client.Get(endpoint, headers) :
                             method == "POST" ? client.Post(endpoint, headers, payload ? payload->dump() : "{}", "application/json") :
                             method == "DELETE" ? client.Delete(endpoint, headers) : httplib::Result(nullptr, httplib::Error::Unknown);
    if (!result) fail(502, "连接对方服务器失败");
    if (result->body.size() > MAX_JSON) fail(502, "远端响应过大");
    auto data = json::parse(result->body, nullptr, false);
    if (!data.is_object()) fail(502, "远端响应格式无效");
    return {result->status, data};
}

void Service::peer_actions(Db& db, const httplib::Request& req, httplib::Response& res) {
    if (req.path == "/api/peer/request" && req.method == "POST") {
        auto body = json_body(req);
        auto source_id = as_string(body, "server_id"), source_name = trim(as_string(body, "server_name"));
        auto token = as_string(body, "token");
        if (source_id.size() != 32 || source_name.empty() || source_name.size() > 80 || token.size() < 40)
            fail(400, "服务器申请信息无效");
        if (source_id == setting(db, "server_id")) fail(400, "不能连接本服务器");
        auto id = random_hex(16);
        db.query("INSERT INTO remote_incoming(id,source_id,source_name,token_hash,status,created_at) "
                 "VALUES(?,?,?,?,'pending',?)",
                 json::array({id, source_id, source_name, sha256(token), now()}));
        return response_json(res, 202, {{"request_id", id}, {"server_id", setting(db, "server_id")},
                                        {"server_name", setting(db, "server_name")}, {"status", "pending"}});
    }
    if (req.path == "/api/peer/status" && req.method == "GET") {
        auto row = peer_identity(db, req);
        if (!req.has_param("request_id") || req.get_param_value("request_id") != as_string(row, "id"))
            fail(403, "申请不匹配");
        return response_json(res, 200, {{"status", row["status"]}, {"server_name", setting(db, "server_name")}});
    }
    if (req.path.rfind("/api/peer/files/", 0) == 0) {
        auto row = peer_identity(db, req);
        if (row["status"] != "approved") fail(403, "外连尚未获批");
        auto operation = req.path.substr(std::string("/api/peer/files/").size());
        json user = {{"role", "peer"}, {"peer_id", row["id"]}};
        if (operation == "list" && req.method == "GET") return list_files(db, user, req, res);
        if (operation == "download" && req.method == "GET") return download(db, user, req, res);
        if (operation == "folder.zip" && req.method == "GET") return folder_zip(db, user, req, res);
        if ((operation == "mkdir" || operation == "move") && req.method == "POST") {
            auto rewritten = req;
            rewritten.path = "/api/" + operation;
            return file_actions(db, user, rewritten, res);
        }
        if (operation == "delete" && req.method == "DELETE") {
            auto rewritten = req;
            rewritten.path = "/api/delete";
            return file_actions(db, user, rewritten, res);
        }
        fail(404, "远端接口不存在");
    }
    fail(404, "远端接口不存在");
}

void Service::remote_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    require_super(user);
    if (req.path == "/api/remotes" && req.method == "GET")
        return response_json(res, 200, {{"outgoing", db.query(
            "SELECT id,url,remote_name,status,created_at FROM remote_outgoing ORDER BY id DESC")}});
    if (req.path == "/api/remotes" && req.method == "POST") {
        auto url = remote_url(as_string(json_body(req), "url"));
        auto token = b64(random_bytes(48));
        json payload = {{"server_id", setting(db, "server_id")},
                        {"server_name", setting(db, "server_name")}, {"token", token}};
        auto [status, result] = peer_call(url, "POST", "/api/peer/request", "", &payload);
        if (status != 202 || !result.contains("request_id") || !result.contains("server_id") || !result.contains("server_name"))
            fail(502, as_string(result, "error", "对方不是兼容的 MyFolder 后端"));
        db.query("INSERT INTO remote_outgoing(url,remote_id,remote_name,token,request_id,status,created_at) "
                 "VALUES(?,?,?,?,?,'pending',?)",
                 json::array({url, result["server_id"], result["server_name"], token, result["request_id"], now()}));
        return response_json(res, 201, {{"ok", true}});
    }
    if (req.path == "/api/remotes/sync" && req.method == "POST") {
        json_body(req);
        int updated = 0;
        json errors = json::array();
        for (const auto& row : db.query("SELECT * FROM remote_outgoing WHERE status IN ('pending','approved')")) {
            try {
                auto endpoint = "/api/peer/status?request_id=" + url_encode(as_string(row, "request_id"));
                auto [status, result] = peer_call(as_string(row, "url"), "GET", endpoint, as_string(row, "token"));
                auto state = as_string(result, "status");
                if (status == 200 && (state == "pending" || state == "approved" || state == "rejected")) {
                    db.query("UPDATE remote_outgoing SET status=?,remote_name=? WHERE id=?",
                             json::array({state, as_string(result, "server_name", as_string(row, "remote_name")).substr(0, 80), row["id"]}));
                    ++updated;
                } else errors.push_back(row["remote_name"]);
            } catch (...) { errors.push_back(row["remote_name"]); }
        }
        return response_json(res, 200, {{"ok", true}, {"updated", updated}, {"errors", errors}});
    }
    if (req.path == "/api/remotes/incoming" && req.method == "GET") {
        json incoming = json::array();
        for (auto row : db.query("SELECT id,source_name,status,created_at FROM remote_incoming ORDER BY created_at DESC")) {
            row["grants"] = db.query("SELECT id,path,upload,download,modify,delete_allowed FROM remote_grants "
                                      "WHERE incoming_id=? ORDER BY path", json::array({row["id"]}));
            incoming.push_back(std::move(row));
        }
        return response_json(res, 200, {{"incoming", incoming}});
    }
    if (req.path == "/api/remotes/incoming/review" && req.method == "POST") {
        auto body = json_body(req);
        auto id = as_string(body, "id"), decision = as_string(body, "decision");
        if (decision != "approve" && decision != "reject") fail(400, "审核决定无效");
        auto row = db.one("SELECT status FROM remote_incoming WHERE id=?", json::array({id}));
        if (row.is_null() || row["status"] != "pending") fail(404, "待审核连接不存在");
        if (decision == "approve" && db.one("SELECT id FROM remote_grants WHERE incoming_id=? LIMIT 1", json::array({id})).is_null())
            fail(400, "请先为对方设置至少一条路径权限");
        db.query("UPDATE remote_incoming SET status=? WHERE id=?",
                 json::array({decision == "approve" ? "approved" : "rejected", id}));
        return response_json(res, 200, {{"ok", true}});
    }
    if (req.path == "/api/remotes/incoming/grants" && req.method == "POST") {
        auto body = json_body(req);
        auto id = as_string(body, "id");
        auto row = db.one("SELECT status FROM remote_incoming WHERE id=?", json::array({id}));
        if (row.is_null() || row["status"] == "rejected") fail(404, "连接申请不存在");
        auto logical = clean_path(as_string(body, "path"));
        if (!fs::is_directory(disk_path(cfg_, logical))) fail(404, "授权路径必须是已存在文件夹");
        bool upload = body.value("upload", false), download = body.value("download", false);
        bool modify = body.value("modify", false), remove = body.value("delete", false);
        if (!(upload || download || modify || remove)) fail(400, "请至少选择一项权限");
        db.query("INSERT INTO remote_grants(incoming_id,path,upload,download,modify,delete_allowed) VALUES(?,?,?,?,?,?) "
                 "ON CONFLICT(incoming_id,path) DO UPDATE SET upload=excluded.upload,download=excluded.download,"
                 "modify=excluded.modify,delete_allowed=excluded.delete_allowed",
                 json::array({id, logical, upload, download, modify, remove}));
        return response_json(res, 200, {{"ok", true}});
    }
    if (req.path == "/api/remotes/incoming/grants" && req.method == "DELETE") {
        int64_t id;
        try { id = query_int(req, "id"); } catch (...) { fail(400, "授权 ID 无效"); }
        db.query("DELETE FROM remote_grants WHERE id=?", json::array({id}));
        return response_json(res, 200, {{"ok", true}});
    }
    fail(404, "接口不存在");
}

static std::pair<int64_t, std::string> proxy_route(const std::string& path) {
    static const std::string prefix = "/api/remote/";
    if (path.rfind(prefix, 0) != 0) fail(404, "远端接口不存在");
    auto remaining = path.substr(prefix.size());
    auto slash = remaining.find('/');
    if (slash == std::string::npos || remaining.find('/', slash + 1) != std::string::npos)
        fail(404, "远端接口不存在");
    int64_t id;
    try { id = std::stoll(remaining.substr(0, slash)); }
    catch (...) { fail(400, "服务器 ID 无效"); }
    return {id, remaining.substr(slash + 1)};
}
static json outgoing(Db& db, int64_t id) {
    auto row = db.one("SELECT url,token,status FROM remote_outgoing WHERE id=?", json::array({id}));
    if (row.is_null() || row["status"] != "approved") fail(404, "外连服务器尚未获批");
    return row;
}
static bool valid_proxy_method(const std::string& operation, const std::string& method) {
    if (operation == "list" || operation == "download" || operation == "folder.zip") return method == "GET";
    if (operation == "upload") return method == "PUT";
    if (operation == "mkdir" || operation == "move") return method == "POST";
    return operation == "delete" && method == "DELETE";
}
static json intersect_permissions(const json& remote, const std::set<std::string>& local) {
    json result = json::array();
    if (!remote.is_array()) return result;
    for (const auto& key : remote) if (key.is_string() && local.count(key.get<std::string>())) result.push_back(key);
    return result;
}

void Service::proxy_actions(Db& db, const json& user, const httplib::Request& req, httplib::Response& res) {
    auto [id, operation] = proxy_route(req.path);
    if (!valid_proxy_method(operation, req.method)) fail(404, "远端接口不存在");
    if (operation == "upload") fail(404, "远端接口不存在");
    auto remote = outgoing(db, id);
    std::string endpoint = "/api/peer/files/" + operation;
    std::string logical;
    json body;
    if (operation == "list" || operation == "download" || operation == "folder.zip" || operation == "delete") {
        logical = logical_arg(req);
        endpoint += "?path=" + url_encode(logical);
        if (operation == "list" && !visible(db, user, logical)) fail(403, "该路径不可见");
        if (operation == "download" || operation == "folder.zip") require_permission(db, user, logical, "download");
        if (operation == "delete") require_permission(db, user, logical, "delete");
    } else {
        body = json_body(req);
        if (operation == "mkdir") require_permission(db, user, parent_of(clean_path(as_string(body, "path"))), "upload");
        if (operation == "move") {
            require_permission(db, user, clean_path(as_string(body, "source")), "modify");
            require_permission(db, user, parent_of(clean_path(as_string(body, "destination"))), "upload");
        }
    }
    if (operation == "download" || operation == "folder.zip") {
        auto temporary = cfg_.data / path_from_utf8(".proxy-" + random_hex(12));
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("无法创建外连下载临时文件");
        std::string error_body;
        int upstream_status = 0;
        std::string mime = operation == "folder.zip" ? "application/zip" : "application/octet-stream";
        auto client = make_client(as_string(remote, "url"));
        httplib::Headers headers{{"Authorization", "Bearer " + as_string(remote, "token")}};
        auto result = client.Get(endpoint, headers,
            [&](const httplib::Response& response) {
                upstream_status = response.status;
                auto content_type = response.get_header_value("Content-Type");
                if (!content_type.empty()) mime = content_type;
                return true;
            },
            [&](const char* data, size_t count) {
                if (upstream_status >= 400) {
                    if (error_body.size() + count > MAX_JSON) return false;
                    error_body.append(data, count);
                    return true;
                }
                file.write(data, static_cast<std::streamsize>(count));
                return static_cast<bool>(file);
            });
        file.close();
        if (!result) { fs::remove(temporary); fail(502, "访问外连服务器失败"); }
        if (result->status >= 400) {
            fs::remove(temporary);
            auto data = json::parse(error_body, nullptr, false);
            if (!data.is_object()) fail(502, "远端响应格式无效");
            return response_json(res, result->status, data);
        }
        try {
            auto filename = operation == "folder.zip" ?
                (logical.empty() ? "files" : path_from_utf8(logical).filename().u8string()) + ".zip" :
                path_from_utf8(logical).filename().u8string();
            serve_file(temporary, mime, res, filename, "no-store", true);
        } catch (...) { fs::remove(temporary); throw; }
        return;
    }
    auto [status, result] = peer_call(as_string(remote, "url"), req.method, endpoint, as_string(remote, "token"),
                                      body.is_object() ? &body : nullptr);
    if (operation == "list" && status == 200) {
        auto grants = grants_for(db, user);
        json items = json::array();
        for (auto item : result.value("items", json::array())) {
            auto child = as_string(item, "path");
            if (!visible(db, user, child, &grants)) continue;
            item["permissions"] = intersect_permissions(item.value("permissions", json::array()), flags(db, user, child, &grants));
            items.push_back(std::move(item));
        }
        result["items"] = items;
        result["permissions"] = intersect_permissions(result.value("permissions", json::array()), flags(db, user, logical, &grants));
    }
    response_json(res, status, result);
}

void Service::proxy_upload(Db& db, const json& user, const httplib::Request& req,
                           httplib::Response& res, const httplib::ContentReader& reader) {
    auto [id, operation] = proxy_route(req.path);
    if (operation != "upload") fail(404, "远端接口不存在");
    auto remote = outgoing(db, id);
    auto logical = logical_arg(req);
    if (!flags(db, user, parent_of(logical)).count("upload") && !flags(db, user, logical).count("modify"))
        fail(403, "该路径没有上传或修改权限");
    if (!req.has_header("Content-Length")) fail(411, "上传需要 Content-Length");
    if (req.has_header("Content-Encoding") && req.get_header_value("Content-Encoding") != "identity")
        fail(415, "上传不支持压缩请求体");
    uint64_t size = req.get_header_value_u64("Content-Length");
    auto temporary = cfg_.data / path_from_utf8(".proxy-" + random_hex(12));
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("无法创建外连上传临时文件");
        uint64_t received = 0;
        bool complete = reader([&](const char* data, size_t count) {
            if (count > size - received) return false;
            output.write(data, static_cast<std::streamsize>(count));
            received += count;
            return static_cast<bool>(output);
        });
        output.close();
        if (!complete || received != size) fail(400, "上传内容不完整");
        auto source = std::make_shared<std::ifstream>(temporary, std::ios::binary);
        if (!*source) throw std::runtime_error("读取外连上传临时文件失败");
        auto client = make_client(as_string(remote, "url"));
        httplib::Headers headers{{"Authorization", "Bearer " + as_string(remote, "token")}};
        auto result = client.Put("/api/peer/files/upload?path=" + url_encode(logical), headers,
            static_cast<size_t>(size), [source](size_t offset, size_t length, httplib::DataSink& sink) {
                std::array<char, CHUNK> buffer{};
                source->clear();
                source->seekg(static_cast<std::streamoff>(offset));
                auto amount = static_cast<std::streamsize>(std::min(length, buffer.size()));
                source->read(buffer.data(), amount);
                auto read = source->gcount();
                return read > 0 && sink.write(buffer.data(), static_cast<size_t>(read));
            }, "application/octet-stream");
        source->close();
        fs::remove(temporary);
        if (!result) fail(502, "访问外连服务器失败");
        if (result->body.size() > MAX_JSON) fail(502, "远端响应过大");
        auto data = json::parse(result->body, nullptr, false);
        if (!data.is_object()) fail(502, "远端响应格式无效");
        response_json(res, result->status, data);
    } catch (...) {
        std::error_code error;
        fs::remove(temporary, error);
        throw;
    }
}

static void check_origin(const httplib::Request& req) {
    if (req.method != "POST" && req.method != "PUT" && req.method != "DELETE") return;
    auto origin = req.get_header_value("Origin");
    if (origin.empty()) return;
    auto scheme = req.get_header_value("X-Forwarded-Proto") == "https" ? "https" : "http";
    if (origin != std::string(scheme) + "://" + req.get_header_value("Host"))
        fail(403, "请求来源不匹配");
}

void Service::dispatch(const httplib::Request& req, httplib::Response& res) {
    if (req.path.rfind("/api/", 0) != 0) {
        if (req.method != "GET") fail(405, "不支持的操作");
        return static_file(req, res);
    }
    check_origin(req);
    Db db(cfg_.db);
    if (req.path == "/api/login" && req.method == "POST") {
        auto body = json_body(req);
        auto name = as_string(body, "username"), password = as_string(body, "password");
        auto row = db.one("SELECT * FROM users WHERE username=?", json::array({name}));
        if (row.is_null() || !verify_password(password, as_string(row, "password"))) fail(401, "用户名或密码错误");
        if (row.value("reset_until", 0) && row.value("reset_until", 0) <= now())
            fail(401, "临时密码已过期，请重新提交忘记密码申请");
        auto token = b64(random_bytes(32));
        for (char& c : token) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
        token.erase(std::remove(token.begin(), token.end(), '='), token.end());
        db.query("INSERT INTO sessions VALUES(?,?,?)", json::array({sha256(token), row["id"], now() + 86400}));
        response_json(res, 200, public_user(row));
        res.set_header("Set-Cookie", "session=" + token + "; Path=/; HttpOnly; SameSite=Strict" +
                                     (req.get_header_value("X-Forwarded-Proto") == "https" ? "; Secure" : ""));
        return;
    }
    if (req.path == "/api/password-requests" && req.method == "POST") {
        auto name = trim(as_string(json_body(req), "username"));
        auto row = db.one("SELECT id,is_superadmin FROM users WHERE username=?", json::array({name}));
        if (!row.is_null() && !row.value("is_superadmin", 0)) {
            auto exists = db.one("SELECT id FROM password_requests WHERE user_id=? AND status='pending'", json::array({row["id"]}));
            if (exists.is_null()) db.query("INSERT INTO password_requests(user_id,status,created_at) VALUES(?,'pending',?)",
                                           json::array({row["id"], now()}));
        }
        return response_json(res, 200, {{"ok", true}, {"message", "如账号存在，申请已提交给上级管理员"}});
    }
    if (req.path.rfind("/api/peer/", 0) == 0) return peer_actions(db, req, res);
    auto user = identity(db, req);
    if (req.path == "/api/me" || req.path == "/api/logout" || req.path == "/api/password")
        return account_actions(db, user, req, res);
    if (req.path.rfind("/api/remote/", 0) == 0) return proxy_actions(db, user, req, res);
    if (req.path.rfind("/api/remotes", 0) == 0) return remote_actions(db, user, req, res);
    if (req.path == "/api/users" || req.path == "/api/grants" ||
        req.path == "/api/password-requests" || req.path == "/api/password-requests/review" ||
        req.path == "/api/server-info") return admin_actions(db, user, req, res);
    return file_actions(db, user, req, res);
}

void Service::upload(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
    check_origin(req);
    Db db(cfg_.db);
    if (req.path == "/api/peer/files/upload") {
        auto peer = peer_identity(db, req);
        if (peer["status"] != "approved") fail(403, "外连尚未获批");
        json user = {{"role", "peer"}, {"peer_id", peer["id"]}};
        return upload_file(db, user, req, res, reader);
    }
    auto user = identity(db, req);
    if (req.path == "/api/upload") return upload_file(db, user, req, res, reader);
    if (req.path.rfind("/api/remote/", 0) == 0) return proxy_upload(db, user, req, res, reader);
    fail(404, "接口不存在");
}

static void handle_errors(const std::function<void()>& work, httplib::Response& res) {
    try { work(); }
    catch (const ApiError& error) { response_json(res, error.status, {{"error", error.what()}}); }
    catch (const std::invalid_argument& error) { response_json(res, 400, {{"error", error.what()}}); }
    catch (const nlohmann::json::exception& error) { response_json(res, 400, {{"error", error.what()}}); }
    catch (const std::exception& error) {
        std::cerr << "backend error: " << error.what() << std::endl;
        response_json(res, 500, {{"error", "服务器内部错误"}});
    }
}

// Keep the domain handlers independent of the HTTP server while Drogon owns all
// listening sockets, request body spooling, and response streaming.
static bool upload_route(const std::string& path) {
    return path == "/api/upload" || path == "/api/peer/files/upload" ||
           (path.rfind("/api/remote/", 0) == 0 &&
            path.size() >= 7 && path.compare(path.size() - 7, 7, "/upload") == 0);
}

static httplib::Request adapt_request(const drogon::HttpRequestPtr& input, bool upload) {
    httplib::Request req;
    switch (input->method()) {
    case drogon::Get: req.method = "GET"; break;
    case drogon::Post: req.method = "POST"; break;
    case drogon::Put: req.method = "PUT"; break;
    case drogon::Delete: req.method = "DELETE"; break;
    default: req.method = "OTHER"; break;
    }
    req.path = input->getPath();
    req.target = req.path + (input->getQuery().empty() ? "" : "?" + input->getQuery());
    for (const auto& [key, value] : input->getHeaders()) req.headers.emplace(key, value);
    req.headers.erase("Cookie");
    if (!input->getCookie("session").empty())
        req.headers.emplace("Cookie", "session=" + input->getCookie("session"));
    for (const auto& [key, value] : input->getParameters()) req.params.emplace(key, value);
    if (!upload && input->bodyLength() <= MAX_JSON)
        req.body.assign(input->bodyData(), input->bodyLength());
    return req;
}

struct StreamState {
    httplib::ContentProvider provider;
    httplib::ContentProviderResourceReleaser releaser;
    size_t offset = 0;
    size_t remaining = 0;
    bool successful = true;
    bool released = false;
    ~StreamState() { release(); }
    void release() {
        if (!released) {
            released = true;
            if (releaser) releaser(successful && remaining == 0);
        }
    }
};

static drogon::HttpResponsePtr adapt_response(httplib::Response& source,
                                              const drogon::HttpRequestPtr& request) {
    const bool streaming = static_cast<bool>(source.content_provider_);
    size_t begin = 0;
    size_t length = source.content_length_;
    if (streaming) {
        auto range = request->getHeader("range");
        if (!range.empty()) {
            bool valid = false;
            if (range.rfind("bytes=", 0) == 0 && range.find(',') == std::string::npos) {
                auto spec = range.substr(6);
                auto dash = spec.find('-');
                if (dash != std::string::npos) {
                    try {
                        if (dash == 0) {
                            auto suffix = std::stoull(spec.substr(1));
                            if (suffix > 0 && length > 0) {
                                begin = suffix >= length ? 0 : length - static_cast<size_t>(suffix);
                                valid = true;
                            }
                        } else {
                            auto first = std::stoull(spec.substr(0, dash));
                            if (first < length) {
                                begin = static_cast<size_t>(first);
                                if (dash + 1 < spec.size()) {
                                    auto last = std::stoull(spec.substr(dash + 1));
                                    if (last >= first) length = std::min(length, static_cast<size_t>(last + 1));
                                }
                                valid = length > begin;
                            }
                        }
                    } catch (...) { valid = false; }
                }
            }
            if (!valid) {
                auto error = drogon::HttpResponse::newHttpResponse();
                error->setStatusCode(drogon::k416RequestedRangeNotSatisfiable);
                error->addHeader("Content-Range", "bytes */" + std::to_string(source.content_length_));
                return error;
            }
            source.status = 206;
        }
        length -= begin;
    }
    drogon::HttpResponsePtr result;
    if (streaming && length > 0) {
        auto state = std::make_shared<StreamState>();
        state->provider = std::move(source.content_provider_);
        state->releaser = std::move(source.content_provider_resource_releaser_);
        state->offset = begin;
        state->remaining = length;
        result = drogon::HttpResponse::newStreamResponse(
            [state](char* buffer, size_t capacity) -> size_t {
                if (!buffer) { state->release(); return 0; }
                if (state->remaining == 0) return 0;
                size_t written = 0;
                httplib::DataSink sink;
                sink.write = [&](const char* data, size_t size) {
                    if (size > capacity - written || size > state->remaining) return false;
                    std::memcpy(buffer + written, data, size);
                    written += size;
                    return true;
                };
                sink.is_writable = [] { return true; };
                bool ok = state->provider(state->offset, std::min(capacity, state->remaining), sink);
                state->successful = state->successful && ok && written > 0;
                state->offset += written;
                state->remaining -= written;
                return written;
            });
    } else {
        result = drogon::HttpResponse::newHttpResponse();
        result->setBody(std::move(source.body));
    }
    result->setStatusCode(static_cast<drogon::HttpStatusCode>(source.status > 0 ? source.status : 200));
    for (const auto& [key, value] : source.headers) {
        if (lower_ascii(key) == "content-type") result->setContentTypeString(value);
        else result->addHeader(key, value);
    }
    if (streaming) {
        result->addHeader("Content-Length", std::to_string(length));
        if (source.status == 206)
            result->addHeader("Content-Range", "bytes " + std::to_string(begin) + "-" +
                              std::to_string(begin + length - 1) + "/" +
                              std::to_string(source.content_length_));
    }
    return result;
}

int main(int argc, char** argv) {
    try {
        auto cfg = config();
        if (argc > 1 && std::string(argv[1]) == "--init-admin") {
            initialize(cfg);
            Db db(cfg.db);
            auto existing = db.one("SELECT id FROM users WHERE role='admin' LIMIT 1");
            if (!existing.is_null()) {
                std::cout << "管理员已存在，未修改账号。" << std::endl;
                return 0;
            }
            auto password = random_hex(16);
            db.query("INSERT INTO users(username,password,role,created_at,is_superadmin) VALUES(?,?,'admin',?,1)",
                     json::array({"admin", password_hash(password), now()}));
            std::cout << "用户名: admin\n初始密码: " << password << std::endl;
            return 0;
        }
        Service service(cfg);
        httplib::ThreadPool workers(static_cast<size_t>(cfg.workers), static_cast<size_t>(cfg.workers * 4));
        drogon::app().setClientMaxBodySize(cfg.max_request_bytes);
        drogon::app().setClientMaxMemoryBodySize(64 * 1024);
        drogon::app().setUploadPath(path_string(cfg.data));
        drogon::app().setDocumentRoot(path_string(cfg.web));
        drogon::app().setThreadNum(2);
        drogon::app().addListener(cfg.host, static_cast<uint16_t>(cfg.port));
        drogon::app().registerHandlerViaRegex("/.*",
            [&](const drogon::HttpRequestPtr& input,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                if (!workers.enqueue([&, input, callback]() mutable {
                    httplib::Response output;
                    try {
                        auto path = input->getPath();
                        bool upload = input->method() == drogon::Put && upload_route(path);
                        auto req = adapt_request(input, upload);
                        if (input->method() == drogon::Put && !upload) {
                            response_json(output, 404, {{"error", "接口不存在"}});
                        } else if (!upload && input->bodyLength() > MAX_JSON) {
                            response_json(output, 413, {{"error", "请求内容过大"}});
                        } else if (upload) {
                            httplib::ContentReader reader(
                                [input](httplib::ContentReceiver receive) {
                                    auto data = input->bodyData();
                                    size_t size = input->bodyLength();
                                    for (size_t pos = 0; pos < size; pos += CHUNK)
                                        if (!receive(data + pos, std::min(CHUNK, size - pos))) return false;
                                    return true;
                                },
                                [](httplib::MultipartContentHeader, httplib::ContentReceiver) { return false; });
                            handle_errors([&] { service.upload(req, output, reader); }, output);
                        } else {
                            handle_errors([&] { service.dispatch(req, output); }, output);
                        }
                        callback(adapt_response(output, input));
                    } catch (const std::exception& error) {
                        std::cerr << "HTTP adapter error: " << error.what() << std::endl;
                        response_json(output, 500, {{"error", "服务器内部错误"}});
                        callback(adapt_response(output, input));
                    }
                })) {
                    auto busy = drogon::HttpResponse::newHttpResponse();
                    busy->setStatusCode(drogon::k503ServiceUnavailable);
                    callback(busy);
                }
            });
        std::cout << "MyFolder LAN Drogon listening on " << cfg.host << ':' << cfg.port << std::endl;
        drogon::app().run();
        workers.shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "启动失败: " << error.what() << std::endl;
        return 1;
    }
}
