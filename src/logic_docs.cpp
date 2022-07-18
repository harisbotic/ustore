/**
 * @file logic_docs.cpp
 * @author Ashot Vardanian
 *
 * @brief Document storage using "nlohmann/JSON" lib.
 * Sits on top of any @see "ukv.h"-compatible system.
 */

#include <vector>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ukv/docs.hpp"
#include "helpers.hpp"

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

using json_t = nlohmann::json;
using json_ptr_t = json_t::json_pointer;

constexpr ukv_format_t internal_format_k = ukv_format_msgpack_k;

/**
 * @brief Extracts a select subset of keys by from input document.
 *
 * Can be implemented through flattening, sampling and unflattening.
 * https://json.nlohmann.me/api/json_pointer/
 */
json_t sample_fields(json_t&& original,
                     std::vector<json_ptr_t> const& json_pointers,
                     std::vector<std::string> const& json_pointers_strs) {

    if (json_pointers.empty())
        return std::move(original);

    json_t empty {nullptr};
    json_t result = json_t::object();
    for (std::size_t ptr_idx = 0; ptr_idx != json_pointers.size(); ++ptr_idx) {

        auto const& ptr = json_pointers[ptr_idx];
        auto const& ptr_str = json_pointers_strs[ptr_idx];

        // An exception-safe approach to searching for JSON-pointers:
        // https://json.nlohmann.me/api/basic_json/at/#exceptions
        // https://json.nlohmann.me/api/basic_json/operator%5B%5D/#exceptions
        // https://json.nlohmann.me/api/basic_json/value/#exception-safety
        auto found = original.value(ptr, empty);
        if (found != empty)
            result[ptr_str] = std::move(found);
    }

    // https://json.nlohmann.me/features/json_pointer/
    // https://json.nlohmann.me/api/basic_json/flatten/
    // https://json.nlohmann.me/api/basic_json/unflatten/
    return result.unflatten();
}

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

struct export_to_value_t : public nlohmann::detail::output_adapter_protocol<char>,
                           public std::enable_shared_from_this<export_to_value_t> {
    value_t* value_ptr = nullptr;

    export_to_value_t() = default;
    export_to_value_t(value_t& value) noexcept : value_ptr(&value) {}

    void write_character(char c) override { value_ptr->push_back(static_cast<byte_t>(c)); }
    void write_characters(char const* s, std::size_t length) override {
        auto ptr = reinterpret_cast<byte_t const*>(s);
        value_ptr->insert(value_ptr->size(), ptr, ptr + length);
    }
};

json_t parse_any(value_view_t bytes, ukv_format_t const c_format, ukv_error_t* c_error) {
    auto str = reinterpret_cast<char const*>(bytes.begin());
    auto len = bytes.size();
    switch (c_format) {
    case ukv_format_json_k:
    case ukv_format_json_patch_k: return json_t::parse(str, str + len, nullptr, true, false);
    case ukv_format_msgpack_k: return json_t::from_msgpack(str, str + len, true, false);
    case ukv_format_bson_k: return json_t::from_bson(str, str + len, true, false);
    case ukv_format_cbor_k: return json_t::from_cbor(str, str + len, true, false);
    case ukv_format_ubjson_k: return json_t::from_ubjson(str, str + len, true, false);
    default: *c_error = "Unsupported unput format"; return {};
    }
}

/**
 * The JSON package provides a number of simple interfaces, which only work with simplest STL types
 * and always allocate the output objects, without the ability to reuse previously allocated memory,
 * including: `dump`, `to_msgpack`, `to_bson`, `to_cbor`, `to_ubjson`.
 * They have more flexible alternatives in the form of `nlohmann::detail::serializer`s,
 * that will accept our custom adapter. Unfortunately, they require a bogus shared pointer. WHY?!
 */
void dump_any(json_t const& json,
              ukv_format_t const c_format,
              std::shared_ptr<export_to_value_t> const& value,
              ukv_error_t* c_error) {

    using text_serializer_t = nlohmann::detail::serializer<json_t>;
    using binary_serializer_t = nlohmann::detail::binary_writer<json_t, char>;

    switch (c_format) {
    case ukv_format_json_k: return text_serializer_t(value, ' ').dump(json, false, false, 0, 0);
    case ukv_format_msgpack_k: return binary_serializer_t(value).write_msgpack(json);
    case ukv_format_bson_k: return binary_serializer_t(value).write_bson(json);
    case ukv_format_cbor_k: return binary_serializer_t(value).write_cbor(json);
    case ukv_format_ubjson_k: return binary_serializer_t(value).write_ubjson(json, true, true);
    default: *c_error = "Unsupported unput format"; break;
    }
}

