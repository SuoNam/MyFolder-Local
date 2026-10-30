// Separate roots: legacy files are never silently made public or assigned to a user.
static void initialize_spaces(Db& db) {
    db.raw(R"SQL(
CREATE TABLE IF NOT EXISTS user_storage(
 user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
 storage_key TEXT NOT NULL UNIQUE,quota INTEGER NOT NULL DEFAULT 5368709120 CHECK(quota>0));
CREATE TABLE IF NOT EXISTS public_grants(
 user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
 upload INTEGER NOT NULL DEFAULT 0,modify INTEGER NOT NULL DEFAULT 0,delete_allowed INTEGER NOT NULL DEFAULT 0);
)SQL");
    for (const auto& row : db.query("SELECT id FROM users"))
        db.query("INSERT OR IGNORE INTO user_storage(user_id,storage_key) VALUES(?,?)", json::array({row["id"], random_hex(16)}));
}

static json ensure_storage(Db& db, const json& id) {
    auto row = db.one("SELECT * FROM user_storage WHERE user_id=?", json::array({id}));
    if (row.is_null()) {
        db.query("INSERT OR IGNORE INTO user_storage(user_id,storage_key) VALUES(?,?)", json::array({id, random_hex(16)}));
        row = db.one("SELECT * FROM user_storage WHERE user_id=?", json::array({id}));
    }
    return row;
}
static Config space_config(const Config& base, Db& db, const json& user) {
    auto cfg = base;
    const auto space = user.value("_space", "legacy");
    if (space == "legacy") return cfg;
    if (space != "personal" && space != "public") fail(404, "文件区不存在");
    auto container = base.data / space;
    if (fs::is_symlink(fs::symlink_status(container))) fail(400, "存储根目录不能是符号链接");
    cfg.files = space == "personal" ? container / as_string(ensure_storage(db, user["id"]), "storage_key") : container;
    if (fs::is_symlink(fs::symlink_status(cfg.files))) fail(400, "存储根目录不能是符号链接");
    fs::create_directories(cfg.files);
    return cfg;
}
static void check_capacity(uint64_t used, uint64_t old, uint64_t size, uint64_t pending,
                           uint64_t limit, const std::string& message) {
    // Allow shrinking/deleting while over quota; shrinking does not grant new write capacity.
    const auto delta = size > old ? size - old : 0;
    if (delta && (used > limit || delta > limit - used || pending > limit - used - delta)) fail(413, message);
}
static std::pair<json, httplib::Request> scoped_request(const json& user, const httplib::Request& req) {
    const auto suffix = req.path.substr(std::string("/api/spaces/").size());
    const auto slash = suffix.find('/');
    if (slash == std::string::npos) fail(404, "文件区接口不存在");
    const auto space = suffix.substr(0, slash), operation = suffix.substr(slash + 1);
    if (space != "personal" && space != "public") fail(404, "文件区不存在");
    if (operation != "list" && operation != "download" && operation != "folder.zip" && operation != "mkdir" &&
        operation != "move" && operation != "delete" && operation != "upload") fail(404, "文件区接口不存在");
    auto scoped = user; scoped["_space"] = space;
    auto rewritten = req; rewritten.path = "/api/" + operation;
    return {scoped, rewritten};
}
