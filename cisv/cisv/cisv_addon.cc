#include <napi.h>
#include "cisv/parser.h"
#include "cisv/transformer.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <climits>
#include <cmath>

namespace {

static bool isInvalidConfigChar(char c) {
    return c == '\0' || c == '\n' || c == '\r';
}

static void ValidateSingleCharOption(
    Napi::Env env,
    const Napi::Object &options,
    const char *option_name,
    char *target,
    bool allow_null = false
) {
    if (!options.Has(option_name)) {
        return;
    }

    Napi::Value value = options.Get(option_name);
    if (allow_null && (value.IsNull() || value.IsUndefined())) {
        *target = 0;
        return;
    }

    if (!value.IsString()) {
        throw Napi::TypeError::New(env, std::string(option_name) + " must be a string");
    }

    std::string raw = value.As<Napi::String>();
    if (raw.size() != 1) {
        throw Napi::TypeError::New(env, std::string(option_name) + " must be exactly 1 character");
    }
    if (isInvalidConfigChar(raw[0])) {
        throw Napi::TypeError::New(env, std::string("Invalid ") + option_name + " character");
    }

    *target = raw[0];
}

static double MaxJsSafeInteger() {
    return 9007199254740991.0;
}

static bool IsWholeNumber(double value) {
    return std::isfinite(value) && std::floor(value) == value;
}

static void ApplyBooleanOption(
    Napi::Env env,
    const Napi::Object &options,
    const char *option_name,
    bool *target
) {
    if (!options.Has(option_name)) {
        return;
    }

    Napi::Value value = options.Get(option_name);
    if (!value.IsBoolean()) {
        throw Napi::TypeError::New(env, std::string(option_name) + " must be a boolean");
    }

    *target = value.As<Napi::Boolean>();
}

static void ApplySizeOption(
    Napi::Env env,
    const Napi::Object &options,
    const char *option_name,
    size_t *target
) {
    if (!options.Has(option_name)) {
        return;
    }

    Napi::Value value = options.Get(option_name);
    if (value.IsNull() || value.IsUndefined()) {
        *target = 0;
        return;
    }
    if (!value.IsNumber()) {
        throw Napi::TypeError::New(env, std::string(option_name) + " must be a number");
    }

    double raw = value.As<Napi::Number>().DoubleValue();
    double max_value = static_cast<double>(SIZE_MAX);
    if (!IsWholeNumber(raw) || raw < 0.0 || raw > max_value || raw > MaxJsSafeInteger()) {
        throw Napi::RangeError::New(env, std::string(option_name) + " is out of range");
    }

    *target = static_cast<size_t>(raw);
}

static void ApplyLineOption(
    Napi::Env env,
    const Napi::Object &options,
    const char *option_name,
    int *target
) {
    if (!options.Has(option_name)) {
        return;
    }

    Napi::Value value = options.Get(option_name);
    if (value.IsNull() || value.IsUndefined()) {
        *target = 0;
        return;
    }
    if (!value.IsNumber()) {
        throw Napi::TypeError::New(env, std::string(option_name) + " must be a number");
    }

    double raw = value.As<Napi::Number>().DoubleValue();
    if (!IsWholeNumber(raw) || raw < 0.0 || raw > static_cast<double>(INT_MAX)) {
        throw Napi::RangeError::New(env, std::string(option_name) + " is out of range");
    }

    *target = static_cast<int>(raw);
}

static void ValidateConfigSemantics(Napi::Env env, const cisv_config &config) {
    if (config.delimiter == config.quote) {
        throw Napi::TypeError::New(env, "delimiter and quote cannot be the same");
    }
    if (config.escape != '\0' && config.escape == config.delimiter) {
        throw Napi::TypeError::New(env, "escape and delimiter cannot be the same");
    }
    if (config.escape != '\0' && config.escape == config.quote) {
        throw Napi::TypeError::New(env, "escape and quote cannot be the same");
    }
    if (config.comment != '\0' &&
        (config.comment == config.delimiter || config.comment == config.quote || config.comment == config.escape)) {
        throw Napi::TypeError::New(env, "comment cannot conflict with delimiter, quote, or escape");
    }

    int effective_from = config.from_line > 0 ? config.from_line : 1;
    if (config.to_line != 0 && config.to_line < effective_from) {
        throw Napi::RangeError::New(env, "toLine must be >= fromLine");
    }
}

static void ApplyConfigOptions(Napi::Env env, const Napi::Object &options, cisv_config *config) {
    ValidateSingleCharOption(env, options, "delimiter", &config->delimiter);
    ValidateSingleCharOption(env, options, "quote", &config->quote);
    ValidateSingleCharOption(env, options, "escape", &config->escape, true);
    ValidateSingleCharOption(env, options, "comment", &config->comment, true);

    ApplyBooleanOption(env, options, "skipEmptyLines", &config->skip_empty_lines);
    ApplyBooleanOption(env, options, "trim", &config->trim);
    ApplyBooleanOption(env, options, "relaxed", &config->relaxed);
    ApplyBooleanOption(env, options, "skipLinesWithError", &config->skip_lines_with_error);

    ApplySizeOption(env, options, "maxRowSize", &config->max_row_size);
    ApplyLineOption(env, options, "fromLine", &config->from_line);
    ApplyLineOption(env, options, "toLine", &config->to_line);

    ValidateConfigSemantics(env, *config);
}

// =============================================================================
// SECURITY: UTF-8 validation to prevent V8 crashes on invalid input
// Invalid UTF-8 data can cause Napi::String::New to throw or crash
// =============================================================================
static bool isValidUtf8(const char* data, size_t len) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;

    while (i < len) {
        unsigned char c = bytes[i];

        if (c < 0x80) {
            // ASCII: single byte (0x00-0x7F)
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte sequence (0xC0-0xDF)
            if (i + 1 >= len) return false;
            if ((bytes[i + 1] & 0xC0) != 0x80) return false;
            // Overlong check: C0-C1 are invalid
            if (c < 0xC2) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte sequence (0xE0-0xEF)
            if (i + 2 >= len) return false;
            if ((bytes[i + 1] & 0xC0) != 0x80) return false;
            if ((bytes[i + 2] & 0xC0) != 0x80) return false;
            // Overlong check for E0
            if (c == 0xE0 && bytes[i + 1] < 0xA0) return false;
            // Surrogate check (U+D800-U+DFFF)
            if (c == 0xED && bytes[i + 1] >= 0xA0) return false;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte sequence (0xF0-0xF7)
            if (i + 3 >= len) return false;
            if ((bytes[i + 1] & 0xC0) != 0x80) return false;
            if ((bytes[i + 2] & 0xC0) != 0x80) return false;
            if ((bytes[i + 3] & 0xC0) != 0x80) return false;
            // Overlong check for F0
            if (c == 0xF0 && bytes[i + 1] < 0x90) return false;
            // Check for code points > U+10FFFF
            if (c == 0xF4 && bytes[i + 1] >= 0x90) return false;
            if (c > 0xF4) return false;
            i += 4;
        } else {
            // Invalid leading byte
            return false;
        }
    }
    return true;
}

// Fast path for common ASCII-only CSV data.
static inline bool isAllAscii(const char* data, size_t len) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;

    // Check machine-word chunks first.
    const size_t word_size = sizeof(uintptr_t);
    const uintptr_t high_mask = sizeof(uintptr_t) == 8
        ? static_cast<uintptr_t>(0x8080808080808080ULL)
        : static_cast<uintptr_t>(0x80808080UL);

    while (i + word_size <= len) {
        uintptr_t word;
        memcpy(&word, bytes + i, word_size);
        if (word & high_mask) {
            return false;
        }
        i += word_size;
    }

    while (i < len) {
        if (bytes[i] & 0x80) {
            return false;
        }
        i++;
    }
    return true;
}

