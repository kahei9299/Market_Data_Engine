// Generate synthetic Binance-format depth update JSONL for replay and benchmarks.
// Usage: generate_data <count> <output_file>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: generate_data <count> <output_file>\n";
        return EXIT_FAILURE;
    }

    int count = std::atoi(argv[1]);
    if (count <= 0) {
        std::cerr << "count must be positive\n";
        return EXIT_FAILURE;
    }

    std::ofstream out(argv[2]);
    if (!out.is_open()) {
        std::cerr << "cannot open " << argv[2] << '\n';
        return EXIT_FAILURE;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> price_dist(70000, 80000);
    std::uniform_int_distribution<int> qty_dist(0, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> levels_dist(1, 5);

    uint64_t update_id = 1;
    uint64_t event_time = 1700000000000ULL;

    for (int i = 0; i < count; ++i) {
        int num_bids = levels_dist(rng);
        int num_asks = levels_dist(rng);

        uint64_t U = update_id;
        uint64_t u = update_id;
        update_id++;

        out << R"({"e":"depthUpdate","E":)" << event_time
            << R"(,"s":"BTCUSDT","U":)" << U << R"(,"u":)" << u;

        out << R"(,"b":[)";
        for (int j = 0; j < num_bids; ++j) {
            if (j > 0) out << ',';
            int price = price_dist(rng);
            int qty = qty_dist(rng);
            out << "[\"" << price << ".00000000\",\""
                << qty << ".00000000\"]";
        }

        out << R"(],"a":[)";
        for (int j = 0; j < num_asks; ++j) {
            if (j > 0) out << ',';
            int price = price_dist(rng);
            int qty = qty_dist(rng);
            out << "[\"" << price << ".00000000\",\""
                << qty << ".00000000\"]";
        }

        out << "]}\n";
        event_time++;
    }

    std::cerr << "generated " << count << " messages to " << argv[2] << '\n';
    return EXIT_SUCCESS;
}
