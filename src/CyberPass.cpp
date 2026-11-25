#include "CyberPass.h"
#include "CyberCrypto.h"
#include <vector>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

PasswordEntry CyberPassVault::passwords[MAX_SD_PASSWORDS];
int CyberPassVault::count = 0;

static const char* VAULT_JSON_PATH = "/passwords.json";
static const char* VAULT_CSV_PATH  = "/passwords.csv";
static const char* VAULT_TEMP_PATH = "/passwords.tmp";
static const char* VAULT_OLD_PATH  = "/passwords.old";

static String cleanServiceName(String rawName, String rawUrl) {
    if (rawName.length() > 0) return rawName;
    if (rawUrl.length() > 0) {
        String clean = rawUrl;
        int proto = clean.indexOf("://");
        if (proto != -1) clean = clean.substring(proto + 3);
        int slash = clean.indexOf('/');
        if (slash != -1) clean = clean.substring(0, slash);
        int port = clean.indexOf(':');
        if (port != -1) clean = clean.substring(0, port);
        if (clean.startsWith("www.")) clean = clean.substring(4);
        int dot = clean.indexOf('.');
        if (dot != -1) clean = clean.substring(0, dot);
        if (clean.length() > 0) {
            clean[0] = toupper(clean[0]);
            return clean;
        }
    }
    return "Account";
}

bool CyberPassVault::init() {
    count = 0;
    if (!CyberCrypto::isReady()) return false;

    if (SD.exists(VAULT_TEMP_PATH) && !SD.remove(VAULT_TEMP_PATH)) return false;
    if (SD.exists(VAULT_OLD_PATH)) {
        if (!SD.exists(VAULT_JSON_PATH)) {
            if (!SD.rename(VAULT_OLD_PATH, VAULT_JSON_PATH)) return false;
        } else if (!SD.remove(VAULT_OLD_PATH)) {
            return false;
        }
    }

    // If a raw CSV exists, import it first and clean it up immediately
    if (SD.exists(VAULT_CSV_PATH)) {
        if (importFromSDCSV() < 0) return false;
    }
    return loadFromSD();
}

bool CyberPassVault::loadFromSD() {
    count = 0;
    if (!CyberCrypto::isReady()) return false;
    if (!SD.exists(VAULT_JSON_PATH)) return false;

    File file = SD.open(VAULT_JSON_PATH, FILE_READ);
    if (!file) return false;

    size_t length = file.size();
    std::vector<uint8_t> data(length + 1, 0);
    size_t bytesRead = file.read(data.data(), length);
    file.close();
    if (bytesRead != length) return false;

    String content;
    if (CyberCrypto::isSealed(data.data(), length)) {
        if (!CyberCrypto::open(data.data(), length, "passwords", content)) return false;
    } else {
        content = String(reinterpret_cast<const char*>(data.data()));
    }

    size_t lineStart = 0;
    while (lineStart < content.length()) {
        int lineEnd = content.indexOf('\n', lineStart);
        if (lineEnd < 0) lineEnd = content.length();
        String line = content.substring(lineStart, lineEnd);
        lineStart = static_cast<size_t>(lineEnd) + 1;
        line.trim();
        yield(); // Feed watchdog

        if (line.startsWith("{") && line.indexOf("\"name\"") != -1) {
            // Strip trailing comma if present
            if (line.endsWith(",")) {
                line.remove(line.length() - 1);
            }

            JsonDocument obj;
            DeserializationError err = deserializeJson(obj, line);
            if (!err && count < MAX_SD_PASSWORDS) {
                passwords[count].name     = obj["name"].is<String>() ? obj["name"].as<String>() : "";
                passwords[count].url      = obj["url"].is<String>() ? obj["url"].as<String>() : "";
                passwords[count].username = obj["username"].is<String>() ? obj["username"].as<String>() : "";
                passwords[count].password = obj["password"].is<String>() ? obj["password"].as<String>() : "";
                passwords[count].note     = obj["note"].is<String>() ? obj["note"].as<String>() : "";
                count++;
            }
        }
        if (count >= MAX_SD_PASSWORDS) break;
    }
    return true;
}

