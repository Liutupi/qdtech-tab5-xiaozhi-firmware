#pragma once
#include <string>
class Settings {
public:
    Settings(const std::string& ns, bool rw = false) : ns_(ns) {}
    int GetInt(const std::string& key, int def = 0);
    void SetInt(const std::string& key, int v);
    std::string GetString(const std::string& key, const std::string& def = "");
    void SetString(const std::string& key, const std::string& v);
    void EraseKey(const std::string&) {}
private:
    std::string ns_;
};
