#include "gallery.h"
#include "config.h"
#include "epd_driver.h"
#include <LittleFS.h>
#include <Preferences.h>

#if EPD_BPP < 2
#error "Gallery requires a 2bpp color panel (EPD_BPP >= 2)"
#endif

static const char *IMG_DIR = "/img";
static const int   MAX_IMAGES = 20;
static const uint32_t MIN_FREE_BYTES = COLOR_BUF_LEN + 32 * 1024;

static bool fsReady = false;
static File uploadFile;
static String uploadPath;   // full path of the in-progress upload file
static size_t uploadWritten = 0;
static size_t uploadOverflow = 0;
static int uploadId = -1;

static Preferences galPrefs;

// ── Helpers ─────────────────────────────────────────────────

static String sanitizeName(const String &raw) {
    String name;
    name.reserve(min((int)raw.length(), 48));
    for (unsigned int i = 0; i < raw.length() && name.length() < 48; i++) {
        char c = raw.charAt(i);
        if (c < 32 || c == '/' || c == '\\' || c == '?' || c == '*' ||
            c == '"' || c == ':' || c == '<' || c == '>' || c == '|' || c == '.') {
            continue;
        }
        name += c;
    }
    if (name.length() == 0) name = "image";
    return name;
}

// Parse "123_my cat" -> id=123. Returns -1 on parse failure.
static int idFromFilename(const char *name) {
    int id = 0;
    int i = 0;
    while (name[i] >= '0' && name[i] <= '9') {
        id = id * 10 + (name[i] - '0');
        if (id > 999999) return -1;
        i++;
    }
    if (i == 0 || name[i] != '_') return -1;
    return id;
}

// Find the stored file path for an id. Returns false if not found.
static bool pathForId(int id, String &pathOut) {
    if (!fsReady || id < 0) return false;
    File dir = LittleFS.open(IMG_DIR);
    if (!dir || !dir.isDirectory()) return false;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String fname = f.name();
        // Strip leading directory portion returned by some cores ("/img/1_a.raw")
        int slash = fname.lastIndexOf('/');
        if (slash >= 0) fname = fname.substring(slash + 1);
        f.close();
        if (idFromFilename(fname.c_str()) == id) {
            pathOut = String(IMG_DIR) + "/" + fname;
            return true;
        }
    }
    return false;
}

static bool writeCurrentId(int id) {
    galPrefs.begin("gal", false);
    galPrefs.putInt("cur", id);
    galPrefs.end();
    return true;
}

static int nextId() {
    galPrefs.begin("gal", true);
    int id = galPrefs.getInt("next", 1);
    galPrefs.end();
    galPrefs.begin("gal", false);
    galPrefs.putInt("next", id + 1);
    galPrefs.end();
    return id;
}

static String jsonEscape(const String &s) {
    String out;
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else out += c;
    }
    return out;
}

// ── Public API ──────────────────────────────────────────────

bool galleryInit() {
    if (!LittleFS.begin(true)) {  // true = format on failure
        Serial.println("[GAL] LittleFS mount failed");
        return false;
    }
    if (!LittleFS.exists(IMG_DIR)) {
        LittleFS.mkdir(IMG_DIR);
    }
    // RW 模式打开一次：命名空间不存在时会自动创建，
    // 避免后续只读打开（galleryCurrentId 等）每次报 nvs_open NOT_FOUND
    galPrefs.begin("gal", false);
    galPrefs.end();

    fsReady = true;
    Serial.printf("[GAL] LittleFS ready, %u/%u bytes used, %d image(s)\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
                  galleryCount());
    return true;
}

uint32_t galleryFreeBytes() {
    if (!fsReady) return 0;
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

int galleryCount() {
    if (!fsReady) return 0;
    File dir = LittleFS.open(IMG_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        f.close();
        n++;
    }
    return n;
}

String galleryListJson() {
    String json = "{\"images\":[";
    int current = galleryCurrentId();
    if (fsReady) {
        // One directory scan captures id/name/size; then sort by id ascending
        struct Entry { int id; String name; size_t size; };
        Entry entries[MAX_IMAGES];
        int n = 0;
        File dir = LittleFS.open(IMG_DIR);
        if (dir && dir.isDirectory()) {
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                if (n >= MAX_IMAGES) { f.close(); break; }
                String fname = f.name();
                int slash = fname.lastIndexOf('/');
                if (slash >= 0) fname = fname.substring(slash + 1);
                size_t sz = f.size();
                f.close();
                int id = idFromFilename(fname.c_str());
                if (id < 0) continue;
                entries[n].id = id;
                entries[n].size = sz;
                String nm = fname.substring(fname.indexOf('_') + 1);
                if (nm.endsWith(".raw")) nm = nm.substring(0, nm.length() - 4);
                entries[n].name = nm;
                n++;
            }
        }
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (entries[j].id < entries[i].id) {
                    Entry t = entries[i]; entries[i] = entries[j]; entries[j] = t;
                }

        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"id\":" + String(entries[i].id);
            json += ",\"name\":\"" + jsonEscape(entries[i].name) + "\"";
            json += ",\"size\":" + String((unsigned)entries[i].size);
            if (entries[i].id == current) json += ",\"current\":true";
            json += "}";
        }
    }
    json += "],\"current\":" + String(current);
    json += ",\"free\":" + String((unsigned)galleryFreeBytes());
    json += "}";
    return json;
}