void update_docs( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    write_tasks_soa_t const& tasks,
    strided_iterator_gt<ukv_str_view_t const>,
    ukv_size_t const n,
    ukv_options_t const c_options,
    ukv_format_t const c_format,
    stl_arena_t& arena,
    ukv_error_t* c_error) {

    prepare_memory(arena.updated_vals, n, c_error);
    if (*c_error)
        return;

    auto exporter_on_heap = std::make_shared<export_to_value_t>();
    for (ukv_size_t i = 0; i != n; ++i) {
        auto task = tasks[i];
        auto& serialized = arena.updated_vals[i];
        if (task.is_deleted()) {
            serialized.reset();
            continue;
        }

        auto parsed = parse_any(task.view(), c_format, c_error);
        if (*c_error)
            return;
        if (parsed.is_discarded()) {
            *c_error = "Couldn't parse inputs";
            return;
        }

        exporter_on_heap->value_ptr = &serialized;
        dump_any(parsed, ukv_format_msgpack_k, exporter_on_heap, c_error);
        if (*c_error)
            return;
    }

    ukv_val_len_t offset = 0;
    ukv_arena_t arena_ptr = &arena;
    ukv_write( //
        c_db,
        c_txn,
        n,
        tasks.cols.get(),
        tasks.cols.stride(),
        tasks.keys.get(),
        tasks.keys.stride(),
        arena.updated_vals.front().internal_cptr(),
        sizeof(value_t),
        &offset,
        0,
        arena.updated_vals.front().internal_length(),
        sizeof(value_t),
        c_options,
        &arena_ptr,
        c_error);
}

void update_fields( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    write_tasks_soa_t const& tasks,
    strided_iterator_gt<ukv_str_view_t const> fields,
    ukv_size_t const n,
    ukv_options_t const c_options,
    ukv_format_t const c_format,
    stl_arena_t& arena,
    ukv_error_t* c_error) {

    // When only specific fields are of interest, we are forced to:
    // 1. read the entire entries,
    // 2. parse them,
    // 3. locate the requested keys,
    // 4. replace them with provided values, or patch nested objects.

    std::vector<json_t> parsed(n);
    std::vector<json_ptr_t> fields_ptrs;

    std::vector<buffer_t> serialized(n);

    if (parsed[0].is_discarded()) {
        *c_error = "Couldn't parse inputs";
        return;
    }
}

void ukv_docs_write( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,
    ukv_format_t const c_format,

    ukv_val_ptr_t const* c_vals,
    ukv_size_t const c_vals_stride,

    ukv_val_len_t const* c_offs,
    ukv_size_t const c_offs_stride,

    ukv_val_len_t const* c_lens,
    ukv_size_t const c_lens_stride,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    // If user wants the entire doc in the same format, as the one we use internally,
    // this request can be passed entirely to the underlying Key-Value store.
    if (!c_fields && c_format == internal_format_k)
        return ukv_write(c_db,
                         c_txn,
                         c_tasks_count,
                         c_cols,
                         c_cols_stride,
                         c_keys,
                         c_keys_stride,
                         c_options,
                         c_vals,
                         c_vals_stride,
                         c_offs,
                         c_offs_stride,
                         c_lens,
                         c_lens_stride,
                         c_arena,
                         c_error);

    if (!c_db) {
        *c_error = "DataBase is NULL!";
        return;
    }

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};
    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};
    strided_iterator_gt<ukv_val_ptr_t const> vals {c_vals, c_vals_stride};
    strided_iterator_gt<ukv_val_len_t const> offs {c_offs, c_offs_stride};
    strided_iterator_gt<ukv_val_len_t const> lens {c_lens, c_lens_stride};
    write_tasks_soa_t tasks {cols, keys, vals, offs, lens};

    try {
        auto func = fields ? &update_fields : &update_docs;
        func(c_db, c_txn, tasks, fields, c_tasks_count, c_options, c_format, arena, c_error);
    }
    catch (std::bad_alloc const&) {
        *c_error = "Failed to allocate memory!";
    }
}

