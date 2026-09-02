/*
 *  strategy_config.h - a tiny, reusable strategy-configuration framework
 *  for SDCShield memory tests.
 *
 *  Purpose
 *  -------
 *  Some stress tests (e.g. memcpy_rewr) want to be runnable under several
 *  distinct "strategies" (a NUMA-cross-node barrage, an intra-die L3
 *  brawl, a few-producer/many-consumer invalidate storm, ...). Rather than
 *  hard-coding each variant, this header lets a test ship a plain-text
 *  config file describing any number of strategies and pick one at run
 *  time. Multiple strategies listed in the file are cycled ("round-robin")
 *  by invoking the test repeatedly with different indices, so each strategy
 *  runs as its own clean forked test lifetime.
 *
 *  Config file grammar
 *  -------------------
 *    - Blank lines and lines whose first non-space character is '#' are
 *      ignored (comments).
 *    - A line of the form  [strategy: name]   begins a new strategy block
 *      and gives it a human-readable name.
 *    - A line of the form  key = value         adds a parameter to the
 *      strategy block that is currently open.
 *  Everything else is an error (reported via the returned status).
 *
 *  Usage by a test
 *  ---------------
 *    StrategySet set;
 *    if (!strategy_config_load(set, default_path)) { ... skip or fail ... }
 *    size_t idx = 0;
 *    const Strategy *s = strategy_config_pick(set, "MYTEST_STRATEGY_INDEX", &idx);
 *    const std::string *bs = strategy_config_get(*s, "block_size");
 *
 *  The two environment variables consulted are:
 *    - SANDSTONE_STRATEGY_CONF    : override the config-file path (else the
 *                                   path passed to strategy_config_load is
 *                                   used, which the caller derives from
 *                                   __FILE__'s directory by default).
 *    - SANDSTONE_STRATEGY_INDEX   : 0-based index of the strategy to run;
 *                                   out-of-range values wrap (modulo),
 *                                   giving the round-robin semantics.
 *
 *  This header is arch-independent and reusable by any test that links it.
 *
 *  SPDX-License-Identifier: Apache-2.0
 */
#ifndef SANDSTONE_STRATEGY_CONFIG_H
#define SANDSTONE_STRATEGY_CONFIG_H

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Strategy {
    std::string name;
    std::map<std::string, std::string> params;

    /* Look up a parameter; returns nullptr if absent. */
    const std::string *get(const std::string &key) const
    {
        auto it = params.find(key);
        return (it == params.end()) ? nullptr : &it->second;
    }
    /* Convenience: fetch and parse as a long, with a fallback default. */
    long get_long(const std::string &key, long fallback) const
    {
        const std::string *v = get(key);
        if (!v || v->empty())
            return fallback;
        try {
            return std::stol(*v);
        } catch (...) {
            return fallback;
        }
    }
};

struct StrategySet {
    std::vector<Strategy> strategies;
    std::string error;            /* non-empty if parsing failed */

    bool ok() const { return error.empty(); }
    bool empty() const { return strategies.empty(); }
    size_t size() const { return strategies.size(); }
};

/*
 * Load strategies from a file. The actual path used is the value of the
 * SANDSTONE_STRATEGY_CONF environment variable if set, otherwise @p
 * default_path. On failure (file missing / parse error) set.error is
 * filled and set.strategies may be partial; ok() returns false.
 */
inline bool strategy_config_load(StrategySet &set, const std::string &default_path)
{
    set.strategies.clear();
    set.error.clear();

    const char *override = std::getenv("SANDSTONE_STRATEGY_CONF");
    std::string path = override ? override : default_path;

    std::ifstream f(path);
    if (!f.is_open()) {
        /* include errno for diagnosis (e.g. EACCES, ENOENT, sandbox) */
        int e = errno;
        set.error = "cannot open strategy config: " + path +
                   " (errno " + std::to_string(e) + ": " +
                   std::strerror(e) + ")";
        return false;
    }

    Strategy *current = nullptr;
    std::string line;
    size_t lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;

        /* Find first non-space. */
        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;

        if (i >= line.size())            continue;   /* blank line */
        if (line[i] == '#')              continue;   /* comment */

        std::string trimmed = line.substr(i);
        /* strip trailing whitespace */
        while (!trimmed.empty() &&
               std::isspace(static_cast<unsigned char>(trimmed.back())))
            trimmed.pop_back();

        if (trimmed.empty())             continue;

        /* New strategy block: [strategy: name] */
        if (trimmed.front() == '[') {
            std::string::size_type close = trimmed.find(']');
            if (close == std::string::npos) {
                std::ostringstream os;
                os << "line " << lineno << ": ']' missing in section header";
                set.error = os.str();
                return false;
            }
            std::string header = trimmed.substr(1, close - 1);
            /* allow "strategy: name" or just "name" inside the brackets */
            const std::string prefix = "strategy:";
            std::string name = header;
            if (header.size() >= prefix.size() &&
                header.compare(0, prefix.size(), prefix) == 0) {
                name = header.substr(prefix.size());
            }
            /* trim name */
            size_t b = 0, e = name.size();
            while (b < e && std::isspace(static_cast<unsigned char>(name[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(name[e - 1]))) --e;
            name = name.substr(b, e - b);

            set.strategies.push_back(Strategy{});
            set.strategies.back().name = name;
            current = &set.strategies.back();
            continue;
        }

        /* key = value */
        if (!current) {
            std::ostringstream os;
            os << "line " << lineno
               << ": parameter outside any [strategy: ...] block";
            set.error = os.str();
            return false;
        }
        std::string::size_type eq = trimmed.find('=');
        if (eq == std::string::npos) {
            std::ostringstream os;
            os << "line " << lineno << ": expected 'key = value'";
            set.error = os.str();
            return false;
        }
        std::string key = trimmed.substr(0, eq);
        std::string val = trimmed.substr(eq + 1);
        /* trim both */
        while (!key.empty() &&
               std::isspace(static_cast<unsigned char>(key.back())))
            key.pop_back();
        while (!val.empty() &&
               std::isspace(static_cast<unsigned char>(val.front())))
            val.erase(val.begin());
        current->params[key] = val;
    }

    if (set.strategies.empty()) {
        set.error = "no [strategy: ...] blocks found in " + path;
        return false;
    }
    return true;
}

/*
 * Pick the strategy to run. The 0-based index comes from the environment
 * variable @p index_env (e.g. "SANDSTONE_STRATEGY_INDEX"); if unset, 0 is
 * used. Out-of-range indices wrap modulo the set size (round-robin). If
 * @p chosen_index is non-null, the resolved index is stored there. Returns
 * nullptr if the set is empty.
 */
inline const Strategy *strategy_config_pick(const StrategySet &set,
                                           const char *index_env,
                                           size_t *chosen_index = nullptr)
{
    if (set.strategies.empty())
        return nullptr;

    size_t idx = 0;
    if (const char *s = std::getenv(index_env)) {
        try {
            long v = std::stol(s);
            if (v < 0)
                v = 0;
            idx = static_cast<size_t>(v);
        } catch (...) {
            idx = 0;
        }
    }
    idx %= set.strategies.size();
    if (chosen_index)
        *chosen_index = idx;
    return &set.strategies[idx];
}

#endif /* SANDSTONE_STRATEGY_CONFIG_H */
