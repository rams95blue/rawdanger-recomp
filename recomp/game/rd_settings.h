#pragma once
// Tiny INI reader/writer for rawdanger.ini (no dependencies).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace rawdanger
{
    class Settings
    {
    public:
        static std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        static std::string trim(const std::string &s)
        {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                return {};
            const auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        bool load(const std::filesystem::path &file)
        {
            std::ifstream in(file);
            if (!in)
                return false;
            std::string line, section;
            while (std::getline(in, line))
            {
                // strip comments (; or #) that start a line or follow whitespace
                for (size_t i = 0; i < line.size(); ++i)
                {
                    if ((line[i] == ';' || line[i] == '#') && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))))
                    {
                        line.resize(i);
                        break;
                    }
                }
                line = trim(line);
                if (line.empty())
                    continue;
                if (line.front() == '[' && line.back() == ']')
                {
                    section = lower(trim(line.substr(1, line.size() - 2)));
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                m_values[section + "." + lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
            }
            return true;
        }

        std::string str(const std::string &key, const std::string &def = {}) const
        {
            auto it = m_values.find(lower(key));
            return it == m_values.end() ? def : it->second;
        }

        bool flag(const std::string &key, bool def) const
        {
            const std::string v = lower(str(key));
            if (v.empty())
                return def;
            return v == "1" || v == "true" || v == "yes" || v == "on";
        }

        float number(const std::string &key, float def) const
        {
            const std::string v = str(key);
            if (v.empty())
                return def;
            try
            {
                return std::stof(v);
            }
            catch (...)
            {
                return def;
            }
        }

        bool has(const std::string &key) const { return m_values.count(lower(key)) != 0; }

    private:
        std::map<std::string, std::string> m_values;
    };
}
