#include "exsim/core/exchange.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: replay_check <journal_path> <max_symbol>\n";
    return 2;
  }
  exsim::core::ExchangeConfig cfg;
  cfg.max_symbol = static_cast<exsim::SymbolId>(std::stoul(argv[2]));
  auto result = exsim::core::replay_journal(argv[1], cfg);
  if (result.error) {
    std::cerr << "replay error: " << *result.error << "\n";
    return 1;
  }
  std::cout << "records=" << result.records << "\n";
  std::cout << "book_checksum=" << result.book_checksum << "\n";
  std::cout << "md_checksum=" << result.md_checksum << "\n";
  return 0;
}