bool galleryUploadStart(int &id, const String &name, String *errOut) {
    if (!fsReady) {
        if (errOut) *errOut = "文件系统未就绪";
        return false;
    }
    if (galleryCount() >= MAX_IMAGES) {
        if (errOut) *errOut = "图片数量已达上限（" + String(MAX_IMAGES) + " 张），请先删除";
        return false;
    }
    if (galleryFreeBytes() < MIN_FREE_BYTES) {
        if (errOut) *errOut = "存储空间不足，请先删除部分图片";
        return false;
    }

    uploadId = nextId();
    String fname = String(IMG_DIR) + "/" + String(uploadId) + "_" + sanitizeName(name) + ".raw";
    uploadFile = LittleFS.open(fname, "w");
    if (!uploadFile) {
        if (errOut) *errOut = "无法创建文件";
        uploadId = -1;
        return false;
    }
    uploadWritten = 0;
    uploadOverflow = 0;
    uploadPath = fname;
    id = uploadId;
    Serial.printf("[GAL] upload start id=%d -> %s\n", uploadId, fname.c_str());
    return true;
}

void galleryUploadData(const uint8_t *buf, size_t len) {
    if (!uploadFile) return;
    size_t space = (size_t)COLOR_BUF_LEN - uploadWritten;
    size_t toWrite = len;
    if (toWrite > space) {
        uploadOverflow += toWrite - space;  // accept but discard excess
        toWrite = space;
    }
    if (toWrite > 0) {
        uploadWritten += uploadFile.write(buf, toWrite);
    }
}

int galleryUploadEnd(const String &name, String *errOut) {
    (void)name;  // name already encoded in the file path at start
    if (!uploadFile) {
        if (errOut) *errOut = "没有进行中的上传";
        return -1;
    }
    String fname = uploadPath;
    uploadFile.close();

    if (uploadWritten != (size_t)COLOR_BUF_LEN || uploadOverflow > 0) {
        size_t written = uploadWritten;
        Serial.printf("[GAL] upload size mismatch: %u (+%u overflow) != %d, removing\n",
                      (unsigned)written, (unsigned)uploadOverflow, COLOR_BUF_LEN);
        LittleFS.remove(fname);
        uploadId = -1;
        uploadWritten = 0;
        uploadOverflow = 0;
        if (errOut) {
            *errOut = "数据大小不符（应为 " + String(COLOR_BUF_LEN) +
                      " 字节，收到 " + String((unsigned)written) + "）";
        }
        return -1;
    }

    Serial.printf("[GAL] upload done id=%d, %u bytes\n", uploadId, (unsigned)uploadWritten);
    int id = uploadId;
    uploadId = -1;
    uploadWritten = 0;
    return id;
}

bool galleryDisplayById(int id) {
    String path;
    if (!pathForId(id, path)) {
        Serial.printf("[GAL] display: id %d not found\n", id);
        return false;
    }
    if (!ensureColorBuf()) {
        Serial.println("[GAL] colorBuf alloc failed");
        return false;
    }
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t total = 0;
    while (total < (size_t)COLOR_BUF_LEN && f.available()) {
        size_t got = f.read(colorBuf + total, COLOR_BUF_LEN - total);
        if (got == 0) break;
        total += got;
    }
    f.close();
    if (total != (size_t)COLOR_BUF_LEN) {
        Serial.printf("[GAL] short read: %u/%d\n", (unsigned)total, COLOR_BUF_LEN);
        return false;
    }
    Serial.printf("[GAL] displaying id=%d (%s)\n", id, path.c_str());
    // epdDisplay2bpp blocks ~15s and frees colorBuf when done
    epdDisplay2bpp(colorBuf);
    writeCurrentId(id);
    return true;
}

bool galleryCycle(int dir) {
    int ids[MAX_IMAGES];
    int n = 0;
    if (!fsReady) return false;
    File dirH = LittleFS.open(IMG_DIR);
    if (!dirH || !dirH.isDirectory()) return false;
    for (File f = dirH.openNextFile(); f && n < MAX_IMAGES; f = dirH.openNextFile()) {
        String fname = f.name();
        int slash = fname.lastIndexOf('/');
        if (slash >= 0) fname = fname.substring(slash + 1);
        f.close();
        int id = idFromFilename(fname.c_str());
        if (id >= 0) ids[n++] = id;
    }
    if (n == 0) return false;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (ids[j] < ids[i]) { int t = ids[i]; ids[i] = ids[j]; ids[j] = t; }

    int current = galleryCurrentId();
    int pos = -1;
    for (int i = 0; i < n; i++)
        if (ids[i] == current) { pos = i; break; }
    int next;
    if (pos < 0) {
        next = (dir >= 0) ? 0 : n - 1;
    } else {
        next = (pos + (dir >= 0 ? 1 : -1) + n) % n;
    }
    return galleryDisplayById(ids[next]);
}

bool galleryDeleteById(int id) {
    String path;
    if (!pathForId(id, path)) return false;
    bool ok = LittleFS.remove(path);
    if (ok) {
        Serial.printf("[GAL] deleted id=%d (%s)\n", id, path.c_str());
        if (galleryCurrentId() == id) writeCurrentId(-1);
    }
    return ok;
}

bool galleryClearScreen() {
    if (!ensureColorBuf()) return false;
    memset(colorBuf, 0x55, COLOR_BUF_LEN);  // 0b01 = white for all pixels
    epdDisplay2bpp(colorBuf);
    writeCurrentId(-1);
    return true;
}

int galleryCurrentId() {
    galPrefs.begin("gal", true);
    int id = galPrefs.getInt("cur", -1);
    galPrefs.end();
    return id;
}

bool galleryExists(int id) {
    String path;
    return pathForId(id, path);
}
