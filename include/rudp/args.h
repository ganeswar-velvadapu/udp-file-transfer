#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace rudp {

// Minimal "--key value" / "--flag" / positional command-line parser.
class Args {
public:
    Args(int argc, char** argv, std::set<std::string> boolean_flags) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0 || (a.size() == 2 && a[0] == '-' && a[1] != '-')) {
                std::string key = a.substr(a[1] == '-' ? 2 : 1);
                if (boolean_flags.count(key)) {
                    values_[key] = "1";
                } else {
                    if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
                    values_[key] = argv[++i];
                }
            } else {
                positional_.push_back(a);
            }
        }
    }

    bool has(const std::string& key) const { return values_.count(key) > 0; }

    std::string get(const std::string& key, const std::string& def) const {
        auto it = values_.find(key);
        return it == values_.end() ? def : it->second;
    }

    long long get_int(const std::string& key, long long def) const {
        return has(key) ? std::stoll(values_.at(key)) : def;
    }

    double get_double(const std::string& key, double def) const {
        return has(key) ? std::stod(values_.at(key)) : def;
    }

    const std::vector<std::string>& positional() const { return positional_; }

private:
    std::map<std::string, std::string> values_;
    std::vector<std::string> positional_;
};

}  // namespace rudp
