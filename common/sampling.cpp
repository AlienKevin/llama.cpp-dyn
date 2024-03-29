#include "sampling.h"
#include <fstream>
#include <chrono>
#include <regex>

static uint64_t prev_sampling_time = 0;

struct llama_sampling_context * llama_sampling_init(const struct llama_sampling_params & params) {
    struct llama_sampling_context * result = new llama_sampling_context();

    result->params  = params;
    result->grammar = nullptr;

    // if there is a grammar, parse it
    if (!params.grammar.empty()) {
        result->parsed_grammar = grammar_parser::parse(params.grammar.c_str());

        // will be empty (default) if there are parse errors
        if (result->parsed_grammar.rules.empty()) {
            fprintf(stderr, "%s: failed to parse grammar\n", __func__);
            return nullptr;
        }

        std::vector<const llama_grammar_element *> grammar_rules(result->parsed_grammar.c_rules());

        result->grammar = llama_grammar_init(
                grammar_rules.data(),
                grammar_rules.size(), result->parsed_grammar.symbol_ids.at("root"));
    }

    result->prev.resize(params.n_prev);

    return result;
}

void llama_sampling_free(struct llama_sampling_context * ctx) {
    if (ctx->grammar != NULL) {
        llama_grammar_free(ctx->grammar);
    }

    delete ctx;
}

void llama_sampling_reset(llama_sampling_context * ctx) {
    if (ctx->grammar != NULL) {
        llama_grammar_free(ctx->grammar);
        ctx->grammar = NULL;
    }

    if (!ctx->parsed_grammar.rules.empty()) {
        std::vector<const llama_grammar_element *> grammar_rules(ctx->parsed_grammar.c_rules());

        ctx->grammar = llama_grammar_init(
                grammar_rules.data(),
                grammar_rules.size(), ctx->parsed_grammar.symbol_ids.at("root"));
    }

    std::fill(ctx->prev.begin(), ctx->prev.end(), 0);
    ctx->cur.clear();
    ctx->prev_all.clear();
    ctx->prelude_len = 0;
}

void llama_sampling_cp(llama_sampling_context * src, llama_sampling_context * dst) {
    if (dst->grammar) {
        llama_grammar_free(dst->grammar);
        dst->grammar = nullptr;
    }

    if (src->grammar) {
        dst->grammar = llama_grammar_copy(src->grammar);
    }

    dst->prev = src->prev;
    dst->prev_all = src->prev_all;
    dst->prelude_len = src->prelude_len;
}

llama_token llama_sampling_last(llama_sampling_context * ctx) {
    return ctx->prev.back();
}

std::string llama_sampling_prev_str(llama_sampling_context * ctx_sampling, llama_context * ctx_main, int n) {
    const int size = ctx_sampling->prev.size();

    n = std::min(n, size);

    std::string result;

    for (int i = size - n; i < size; i++) {
        result += llama_token_to_piece(ctx_main, ctx_sampling->prev[i]);
    }

    return result;
}

void llama_sampling_set_prelude_len(llama_sampling_context * ctx, size_t prelude_len) {
    ctx->prelude_len = prelude_len;
}

std::string llama_sampling_prev_all_str(llama_sampling_context * ctx_sampling, llama_context * ctx_main, int start_skip_tokens, int end_skip_tokens) {
    std::string result;

    auto total_tokens = ctx_sampling->prev_all.size();
    for (auto i = start_skip_tokens; i < total_tokens - end_skip_tokens; ++i) {
        auto token = ctx_sampling->prev_all[i];
        result += llama_token_to_piece(ctx_main, token);
    }

    return result;
}

std::string llama_sampling_print(const llama_sampling_params & params) {
    char result[1024];

    snprintf(result, sizeof(result),
            "\trepeat_last_n = %d, repeat_penalty = %.3f, frequency_penalty = %.3f, presence_penalty = %.3f\n"
            "\ttop_k = %d, tfs_z = %.3f, top_p = %.3f, min_p = %.3f, typical_p = %.3f, temp = %.3f\n"
            "\tmirostat = %d, mirostat_lr = %.3f, mirostat_ent = %.3f",
            params.penalty_last_n, params.penalty_repeat, params.penalty_freq, params.penalty_present,
            params.top_k, params.tfs_z, params.top_p, params.min_p, params.typical_p, params.temp,
            params.mirostat, params.mirostat_eta, params.mirostat_tau);

    return std::string(result);
}

