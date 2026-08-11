#define F7LY_PERF_NO_MAIN
#include "f7ly_perf.cc"
#include "../../kernel/libs/perf_diag_algorithms.hh"

#include <array>
#include <cassert>

int main()
{
    const auto lines = validated_lines(
        "f7ly-perf-v1\tmetrics\n"
        "snapshot_id\tepoch\ttimestamp_ticks\tcpu\tid\tname\tkind\tunit\tvalue\n"
        "1\t2\t100\t0\t1\tfile_cache.hits\tcounter\thits\t7\n",
        "metrics");
    assert(lines.size() == 3);
    const auto fields = split(lines[2], '\t');
    assert(fields.size() == 9 && parse_u64(fields[8]) == 7);

    const auto profile = parse_profile_content(
        "f7ly-perf-v1\tprofile\n"
        "snapshot_id\tepoch\ttimestamp_ticks\trecord\tcpu\tcount\tdepth\tpcs\n"
        "2\t4\t200\tstats\t0\t9\t0\tdropped_full=1,invalid_pc=2,user_skipped=3,unwind_failed=4\n"
        "2\t4\t200\tflat\t0\t5\t1\t0x1010\n"
        "2\t4\t200\tcallchain\t1\t4\t2\t0x1010,0x1100\n");
    assert(profile.size() == 3);
    assert(profile[0].count == 9 && profile[0].dropped_full == 1 &&
           profile[0].invalid_pc == 2 && profile[0].user_skipped == 3 &&
           profile[0].unwind_failed == 4);
    assert(profile[1].pcs.size() == 1 && profile[1].pcs[0] == 0x1010);
    assert(profile[2].pcs.size() == 2 && profile[2].pcs[1] == 0x1100);

    assert(delta_value(10, 19) == 9);
    assert(delta_value(19, 10) == 0);
    assert(perfdiag::detail::epoch_value(4, 4, 99) == 99);
    assert(perfdiag::detail::epoch_value(3, 4, 99) == 0);
    assert(perfdiag::detail::aggregate_max(8, 3) == 8);
    assert(perfdiag::detail::aggregate_max(8, 12) == 12);

    const std::vector<Symbol> symbols{{0x1000, 0x1100, "first"}, {0x1100, 0x1200, "second"}};
    assert(symbol_for(0x1000, symbols) == "first");
    assert(symbol_for(0x10ff, symbols) == "first+0xff");
    assert(symbol_for(0x1100, symbols) == "second");
    assert(symbol_for(0x1200, symbols) == "0x1200");

    std::array<uint64_t, 8> table{};
    auto insert = [&](uint64_t pc) {
        const size_t begin = perfdiag::detail::hash_pc(pc) % table.size();
        for (size_t probe = 0; probe < table.size(); ++probe)
        {
            uint64_t &slot = table[(begin + probe) % table.size()];
            if (slot == 0 || slot == pc)
            {
                slot = pc;
                return true;
            }
        }
        return false;
    };
    for (uint64_t pc = 1; pc <= table.size(); ++pc) assert(insert(pc));
    assert(!insert(99));

    const uint64_t chain_a[] = {0x1000, 0x1100};
    const uint64_t chain_b[] = {0x1000, 0x1110};
    assert(perfdiag::detail::hash_chain(chain_a, 2) != perfdiag::detail::hash_chain(chain_b, 2));
    assert(perfdiag::detail::stack_word_valid(0x2010, 0x2000, 0x3000));
    assert(!perfdiag::detail::stack_word_valid(0x2003, 0x2000, 0x3000));
    assert(perfdiag::detail::next_frame_valid(0x2100, 0x2200, 0x3000));
    assert(!perfdiag::detail::next_frame_valid(0x2200, 0x2100, 0x3000));

    assert(json_escape("a\n\"") == "a\\n\\\"");
    std::cout << "f7ly-perf native tests passed\n";
    return 0;
}