// Create Napi::String with UTF-8 validation (safe version)
// Falls back to replacement character representation for invalid UTF-8
static napi_value SafeNewStringValue(napi_env env, const char* data, size_t len) {
    // Short fields are extremely common in CSV; avoid heavier ASCII/UTF-8 scans.
    if (len <= 32) {
        bool ascii = true;
        for (size_t i = 0; i < len; i++) {
            if (static_cast<unsigned char>(data[i]) & 0x80) {
                ascii = false;
                break;
            }
        }

        napi_value short_value = nullptr;
        if (ascii) {
            if (napi_create_string_latin1(env, data, len, &short_value) == napi_ok && short_value) {
                return short_value;
            }
        } else if (isValidUtf8(data, len)) {
            if (napi_create_string_utf8(env, data, len, &short_value) == napi_ok && short_value) {
                return short_value;
            }
        }
    }

    // Fastest path: ASCII-only data is valid Latin-1.
    // Using Latin-1 creation avoids UTF-8 decoding overhead.
    if (isAllAscii(data, len)) {
        napi_value latin1_value = nullptr;
        if (napi_create_string_latin1(env, data, len, &latin1_value) == napi_ok && latin1_value) {
            return latin1_value;
        }
        // Fallback to UTF-8 path if Latin-1 creation fails unexpectedly.
        napi_value utf8_value = nullptr;
        napi_create_string_utf8(env, data, len, &utf8_value);
        return utf8_value;
    }

    if (isValidUtf8(data, len)) {
        napi_value utf8_value = nullptr;
        napi_create_string_utf8(env, data, len, &utf8_value);
        return utf8_value;
    }

    // Invalid UTF-8 - replace invalid bytes with replacement character
    // This prevents V8 crashes while preserving data visibility
    std::string safe_str;
    safe_str.reserve(len);

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;

    while (i < len) {
        unsigned char c = bytes[i];

        if (c < 0x80) {
            safe_str += static_cast<char>(c);
            i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len &&
                   (bytes[i + 1] & 0xC0) == 0x80 && c >= 0xC2) {
            safe_str += static_cast<char>(c);
            safe_str += static_cast<char>(bytes[i + 1]);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len &&
                   (bytes[i + 1] & 0xC0) == 0x80 &&
                   (bytes[i + 2] & 0xC0) == 0x80) {
            safe_str += static_cast<char>(c);
            safe_str += static_cast<char>(bytes[i + 1]);
            safe_str += static_cast<char>(bytes[i + 2]);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len &&
                   (bytes[i + 1] & 0xC0) == 0x80 &&
                   (bytes[i + 2] & 0xC0) == 0x80 &&
                   (bytes[i + 3] & 0xC0) == 0x80 && c <= 0xF4) {
            safe_str += static_cast<char>(c);
            safe_str += static_cast<char>(bytes[i + 1]);
            safe_str += static_cast<char>(bytes[i + 2]);
            safe_str += static_cast<char>(bytes[i + 3]);
            i += 4;
        } else {
            // Invalid byte - use UTF-8 replacement character U+FFFD
            safe_str += "\xEF\xBF\xBD";
            i++;
        }
    }

    napi_value safe_value = nullptr;
    napi_create_string_utf8(env, safe_str.c_str(), safe_str.length(), &safe_value);
    return safe_value;
}

static Napi::String SafeNewString(Napi::Env env, const char* data, size_t len) {
    return Napi::String(env, SafeNewStringValue(env, data, len));
}

static napi_value NewLatin1StringValue(napi_env env, const char* data, size_t len) {
    napi_value value = nullptr;
    if (napi_create_string_latin1(env, data, len, &value) == napi_ok && value) {
        return value;
    }
    napi_create_string_utf8(env, data, len, &value);
    return value;
}

static napi_value NewCsvStringValue(
    napi_env env,
    const char* data,
    size_t len,
    bool ascii_only
) {
    return ascii_only ? NewLatin1StringValue(env, data, len) : SafeNewStringValue(env, data, len);
}

static bool rowsAreAscii(const std::vector<std::vector<std::string>> &rows) {
    for (const auto &row : rows) {
        for (const auto &field : row) {
            if (!isAllAscii(field.data(), field.size())) {
                return false;
            }
        }
    }
    return true;
}

static bool canUseSimpleLfFastPath(const cisv_config &config) {
    return config.escape == '\0' &&
        config.comment == '\0' &&
        !config.trim &&
        !config.skip_empty_lines &&
        !config.relaxed &&
        !config.skip_lines_with_error &&
        config.max_row_size == 0 &&
        config.from_line <= 1 &&
        config.to_line == 0;
}

static bool tryParseSimpleLfToJsRows(
    napi_env env,
    const uint8_t *data,
    size_t len,
    const cisv_config &config,
    napi_value *out
) {
    *out = nullptr;
    if (!data || !canUseSimpleLfFastPath(config)) {
        return false;
    }
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        return false;
    }

    size_t row_count = 0;
    bool saw_data_in_row = false;
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = data[i];
        if (c == static_cast<uint8_t>(config.quote) || c == '\r' || (c & 0x80)) {
            return false;
        }
        if (c == '\n') {
            row_count++;
            saw_data_in_row = false;
        } else {
            saw_data_in_row = true;
        }
    }
    if (saw_data_in_row || (len > 0 && data[len - 1] != '\n')) {
        row_count++;
    }

    napi_value rows;
    if (napi_create_array_with_length(env, row_count, &rows) != napi_ok) {
        return false;
    }

    size_t row_idx = 0;
    size_t line_start = 0;
    while (line_start < len && row_idx < row_count) {
        size_t line_end = line_start;
        while (line_end < len && data[line_end] != '\n') {
            line_end++;
        }

        size_t field_count = 1;
        for (size_t i = line_start; i < line_end; i++) {
            if (data[i] == static_cast<uint8_t>(config.delimiter)) {
                field_count++;
            }
        }

        napi_value row;
        if (napi_create_array_with_length(env, field_count, &row) != napi_ok) {
            return false;
        }

        size_t field_idx = 0;
        size_t field_start = line_start;
        for (size_t i = line_start; i <= line_end; i++) {
            if (i == line_end || data[i] == static_cast<uint8_t>(config.delimiter)) {
                napi_value field = NewLatin1StringValue(
                    env,
                    reinterpret_cast<const char*>(data + field_start),
                    i - field_start);
                if (!field || napi_set_element(env, row, field_idx, field) != napi_ok) {
                    return false;
                }
                field_idx++;
                field_start = i + 1;
            }
        }

        if (napi_set_element(env, rows, row_idx, row) != napi_ok) {
            return false;
        }
        row_idx++;

        if (line_end == len) {
            break;
        }
        line_start = line_end + 1;
    }

    *out = rows;
    return true;
}

// Extended RowCollector that handles transforms
struct RowCollector {
    std::vector<std::string> current;
    std::vector<std::vector<std::string>> rows;
    cisv_transform_pipeline_t* pipeline;
    int current_field_index;

    // JavaScript transforms stored separately
    std::unordered_map<int, Napi::FunctionReference> js_transforms;
    Napi::Env env;

    RowCollector() : pipeline(nullptr), current_field_index(0), env(nullptr) {
        // DON'T create the pipeline here - do it lazily when needed
        pipeline = nullptr;
    }

    ~RowCollector() {
        cleanup();
    }

    void cleanup() {
        if (pipeline) {
            cisv_transform_pipeline_destroy(pipeline);
            pipeline = nullptr;
        }
        // SECURITY FIX: Properly release all persistent references to prevent memory leak
        // Napi::Persistent references must be Reset() before being destroyed
        for (auto& pair : js_transforms) {
            if (!pair.second.IsEmpty()) {
                pair.second.Reset();  // Release the persistent handle
            }
        }
        js_transforms.clear();
        rows.clear();
        current.clear();
        current_field_index = 0;
        env = nullptr;
    }

    // Lazy initialization of pipeline
    void ensurePipeline() {
        if (!pipeline) {
            pipeline = cisv_transform_pipeline_create(16);
        }
    }

