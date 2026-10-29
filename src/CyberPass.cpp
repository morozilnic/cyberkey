#include "CyberPass.h"
#include <vector>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

PasswordEntry CyberPassVault::passwords[MAX_SD_PASSWORDS];
int CyberPassVault::count = 0;

static const char* VAULT_JSON_PATH = "/passwords.json";
static const char* VAULT_CSV_PATH  = "/passwords.csv";

// Helper function to extract a clean service name from a URL or name
static String cleanServiceName(String name, String url) {
    name.trim();
    if (name.length() > 0 && !name.startsWith("http://") && !name.startsWith("https://")) {
        return name;
    }
    // Extract domain from URL
    if (url.startsWith("https://")) url = url.substring(8);
    else if (url.startsWith("http://")) url = url.substring(7);
    if (url.startsWith("www.")) url = url.substring(4);
    int slashIdx = url.indexOf('/');
    if (slashIdx > 0) url = url.substring(0, slashIdx);
    int portIdx = url.indexOf(':');
    if (portIdx > 0) url = url.substring(0, portIdx);
    url.trim();
    return url.length() > 0 ? url : (name.length() > 0 ? name : "Unknown");
}

bool CyberPassVault::init() {
    bool loaded = loadFromSD();
    // If a passwords.csv exists on SD root, auto-import it if vault is empty
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

bool CyberPassVault::saveToSD() {
    File file = SD.open(VAULT_JSON_PATH, FILE_WRITE);
    if (!file) return false;

    JsonDocument doc;
    JsonArray arr = doc["passwords"].to<JsonArray>();
    for (int i = 0; i < count; ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"]     = passwords[i].name;
        obj["url"]      = passwords[i].url;
        obj["username"] = passwords[i].username;
        obj["password"] = passwords[i].password;
        if (passwords[i].note.length() > 0) {
            obj["note"] = passwords[i].note;
        }
    }

    serializeJson(doc, file);
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

    // Default Google Password Manager CSV column indices
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
            // Parse headers dynamically
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

        // Check if an existing entry matches service name + username
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
        // Substring match
        for (int i = 0; i < count; ++i) {
            String lowerName = passwords[i].name;
            lowerName.toLowerCase();
            String lowerIssuer = issuer;
            lowerIssuer.toLowerCase();
            if (lowerName.indexOf(lowerIssuer) >= 0 || lowerIssuer.indexOf(lowerName) >= 0) {
                return i;
            }
        }
    }

    // 3rd priority: match on username
    if (username.length() > 0) {
        for (int i = 0; i < count; ++i) {
            if (passwords[i].username.equalsIgnoreCase(username)) {
                return i;
            }
        }
    }

    return -1;
}

void CyberPassVault::wipe() {
    count = 0;
    if (SD.exists(VAULT_JSON_PATH)) {
        SD.remove(VAULT_JSON_PATH);
    }
}
