#include "CyberPass.h"
#include <vector>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

PasswordEntry CyberPassVault::passwords[MAX_SD_PASSWORDS];
int CyberPassVault::count = 0;

static const char* VAULT_JSON_PATH = "/passwords.json";
static const char* VAULT_CSV_PATH  = "/passwords.csv";

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
    bool loaded = loadFromSD();
    if (SD.exists(VAULT_CSV_PATH)) {
        importFromSDCSV();
    }
    return loaded;
}

bool CyberPassVault::loadFromSD() {
    count = 0;
    if (!SD.exists(VAULT_JSON_PATH)) return false;

    File file = SD.open(VAULT_JSON_PATH, FILE_READ);
    if (!file) return false;

    // Use streaming line-by-line reading to prevent heap exhaustion
    // Format written by saveToSD is pretty-printed or one entry per line
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) return false;

    JsonArray arr = doc["passwords"].as<JsonArray>();
    for (JsonObject obj : arr) {
        if (count >= MAX_SD_PASSWORDS) break;
        passwords[count].name     = obj["name"].is<String>() ? obj["name"].as<String>() : "";
        passwords[count].url      = obj["url"].is<String>() ? obj["url"].as<String>() : "";
        passwords[count].username = obj["username"].is<String>() ? obj["username"].as<String>() : "";
        passwords[count].password = obj["password"].is<String>() ? obj["password"].as<String>() : "";
        passwords[count].note     = obj["note"].is<String>() ? obj["note"].as<String>() : "";
        count++;
    }
    return true;
}

// Writes JSON directly as a stream without allocating a massive 40KB JsonDocument in RAM
bool CyberPassVault::saveToSD() {
    File file = SD.open(VAULT_JSON_PATH, FILE_WRITE);
    if (!file) return false;

    file.print("{\"passwords\":[\n");
    for (int i = 0; i < count; ++i) {
        file.print("  {");
        file.print("\"name\":");
        // Use small scratch doc to safely serialize escaped strings
        JsonDocument scratch;
        scratch["s"] = passwords[i].name;
        String sName;
        serializeJson(scratch["s"], sName);
        file.print(sName);

        file.print(",\"url\":");
        scratch["s"] = passwords[i].url;
        String sUrl;
        serializeJson(scratch["s"], sUrl);
        file.print(sUrl);

        file.print(",\"username\":");
        scratch["s"] = passwords[i].username;
        String sUser;
        serializeJson(scratch["s"], sUser);
        file.print(sUser);

        file.print(",\"password\":");
        scratch["s"] = passwords[i].password;
        String sPass;
        serializeJson(scratch["s"], sPass);
        file.print(sPass);

        if (passwords[i].note.length() > 0) {
            file.print(",\"note\":");
            scratch["s"] = passwords[i].note;
            String sNote;
            serializeJson(scratch["s"], sNote);
            file.print(sNote);
        }
        file.print("}");
        if (i < count - 1) file.print(",");
        file.print("\n");
        yield();
    }
    file.print("]}\n");
    file.close();
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
        saveToSD();
    }
    return imported;
}

int CyberPassVault::importCSVFile(File& file) {
    if (!file) return 0;

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
        saveToSD();
    }
    return imported;
}

int CyberPassVault::importFromSDCSV() {
    if (!SD.exists(VAULT_CSV_PATH)) return 0;
    File file = SD.open(VAULT_CSV_PATH, FILE_READ);
    if (!file) return 0;
    int imported = importCSVFile(file);
    file.close();
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
}