bool CyberPassVault::saveToSD() {
    if (!CyberCrypto::isReady()) return false;

    String plaintext = "{\"passwords\":[\n";
    for (int i = 0; i < count; ++i) {
        plaintext += "  {";
        plaintext += "\"name\":";
        JsonDocument scratch;
        scratch["s"] = passwords[i].name;
        String sName;
        serializeJson(scratch["s"], sName);
        plaintext += sName;

        plaintext += ",\"url\":";
        scratch["s"] = passwords[i].url;
        String sUrl;
        serializeJson(scratch["s"], sUrl);
        plaintext += sUrl;

        plaintext += ",\"username\":";
        scratch["s"] = passwords[i].username;
        String sUser;
        serializeJson(scratch["s"], sUser);
        plaintext += sUser;

        plaintext += ",\"password\":";
        scratch["s"] = passwords[i].password;
        String sPass;
        serializeJson(scratch["s"], sPass);
        plaintext += sPass;

        if (passwords[i].note.length() > 0) {
            plaintext += ",\"note\":";
            scratch["s"] = passwords[i].note;
            String sNote;
            serializeJson(scratch["s"], sNote);
            plaintext += sNote;
        }
        plaintext += "}";
        if (i < count - 1) plaintext += ",";
        plaintext += "\n";
        yield();
    }
    plaintext += "]}\n";

    std::vector<uint8_t> encrypted;
    if (!CyberCrypto::seal(plaintext, "passwords", encrypted)) return false;

    if (SD.exists(VAULT_TEMP_PATH) && !SD.remove(VAULT_TEMP_PATH)) return false;
    File temp = SD.open(VAULT_TEMP_PATH, FILE_WRITE);
    if (!temp || temp.write(encrypted.data(), encrypted.size()) != encrypted.size()) {
        if (temp) temp.close();
        SD.remove(VAULT_TEMP_PATH);
        return false;
    }
    temp.close();

    const bool hadOriginal = SD.exists(VAULT_JSON_PATH);
    if (hadOriginal) {
        if (SD.exists(VAULT_OLD_PATH) && !SD.remove(VAULT_OLD_PATH)) {
            SD.remove(VAULT_TEMP_PATH);
            return false;
        }
        if (!SD.rename(VAULT_JSON_PATH, VAULT_OLD_PATH)) {
            SD.remove(VAULT_TEMP_PATH);
            return false;
        }
    }

    if (!SD.rename(VAULT_TEMP_PATH, VAULT_JSON_PATH)) {
        if (hadOriginal) SD.rename(VAULT_OLD_PATH, VAULT_JSON_PATH);
        SD.remove(VAULT_TEMP_PATH);
        return false;
    }
    if (hadOriginal && !SD.remove(VAULT_OLD_PATH)) return false;
    return true;
}