    // Apply both C and JS transforms
    std::string applyTransforms(const char* data, size_t len, int field_index) {
        std::string result(data, len);

        // First apply C transforms
        if (pipeline && pipeline->count > 0) {
            cisv_transform_result_t c_result = cisv_transform_apply(
                pipeline,
                field_index,
                result.c_str(),
                result.length()
            );

            if (c_result.data) {
                result = std::string(c_result.data, c_result.len);
                // IMPORTANT: Free the result if it was allocated
                if (c_result.needs_free) {
                    cisv_transform_result_free(&c_result);
                }
            }
        }

        // Then apply JavaScript transforms if we have an environment
        if (env) {
            // Apply field-specific transform
            auto it = js_transforms.find(field_index);
            if (it != js_transforms.end() && !it->second.IsEmpty()) {
                try {
                    // SECURITY: Use safe string creation to handle invalid UTF-8
                    Napi::String input = SafeNewString(env, result.c_str(), result.length());
                    Napi::Number field = Napi::Number::New(env, field_index);

                    Napi::Value js_result = it->second.Call({input, field});

                    if (js_result.IsString()) {
                        result = js_result.As<Napi::String>().Utf8Value();
                    }
                } catch (const Napi::Error& e) {
                    // Keep original result but log the error
                    fprintf(stderr, "CISV: JS transform error for field %d: %s\n",
                            field_index, e.Message().c_str());
                } catch (const std::exception& e) {
                    fprintf(stderr, "CISV: C++ exception in JS transform: %s\n", e.what());
                } catch (...) {
                    fprintf(stderr, "CISV: Unknown exception in JS transform for field %d\n",
                            field_index);
                }
            }

            // Apply transforms that apply to all fields (-1 index)
            auto it_all = js_transforms.find(-1);
            if (it_all != js_transforms.end() && !it_all->second.IsEmpty()) {
                try {
                    // SECURITY: Use safe string creation to handle invalid UTF-8
                    Napi::String input = SafeNewString(env, result.c_str(), result.length());
                    Napi::Number field = Napi::Number::New(env, field_index);

                    Napi::Value js_result = it_all->second.Call({input, field});

                    if (js_result.IsString()) {
                        result = js_result.As<Napi::String>().Utf8Value();
                    }
                } catch (const Napi::Error& e) {
                    // Keep original result but log the error
                    fprintf(stderr, "CISV: JS transform error (all fields): %s\n", e.Message().c_str());
                } catch (const std::exception& e) {
                    fprintf(stderr, "CISV: C++ exception in JS transform: %s\n", e.what());
                } catch (...) {
                    fprintf(stderr, "CISV: Unknown exception in JS transform (all fields)\n");
                }
            }
        }

        return result;
    }
};

static void field_cb(void *user, const char *data, size_t len) {
    auto *rc = reinterpret_cast<RowCollector *>(user);

    // Fast path: no transforms - avoid unnecessary string copies
    bool has_c_transforms = rc->pipeline && rc->pipeline->count > 0;
    bool has_js_transforms = !rc->js_transforms.empty();

    if (!has_c_transforms && !has_js_transforms) {
        rc->current.emplace_back(data, len);
        rc->current_field_index++;
        return;
    }

    // Slow path: apply transforms
    std::string transformed = rc->applyTransforms(data, len, rc->current_field_index);
    rc->current.emplace_back(std::move(transformed));
    rc->current_field_index++;
}

static void row_cb(void *user) {
    auto *rc = reinterpret_cast<RowCollector *>(user);
    rc->rows.emplace_back(std::move(rc->current));
    rc->current.clear();
    rc->current_field_index = 0;  // Reset field index for next row
}

static void error_cb(void *user, int line, const char *msg) {
    // Log errors to stderr for now
    fprintf(stderr, "CSV Parse Error at line %d: %s\n", line, msg);
}

static bool validateNumThreads(int num_threads, std::string &error) {
    if (num_threads < 0) {
        error = "numThreads must be >= 0";
        return false;
    }
    return true;
}

static bool collectParallelRows(
    cisv_result_t **results,
    int result_count,
    std::vector<std::vector<std::string>> &rows,
    std::string &error
) {
    size_t total_rows = 0;
    for (int chunk = 0; chunk < result_count; chunk++) {
        cisv_result_t *result = results[chunk];
        if (!result) {
            continue;
        }
        if (result->error_code != 0) {
            error = result->error_message[0] ? result->error_message : "parse error";
            return false;
        }
        total_rows += result->row_count;
    }

    rows.clear();
    rows.reserve(total_rows);

    for (int chunk = 0; chunk < result_count; chunk++) {
        cisv_result_t *result = results[chunk];
        if (!result) {
            continue;
        }

        for (size_t i = 0; i < result->row_count; i++) {
            cisv_row_t *row = &result->rows[i];
            std::vector<std::string> out_row;
            out_row.reserve(row->field_count);
            for (size_t j = 0; j < row->field_count; j++) {
                out_row.emplace_back(row->fields[j], row->field_lengths[j]);
            }
            rows.emplace_back(std::move(out_row));
        }
    }

    return true;
}

static Napi::Array rowsToJsArray(Napi::Env env, const std::vector<std::vector<std::string>> &rows) {
    Napi::Array out = Napi::Array::New(env, rows.size());
    const bool ascii_only = rowsAreAscii(rows);
    for (size_t i = 0; i < rows.size(); i++) {
        Napi::Array row = Napi::Array::New(env, rows[i].size());
        for (size_t j = 0; j < rows[i].size(); j++) {
            const std::string &field = rows[i][j];
            napi_set_element(
                env,
                row,
                j,
                NewCsvStringValue(env, field.c_str(), field.length(), ascii_only));
        }
        out[i] = row;
    }
    return out;
}

class ParseFileWorker final : public Napi::AsyncWorker {
public:
    ParseFileWorker(
        Napi::Env env,
        std::string path,
        cisv_config config,
        Napi::Promise::Deferred deferred
    ) : Napi::AsyncWorker(env),
        path_(std::move(path)),
        config_(config),
        deferred_(deferred) {}

    void Execute() override {
        cisv_result_t *result = cisv_parse_file_batch(path_.c_str(), &config_);
        if (!result) {
            SetError("parse error: " + std::string(strerror(errno)));
            return;
        }

        if (result->error_code != 0) {
            std::string msg = result->error_message[0] ? result->error_message : "parse error";
            if (msg.rfind("parse error", 0) != 0) {
                msg = "parse error: " + msg;
            }
            SetError(msg);
            cisv_result_free(result);
            return;
        }

        rows_.reserve(result->row_count);
        for (size_t i = 0; i < result->row_count; i++) {
            cisv_row_t *row = &result->rows[i];
            std::vector<std::string> out_row;
            out_row.reserve(row->field_count);
            for (size_t j = 0; j < row->field_count; j++) {
                out_row.emplace_back(row->fields[j], row->field_lengths[j]);
            }
            rows_.emplace_back(std::move(out_row));
        }

        cisv_result_free(result);
    }

    void OnOK() override {
        deferred_.Resolve(rowsToJsArray(Env(), rows_));
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    std::string path_;
    cisv_config config_;
    Napi::Promise::Deferred deferred_;
    std::vector<std::vector<std::string>> rows_;
};

class ParseFileParallelWorker final : public Napi::AsyncWorker {
public:
    ParseFileParallelWorker(
        Napi::Env env,
        std::string path,
        cisv_config config,
        int num_threads,
        Napi::Promise::Deferred deferred
    ) : Napi::AsyncWorker(env),
        path_(std::move(path)),
        config_(config),
        num_threads_(num_threads),
        deferred_(deferred) {}

    void Execute() override {
        if (!validateNumThreads(num_threads_, error_)) {
            SetError(error_);
            return;
        }

        int result_count = 0;
        cisv_result_t **results = cisv_parse_file_parallel(path_.c_str(), &config_, num_threads_, &result_count);
        if (!results) {
            SetError("parse error: " + std::string(strerror(errno)));
            return;
        }

        bool ok = collectParallelRows(results, result_count, rows_, error_);
        cisv_results_free(results, result_count);

        if (!ok) {
            SetError(error_);
        }
    }

    void OnOK() override {
        deferred_.Resolve(rowsToJsArray(Env(), rows_));
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    std::string path_;
    cisv_config config_;
    int num_threads_;
    Napi::Promise::Deferred deferred_;
    std::vector<std::vector<std::string>> rows_;
    std::string error_;
};

} // namespace

class CisvParser : public Napi::ObjectWrap<CisvParser> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "cisvParser", {
            InstanceMethod("parseSync", &CisvParser::ParseSync),
            InstanceMethod("parseSyncParallel", &CisvParser::ParseSyncParallel),
            InstanceMethod("parse", &CisvParser::ParseAsync),
            InstanceMethod("parseParallel", &CisvParser::ParseParallel),
            InstanceMethod("parseString", &CisvParser::ParseString),
            InstanceMethod("write", &CisvParser::Write),
            InstanceMethod("end", &CisvParser::End),
            InstanceMethod("getRows", &CisvParser::GetRows),
            InstanceMethod("clear", &CisvParser::Clear),
            InstanceMethod("transform", &CisvParser::Transform),
            InstanceMethod("removeTransform", &CisvParser::RemoveTransform),
            InstanceMethod("clearTransforms", &CisvParser::ClearTransforms),
            InstanceMethod("getStats", &CisvParser::GetStats),
            InstanceMethod("getTransformInfo", &CisvParser::GetTransformInfo),
            InstanceMethod("destroy", &CisvParser::Destroy),
            InstanceMethod("setConfig", &CisvParser::SetConfig),
            InstanceMethod("getConfig", &CisvParser::GetConfig),
            InstanceMethod("transformByName", &CisvParser::TransformByName),
            InstanceMethod("setHeaderFields", &CisvParser::SetHeaderFields),
            InstanceMethod("removeTransformByName", &CisvParser::RemoveTransformByName),