std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::string extract_substring_after_delimiter(const std::string& str, const std::string& delimiter) {
    size_t pos = str.find(delimiter);
    if (pos != std::string::npos) {
        // Add the length of the delimiter to 'pos' to start after the delimiter
        pos += delimiter.length();

        // Extract the substring from 'pos' to the end of the string
        std::string extracted = str.substr(pos);

        // Optional: trim leading whitespace if needed
        size_t start = extracted.find_first_not_of(" \n\r\t\f\v");
        return (start == std::string::npos) ? "" : extracted.substr(start);
    }
    return ""; // Return empty string if delimiter is not found
}

std::string fix_grammar(const std::string& grammar) {
    std::string output = std::regex_replace(grammar, std::regex(R"(whitespace ::= \[ \\n\]\+)"), R"(whitespace ::= [ \n]*)");
    output = std::regex_replace(output, std::regex(R"(::= "whitespace")"), R"(::= whitespace)");
    output = std::regex_replace(output, std::regex(R"(new_tokens)"), R"(new-tokens)");
    output = std::regex_replace(output, std::regex(R"(new-tokens ::= whitespace \| (.+))"), R"(new-tokens ::= whitespace ($1))");
    return output;
}

std::string llama_sampling_order_print(const llama_sampling_params & params) {
    std::string result = "CFG -> Penalties ";
    if (params.mirostat == 0) {
        for (auto s : params.samplers_sequence) {
            switch (s) {
                case 'k': result += "-> top_k "; break;
                case 'f': result += "-> tfs_z "; break;
                case 'y': result += "-> typical_p "; break;
                case 'p': result += "-> top_p "; break;
                case 'm': result += "-> min_p "; break;
                case 't': result += "-> temp "; break;
                default : break;
            }
        }
    } else {
        result += "-> mirostat ";
    }

    return result;
}

// no reasons to expose this function in header
static void sampler_queue(
                   struct llama_context * ctx_main,
            const llama_sampling_params & params,
                 llama_token_data_array & cur_p,
                                 size_t & min_keep) {
    const int n_vocab = llama_n_vocab(llama_get_model(ctx_main));

    const float         temp              = params.temp;
    const int32_t       top_k             = params.top_k <= 0 ? n_vocab : params.top_k;
    const float         top_p             = params.top_p;
    const float         min_p             = params.min_p;
    const float         tfs_z             = params.tfs_z;
    const float         typical_p         = params.typical_p;
    const std::string & samplers_sequence = params.samplers_sequence;

    for (auto s : samplers_sequence) {
        switch (s){
            case 'k': llama_sample_top_k    (ctx_main, &cur_p, top_k,     min_keep); break;
            case 'f': llama_sample_tail_free(ctx_main, &cur_p, tfs_z,     min_keep); break;
            case 'y': llama_sample_typical  (ctx_main, &cur_p, typical_p, min_keep); break;
            case 'p': llama_sample_top_p    (ctx_main, &cur_p, top_p,     min_keep); break;
            case 'm': llama_sample_min_p    (ctx_main, &cur_p, min_p,     min_keep); break;
            case 't': llama_sample_temp     (ctx_main, &cur_p, temp); break;
            default : break;
        }
    }
}

std::string escape_string(const std::string& input) {
    std::string output;
    for (char c : input) {
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '\"': output += "\\\""; break;
            default: output += c; break;
        }
    }
    return output;
}

// Function to check if the string ends with a substring repeating 5 or more times
bool ends_with_repeated_substring(const std::string& str, int max_length, int min_repetitions) {
    // Check for excessively repeated spaces (>= 40 times)
    if (str.length() >= 40) {
        std::string last_sub = str.substr(str.length() - 40);
        if (std::all_of(last_sub.begin(), last_sub.end(), [](char c) { return c == ' ' || c == '\t'; })) {
            return true;
        }
    }
    
    for (int len = 1; len <= max_length; ++len) { // Length of the substring
        if (str.length() < min_repetitions * len) continue; // Ensure there's enough length for the minimum repetitions

        bool is_repeating = true;
        std::string last_sub = str.substr(str.length() - len); // Last substring of length 'len'

        // Check if the substring is a continuous stretch of spaces or tabs
        if (std::all_of(last_sub.begin(), last_sub.end(), [](char c) { return c == ' ' || c == '\t'; })) {
            continue;
        }

        // Check for the minimum number of consecutive repetitions
        for (int rep = 1; rep < min_repetitions; ++rep) {
            std::string test_sub = str.substr(str.length() - (rep + 1) * len, len);
            if (last_sub != test_sub) {
                is_repeating = false;
                break; // No need to check further if any preceding substring doesn't match
            }
        }

        if (is_repeating) {
            return true; // Found a substring that repeats the minimum number of times
        }
    }
    return false; // No such repetition found
}