// Parse a single CSV row handling quotes properly
static void parseCSVRow(const String& line, std::vector<String>& fields) {
    fields.clear();
    String field = "";
    bool inQuotes = false;
    for (size_t i = 0; i < line.length(); ++i) {
        char c = line[i];
        if (c == '\"') {
            if (inQuotes && i + 1 < line.length() && line[i + 1] == '\"') {
                field += '\"';
                i++;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (c == ',' && !inQuotes) {
            fields.push_back(field);
            field = "";
        } else if (c == '\r') {
            // skip carriage return
        } else {
            field += c;
        }
    }
    fields.push_back(field);
}

int CyberPassVault::importGoogleCSV(const String& csvText) {
    if (!CyberCrypto::isReady()) return 0;
    if (csvText.length() == 0) return 0;

    int imported = 0;
    int lineStart = 0;
    int len = csvText.length();
    bool isHeader = true;

    int colName = 0;
    int colUrl = 1;
    int colUser = 2;
    int colPass = 3;
    int colNote = 4;

    while (lineStart < len) {
        int lineEnd = csvText.indexOf('\n', lineStart);
        if (lineEnd == -1) lineEnd = len;

        String line = csvText.substring(lineStart, lineEnd);
        lineStart = lineEnd + 1;
        line.trim();
        if (line.length() == 0) continue;

        std::vector<String> fields;
        parseCSVRow(line, fields);

        if (isHeader) {
            isHeader = false;
            for (size_t i = 0; i < fields.size(); ++i) {
                String h = fields[i];
                h.toLowerCase();
                h.trim();
                if (h == "name") colName = i;
                else if (h == "url") colUrl = i;
                else if (h == "username" || h == "user") colUser = i;
                else if (h == "password" || h == "pass") colPass = i;
                else if (h == "note" || h == "notes") colNote = i;
            }
            continue;
        }

        if (fields.size() <= (size_t)colPass) continue;

        String rawName = fields.size() > (size_t)colName ? fields[colName] : "";
        String rawUrl  = fields.size() > (size_t)colUrl ? fields[colUrl] : "";
        String user    = fields.size() > (size_t)colUser ? fields[colUser] : "";
        String pass    = fields.size() > (size_t)colPass ? fields[colPass] : "";
        String note    = fields.size() > (size_t)colNote ? fields[colNote] : "";

        rawName.trim();
        rawUrl.trim();
        user.trim();
        pass.trim();
        if (pass.length() == 0) continue;

        String sName = cleanServiceName(rawName, rawUrl);

        bool updated = false;
        for (int i = 0; i < count; ++i) {
            if (passwords[i].name.equalsIgnoreCase(sName) && passwords[i].username.equalsIgnoreCase(user)) {
                passwords[i].password = pass;
                passwords[i].url = rawUrl;
                if (note.length() > 0) passwords[i].note = note;
                updated = true;
                imported++;
                break;
            }
        }

        if (!updated && count < MAX_SD_PASSWORDS) {
            passwords[count].name     = sName;
            passwords[count].url      = rawUrl;
            passwords[count].username = user;
            passwords[count].password = pass;
            passwords[count].note     = note;
            count++;
            imported++;
        }
    }

    if (imported > 0) {
        if (!saveToSD()) return -1;
    }
    return imported;
}

int CyberPassVault::importCSVFile(File& file) {
    if (!file || !CyberCrypto::isReady()) return 0;

    int imported = 0;
    bool isHeader = true;
    int colName = 0;
    int colUrl = 1;
    int colUser = 2;
    int colPass = 3;
    int colNote = 4;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        std::vector<String> fields;
        parseCSVRow(line, fields);

        if (isHeader) {
            isHeader = false;
            for (size_t i = 0; i < fields.size(); ++i) {
                String h = fields[i];
                h.toLowerCase();
                h.trim();
                if (h == "name") colName = i;
                else if (h == "url") colUrl = i;
                else if (h == "username" || h == "user") colUser = i;
                else if (h == "password" || h == "pass") colPass = i;
                else if (h == "note" || h == "notes") colNote = i;
            }
            continue;
        }

        if (fields.size() <= (size_t)colPass) continue;

        String rawName = fields.size() > (size_t)colName ? fields[colName] : "";
        String rawUrl  = fields.size() > (size_t)colUrl ? fields[colUrl] : "";
        String user    = fields.size() > (size_t)colUser ? fields[colUser] : "";
        String pass    = fields.size() > (size_t)colPass ? fields[colPass] : "";
        String note    = fields.size() > (size_t)colNote ? fields[colNote] : "";

        rawName.trim();
        rawUrl.trim();
        user.trim();
        pass.trim();
        if (pass.length() == 0) continue;

        String sName = cleanServiceName(rawName, rawUrl);

        bool updated = false;
        for (int i = 0; i < count; ++i) {
            if (passwords[i].name.equalsIgnoreCase(sName) && passwords[i].username.equalsIgnoreCase(user)) {
                passwords[i].password = pass;
                passwords[i].url = rawUrl;
                if (note.length() > 0) passwords[i].note = note;
                updated = true;
                imported++;
                break;
            }
        }

        if (!updated && count < MAX_SD_PASSWORDS) {
            passwords[count].name     = sName;
            passwords[count].url      = rawUrl;
            passwords[count].username = user;
            passwords[count].password = pass;
            passwords[count].note     = note;
            count++;
            imported++;
        }
        yield();
    }

    if (imported > 0) {
        if (!saveToSD()) return -1;
    }
    return imported;
}

int CyberPassVault::importFromSDCSV() {
    if (!CyberCrypto::isReady()) return 0;
    if (!SD.exists(VAULT_CSV_PATH)) return 0;
    File file = SD.open(VAULT_CSV_PATH, FILE_READ);
    if (!file) return 0;
    int imported = importCSVFile(file);
    file.close();
    if (imported >= 0) SD.remove(VAULT_CSV_PATH);
    return imported;
}

int CyberPassVault::findMatch(const String& issuer, const String& username) {
    if (count == 0) return -1;

    // 1st priority: exact match on service/issuer AND username
    if (issuer.length() > 0 && username.length() > 0) {
        for (int i = 0; i < count; ++i) {
            if (passwords[i].name.equalsIgnoreCase(issuer) && passwords[i].username.equalsIgnoreCase(username)) {
                return i;
            }
        }
    }

    // 2nd priority: case-insensitive match on service name
    if (issuer.length() > 0) {
        for (int i = 0; i < count; ++i) {
            if (passwords[i].name.equalsIgnoreCase(issuer)) {
                return i;
            }
        }
        for (int i = 0; i < count; ++i) {
            if (passwords[i].name.indexOf(issuer) >= 0 || issuer.indexOf(passwords[i].name) >= 0) {
                return i;
            }
        }
    }

    // 3rd priority: match username if unique
    if (username.length() > 0) {
        int foundIdx = -1;
        int foundCount = 0;
        for (int i = 0; i < count; ++i) {
            if (passwords[i].username.equalsIgnoreCase(username)) {
                foundIdx = i;
                foundCount++;
            }
        }
        if (foundCount == 1) return foundIdx;
    }

    return -1;
}

void CyberPassVault::wipe() {
    count = 0;
    if (SD.exists(VAULT_JSON_PATH)) SD.remove(VAULT_JSON_PATH);
    if (SD.exists(VAULT_CSV_PATH)) SD.remove(VAULT_CSV_PATH);
    if (SD.exists(VAULT_TEMP_PATH)) SD.remove(VAULT_TEMP_PATH);
    if (SD.exists(VAULT_OLD_PATH)) SD.remove(VAULT_OLD_PATH);
}