            // Iterator API methods
            InstanceMethod("openIterator", &CisvParser::OpenIterator),
            InstanceMethod("fetchRow", &CisvParser::FetchRow),
            InstanceMethod("closeIterator", &CisvParser::CloseIterator),

            StaticMethod("countRows", &CisvParser::CountRows),
            StaticMethod("countRowsWithConfig", &CisvParser::CountRowsWithConfig)
        });

        exports.Set("cisvParser", func);
        return exports;
    }

    CisvParser(const Napi::CallbackInfo &info) : Napi::ObjectWrap<CisvParser>(info) {
        rc_ = nullptr;
        parser_ = nullptr;
        parse_time_ = 0;
        total_bytes_ = 0;
        is_destroyed_ = false;
        iterator_ = nullptr;
        batch_result_ = nullptr;
        stream_buffering_active_ = true;
        pending_buffer_data_ = nullptr;
        pending_buffer_size_ = 0;

        // Initialize configuration with defaults
        cisv_config_init(&config_);

        config_.max_row_size = 0;

        // Handle constructor options if provided
        if (info.Length() > 0 && info[0].IsObject()) {
            Napi::Object options = info[0].As<Napi::Object>();
            ApplyConfigFromObject(options);
        }

        rc_ = new RowCollector();

        // Set callbacks
        config_.field_cb = field_cb;
        config_.row_cb = row_cb;
        config_.error_cb = error_cb;
        config_.user = rc_;
    }

    ~CisvParser() {
        Cleanup();
    }

    // Apply configuration from JavaScript object
    void ApplyConfigFromObject(Napi::Object options) {
        Napi::Env env = options.Env();
        cisv_config next = config_;
        ApplyConfigOptions(env, options, &next);
        config_ = next;
    }

    // Set configuration after creation
    void SetConfig(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() != 1 || !info[0].IsObject()) {
            throw Napi::TypeError::New(env, "Expected configuration object");
        }

        Napi::Object options = info[0].As<Napi::Object>();
        ApplyConfigFromObject(options);

        // Recreate the streaming parser only if it has already been instantiated.
        if (parser_) {
            cisv_parser_destroy(parser_);
            parser_ = nullptr;
            ensureParser(env);
        }
    }

    // Get current configuration
    Napi::Value GetConfig(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        Napi::Object config = Napi::Object::New(env);

        // Character configurations
        config.Set("delimiter", Napi::String::New(env, std::string(1, config_.delimiter)));
        config.Set("quote", Napi::String::New(env, std::string(1, config_.quote)));

        if (config_.escape) {
            config.Set("escape", Napi::String::New(env, std::string(1, config_.escape)));
        } else {
            config.Set("escape", env.Null());
        }

        if (config_.comment) {
            config.Set("comment", Napi::String::New(env, std::string(1, config_.comment)));
        } else {
            config.Set("comment", env.Null());
        }

        // Boolean options
        config.Set("skipEmptyLines", Napi::Boolean::New(env, config_.skip_empty_lines));
        config.Set("trim", Napi::Boolean::New(env, config_.trim));
        config.Set("relaxed", Napi::Boolean::New(env, config_.relaxed));
        config.Set("skipLinesWithError", Napi::Boolean::New(env, config_.skip_lines_with_error));

        // Numeric options
        config.Set("maxRowSize", Napi::Number::New(env, config_.max_row_size));
        config.Set("fromLine", Napi::Number::New(env, config_.from_line));
        config.Set("toLine", Napi::Number::New(env, config_.to_line));

        return config;
    }

    // Explicit cleanup method
    void Cleanup() {
        if (!is_destroyed_) {
            // Close iterator if open
            if (iterator_) {
                cisv_iterator_close(iterator_);
                iterator_ = nullptr;
            }
            if (parser_) {
                cisv_parser_destroy(parser_);
                parser_ = nullptr;
            }
            if (rc_) {
                rc_->cleanup();
                delete rc_;
                rc_ = nullptr;
            }
            clearBatchResult();
            clearFastRows();
            pending_stream_.clear();
            clearPendingBuffer();
            is_destroyed_ = true;
        }
    }

    // Explicit destroy method callable from JavaScript
    void Destroy(const Napi::CallbackInfo &info) {
        Cleanup();
    }

    // Synchronous file parsing
    Napi::Value ParseSync(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() != 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        std::string path = info[0].As<Napi::String>();

        auto start = std::chrono::high_resolution_clock::now();

        resetRowState();

        int result = 0;
        if (!hasTransforms()) {
            cisv_result_t *batch = cisv_parse_file_batch(path.c_str(), &config_);
            if (!batch) {
                throw Napi::Error::New(env, "parse error: " + std::string(strerror(errno)));
            }
            if (batch->error_code != 0) {
                std::string msg = batch->error_message[0] ? batch->error_message : "parse error";
                cisv_result_free(batch);
                throw Napi::Error::New(env, msg);
            }
            clearBatchResult();
            batch_result_ = batch;
        } else {
            // Set environment for JS transforms
            rc_->env = env;
            ensureParser(env);
            result = cisv_parser_parse_file(parser_, path.c_str());
            // Clear the environment reference after parsing
            rc_->env = nullptr;
            if (result < 0) {
                throw Napi::Error::New(env, "parse error: " + std::to_string(result));
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        parse_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        return drainRows(env);
    }

    // Parse string content
    Napi::Value ParseString(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() != 1 || (!info[0].IsString() && !info[0].IsBuffer())) {
            throw Napi::TypeError::New(env, "Expected CSV string or Buffer");
        }

        const char *content_data = nullptr;
        size_t content_len = 0;
        std::string content_storage;

        if (info[0].IsBuffer()) {
            auto buffer = info[0].As<Napi::Buffer<char>>();
            content_data = buffer.Data();
            content_len = buffer.Length();
        } else {
            content_storage = info[0].As<Napi::String>();
            content_data = content_storage.data();
            content_len = content_storage.size();
        }

        resetRowState();

        if (!hasTransforms()) {
            cisv_result_t *batch = cisv_parse_string_batch(content_data, content_len, &config_);
            if (!batch) {
                throw Napi::Error::New(env, "parse error: " + std::string(strerror(errno)));
            }
            if (batch->error_code != 0) {
                std::string msg = batch->error_message[0] ? batch->error_message : "parse error";
                cisv_result_free(batch);
                throw Napi::Error::New(env, msg);
            }
            clearBatchResult();
            batch_result_ = batch;
        } else {
            // Set environment for JS transforms
            rc_->env = env;
            ensureParser(env);

            // Write the string content as chunks
            cisv_parser_write(parser_, reinterpret_cast<const uint8_t*>(content_data), content_len);
            cisv_parser_end(parser_);

            // Clear the environment reference after parsing
            rc_->env = nullptr;
        }

        total_bytes_ = content_len;

        return drainRows(env);
    }

    Napi::Value ParseSyncParallel(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() < 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        int num_threads = 0;
        if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull()) {
            if (!info[1].IsNumber()) {
                throw Napi::TypeError::New(env, "numThreads must be a number");
            }
            num_threads = info[1].As<Napi::Number>().Int32Value();
        }

        std::string validation_error;
        if (!validateNumThreads(num_threads, validation_error)) {
            throw Napi::TypeError::New(env, validation_error);
        }

        std::string path = info[0].As<Napi::String>().Utf8Value();
        int result_count = 0;
        cisv_result_t **results = cisv_parse_file_parallel(
            path.c_str(),
            &config_,
            num_threads,
            &result_count);
        if (!results) {
            throw Napi::Error::New(env, "parse error: " + std::string(strerror(errno)));
        }

        std::vector<std::vector<std::string>> rows;
        std::string error;
        bool ok = collectParallelRows(results, result_count, rows, error);
        cisv_results_free(results, result_count);

        if (!ok) {
            throw Napi::Error::New(env, error);
        }

        return rowsToJsArray(env, rows);
    }

    // Write chunk for streaming
    void Write(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() != 1) {
            throw Napi::TypeError::New(env, "Expected one argument");
        }

        // Streaming writes produce row-callback data, not batch results.
        clearBatchResult();
        clearFastRows();

        // Set environment for JS transforms
        rc_->env = env;

        const uint8_t* chunk_data = nullptr;
        size_t chunk_size = 0;
        std::string chunk_storage;

        if (info[0].IsBuffer()) {
            auto buf = info[0].As<Napi::Buffer<uint8_t>>();
            chunk_data = buf.Data();
            chunk_size = buf.Length();
        } else if (info[0].IsString()) {
            chunk_storage = info[0].As<Napi::String>();
            chunk_data = reinterpret_cast<const uint8_t*>(chunk_storage.data());
            chunk_size = chunk_storage.size();
        } else {
            throw Napi::TypeError::New(env, "Expected Buffer or String");
        }

        // Check for overflow before adding to total_bytes_
        if (chunk_size > SIZE_MAX - total_bytes_) {
            throw Napi::Error::New(env, "Total bytes would overflow");
        }

        // Fast streaming mode:
        // Buffer chunks when no transforms/iterator are active and batch-parse on end().
        // If buffered payload exceeds threshold, flush once to parser and continue streaming.
        if (!hasTransforms() && iterator_ == nullptr) {
            if (stream_buffering_active_) {
                if (chunk_size > 0 && pending_stream_.empty() && pending_buffer_size_ == 0 && info[0].IsBuffer()) {
                    auto buf = info[0].As<Napi::Buffer<uint8_t>>();
                    pending_buffer_ref_ = Napi::Persistent(buf.As<Napi::Object>());
                    pending_buffer_data_ = buf.Data();
                    pending_buffer_size_ = buf.Length();
                    total_bytes_ += chunk_size;
                    return;
                }

                if (pending_buffer_size_ > 0) {
                    if (pending_buffer_size_ > SIZE_MAX - pending_stream_.size()) {
                        throw Napi::Error::New(env, "Buffered stream size would overflow");
                    }
                    pending_stream_.append(
                        reinterpret_cast<const char*>(pending_buffer_data_),
                        pending_buffer_size_);
                    clearPendingBuffer();
                }

                if (chunk_size > SIZE_MAX - pending_stream_.size()) {
                    throw Napi::Error::New(env, "Buffered stream size would overflow");
                }
                pending_stream_.append(reinterpret_cast<const char*>(chunk_data), chunk_size);
                total_bytes_ += chunk_size;

                if (pending_stream_.size() > kStreamBufferLimitBytes) {
                    flushPendingStreamToParser();
                    stream_buffering_active_ = false;
                }
                return;
            }
        } else if (!pending_stream_.empty() || pending_buffer_size_ > 0) {
            flushPendingStreamToParser();
            stream_buffering_active_ = false;
        }

        ensureParser(env);
        cisv_parser_write(parser_, chunk_data, chunk_size);
        total_bytes_ += chunk_size;
    }

    void End(const Napi::CallbackInfo &info) {
        if (is_destroyed_) {
            return;
        }

        if (stream_buffering_active_ && !pending_stream_.empty() &&
            !hasTransforms() && iterator_ == nullptr &&
            rc_ && rc_->rows.empty() && rc_->current.empty()) {
            napi_value fast_rows = nullptr;
            if (tryParseSimpleLfToJsRows(
                    info.Env(),
                    reinterpret_cast<const uint8_t*>(pending_stream_.data()),
                    pending_stream_.size(),
                    config_,
                    &fast_rows)) {
                clearBatchResult();
                clearFastRows();
                fast_rows_ref_ = Napi::Persistent(Napi::Value(info.Env(), fast_rows).As<Napi::Object>());
                pending_stream_.clear();
                rc_->env = nullptr;
                return;
            }

            cisv_result_t *batch = cisv_parse_string_batch(
                pending_stream_.data(), pending_stream_.size(), &config_);
            if (!batch) {
                throw Napi::Error::New(info.Env(), "parse error: " + std::string(strerror(errno)));
            }
            if (batch->error_code != 0) {
                std::string msg = batch->error_message[0] ? batch->error_message : "parse error";
                cisv_result_free(batch);
                throw Napi::Error::New(info.Env(), msg);
            }
            clearBatchResult();
            clearFastRows();
            batch_result_ = batch;
            pending_stream_.clear();
            rc_->env = nullptr;
            return;
        }

        if (stream_buffering_active_ && pending_buffer_size_ > 0 &&
            pending_stream_.empty() &&
            !hasTransforms() && iterator_ == nullptr &&
            rc_ && rc_->rows.empty() && rc_->current.empty()) {
            napi_value fast_rows = nullptr;
            if (tryParseSimpleLfToJsRows(
                    info.Env(),
                    pending_buffer_data_,
                    pending_buffer_size_,
                    config_,
                    &fast_rows)) {
                clearBatchResult();
                clearFastRows();
                fast_rows_ref_ = Napi::Persistent(Napi::Value(info.Env(), fast_rows).As<Napi::Object>());
                clearPendingBuffer();
                rc_->env = nullptr;
                return;
            }

            cisv_result_t *batch = cisv_parse_string_batch(
                reinterpret_cast<const char*>(pending_buffer_data_),
                pending_buffer_size_,
                &config_);
            if (!batch) {
                clearPendingBuffer();
                throw Napi::Error::New(info.Env(), "parse error: " + std::string(strerror(errno)));
            }
            if (batch->error_code != 0) {
                std::string msg = batch->error_message[0] ? batch->error_message : "parse error";
                cisv_result_free(batch);
                clearPendingBuffer();
                throw Napi::Error::New(info.Env(), msg);
            }
            clearBatchResult();
            clearFastRows();
            batch_result_ = batch;
            clearPendingBuffer();
            rc_->env = nullptr;
            return;
        }

        if (!pending_stream_.empty() || pending_buffer_size_ > 0) {
            flushPendingStreamToParser();
            stream_buffering_active_ = false;
        }

        ensureParser(info.Env());
        cisv_parser_end(parser_);
        // Clear the environment reference after ending to prevent stale references
        rc_->env = nullptr;
        // Note: JS transforms stored in rc_->js_transforms remain valid
        // as they are Persistent references managed by the addon lifecycle
    }

    Napi::Value GetRows(const Napi::CallbackInfo &info) {
        if (is_destroyed_) {
            Napi::Env env = info.Env();
            throw Napi::Error::New(env, "Parser has been destroyed");
        }
        if (!pending_stream_.empty()) {
            flushPendingStreamToParser();
            stream_buffering_active_ = false;
        }
        if (pending_buffer_size_ > 0) {
            flushPendingStreamToParser();
            stream_buffering_active_ = false;
        }
        return drainRows(info.Env());
    }

    void Clear(const Napi::CallbackInfo &info) {
        if (!is_destroyed_ && rc_) {
            clearBatchResult();
            clearFastRows();
            rc_->rows.clear();
            rc_->current.clear();
            rc_->current_field_index = 0;
            total_bytes_ = 0;
            parse_time_ = 0;
            pending_stream_.clear();
            clearPendingBuffer();
            stream_buffering_active_ = true;
            // Also clear the environment reference
            rc_->env = nullptr;
        }
    }

    // Add transform using native C transformer or JavaScript function
    Napi::Value Transform(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() < 2) {
            throw Napi::TypeError::New(env, "Expected field index and transform type/function");
        }

        if (!info[0].IsNumber()) {
            throw Napi::TypeError::New(env, "Field index must be a number");
        }

        int field_index = info[0].As<Napi::Number>().Int32Value();

        // Ensure pipeline exists (lazy initialization)
        rc_->ensurePipeline();

        // Store the environment
        rc_->env = env;

        // Handle string transform types - using the actual C transformer
        if (info[1].IsString()) {
            std::string transform_type = info[1].As<Napi::String>();
            cisv_transform_type_t type;

            // Map string to C enum
            if (transform_type == "uppercase") {
                type = TRANSFORM_UPPERCASE;
            } else if (transform_type == "lowercase") {
                type = TRANSFORM_LOWERCASE;
            } else if (transform_type == "trim") {
                type = TRANSFORM_TRIM;
            } else if (transform_type == "to_int" || transform_type == "int") {
                type = TRANSFORM_TO_INT;
            } else if (transform_type == "to_float" || transform_type == "float") {
                type = TRANSFORM_TO_FLOAT;
            } else if (transform_type == "base64_encode" || transform_type == "base64") {
                type = TRANSFORM_BASE64_ENCODE;
            } else {
                throw Napi::Error::New(env, "Unknown transform type: " + transform_type);
            }

            // Create context if provided
            cisv_transform_context_t* ctx = nullptr;
            if (info.Length() >= 3 && info[2].IsObject()) {
                Napi::Object context_obj = info[2].As<Napi::Object>();
                ctx = (cisv_transform_context_t*)calloc(1, sizeof(cisv_transform_context_t));
                if (!ctx) {
                    throw Napi::Error::New(env, "Memory allocation failed for transform context");
                }

                // Extract context properties if they exist
                if (context_obj.Has("key")) {
                    Napi::Value key_val = context_obj.Get("key");
                    if (key_val.IsString()) {
                        std::string key = key_val.As<Napi::String>();
                        ctx->key = strdup(key.c_str());
                        if (!ctx->key) {
                            free(ctx);
                            throw Napi::Error::New(env, "Memory allocation failed for key");
                        }
                        ctx->key_len = key.length();
                    }
                }

                if (context_obj.Has("iv")) {
                    Napi::Value iv_val = context_obj.Get("iv");
                    if (iv_val.IsString()) {
                        std::string iv = iv_val.As<Napi::String>();
                        ctx->iv = strdup(iv.c_str());
                        if (!ctx->iv) {
                            if (ctx->key) free((void*)ctx->key);
                            free(ctx);
                            throw Napi::Error::New(env, "Memory allocation failed for iv");
                        }
                        ctx->iv_len = iv.length();
                    }
                }
            }

            // Add to the C transform pipeline
            if (cisv_transform_pipeline_add(rc_->pipeline, field_index, type, ctx) < 0) {
                // Clean up context if adding failed
                if (ctx) {
                    if (ctx->key) free((void*)ctx->key);
                    if (ctx->iv) free((void*)ctx->iv);
                    if (ctx->extra) free(ctx->extra);
                    free(ctx);
                }
                throw Napi::Error::New(env, "Failed to add transform");
            }

        } else if (info[1].IsFunction()) {
            // Handle JavaScript function transforms
            Napi::Function func = info[1].As<Napi::Function>();

            // Store the function reference for this field
            rc_->js_transforms[field_index] = Napi::Persistent(func);

        } else {
            throw Napi::TypeError::New(env, "Transform must be a string type or function");
        }

        return info.This();  // Return this for chaining
    }

