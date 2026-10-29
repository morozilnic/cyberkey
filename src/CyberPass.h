#pragma once
#include <Arduino.h>
#include <FS.h>

struct PasswordEntry {
    String name;      // Service/Site name, e.g. "Google", "GitHub"
    String url;       // Website URL, e.g. "https://accounts.google.com"
    String username;  // Username or email
    String password;  // Password
    String note;      // Optional note
};

#define MAX_SD_PASSWORDS 300

class CyberPassVault {
public:
    static PasswordEntry passwords[MAX_SD_PASSWORDS];
    static int count;

    // Initializes SD and loads /passwords.json (or converts /passwords.csv if found)
    static bool init();

    // Loads passwords from /passwords.json on SD
    static bool loadFromSD();

    // Saves current passwords into /passwords.json on SD
    static bool saveToSD();

    // Parses a Google Password Manager CSV text and appends/merges into SD vault
    static int importGoogleCSV(const String& csvText);
    static int importCSVFile(File& file);

    // Imports from /passwords.csv file directly on the SD card root
    static int importFromSDCSV();

    // Finds matching password entry for given service name or username
    static int findMatch(const String& issuer, const String& username);

    // Clears all stored passwords from SD
    static void wipe();
};
