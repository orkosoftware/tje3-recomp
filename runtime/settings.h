// Player options, kept in <user dir>/tje3.ini as "key = value" lines.
#pragma once
#include <string>

namespace tje {

void settings_load(const std::string& user_dir);   // creates the file with defaults if missing
bool setting_bool(const char* key);
int setting_int(const char* key);
void setting_set(const char* key, int value);       // changes one option and saves the file

}  // namespace tje