Napi::Value TransformByName(const Napi::CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (is_destroyed_) {
        throw Napi::Error::New(env, "Parser has been destroyed");
    }

    if (info.Length() < 2) {
        throw Napi::TypeError::New(env, "Expected field name and transform type/function");
    }

    if (!info[0].IsString()) {
        throw Napi::TypeError::New(env, "Field name must be a string");
    }

    std::string field_name = info[0].As<Napi::String>();

    // Ensure pipeline exists (lazy initialization)
    rc_->ensurePipeline();

    // Store the environment
    rc_->env = env;

    // Handle string transform types - using the actual C transformer
    if (info[1].IsString()) {
        std::string transform_type = info[1].As<Napi::String>();
        cisv_transform_type_t type;

        // Map string to C enum
        if (transform_type == "uppercase") {
            type = TRANSFORM_UPPERCASE;
        } else if (transform_type == "lowercase") {
            type = TRANSFORM_LOWERCASE;
        } else if (transform_type == "trim") {
            type = TRANSFORM_TRIM;
        } else if (transform_type == "to_int" || transform_type == "int") {
            type = TRANSFORM_TO_INT;
        } else if (transform_type == "to_float" || transform_type == "float") {
            type = TRANSFORM_TO_FLOAT;
        } else if (transform_type == "base64_encode" || transform_type == "base64") {
            type = TRANSFORM_BASE64_ENCODE;
        } else {
            throw Napi::Error::New(env, "Unknown transform type: " + transform_type);
        }

        // Create context if provided
        cisv_transform_context_t* ctx = nullptr;
        if (info.Length() >= 3 && info[2].IsObject()) {
            Napi::Object context_obj = info[2].As<Napi::Object>();
            ctx = (cisv_transform_context_t*)calloc(1, sizeof(cisv_transform_context_t));
            if (!ctx) {
                throw Napi::Error::New(env, "Memory allocation failed for transform context");
            }

            // Extract context properties if they exist
            if (context_obj.Has("key")) {
                Napi::Value key_val = context_obj.Get("key");
                if (key_val.IsString()) {
                    std::string key = key_val.As<Napi::String>();
                    ctx->key = strdup(key.c_str());
                    if (!ctx->key) {
                        free(ctx);
                        throw Napi::Error::New(env, "Memory allocation failed for key");
                    }
                    ctx->key_len = key.length();
                }
            }

            if (context_obj.Has("iv")) {
                Napi::Value iv_val = context_obj.Get("iv");
                if (iv_val.IsString()) {
                    std::string iv = iv_val.As<Napi::String>();
                    ctx->iv = strdup(iv.c_str());
                    if (!ctx->iv) {
                        if (ctx->key) free((void*)ctx->key);
                        free(ctx);
                        throw Napi::Error::New(env, "Memory allocation failed for iv");
                    }
                    ctx->iv_len = iv.length();
                }
            }
        }

        // Add to the C transform pipeline by name
        if (cisv_transform_pipeline_add_by_name(rc_->pipeline, field_name.c_str(), type, ctx) < 0) {
            // Clean up context if adding failed
            if (ctx) {
                if (ctx->key) free((void*)ctx->key);
                if (ctx->iv) free((void*)ctx->iv);
                if (ctx->extra) free(ctx->extra);
                free(ctx);
            }
            throw Napi::Error::New(env, "Failed to add transform for field: " + field_name);
        }

    } else if (info[1].IsFunction()) {
        // Handle JavaScript function transforms by name
        Napi::Function func = info[1].As<Napi::Function>();

        if (!rc_->pipeline || !rc_->pipeline->header_fields) {
            throw Napi::Error::New(env,
                "Header fields are not set. Call setHeaderFields([...]) before transformByName(..., fn).");
        }

        int field_index = -1;
        for (size_t i = 0; i < rc_->pipeline->header_count; i++) {
            if (strcmp(rc_->pipeline->header_fields[i], field_name.c_str()) == 0) {
                field_index = static_cast<int>(i);
                break;
            }
        }

        if (field_index < 0) {
            throw Napi::Error::New(env, "Unknown field name: " + field_name);
        }

        // Store callback in the same map used by applyTransforms().
        rc_->js_transforms[field_index] = Napi::Persistent(func);

    } else {
        throw Napi::TypeError::New(env, "Transform must be a string type or function");
    }

    return info.This();  // Return this for chaining
}