void ukv_docs_read( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const n,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,
    ukv_format_t const c_format,

    ukv_val_len_t** c_found_lengths,
    ukv_val_ptr_t* c_found_values,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    // If user wants the entire doc in the same format, as the one we use internally,
    // this request can be passed entirely to the underlying Key-Value store.
    if (!c_fields && c_format == internal_format_k)
        return ukv_read(c_db,
                        c_txn,
                        n,
                        c_cols,
                        c_cols_stride,
                        c_keys,
                        c_keys_stride,
                        c_options,
                        c_found_lengths,
                        c_found_values,
                        c_arena,
                        c_error);

    if (!c_db) {
        *c_error = "DataBase is NULL!";
        return;
    }

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    prepare_memory(arena.updated_keys, n, c_error);
    if (*c_error)
        return;
    prepare_memory(arena.updated_vals, n, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};
    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};
    read_tasks_soa_t tasks {cols, keys};

    // We can now detect collisions among requested keys,
    // if different fields from the same docs are requested.
    // In that case, we must only fetch the doc once and later
    // slice it into output fields.
    for (ukv_size_t i = 0; i != n; ++i)
        arena.updated_keys[i] = tasks[i].location();
    sort_and_deduplicate(arena.updated_keys);
    // TODO: Handle the common case of requesting the non-colliding
    // all-ascending input sequences of document IDs received during scans
    // without the sort and extra memory.

    ukv_val_len_t* found_lengths = nullptr;
    ukv_val_ptr_t found_values = nullptr;
    ukv_size_t found_count = static_cast<ukv_size_t>(arena.updated_keys.size());
    ukv_read(c_db,
             c_txn,
             found_count,
             &arena.updated_keys[0].collection,
             sizeof(located_key_t),
             &arena.updated_keys[0].key,
             sizeof(located_key_t),
             c_options,
             &found_lengths,
             &found_values,
             c_arena,
             c_error);
    if (*c_error)
        return;

    try {
        // We will later need to locate the data for every separate request.
        // Doing it in O(N) tape iterations every time is too slow.
        // Once we transform to inclusive sums, it will be O(1).
        //      inplace_inclusive_prefix_sum(found_lengths, found_lengths + found_count);
        // Alternatively we can compensate it with additional memory:
        auto parsed_values = std::vector<json_t>(n);
        auto found_tape = taped_values_view_t(found_lengths, found_values, found_count);
        auto found_tape_it = found_tape.begin();
        for (ukv_size_t i = 0; i != found_count; ++i, ++found_tape_it) {
            value_view_t found_value = *found_tape_it;
            json_t& parsed = parsed_values[i];
            parsed = parse_any(found_value, c_format, c_error);

            // This error is extremely unlikely, as we have previously accepted the data into the store.
            if (*c_error)
                return;
        }

        // Now, we need to parse all the entries to later export them into a target format.
        // Potentially sampling certain sub-fields again along the way.
        auto exporter_on_heap = std::make_shared<export_to_value_t>();
        auto temporary_buffer = value_t();
        exporter_on_heap->value_ptr = &temporary_buffer;
        auto null_object = json_t(nullptr);
        arena.growing_tape.clear();

        for (ukv_size_t i = 0; i != n; ++i) {
            auto task = tasks[i];
            auto parsed_idx = offset_in_sorted(arena.updated_keys, task.location());
            json_t& parsed = parsed_values[parsed_idx];

            if (fields && fields[i]) {
                try {
                    if (fields[i][0] == '/') {
                        json_ptr_t field_ptr {fields[i]};
                        json_t& sub = parsed.at(field_ptr);
                        dump_any(sub, c_format, exporter_on_heap, c_error);
                    }
                    else {
                        json_t const& sub = parsed.at(fields[i]);
                        dump_any(sub, c_format, exporter_on_heap, c_error);
                    }
                }
                catch (nlohmann::json::out_of_range&) {
                    dump_any(null_object, c_format, exporter_on_heap, c_error);
                }
            }
            else {
                dump_any(parsed, c_format, exporter_on_heap, c_error);
            }
            if (*c_error)
                return;

            if (c_format == ukv_format_json_k)
                temporary_buffer.push_back(byte_t {0});

            arena.growing_tape.push_back(temporary_buffer);
            temporary_buffer.clear();
        }

        *c_found_lengths = taped_values_view_t(arena.growing_tape).lengths();
        *c_found_values = taped_values_view_t(arena.growing_tape).contents();
    }
    catch (std::bad_alloc const&) {
        *c_error = "Failed to allocate memory!";
    }
}