// https://stackoverflow.com/a/2072890/6798201
inline bool ends_with(std::string const &value, std::string const &ending) {
    if (ending.size() > value.size())
        return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

llama_token llama_sampling_sample(
                  struct llama_sampling_context * ctx_sampling,
                  struct llama_context * ctx_main,
                  struct llama_context * ctx_cfg,
                  const int idx) {
    const llama_sampling_params & params = ctx_sampling->params;

    const int n_vocab = llama_n_vocab(llama_get_model(ctx_main));

    const float   temp            = params.temp;
    const int32_t penalty_last_n  = params.penalty_last_n < 0 ? params.n_prev : params.penalty_last_n;
    const float   penalty_repeat  = params.penalty_repeat;
    const float   penalty_freq    = params.penalty_freq;
    const float   penalty_present = params.penalty_present;
    const int     mirostat        = params.mirostat;
    const float   mirostat_tau    = params.mirostat_tau;
    const float   mirostat_eta    = params.mirostat_eta;
    const bool    penalize_nl     = params.penalize_nl;

    auto & prev = ctx_sampling->prev;
    auto & cur  = ctx_sampling->cur;

    llama_token id = 0;

    float * logits = llama_get_logits_ith(ctx_main, idx);

    // apply params.logit_bias map
    for (auto it = params.logit_bias.begin(); it != params.logit_bias.end(); it++) {
        logits[it->first] += it->second;
    }

    cur.clear();

    for (llama_token token_id = 0; token_id < n_vocab; token_id++) {
        cur.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
    }

    llama_token_data_array cur_p = { cur.data(), cur.size(), false };

    if (ctx_cfg) {
        llama_sample_classifier_free_guidance(ctx_main, &cur_p, ctx_cfg, params.cfg_scale);
    }

    // apply penalties
    if (!prev.empty()) {
        const float nl_logit = logits[llama_token_nl(llama_get_model(ctx_main))];

        llama_sample_repetition_penalties(ctx_main, &cur_p,
                prev.data() + prev.size() - penalty_last_n,
                penalty_last_n, penalty_repeat, penalty_freq, penalty_present);

        if (!penalize_nl) {
            for (size_t idx = 0; idx < cur_p.size; idx++) {
                if (cur_p.data[idx].id == llama_token_nl(llama_get_model(ctx_main))) {
                    cur_p.data[idx].logit = nl_logit;
                    break;
                }
            }
        }
    }

    // Early exit when a function is finished
    std::string last_few_tokens_str = "";
    for (int i = 3; i > 0; i--) {
        last_few_tokens_str += llama_token_to_piece(ctx_main, ctx_sampling->prev_all[ctx_sampling->prev_all.size() - i]);
    }

    if (ends_with(last_few_tokens_str, "in\n\n")) {
        exit(0);
    }

    auto prev_all_str = llama_sampling_prev_all_str(ctx_sampling, ctx_main, ctx_sampling->prelude_len, 0);
    int max_length = 30; // Maximum length of substrings to check for repetitions
    int min_repetitions = 5; // Minimum number of times a substring must repeat to count
    if (ends_with_repeated_substring(prev_all_str, max_length, min_repetitions))
    {
        exit(0);
    }

    if (!params.dynamic_grammar.empty()) {
        // The last token just sampled will be the new token
        auto new_token = llama_token_to_piece(ctx_main, ctx_sampling->prev_all[ctx_sampling->prev_all.size() - 1]);
        std::string command = "node ../lsp.js COMPLETIONS " + params.dynamic_grammar + " --prelude ../autoregressive.prelude --debug --new-token \"" + escape_string(new_token) + "\" ";
        command += "\"" + escape_string(llama_sampling_prev_all_str(ctx_sampling, ctx_main, ctx_sampling->prelude_len, 1)) + "\"";

        std::string output = exec(command.c_str());
        std::string grammar_str = fix_grammar(extract_substring_after_delimiter(output, "LSP: Grammar:\n"));
        
        std::ofstream log_file;
        // Open the log file in append mode
        log_file.open("log.txt", std::ios::app);

        if (log_file.is_open()) {
            // Write the log message to the file
            log_file << std::endl << "================" << std::endl << llama_sampling_prev_all_str(ctx_sampling, ctx_main, ctx_sampling->prelude_len, 0) << std::endl << std::endl;
            // log_file << "\nCommand:" << std::endl;
            // log_file << command << std::endl;
            log_file << output << std::endl;

            // Close the file
            log_file.close();
        } else {
            // Handle the error if the file couldn't be opened
            std::cerr << "Unable to open the log file." << std::endl;
        }
        
        ctx_sampling->parsed_grammar = grammar_parser::parse(grammar_str.c_str());

        // will be empty (default) if there are parse errors
        if (ctx_sampling->parsed_grammar.rules.empty()) {
            fprintf(stderr, "%s: failed to parse grammar\n", __func__);
        } else {
            std::vector<const llama_grammar_element *> grammar_rules(ctx_sampling->parsed_grammar.c_rules());
            if (ctx_sampling->grammar != NULL) {
                llama_grammar_free(ctx_sampling->grammar);
            }
            ctx_sampling->grammar = llama_grammar_init(
                    grammar_rules.data(),
                    grammar_rules.size(), ctx_sampling->parsed_grammar.symbol_ids.at("root"));
            llama_sample_grammar(ctx_main, &cur_p, ctx_sampling->grammar);
        }
    } else if (ctx_sampling->grammar != NULL) {
        // std::ofstream log_file;
        // // Open the log file in append mode
        // log_file.open("log_grammar.txt", std::ios::app);

        // if (log_file.is_open()) {
        //     // Write the log message to the file
        //     time_t elapsed;
        //     time_t current_sampling_time = std::chrono::system_clock::now().time_since_epoch().count();
        //     if (prev_sampling_time > 0) {
        //         elapsed = current_sampling_time - prev_sampling_time;
        //     } else {
        //         elapsed = 0;
        //     }
        //     prev_sampling_time = current_sampling_time;
        //     log_file << llama_grammar_get_stack_size(ctx_sampling->grammar) << "," << elapsed << std::endl;

        //     // Close the file
        //     log_file.close();
        // } else {
        //     // Handle the error if the file couldn't be opened
        //     std::cerr << "Unable to open the log_grammar.txt" << std::endl;
        // }
        llama_sample_grammar(ctx_main, &cur_p, ctx_sampling->grammar);
    } else {
        std::ofstream log_file;
        // Open the log file in append mode
        log_file.open("log.txt", std::ios::app);

        if (log_file.is_open()) {
            // Write the log message to the file
            log_file << std::endl << "================" << std::endl << llama_sampling_prev_all_str(ctx_sampling, ctx_main, ctx_sampling->prelude_len, 0) << std::endl << std::endl;

            // Close the file
            log_file.close();
        } else {
            // Handle the error if the file couldn't be opened
            std::cerr << "Unable to open the log file." << std::endl;
        }
    }

    if (temp < 0.0) {
        // greedy sampling, with probs
        llama_sample_softmax(ctx_main, &cur_p);
        id = cur_p.data[0].id;
    } else if (temp == 0.0) {
        // greedy sampling, no probs
        id = llama_sample_token_greedy(ctx_main, &cur_p);
    } else {
        if (mirostat == 1) {
            const int mirostat_m = 100;
            llama_sample_temp(ctx_main, &cur_p, temp);
            id = llama_sample_token_mirostat(ctx_main, &cur_p, mirostat_tau, mirostat_eta, mirostat_m, &ctx_sampling->mirostat_mu);
        } else if (mirostat == 2) {
            llama_sample_temp(ctx_main, &cur_p, temp);
            id = llama_sample_token_mirostat_v2(ctx_main, &cur_p, mirostat_tau, mirostat_eta, &ctx_sampling->mirostat_mu);
        } else {
            // temperature sampling
            size_t min_keep = std::max(1, params.n_probs);

            sampler_queue(ctx_main, params, cur_p, min_keep);

            id = llama_sample_token(ctx_main, &cur_p);

            //{
            //    const int n_top = 10;
            //    LOG("top %d candidates:\n", n_top);

            //    for (int i = 0; i < n_top; i++) {
            //        const llama_token id = cur_p.data[i].id;
            //        (void)id; // To avoid a warning that id is unused when logging is disabled.
            //        LOG(" - %5d: '%12s' (%.3f)\n", id, llama_token_to_piece(ctx_main, id).c_str(), cur_p.data[i].p);
            //    }
            //}

            LOG("sampled token: %5d: '%s'\n", id, llama_token_to_piece(ctx_main, id).c_str());
        }
    }

    return id;
}

void llama_sampling_accept(
        struct llama_sampling_context * ctx_sampling,
        struct llama_context * ctx_main,
        llama_token id,
        bool apply_grammar) {
    ctx_sampling->prev.erase(ctx_sampling->prev.begin());
    ctx_sampling->prev.push_back(id);
    ctx_sampling->prev_all.push_back(id);

    if (ctx_sampling->grammar != NULL && apply_grammar) {
        llama_grammar_accept_token(ctx_main, ctx_sampling->grammar, id);
    }
}