void SetHeaderFields(const Napi::CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (is_destroyed_) {
        throw Napi::Error::New(env, "Parser has been destroyed");
    }

    if (info.Length() != 1 || !info[0].IsArray()) {
        throw Napi::TypeError::New(env, "Expected array of field names");
    }

    Napi::Array field_names = info[0].As<Napi::Array>();
    size_t field_count = field_names.Length();

    // Convert to C array of strings
    const char** c_field_names = (const char**)malloc(field_count * sizeof(char*));
    if (!c_field_names) {
        throw Napi::Error::New(env, "Memory allocation failed");
    }

    // Initialize to NULL for safe cleanup on partial failure
    for (size_t i = 0; i < field_count; i++) {
        c_field_names[i] = nullptr;
    }

    for (size_t i = 0; i < field_count; i++) {
        Napi::Value field_val = field_names[i];
        if (!field_val.IsString()) {
            // Clean up all previously allocated strings
            for (size_t j = 0; j < i; j++) {
                if (c_field_names[j]) free((void*)c_field_names[j]);
            }
            free(c_field_names);
            throw Napi::TypeError::New(env, "Field names must be strings");
        }
        std::string field_str = field_val.As<Napi::String>();
        c_field_names[i] = strdup(field_str.c_str());
        if (!c_field_names[i]) {
            // Clean up all previously allocated strings
            for (size_t j = 0; j < i; j++) {
                if (c_field_names[j]) free((void*)c_field_names[j]);
            }
            free(c_field_names);
            throw Napi::Error::New(env, "Memory allocation failed for field name");
        }
    }

    // Ensure pipeline exists
    rc_->ensurePipeline();

    // Set header fields in the pipeline
    if (cisv_transform_pipeline_set_header(rc_->pipeline, c_field_names, field_count) < 0) {
        // Clean up
        for (size_t i = 0; i < field_count; i++) {
            free((void*)c_field_names[i]);
        }
        free(c_field_names);
        throw Napi::Error::New(env, "Failed to set header fields");
    }

    // Clean up temporary array (the pipeline makes copies)
    for (size_t i = 0; i < field_count; i++) {
        free((void*)c_field_names[i]);
    }
    free(c_field_names);
}

