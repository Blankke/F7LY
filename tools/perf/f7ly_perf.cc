#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
constexpr const char *k_proc_root = "/proc/f7ly/perf";

struct MetricRow
{
    uint64_t snapshot{};
    uint64_t epoch{};
    uint64_t timestamp{};
    unsigned cpu{};
    unsigned id{};
    std::string name;
    std::string kind;
    std::string unit;
    uint64_t value{};
};

struct SyscallRow
{
    uint64_t epoch{};
    uint64_t timestamp{};
    unsigned cpu{};
    unsigned number{};
    std::string name;
    uint64_t count{};
    uint64_t time_ticks{};
};

struct Symbol
{
    uint64_t start{};
    uint64_t end{};
    std::string name;
};

struct ProfileRow
{
    std::string record;
    unsigned cpu{};
    uint64_t count{};
    std::vector<uint64_t> pcs;
    uint64_t dropped_full{};
    uint64_t invalid_pc{};
    uint64_t user_skipped{};
    uint64_t unwind_failed{};
};

[[noreturn]] void fail(const std::string &message)
{
    std::cerr << "f7ly-perf: " << message << '\n';
    std::exit(1);
}

std::string read_file(const std::string &path)
{
    std::ifstream input(path);
    if (!input)
        fail("cannot read " + path + ": " + std::strerror(errno));
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void write_control(const std::string &command)
{
    const std::string path = std::string(k_proc_root) + "/control";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        fail("cannot open " + path + ": " + std::strerror(errno));
    const ssize_t written = ::write(fd, command.data(), command.size());
    const int saved_errno = errno;
    ::close(fd);
    if (written != static_cast<ssize_t>(command.size()))
    {
        errno = saved_errno;
        fail("control command rejected: " + command + ": " + std::strerror(errno));
    }
}

std::vector<std::string> split(const std::string &text, char delimiter)
{
    std::vector<std::string> fields;
    size_t begin = 0;
    for (;;)
    {
        const size_t end = text.find(delimiter, begin);
        fields.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return fields;
}

uint64_t parse_u64(const std::string &value, int base = 10)
{
    if (value.empty())
        fail("empty integer in proc data");
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, base);
    if (errno != 0 || end == value.c_str() || *end != '\0')
        fail("invalid integer in proc data: " + value);
    return static_cast<uint64_t>(parsed);
}

std::vector<std::string> validated_lines(const std::string &content, const std::string &node)
{
    std::vector<std::string> lines;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    if (lines.size() < 2 || lines[0] != "f7ly-perf-v1\t" + node)
        fail("unsupported or malformed " + node + " ABI");
    return lines;
}

std::map<std::string, std::string> read_meta()
{
    const auto lines = validated_lines(read_file(std::string(k_proc_root) + "/meta"), "meta");
    std::map<std::string, std::string> meta;
    for (size_t i = 2; i < lines.size(); ++i)
    {
        const auto fields = split(lines[i], '\t');
        if (fields.size() == 2)
            meta[fields[0]] = fields[1];
    }
    return meta;
}

std::vector<MetricRow> read_metrics()
{
    const auto lines = validated_lines(read_file(std::string(k_proc_root) + "/metrics"), "metrics");
    std::vector<MetricRow> rows;
    for (size_t i = 2; i < lines.size(); ++i)
    {
        if (lines[i].empty())
            continue;
        const auto f = split(lines[i], '\t');
        if (f.size() != 9)
            fail("malformed metrics row");
        rows.push_back({parse_u64(f[0]), parse_u64(f[1]), parse_u64(f[2]),
                        static_cast<unsigned>(parse_u64(f[3])), static_cast<unsigned>(parse_u64(f[4])),
                        f[5], f[6], f[7], parse_u64(f[8])});
    }
    return rows;
}

std::vector<SyscallRow> read_syscalls()
{
    const auto lines = validated_lines(read_file(std::string(k_proc_root) + "/syscalls"), "syscalls");
    std::vector<SyscallRow> rows;
    for (size_t i = 2; i < lines.size(); ++i)
    {
        if (lines[i].empty())
            continue;
        const auto f = split(lines[i], '\t');
        if (f.size() != 8)
            fail("malformed syscalls row");
        rows.push_back({parse_u64(f[1]), parse_u64(f[2]), static_cast<unsigned>(parse_u64(f[3])),
                        static_cast<unsigned>(parse_u64(f[4])), f[5], parse_u64(f[6]), parse_u64(f[7])});
    }
    return rows;
}

std::vector<Symbol> read_symbols()
{
    const auto lines = validated_lines(read_file(std::string(k_proc_root) + "/symbols"), "symbols");
    std::vector<Symbol> symbols;
    for (size_t i = 2; i < lines.size(); ++i)
    {
        if (lines[i].empty())
            continue;
        const auto f = split(lines[i], '\t');
        if (f.size() != 3)
            fail("malformed symbols row");
        symbols.push_back({parse_u64(f[0], 0), parse_u64(f[1], 0), f[2]});
    }
    return symbols;
}

std::vector<ProfileRow> parse_profile_content(const std::string &content)
{
    const auto lines = validated_lines(content, "profile");
    std::vector<ProfileRow> rows;
    for (size_t i = 2; i < lines.size(); ++i)
    {
        if (lines[i].empty())
            continue;
        const auto f = split(lines[i], '\t');
        if (f.size() != 8)
            fail("malformed profile row");
        ProfileRow row{f[3], static_cast<unsigned>(parse_u64(f[4])), parse_u64(f[5]), {}, 0, 0, 0, 0};
        if (row.record == "flat" || row.record == "callchain")
        {
            for (const auto &pc : split(f[7], ','))
                row.pcs.push_back(parse_u64(pc, 0));
        }
        else if (row.record == "stats")
        {
            for (const auto &item : split(f[7], ','))
            {
                const auto pair = split(item, '=');
                if (pair.size() != 2)
                    fail("malformed profile stats");
                const uint64_t value = parse_u64(pair[1]);
                if (pair[0] == "dropped_full") row.dropped_full = value;
                else if (pair[0] == "invalid_pc") row.invalid_pc = value;
                else if (pair[0] == "user_skipped") row.user_skipped = value;
                else if (pair[0] == "unwind_failed") row.unwind_failed = value;
                else fail("unknown profile stats field: " + pair[0]);
            }
        }
        else
            fail("unknown profile record: " + row.record);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<ProfileRow> read_profile()
{
    return parse_profile_content(read_file(std::string(k_proc_root) + "/profile"));
}

std::string json_escape(const std::string &text)
{
    std::ostringstream out;
    for (unsigned char c : text)
    {
        switch (c)
        {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
            else
                out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string symbol_for(uint64_t pc, const std::vector<Symbol> &symbols)
{
    auto it = std::upper_bound(symbols.begin(), symbols.end(), pc,
                               [](uint64_t address, const Symbol &symbol) { return address < symbol.start; });
    if (it == symbols.begin())
    {
        std::ostringstream raw;
        raw << "0x" << std::hex << pc;
        return raw.str();
    }
    --it;
    if (pc >= it->start && pc < it->end)
    {
        std::ostringstream named;
        named << it->name;
        if (pc != it->start)
            named << "+0x" << std::hex << (pc - it->start);
        return named.str();
    }
    std::ostringstream raw;
    raw << "0x" << std::hex << pc;
    return raw.str();
}

int run_command(char **argv, bool redirect_stdout_to_stderr)
{
    const pid_t pid = ::fork();
    if (pid < 0)
        fail(std::string("fork failed: ") + std::strerror(errno));
    if (pid == 0)
    {
        if (redirect_stdout_to_stderr && ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
        {
            std::fprintf(stderr, "f7ly-perf: redirect command stdout failed: %s\n", std::strerror(errno));
            _exit(127);
        }
        ::execvp(argv[0], argv);
        std::fprintf(stderr, "f7ly-perf: exec %s failed: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void command_status(bool json)
{
    const auto meta = read_meta();
    if (json)
    {
        std::cout << '{';
        bool first = true;
        for (const auto &entry : meta)
        {
            if (!first) std::cout << ',';
            first = false;
            std::cout << '"' << json_escape(entry.first) << "\":\"" << json_escape(entry.second) << '"';
        }
        std::cout << "}\n";
        return;
    }
    for (const auto &entry : meta)
        std::cout << std::left << std::setw(30) << entry.first << entry.second << '\n';
}

using MetricMap = std::map<std::pair<unsigned, std::string>, uint64_t>;

MetricMap metric_map(const std::vector<MetricRow> &rows)
{
    MetricMap result;
    for (const auto &row : rows)
        result[{row.cpu, row.name}] = row.value;
    return result;
}

uint64_t delta_value(uint64_t before, uint64_t after)
{
    return after >= before ? after - before : 0;
}

int command_stat(int argc, char **argv)
{
    unsigned interval_ms = 1000;
    unsigned count = 1;
    bool per_cpu = false;
    bool json = false;
    int command_index = -1;
    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--interval-ms" && i + 1 < argc) interval_ms = static_cast<unsigned>(parse_u64(argv[++i]));
        else if (arg == "--count" && i + 1 < argc) count = static_cast<unsigned>(parse_u64(argv[++i]));
        else if (arg == "--per-cpu") per_cpu = true;
        else if (arg == "--json") json = true;
        else if (arg == "--") { command_index = i + 1; break; }
        else fail("unknown stat option: " + arg);
    }
    if (interval_ms == 0 || count == 0)
        fail("stat interval and count must be positive");

    int child_status = 0;
    for (unsigned iteration = 0; iteration < count; ++iteration)
    {
        const auto meta = read_meta();
        const uint64_t timebase = parse_u64(meta.at("timebase_hz"));
        const auto before_rows = read_metrics();
        const auto before_syscalls = read_syscalls();
        if (command_index >= 0)
        {
            if (command_index >= argc)
                fail("stat -- requires a command");
            child_status = run_command(argv + command_index, json);
        }
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        const auto after_rows = read_metrics();
        const auto after_syscalls = read_syscalls();
        if (before_rows.empty() || after_rows.empty() || before_rows.front().epoch != after_rows.front().epoch)
            fail("metrics epoch changed during stat interval");

        const uint64_t elapsed_ticks = after_rows.front().timestamp - before_rows.front().timestamp;
        const double seconds = elapsed_ticks != 0 ? static_cast<double>(elapsed_ticks) / timebase
                                                  : static_cast<double>(interval_ms) / 1000.0;
        const MetricMap before = metric_map(before_rows);
        const MetricMap after = metric_map(after_rows);
        std::map<std::pair<unsigned, std::string>, uint64_t> deltas;
        std::map<std::string, bool> max_aggregate;
        for (const auto &row : after_rows)
            max_aggregate[row.name] = row.kind == "gauge";
        for (const auto &entry : after)
        {
            const auto found = before.find(entry.first);
            deltas[entry.first] = max_aggregate[entry.first.second]
                                      ? entry.second
                                      : delta_value(found == before.end() ? 0 : found->second, entry.second);
        }

        std::map<std::string, uint64_t> totals;
        for (const auto &entry : deltas)
        {
            if (max_aggregate[entry.first.second])
                totals[entry.first.second] = std::max(totals[entry.first.second], entry.second);
            else
                totals[entry.first.second] += entry.second;
        }
        const uint64_t hits = totals["file_cache.hits"];
        const uint64_t misses = totals["file_cache.misses"];
        const double hit_rate = hits + misses == 0 ? 0.0 : 100.0 * hits / (hits + misses);
        uint64_t syscall_count = 0;
        uint64_t syscall_ticks = 0;
        std::map<std::pair<unsigned, unsigned>, SyscallRow> syscall_before;
        for (const auto &row : before_syscalls) syscall_before[{row.cpu, row.number}] = row;
        for (const auto &row : after_syscalls)
        {
            const auto found = syscall_before.find({row.cpu, row.number});
            syscall_count += delta_value(found == syscall_before.end() ? 0 : found->second.count, row.count);
            syscall_ticks += delta_value(found == syscall_before.end() ? 0 : found->second.time_ticks, row.time_ticks);
        }

        if (json)
        {
            std::cout << "{\"iteration\":" << (iteration + 1) << ",\"seconds\":" << seconds
                      << ",\"syscalls\":" << syscall_count
                      << ",\"syscalls_per_second\":" << (syscall_count / seconds)
                      << ",\"average_syscall_time_ticks\":" << (syscall_count ? syscall_ticks / syscall_count : 0)
                      << ",\"file_cache_hit_rate_percent\":" << hit_rate
                      << ",\"ext4_read_bytes_per_second\":" << totals["ext4.read_bytes"] / seconds
                      << ",\"ext4_write_bytes_per_second\":" << totals["ext4.write_bytes"] / seconds
                      << ",\"metrics\":{";
            bool first = true;
            for (const auto &entry : totals)
            {
                if (!first) std::cout << ',';
                first = false;
                std::cout << '"' << json_escape(entry.first) << "\":" << entry.second;
            }
            std::cout << "}}\n";
        }
        else
        {
            std::cout << "interval " << std::fixed << std::setprecision(3) << seconds << " s\n";
            std::cout << "syscalls " << syscall_count << " (" << syscall_count / seconds
                      << "/s), avg " << (syscall_count ? syscall_ticks / syscall_count : 0) << " time_ticks\n";
            std::cout << "file cache hit rate " << std::setprecision(2) << hit_rate << "%\n";
            std::cout << "ext4 read " << totals["ext4.read_bytes"] / seconds
                      << " B/s, write " << totals["ext4.write_bytes"] / seconds << " B/s\n";
            for (const auto &entry : deltas)
            {
                if (entry.second == 0)
                    continue;
                if (per_cpu)
                    std::cout << "cpu" << entry.first.first << ' ';
                else if (entry.first.first != 0)
                    continue;
                const uint64_t value = per_cpu ? entry.second : totals[entry.first.second];
                std::cout << std::left << std::setw(34) << entry.first.second << value << '\n';
            }
        }
        if (command_index >= 0)
            break;
    }
    if (child_status != 0)
        std::cerr << "f7ly-perf: command exited with status " << child_status << '\n';
    return child_status;
}

int command_top(int argc, char **argv)
{
    std::string backend = "auto";
    std::string event = "cycles";
    unsigned frequency = 100;
    uint64_t period = 1000000;
    bool callgraph = false;
    unsigned duration = 5;
    unsigned limit = 20;
    bool json = false;
    int command_index = -1;
    for (int i = 0; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--backend" && i + 1 < argc) backend = argv[++i];
        else if (arg == "--event" && i + 1 < argc) event = argv[++i];
        else if (arg == "--frequency" && i + 1 < argc) frequency = static_cast<unsigned>(parse_u64(argv[++i]));
        else if (arg == "--period" && i + 1 < argc) period = parse_u64(argv[++i]);
        else if (arg == "--callgraph") callgraph = true;
        else if (arg == "--duration" && i + 1 < argc) duration = static_cast<unsigned>(parse_u64(argv[++i]));
        else if (arg == "--limit" && i + 1 < argc) limit = static_cast<unsigned>(parse_u64(argv[++i]));
        else if (arg == "--json") json = true;
        else if (arg == "--") { command_index = i + 1; break; }
        else fail("unknown top option: " + arg);
    }
    if ((backend != "auto" && backend != "timer" && backend != "pmu") ||
        (event != "cycles" && event != "instructions") || frequency == 0 || period == 0 ||
        (command_index < 0 && duration == 0) || limit == 0)
        fail("invalid top option value");

    write_control("profile reset");
    std::ostringstream start;
    start << "profile start backend=" << backend << " event=" << event
          << " frequency=" << frequency << " period=" << period
          << " callchain=" << (callgraph ? 1 : 0);
    write_control(start.str());
    const auto active_meta = read_meta();
    const std::string active_backend = active_meta.at("profile_active_backend");
    int child_status = 0;
    if (command_index >= 0)
    {
        if (command_index >= argc) fail("top -- requires a command");
        child_status = run_command(argv + command_index, json);
    }
    else
        std::this_thread::sleep_for(std::chrono::seconds(duration));
    write_control("profile stop");

    const auto symbols = read_symbols();
    const auto rows = read_profile();
    std::unordered_map<uint64_t, uint64_t> flat;
    uint64_t total = 0;
    uint64_t dropped_full = 0;
    uint64_t invalid_pc = 0;
    uint64_t user_skipped = 0;
    uint64_t unwind_failed = 0;
    std::map<std::vector<uint64_t>, uint64_t> chain_totals;
    for (const auto &row : rows)
    {
        if (row.record == "flat" && !row.pcs.empty())
            flat[row.pcs[0]] += row.count;
        else if (row.record == "callchain")
            chain_totals[row.pcs] += row.count;
        else if (row.record == "stats")
        {
            total += row.count;
            dropped_full += row.dropped_full;
            invalid_pc += row.invalid_pc;
            user_skipped += row.user_skipped;
            unwind_failed += row.unwind_failed;
        }
    }
    std::vector<std::pair<uint64_t, uint64_t>> sorted(flat.begin(), flat.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    if (sorted.size() > limit) sorted.resize(limit);
    std::vector<std::pair<std::vector<uint64_t>, uint64_t>> chains(chain_totals.begin(), chain_totals.end());
    std::sort(chains.begin(), chains.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    if (chains.size() > limit) chains.resize(limit);

    if (json)
    {
        std::cout << "{\"requested_backend\":\"" << json_escape(backend)
                  << "\",\"backend\":\"" << json_escape(active_backend) << "\",\"event\":\""
                  << json_escape(event) << "\",\"samples\":" << total << ",\"hotspots\":[";
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            if (i) std::cout << ',';
            std::cout << "{\"pc\":\"0x" << std::hex << sorted[i].first << std::dec
                      << "\",\"symbol\":\"" << json_escape(symbol_for(sorted[i].first, symbols))
                      << "\",\"count\":" << sorted[i].second
                      << ",\"percent\":" << (total ? 100.0 * sorted[i].second / total : 0.0) << '}';
        }
        std::cout << "],\"dropped\":{\"table_full\":" << dropped_full
                  << ",\"invalid_pc\":" << invalid_pc
                  << ",\"user_skipped\":" << user_skipped
                  << ",\"unwind_failed\":" << unwind_failed << '}';
        if (callgraph)
        {
            std::cout << ",\"callchains\":[";
            for (size_t i = 0; i < chains.size(); ++i)
            {
                if (i) std::cout << ',';
                std::cout << "{\"count\":" << chains[i].second << ",\"frames\":[";
                for (size_t depth = 0; depth < chains[i].first.size(); ++depth)
                {
                    if (depth) std::cout << ',';
                    std::cout << '"' << json_escape(symbol_for(chains[i].first[depth], symbols)) << '"';
                }
                std::cout << "]}";
            }
            std::cout << ']';
        }
        std::cout << "}\n";
    }
    else
    {
        std::cout << "samples " << total << ", backend " << active_backend
                  << " (requested " << backend << "), event " << event << '\n';
        std::cout << "dropped full=" << dropped_full << " invalid_pc=" << invalid_pc
                  << " user_skipped=" << user_skipped << " unwind_failed=" << unwind_failed << '\n';
        std::cout << " percent      count  symbol\n";
        for (const auto &entry : sorted)
            std::cout << std::setw(7) << std::fixed << std::setprecision(2)
                      << (total ? 100.0 * entry.second / total : 0.0) << "% "
                      << std::setw(10) << entry.second << "  " << symbol_for(entry.first, symbols) << '\n';
        if (callgraph)
        {
            for (const auto &chain : chains)
            {
                std::cout << "\n" << chain.second << " samples\n";
                for (uint64_t pc : chain.first)
                    std::cout << "  " << symbol_for(pc, symbols) << '\n';
            }
        }
    }
    if (child_status != 0)
        std::cerr << "f7ly-perf: command exited with status " << child_status << '\n';
    return child_status;
}

void usage()
{
    std::cerr << "usage: f7ly-perf status [--json]\n"
                 "       f7ly-perf stat [--interval-ms N --count N | -- command...] [--per-cpu] [--json]\n"
                 "       f7ly-perf top [--backend auto|timer|pmu] [--event cycles|instructions] "
                 "[--frequency HZ] [--period N] [--callgraph] [--duration SEC | -- command...] [--limit N] [--json]\n"
                 "       f7ly-perf reset [metrics|profile|all]\n";
}
}

#ifndef F7LY_PERF_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "status")
    {
        if (argc > 3 || (argc == 3 && std::string(argv[2]) != "--json")) fail("invalid status option");
        command_status(argc == 3);
    }
    else if (command == "stat") return command_stat(argc - 2, argv + 2);
    else if (command == "top") return command_top(argc - 2, argv + 2);
    else if (command == "reset")
    {
        if (argc > 3) fail("reset accepts at most one target");
        const std::string target = argc == 3 ? argv[2] : "all";
        if (target != "metrics" && target != "profile" && target != "all") fail("invalid reset target");
        write_control(target + " reset");
    }
    else
    {
        usage();
        return 2;
    }
    return 0;
}
#endif
