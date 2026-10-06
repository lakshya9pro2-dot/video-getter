#include "extractor.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <URL> [URL2 ...] [OPTIONS]\n"
              << "       " << prog << " --file <path> [OPTIONS]\n"
              << "       " << prog << " --server [PORT] [OPTIONS]\n\n"
              << "Lightweight headless WPE WebKit concurrent HLS URL extractor.\n\n"
              << "Options:\n"
              << "  --user-agent <string>       Custom User-Agent header (default: Mozilla/5.0 ...)\n"
              << "  --timeout <ms>              Timeout in milliseconds (default: 20000)\n"
              << "  -c, -j, --concurrency <N>   Max concurrent extraction jobs (default: 4)\n"
              << "  -f, --file <path>           Read URLs from file (one per line)\n"
              << "  --stdin                     Read URLs from standard input\n"
              << "  --json                      Output results in JSON format\n"
              << "  --server, --http [port]     Run in concurrent HTTP API server mode (default port: 8080)\n"
              << "  -v, --verbose               Print verbose logs to stderr\n"
              << "  -h, --help                  Show this help message\n\n"
              << "Exit codes:\n"
              << "  0 = HLS URL found (or at least one found in multi-URL mode)\n"
              << "  1 = No HLS URL found (timeout or page ended)\n"
              << "  2 = Invalid input/URL\n"
              << "  3 = Browser initialization error\n"
              << "  4 = Page / network load error\n";
}

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

int main(int argc, char* argv[]) {
    // Unbuffered output
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::vector<std::string> urls;
    std::string file_path;
    bool read_stdin = false;
    bool json_output = false;
    size_t concurrency = 4;
    const char* env_concurrency = std::getenv("CONCURRENCY");
    if (env_concurrency && *env_concurrency) {
        int c = std::atoi(env_concurrency);
        if (c > 0) concurrency = static_cast<size_t>(c);
    }

    ExtractionOptions options;
    bool server_mode = false;
    int server_port = 8080;
    const char* env_port = std::getenv("PORT");
    if (env_port && *env_port) {
        int p = std::atoi(env_port);
        if (p > 0) server_port = p;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--user-agent" && i + 1 < argc) {
            options.user_agent = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            options.timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if ((arg == "-c" || arg == "-j" || arg == "--concurrency") && i + 1 < argc) {
            concurrency = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
            file_path = argv[++i];
        } else if (arg == "--stdin") {
            read_stdin = true;
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "--server" || arg == "--http") {
            server_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                server_port = std::atoi(argv[++i]);
            }
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "-") {
            read_stdin = true;
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 2;
        } else {
            urls.push_back(arg);
        }
    }

    if (server_mode) {
        return run_http_server(server_port, options.user_agent, options.timeout_ms, concurrency, options.verbose);
    }

    if (!file_path.empty()) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open URL file: " << file_path << std::endl;
            return 2;
        }
        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (!line.empty() && line[0] != '#') {
                urls.push_back(line);
            }
        }
    }

    if (read_stdin) {
        std::string line;
        while (std::getline(std::cin, line)) {
            line = trim(line);
            if (!line.empty() && line[0] != '#') {
                urls.push_back(line);
            }
        }
    }

    if (urls.empty()) {
        std::cerr << "Error: No URL specified.\n\n";
        print_usage(argv[0]);
        return 2;
    }

    // Single URL without --json: maintain exact previous output format for 100% backward compatibility
    if (urls.size() == 1 && !json_output) {
        options.url = urls[0];
        ExtractionResult result = run_extraction(options);

        if (result.found) {
            std::cout << result.matched_url << std::endl;
            return 0;
        } else {
            if (!result.error_message.empty() && options.verbose) {
                std::cerr << "Error details: " << result.error_message << std::endl;
            }
            if (result.exit_code == 1) {
                std::cout << "No HLS URL found" << std::endl;
            } else if (!result.error_message.empty()) {
                std::cerr << result.error_message << std::endl;
            }
            return result.exit_code;
        }
    }

    // Multi-URL mode (or single URL with --json)
    MultiExtractionOptions multi_opts;
    multi_opts.urls = urls;
    multi_opts.user_agent = options.user_agent;
    multi_opts.timeout_ms = options.timeout_ms;
    multi_opts.max_concurrency = concurrency;
    multi_opts.verbose = options.verbose;
    multi_opts.output_json = json_output;

    if (json_output) {
        auto results = run_multi_extraction(multi_opts, nullptr);
        std::cout << "[\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& item = results[i];
            std::string item_origin = get_url_origin(item.url);
            std::cout << "  {\n"
                      << "    \"input_url\": \"" << escape_json(item.url) << "\",\n"
                      << "    \"success\": " << (item.result.found ? "true" : "false") << ",\n"
                      << "    \"stream_url\": " << (item.result.found ? ("\"" + escape_json(item.result.matched_url) + "\"") : "null") << ",\n"
                      << "    \"headers\": {\n"
                      << "      \"Origin\": \"" << escape_json(item_origin) << "\",\n"
                      << "      \"Referer\": \"" << escape_json(item_origin) << "\"\n"
                      << "    }";
            if (!item.result.found && !item.result.error_message.empty()) {
                std::cout << ",\n    \"error\": \"" << escape_json(item.result.error_message) << "\"";
            }
            std::cout << "\n  }" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        std::cout << "]\n";

        bool any_found = false;
        for (const auto& item : results) {
            if (item.result.found) {
                any_found = true;
                break;
            }
        }
        return any_found ? 0 : 1;
    } else {
        size_t found_count = 0;
        auto results = run_multi_extraction(multi_opts, [&found_count](const MultiExtractionItem& item) {
            if (item.result.found) {
                found_count++;
                std::cout << "[FOUND]  " << item.url << " -> " << item.result.matched_url << std::endl;
            } else {
                std::cout << "[FAILED] " << item.url;
                if (!item.result.error_message.empty()) {
                    std::cout << " (" << item.result.error_message << ")";
                } else {
                    std::cout << " (No HLS URL found)";
                }
                std::cout << std::endl;
            }
        });

        std::cout << "\nDone. Found: " << found_count << "/" << results.size() << std::endl;
        return (found_count > 0) ? 0 : 1;
    }
}