// Add this method to remove transforms by field name
Napi::Value RemoveTransformByName(const Napi::CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (is_destroyed_) {
        throw Napi::Error::New(env, "Parser has been destroyed");
    }

    if (info.Length() != 1 || !info[0].IsString()) {
        throw Napi::TypeError::New(env, "Expected field name");
    }

    std::string field_name = info[0].As<Napi::String>();

    int field_index = -1;

    // Remove from JavaScript transforms by finding the field index
    if (rc_->pipeline && rc_->pipeline->header_fields) {
        for (size_t i = 0; i < rc_->pipeline->header_count; i++) {
            if (strcmp(rc_->pipeline->header_fields[i], field_name.c_str()) == 0) {
                field_index = static_cast<int>(i);
                auto it = rc_->js_transforms.find(field_index);
                if (it != rc_->js_transforms.end()) {
                    if (!it->second.IsEmpty()) {
                        it->second.Reset();
                    }
                    rc_->js_transforms.erase(it);
                }
                break;
            }
        }
    }

    if (field_index >= 0 && rc_->pipeline) {
        cisv_transform_pipeline_remove_field(rc_->pipeline, field_index);
    }

    return info.This();
}

    Napi::Value RemoveTransform(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() != 1 || !info[0].IsNumber()) {
            throw Napi::TypeError::New(env, "Expected field index");
        }

        int field_index = info[0].As<Napi::Number>().Int32Value();

        // Remove from JavaScript transforms
        auto it = rc_->js_transforms.find(field_index);
        if (it != rc_->js_transforms.end()) {
            if (!it->second.IsEmpty()) {
                it->second.Reset();
            }
            rc_->js_transforms.erase(it);
        }

        if (rc_->pipeline) {
            cisv_transform_pipeline_remove_field(rc_->pipeline, field_index);
        }

        return info.This();
    }

    Napi::Value ClearTransforms(const Napi::CallbackInfo &info) {
        if (is_destroyed_) {
            Napi::Env env = info.Env();
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        // Clear JavaScript transforms
        for (auto &pair : rc_->js_transforms) {
            if (!pair.second.IsEmpty()) {
                pair.second.Reset();
            }
        }
        rc_->js_transforms.clear();

        // Clear C transforms - destroy and DON'T recreate pipeline yet
        if (rc_->pipeline) {
            cisv_transform_pipeline_destroy(rc_->pipeline);
            rc_->pipeline = nullptr;  // Will be recreated lazily when needed
        }

        return info.This();
    }

    // Async file parsing (returns a Promise)
    Napi::Value ParseAsync(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() != 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        std::string path = info[0].As<Napi::String>();

        auto deferred = Napi::Promise::Deferred::New(env);

        // Preserve behavior for transform-enabled parsers (native + JS transforms)
        // until async transform execution is implemented.
        bool has_c_transforms = rc_ && rc_->pipeline && rc_->pipeline->count > 0;
        bool has_js_transforms = rc_ && !rc_->js_transforms.empty();
        if (has_c_transforms || has_js_transforms) {
            try {
                Napi::Value result = ParseSync(info);
                deferred.Resolve(result);
            } catch (const Napi::Error &e) {
                deferred.Reject(e.Value());
            }
            return deferred.Promise();
        }

        // Use batch parser in a worker thread to avoid blocking the event loop.
        cisv_config worker_config = config_;
        worker_config.field_cb = nullptr;
        worker_config.row_cb = nullptr;
        worker_config.error_cb = nullptr;
        worker_config.user = nullptr;

        auto *worker = new ParseFileWorker(env, path, worker_config, deferred);
        worker->Queue();

        return deferred.Promise();
    }

    Napi::Value ParseParallel(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() < 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        int num_threads = 0;
        if (info.Length() > 1 && !info[1].IsUndefined() && !info[1].IsNull()) {
            if (!info[1].IsNumber()) {
                throw Napi::TypeError::New(env, "numThreads must be a number");
            }
            num_threads = info[1].As<Napi::Number>().Int32Value();
        }

        auto deferred = Napi::Promise::Deferred::New(env);
        cisv_config worker_config = config_;
        worker_config.field_cb = nullptr;
        worker_config.row_cb = nullptr;
        worker_config.error_cb = nullptr;
        worker_config.user = nullptr;

        auto *worker = new ParseFileParallelWorker(
            env,
            info[0].As<Napi::String>().Utf8Value(),
            worker_config,
            num_threads,
            deferred);
        worker->Queue();

        return deferred.Promise();
    }

    // Get information about registered transforms
    Napi::Value GetTransformInfo(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        Napi::Object result = Napi::Object::New(env);

        // Count C transforms
        size_t c_transform_count = (rc_ && rc_->pipeline) ? rc_->pipeline->count : 0;
        result.Set("cTransformCount", Napi::Number::New(env, c_transform_count));

        // Count JS transforms
        size_t js_transform_count = rc_ ? rc_->js_transforms.size() : 0;
        result.Set("jsTransformCount", Napi::Number::New(env, js_transform_count));

        // List field indices with transforms
        std::vector<int> field_indices;
        auto add_field_index = [&field_indices](int field_index) {
            if (std::find(field_indices.begin(), field_indices.end(), field_index) == field_indices.end()) {
                field_indices.push_back(field_index);
            }
        };

        if (rc_ && rc_->pipeline) {
            for (size_t i = 0; i < rc_->pipeline->count; i++) {
                add_field_index(rc_->pipeline->transforms[i].field_index);
            }
        }
        if (rc_) {
            for (const auto& pair : rc_->js_transforms) {
                add_field_index(pair.first);
            }
        }

        Napi::Array fields = Napi::Array::New(env, field_indices.size());
        for (size_t i = 0; i < field_indices.size(); i++) {
            fields[i] = Napi::Number::New(env, field_indices[i]);
        }

        result.Set("fieldIndices", fields);

        return result;
    }

    Napi::Value GetStats(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        Napi::Object stats = Napi::Object::New(env);
        size_t row_count = 0;
        size_t field_count = 0;
        if (!fast_rows_ref_.IsEmpty()) {
            Napi::Array rows = fast_rows_ref_.Value().As<Napi::Array>();
            row_count = rows.Length();
            if (row_count > 0) {
                Napi::Value first = rows.Get(static_cast<uint32_t>(0));
                if (first.IsArray()) {
                    field_count = first.As<Napi::Array>().Length();
                }
            }
        } else if (batch_result_) {
            row_count = batch_result_->row_count;
            if (batch_result_->row_count > 0) {
                field_count = batch_result_->rows[0].field_count;
            }
        } else if (rc_) {
            row_count = rc_->rows.size();
            if (!rc_->rows.empty()) {
                field_count = rc_->rows[0].size();
            }
        }

        stats.Set("rowCount", Napi::Number::New(env, row_count));
        stats.Set("fieldCount", Napi::Number::New(env, field_count));
        stats.Set("totalBytes", Napi::Number::New(env, total_bytes_));
        stats.Set("parseTime", Napi::Number::New(env, parse_time_));
        stats.Set("currentLine", Napi::Number::New(env,
            parser_ ? cisv_parser_get_line_number(parser_) : 0));

        return stats;
    }

    // Static method to count rows
    static Napi::Value CountRows(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (info.Length() != 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        std::string path = info[0].As<Napi::String>();
        size_t count = cisv_parser_count_rows(path.c_str());

        return Napi::Number::New(env, count);
    }

    // Static method to count rows with configuration
    static Napi::Value CountRowsWithConfig(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        std::string path = info[0].As<Napi::String>();

        cisv_config config;
        cisv_config_init(&config);

        // Apply configuration if provided
        if (info.Length() > 1 && !info[1].IsNull() && !info[1].IsUndefined() && !info[1].IsObject()) {
            throw Napi::TypeError::New(env, "Config must be an object");
        }

        if (info.Length() > 1 && info[1].IsObject()) {
            Napi::Object options = info[1].As<Napi::Object>();
            ApplyConfigOptions(env, options, &config);
        }

        size_t count = cisv_parser_count_rows_with_config(path.c_str(), &config);

        return Napi::Number::New(env, count);
    }

    // =========================================================================
    // Iterator API - Row-by-row streaming with early exit support
    // =========================================================================

    /**
     * Open a file for row-by-row iteration.
     * Uses the current parser configuration.
     * @param path - Path to CSV file
     * @returns this for chaining
     */
    Napi::Value OpenIterator(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (info.Length() < 1 || !info[0].IsString()) {
            throw Napi::TypeError::New(env, "Expected file path string");
        }

        // Close existing iterator if any
        if (iterator_) {
            cisv_iterator_close(iterator_);
            iterator_ = nullptr;
        }

        std::string path = info[0].As<Napi::String>();

        // Use current parser configuration for the iterator
        iterator_ = cisv_iterator_open(path.c_str(), &config_);
        if (!iterator_) {
            throw Napi::Error::New(env, "Failed to open file for iteration: " + path);
        }

        return info.This();  // Return this for chaining
    }

    /**
     * Fetch the next row from the iterator.
     * @returns Array of strings, or null if at end of file
     */
    Napi::Value FetchRow(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (!iterator_) {
            throw Napi::Error::New(env, "No iterator open. Call openIterator() first.");
        }

        const char **fields;
        const size_t *lengths;
        size_t field_count;

        int result = cisv_iterator_next(iterator_, &fields, &lengths, &field_count);

        if (result == CISV_ITER_EOF) {
            return env.Null();
        }
        if (result == CISV_ITER_ERROR) {
            throw Napi::Error::New(env, "Error reading CSV row");
        }

        napi_value row;
        napi_create_array_with_length(env, field_count, &row);
        bool ascii_only = true;
        for (size_t i = 0; i < field_count; i++) {
            if (!isAllAscii(fields[i], lengths[i])) {
                ascii_only = false;
                break;
            }
        }
        for (size_t i = 0; i < field_count; i++) {
            napi_set_element(env, row, i, NewCsvStringValue(env, fields[i], lengths[i], ascii_only));
        }

        return Napi::Value(env, row);
    }

    /**
     * Close the iterator and release resources.
     * @returns this for chaining
     */
    Napi::Value CloseIterator(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (is_destroyed_) {
            throw Napi::Error::New(env, "Parser has been destroyed");
        }

        if (iterator_) {
            cisv_iterator_close(iterator_);
            iterator_ = nullptr;
        }

        return info.This();  // Return this for chaining
    }

private:
    void ensureParser(Napi::Env env) {
        if (parser_) {
            return;
        }

        config_.field_cb = field_cb;
        config_.row_cb = row_cb;
        config_.error_cb = error_cb;
        config_.user = rc_;

        parser_ = cisv_parser_create_with_config(&config_);
        if (!parser_) {
            throw Napi::Error::New(env, "Failed to create parser");
        }
    }

    void clearBatchResult() {
        if (batch_result_) {
            cisv_result_free(batch_result_);
            batch_result_ = nullptr;
        }
    }

    void clearFastRows() {
        if (!fast_rows_ref_.IsEmpty()) {
            fast_rows_ref_.Reset();
        }
    }

    void clearPendingBuffer() {
        if (!pending_buffer_ref_.IsEmpty()) {
            pending_buffer_ref_.Reset();
        }
        pending_buffer_data_ = nullptr;
        pending_buffer_size_ = 0;
    }

    bool hasTransforms() const {
        bool has_c_transforms = rc_ && rc_->pipeline && rc_->pipeline->count > 0;
        bool has_js_transforms = rc_ && !rc_->js_transforms.empty();
        return has_c_transforms || has_js_transforms;
    }

    void resetRowState() {
        clearBatchResult();
        clearFastRows();
        pending_stream_.clear();
        clearPendingBuffer();
        stream_buffering_active_ = true;
        if (!rc_) return;
        rc_->rows.clear();
        rc_->current.clear();
        rc_->current_field_index = 0;
    }

    void flushPendingStreamToParser() {
        if (pending_buffer_size_ > 0) {
            ensureParser(Env());
            cisv_parser_write(parser_, pending_buffer_data_, pending_buffer_size_);
            clearPendingBuffer();
        }
        if (pending_stream_.empty()) {
            return;
        }
        ensureParser(Env());
        cisv_parser_write(
            parser_,
            reinterpret_cast<const uint8_t*>(pending_stream_.data()),
            pending_stream_.size());
        pending_stream_.clear();
    }

    void loadRowsFromBatch(const cisv_result_t *result) {
        if (!rc_ || !result) return;
        rc_->rows.clear();
        rc_->rows.reserve(result->row_count);

        for (size_t i = 0; i < result->row_count; i++) {
            const cisv_row_t *row = &result->rows[i];
            std::vector<std::string> out_row;
            out_row.reserve(row->field_count);
            for (size_t j = 0; j < row->field_count; j++) {
                out_row.emplace_back(row->fields[j], row->field_lengths[j]);
            }
            rc_->rows.emplace_back(std::move(out_row));
        }
    }

    Napi::Value drainRows(Napi::Env env) {
        if (!fast_rows_ref_.IsEmpty()) {
            return fast_rows_ref_.Value();
        }

        if (batch_result_) {
            napi_value rows;
            napi_create_array_with_length(env, batch_result_->row_count, &rows);
            const bool ascii_only =
                !batch_result_->field_data ||
                batch_result_->field_data_size == 0 ||
                isAllAscii(batch_result_->field_data, batch_result_->field_data_size);
            for (size_t i = 0; i < batch_result_->row_count; ++i) {
                const cisv_row_t *src_row = &batch_result_->rows[i];
                napi_value row;
                napi_create_array_with_length(env, src_row->field_count, &row);
                for (size_t j = 0; j < src_row->field_count; ++j) {
                    napi_set_element(
                        env,
                        row,
                        j,
                        NewCsvStringValue(env, src_row->fields[j], src_row->field_lengths[j], ascii_only));
                }
                napi_set_element(env, rows, i, row);
            }
            return Napi::Value(env, rows);
        }

        if (!rc_) {
            return Napi::Array::New(env, 0);
        }

        napi_value rows;
        napi_create_array_with_length(env, rc_->rows.size(), &rows);
        const bool ascii_only = rowsAreAscii(rc_->rows);

        for (size_t i = 0; i < rc_->rows.size(); ++i) {
            napi_value row;
            napi_create_array_with_length(env, rc_->rows[i].size(), &row);
            for (size_t j = 0; j < rc_->rows[i].size(); ++j) {
                // SECURITY: Use safe string creation to handle invalid UTF-8 in CSV data
                const std::string& field = rc_->rows[i][j];
                napi_set_element(
                    env,
                    row,
                    j,
                    NewCsvStringValue(env, field.c_str(), field.length(), ascii_only));
            }
            napi_set_element(env, rows, i, row);
        }

        // Don't clear here if we want to keep data for multiple reads
        // rc_->rows.clear();

        return Napi::Value(env, rows);
    }

    cisv_parser *parser_;
    cisv_config config_;
    RowCollector *rc_;
    size_t total_bytes_;
    double parse_time_;
    bool is_destroyed_;
    cisv_iterator_t *iterator_;  // For row-by-row iteration
    cisv_result_t *batch_result_;
    Napi::ObjectReference fast_rows_ref_;
    std::string pending_stream_;
    Napi::ObjectReference pending_buffer_ref_;
    const uint8_t *pending_buffer_data_;
    size_t pending_buffer_size_;
    bool stream_buffering_active_;
    static constexpr size_t kStreamBufferLimitBytes = 8 * 1024 * 1024;
};

// Initialize all exports
Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    CisvParser::Init(env, exports);

    // Add version info
    exports.Set("version", Napi::String::New(env, "0.5.0"));

    // Add transform type constants
    Napi::Object transformTypes = Napi::Object::New(env);
    transformTypes.Set("UPPERCASE", Napi::String::New(env, "uppercase"));
    transformTypes.Set("LOWERCASE", Napi::String::New(env, "lowercase"));
    transformTypes.Set("TRIM", Napi::String::New(env, "trim"));
    transformTypes.Set("TO_INT", Napi::String::New(env, "to_int"));
    transformTypes.Set("TO_FLOAT", Napi::String::New(env, "to_float"));
    transformTypes.Set("BASE64_ENCODE", Napi::String::New(env, "base64_encode"));
    exports.Set("TransformType", transformTypes);

    return exports;
}

NODE_API_MODULE(cisv, InitAll)
