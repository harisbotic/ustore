#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <map>
#include <cstring>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <filesystem>

#include <simdjson.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <ustore/ustore.hpp>

using namespace unum::ustore;

constexpr ustore_str_view_t dataset_path_k = "~/Datasets/tweets32K-clean.ndjson";
constexpr size_t docs_count = 1000;
static database_t db;

simdjson::ondemand::object& rewinded(simdjson::ondemand::object& doc) noexcept {
    doc.reset();
    return doc;
}

std::vector<ustore_doc_field_type_t> types;
std::vector<value_view_t> paths;
std::vector<edge_t> vtx_n_edges;
std::vector<std::string> fields;
std::vector<value_view_t> docs;
std::vector<ustore_key_t> keys;

constexpr ustore_str_view_t id = "id";

struct triplet_t {
    ustore_str_view_t raw;
    std::vector<char> strs;
    std::vector<ustore_length_t> offs;
    std::vector<ustore_length_t> lens;
    std::vector<ustore_octet_t> pres;
    ustore_size_t count;
    void fill(std::vector<value_view_t> const& src) {
        count = src.size();
        offs.reserve(src.size());
        lens.reserve(src.size());
        pres.reserve(src.size());
        offs.push_back(0);
        for (auto path : src) {
            for (auto ch : path)
                strs.push_back(char(ch));
            pres.push_back(path.size() ? 1 : 0);
            offs.push_back(strs.size());
            lens.push_back(path.size());
        }
        raw = strs.data();
    }
    ustore_str_view_t const* ptr() { return &raw; }
    ustore_length_t const* offsets() { return offs.data(); }
    ustore_length_t const* lengths() { return lens.data(); }
    ustore_octet_t const* presences() { return pres.data(); }
    ustore_size_t size() { return count; }
};

void make_batch() {

    bool state = true;

    std::string dataset_path = dataset_path_k;
    dataset_path = std::filesystem::path(std::getenv("HOME")) / dataset_path.substr(2);

    auto handle = open(dataset_path.c_str(), O_RDONLY);
    ustore_size_t file_size = std::filesystem::file_size(std::filesystem::path(dataset_path.c_str()));
    auto begin = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, handle, 0);
    std::string_view mapped_content = std::string_view(reinterpret_cast<ustore_char_t const*>(begin), file_size);
    madvise(begin, file_size, MADV_SEQUENTIAL);

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream documents = parser.iterate_many( //
        mapped_content.data(),
        mapped_content.size(),
        1000000ul);

    docs.reserve(docs_count);
    keys.reserve(docs_count);
    vtx_n_edges.reserve(docs_count);
    paths.reserve(docs_count);
    size_t count = 0;
    size_t idx = 0;
    for (auto doc : documents) {
        simdjson::ondemand::object object = doc.get_object().value();
        if (state) {
            simdjson::dom::parser dom_parser;
            simdjson::dom::element doc_ = dom_parser.parse(rewinded(object).raw_json().value());
            simdjson::dom::object obj = doc_.get_object();

            for (auto [key, value] : obj)
                fields.push_back(key.data());
        }
        if (state) {
            types.reserve(fields.size());
            for (auto field : fields) {
                switch (rewinded(object)[field].type()) {
                case simdjson::ondemand::json_type::array: types.push_back(ustore_doc_field_str_k); break;
                case simdjson::ondemand::json_type::object: types.push_back(ustore_doc_field_json_k); break;
                case simdjson::ondemand::json_type::number: {
                    if (rewinded(object)[field].is_integer())
                        types.push_back(ustore_doc_field_i64_k);
                    else
                        types.push_back(ustore_doc_field_f64_k);
                } break;
                case simdjson::ondemand::json_type::string: types.push_back(ustore_doc_field_str_k); break;
                case simdjson::ondemand::json_type::boolean: types.push_back(ustore_doc_field_bool_k); break;
                case simdjson::ondemand::json_type::null: types.push_back(ustore_doc_field_null_k); break;
                default: break;
                }
            }
            state = false;
        }
        docs.push_back(rewinded(object).raw_json().value());
        keys.push_back(rewinded(object)[id]);
        paths.push_back(rewinded(object)[id].raw_json_token().value());
        vtx_n_edges.push_back(edge_t {idx, idx + 1, idx + 2});
        ++count;
        if (count == docs_count)
            break;
        idx += 3;
    }
    close(handle);
}

void test_single_read_n_write() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    ustore_key_t key = std::rand();
    auto str = fmt::format("{{\"_id\":{},\"doc\":\"abcdefghijklmnop\"}}", key);
    value_view_t write_value = str.c_str();

    ustore_docs_write_t write {};
    write.db = db;
    write.error = status.member_ptr();
    write.arena = arena.member_ptr();
    write.collections = &collection;
    write.options = ustore_options_default_k;
    write.tasks_count = 1;
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.lengths = write_value.member_length();
    write.values = write_value.member_ptr();
    write.id_field = "_id";
    ustore_docs_write(&write);
    EXPECT_TRUE(status);

    ustore_bytes_ptr_t read_value = nullptr;
    ustore_docs_read_t read {};
    read.db = db;
    read.error = status.member_ptr();
    read.arena = arena.member_ptr();
    read.options = ustore_options_default_k;
    read.type = ustore_doc_field_json_k;
    read.tasks_count = 1;
    read.collections = &collection;
    read.keys = &key;
    read.values = &read_value;
    ustore_docs_read(&read);
    EXPECT_TRUE(status);
    EXPECT_EQ(std::strcmp(write_value.c_str(), (char const*)read_value), 0);

    db.clear().throw_unhandled();

    write.keys = &key;
    write.id_field = nullptr;
    ustore_docs_write(&write);
    EXPECT_TRUE(status);

    read_value = nullptr;
    ustore_docs_read(&read);
    EXPECT_TRUE(status);
    EXPECT_EQ(std::strcmp(write_value.c_str(), (char const*)read_value), 0);
    db.clear().throw_unhandled();
}

void test_batch_read_n_write() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    ustore_docs_write_t write {};
    write.db = db;
    write.error = status.member_ptr();
    write.arena = arena.member_ptr();
    write.collections = &collection;
    write.options = ustore_options_default_k;
    write.tasks_count = keys.size();
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.keys = keys.data();
    write.keys_stride = sizeof(ustore_key_t);
    write.lengths = docs.front().member_length();
    write.lengths_stride = sizeof(value_view_t);
    write.values = docs.front().member_ptr();
    write.values_stride = sizeof(value_view_t);
    ustore_docs_write(&write);
    EXPECT_TRUE(status);

    ustore_octet_t* presences = nullptr;
    ustore_length_t* offsets = nullptr;
    ustore_length_t* lengths = nullptr;
    ustore_bytes_ptr_t values = nullptr;

    ustore_docs_read_t read {};
    read.db = db;
    read.error = status.member_ptr();
    read.arena = arena.member_ptr();
    read.options = ustore_options_default_k;
    read.type = ustore_doc_field_json_k;
    read.tasks_count = keys.size();
    read.collections = &collection;
    read.keys = keys.data();
    read.keys_stride = sizeof(ustore_key_t);
    read.presences = &presences;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;
    ustore_docs_read(&read);
    EXPECT_TRUE(status);

    strided_iterator_gt<ustore_length_t const> offs {offsets, sizeof(ustore_length_t)};
    strided_iterator_gt<ustore_length_t const> lens {lengths, sizeof(ustore_length_t)};
    strided_iterator_gt<ustore_bytes_cptr_t const> vals {&values, 0};
    bits_view_t preses {presences};

    contents_arg_t contents {preses, offs, lens, vals, keys.size()};

    for (size_t idx = 0; idx < keys.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }

    db.clear().throw_unhandled();

    write.keys = nullptr;
    write.id_field = id;
    ustore_docs_write(&write);
    EXPECT_TRUE(status);

    presences = nullptr;
    offsets = nullptr;
    lengths = nullptr;
    values = nullptr;
    ustore_docs_read(&read);
    EXPECT_TRUE(status);

    for (size_t idx = 0; idx < keys.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }
    db.clear().throw_unhandled();
}

void test_gist() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    ustore_docs_write_t write {};
    write.db = db;
    write.error = status.member_ptr();
    write.arena = arena.member_ptr();
    write.collections = &collection;
    write.options = ustore_options_default_k;
    write.tasks_count = keys.size();
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.keys = keys.data();
    write.keys_stride = sizeof(ustore_key_t);
    write.lengths = docs.front().member_length();
    write.lengths_stride = sizeof(value_view_t);
    write.values = docs.front().member_ptr();
    write.values_stride = sizeof(value_view_t);
    ustore_docs_write(&write);
    EXPECT_TRUE(status);

    ustore_size_t fields_count = 0;
    ustore_length_t* offsets = nullptr;
    ustore_char_t* fields_ptr = nullptr;

    ustore_docs_gist_t gist {};
    gist.db = db;
    gist.error = status.member_ptr();
    gist.arena = arena.member_ptr();
    gist.docs_count = keys.size();
    gist.collections = &collection;
    gist.keys = keys.data();
    gist.keys_stride = sizeof(ustore_key_t);
    gist.fields_count = &fields_count;
    gist.offsets = &offsets;
    gist.fields = &fields_ptr;
    ustore_docs_gist(&gist);

    EXPECT_TRUE(status);
    EXPECT_EQ(fields_count, fields.size());
    for (size_t idx = 0; idx < fields.size(); ++idx) {
        EXPECT_EQ(std::strcmp(fields_ptr + offsets[idx] + 1, fields[idx].c_str()), 0);
    }
    db.clear().throw_unhandled();
}

void test_gather() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    ustore_docs_write_t write {};
    write.db = db;
    write.error = status.member_ptr();
    write.arena = arena.member_ptr();
    write.collections = &collection;
    write.options = ustore_options_default_k;
    write.tasks_count = keys.size();
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.keys = keys.data();
    write.keys_stride = sizeof(ustore_key_t);
    write.lengths = docs.front().member_length();
    write.lengths_stride = sizeof(value_view_t);
    write.values = docs.front().member_ptr();
    write.values_stride = sizeof(value_view_t);
    ustore_docs_write(&write);
    EXPECT_TRUE(status);

    ustore_str_view_t fields_[fields.size()];
    for (size_t idx = 0; idx < fields.size(); ++idx)
        fields_[idx] = fields[idx].data();

    ustore_octet_t** validities = nullptr;
    ustore_byte_t** scalars = nullptr;
    ustore_length_t** offsets = nullptr;
    ustore_length_t** lengths = nullptr;
    ustore_byte_t* strings = nullptr;

    ustore_docs_gather_t gather {};
    gather.db = db;
    gather.error = status.member_ptr();
    gather.arena = arena.member_ptr();
    gather.docs_count = keys.size();
    gather.fields_count = fields.size();
    gather.collections = &collection;
    gather.keys = keys.data();
    gather.keys_stride = sizeof(ustore_key_t);
    gather.fields = fields_;
    gather.fields_stride = sizeof(ustore_str_view_t);
    gather.types = types.data();
    gather.types_stride = sizeof(ustore_doc_field_type_t);
    gather.columns_validities = &validities;
    gather.columns_scalars = &scalars;
    gather.columns_offsets = &offsets;
    gather.columns_lengths = &lengths;
    gather.joined_strings = &strings;
    ustore_docs_gather(&gather);
    EXPECT_TRUE(status);

    db.clear().throw_unhandled();
}

void test_graph_single_upsert() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    ustore_key_t source = std::rand();
    ustore_key_t target = std::rand();
    ustore_key_t edge = std::rand();

    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = status.member_ptr();
    upsert.arena = arena.member_ptr();
    upsert.options = ustore_options_default_k;
    upsert.tasks_count = 1;
    upsert.collections = &collection;
    upsert.edges_ids = &edge;
    upsert.sources_ids = &source;
    upsert.targets_ids = &target;
    ustore_graph_upsert_edges(&upsert);

    EXPECT_TRUE(status);

    ustore_vertex_role_t role = ustore_vertex_role_any_k;
    ustore_vertex_degree_t* degrees = nullptr;
    ustore_key_t* ids = nullptr;
    ustore_key_t keys[2] = {source, target};

    ustore_graph_find_edges_t find {};
    find.db = db;
    find.error = status.member_ptr();
    find.arena = arena.member_ptr();
    find.options = ustore_options_default_k;
    find.tasks_count = 2;
    find.collections = &collection;
    find.vertices = keys;
    find.vertices_stride = sizeof(ustore_key_t);
    find.roles = &role;
    find.degrees_per_vertex = &degrees;
    find.edges_per_vertex = &ids;
    ustore_graph_find_edges(&find);
    EXPECT_TRUE(status);

    ustore_key_t expected[2][3] = {{source, target, edge}, {target, source, edge}};

    size_t ids_count = std::transform_reduce(degrees, degrees + 2, 0ul, std::plus {}, [](ustore_vertex_degree_t d) {
        return d != ustore_vertex_degree_missing_k ? d : 0;
    });
    ids_count *= 3;
    EXPECT_EQ(ids_count, 6);
    for (size_t i = 0, idx = 0; i < 2; ++i) {
        for (size_t j = 0; j < 3; ++j, ++idx) {
            EXPECT_EQ(ids[idx], expected[i][j]);
        }
    }
    db.clear().throw_unhandled();
}

void test_graph_batch_upsert_edges() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    auto strided = edges(vtx_n_edges);
    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = status.member_ptr();
    upsert.arena = arena.member_ptr();
    upsert.options = ustore_options_default_k;
    upsert.tasks_count = vtx_n_edges.size();
    upsert.collections = &collection;
    upsert.edges_ids = strided.edge_ids.begin().get();
    upsert.edges_stride = strided.edge_ids.stride();
    upsert.sources_ids = strided.source_ids.begin().get();
    upsert.sources_stride = strided.source_ids.stride();
    upsert.targets_ids = strided.target_ids.begin().get();
    upsert.targets_stride = strided.target_ids.stride();
    ustore_graph_upsert_edges(&upsert);
    EXPECT_TRUE(status);

    ustore_vertex_role_t role = ustore_vertex_source_k;
    ustore_vertex_degree_t* degrees = nullptr;
    ustore_key_t* ids = nullptr;

    ustore_graph_find_edges_t find {};
    find.db = db;
    find.error = status.member_ptr();
    find.arena = arena.member_ptr();
    find.options = ustore_options_default_k;
    find.tasks_count = strided.source_ids.size();
    find.collections = &collection;
    find.vertices = strided.source_ids.begin().get();
    find.vertices_stride = strided.source_ids.stride();
    find.roles = &role;
    find.degrees_per_vertex = &degrees;
    find.edges_per_vertex = &ids;
    ustore_graph_find_edges(&find);
    EXPECT_TRUE(status);

    size_t ids_count =
        std::transform_reduce(degrees,
                              degrees + strided.source_ids.size(),
                              0ul,
                              std::plus {},
                              [](ustore_vertex_degree_t d) { return d != ustore_vertex_degree_missing_k ? d : 0; });
    ids_count *= 3;

    EXPECT_EQ(ids_count, vtx_n_edges.size() * 3);
    for (size_t idx = 0; idx < ids_count; idx += 3) {
        EXPECT_EQ(ids[idx], vtx_n_edges[idx / 3].source_id);
        EXPECT_EQ(ids[idx + 1], vtx_n_edges[idx / 3].target_id);
        EXPECT_EQ(ids[idx + 2], vtx_n_edges[idx / 3].id);
    }
    db.clear().throw_unhandled();
}

void test_graph_batch_upsert_vtx() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    auto strided = edges(vtx_n_edges);
    ustore_graph_upsert_vertices_t upsert {};
    upsert.db = db;
    upsert.error = status.member_ptr();
    upsert.arena = arena.member_ptr();
    upsert.options = ustore_options_default_k;
    upsert.tasks_count = vtx_n_edges.size();
    upsert.collections = &collection;
    upsert.vertices = strided.source_ids.begin().get();
    upsert.vertices_stride = strided.source_ids.stride();
    ustore_graph_upsert_vertices(&upsert);
    EXPECT_TRUE(status);

    ustore_length_t count_limits = vtx_n_edges.size();
    ustore_length_t* found_counts = nullptr;
    ustore_key_t* found_keys = nullptr;
    ustore_key_t start_keys = 0;

    ustore_scan_t scan {};
    scan.db = db;
    scan.error = status.member_ptr();
    scan.arena = arena.member_ptr();
    scan.tasks_count = 1;
    scan.collections = &collection;
    scan.start_keys = &start_keys;
    scan.count_limits = &count_limits;
    scan.counts = &found_counts;
    scan.keys = &found_keys;
    ustore_scan(&scan);
    EXPECT_TRUE(status);

    EXPECT_EQ(*found_counts, vtx_n_edges.size());
    for (size_t idx = 0; idx < *found_counts; ++idx)
        EXPECT_EQ(found_keys[idx], vtx_n_edges[idx].source_id);
    db.clear().throw_unhandled();
}

void test_graph_find() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    auto strided = edges(vtx_n_edges);
    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = status.member_ptr();
    upsert.arena = arena.member_ptr();
    upsert.options = ustore_options_default_k;
    upsert.tasks_count = vtx_n_edges.size();
    upsert.collections = &collection;
    upsert.edges_ids = strided.edge_ids.begin().get();
    upsert.edges_stride = strided.edge_ids.stride();
    upsert.sources_ids = strided.source_ids.begin().get();
    upsert.sources_stride = strided.source_ids.stride();
    upsert.targets_ids = strided.target_ids.begin().get();
    upsert.targets_stride = strided.target_ids.stride();
    ustore_graph_upsert_edges(&upsert);

    EXPECT_TRUE(status);

    ustore_vertex_role_t role = ustore_vertex_source_k;
    ustore_vertex_degree_t* degrees = nullptr;
    ustore_key_t* ids = nullptr;

    EXPECT_TRUE(status);

    ustore_graph_find_edges_t find {};
    find.db = db;
    find.error = status.member_ptr();
    find.arena = arena.member_ptr();
    find.options = ustore_options_default_k;
    find.tasks_count = strided.source_ids.size();
    find.collections = &collection;
    find.vertices = strided.source_ids.begin().get();
    find.vertices_stride = strided.source_ids.stride();
    find.roles = &role;
    find.degrees_per_vertex = &degrees;
    find.edges_per_vertex = &ids;
    ustore_graph_find_edges(&find);
    EXPECT_TRUE(status);

    size_t ids_count =
        std::transform_reduce(degrees,
                              degrees + strided.source_ids.size(),
                              0ul,
                              std::plus {},
                              [](ustore_vertex_degree_t d) { return d != ustore_vertex_degree_missing_k ? d : 0; });
    ids_count *= 3;

    EXPECT_EQ(ids_count, vtx_n_edges.size() * 3);
    for (size_t idx = 0; idx < ids_count; idx += 3) {
        EXPECT_EQ(ids[idx], vtx_n_edges[idx / 3].source_id);
        EXPECT_EQ(ids[idx + 1], vtx_n_edges[idx / 3].target_id);
        EXPECT_EQ(ids[idx + 2], vtx_n_edges[idx / 3].id);
    }

    role = ustore_vertex_target_k;
    degrees = nullptr;
    ids = nullptr;
    find.tasks_count = strided.target_ids.size();
    find.vertices = strided.target_ids.begin().get();
    find.vertices_stride = strided.target_ids.stride();
    ustore_graph_find_edges(&find);
    EXPECT_TRUE(status);

    ids_count =
        std::transform_reduce(degrees,
                              degrees + strided.target_ids.size(),
                              0ul,
                              std::plus {},
                              [](ustore_vertex_degree_t d) { return d != ustore_vertex_degree_missing_k ? d : 0; });
    ids_count *= 3;

    EXPECT_EQ(ids_count, vtx_n_edges.size() * 3);
    for (size_t idx = 0; idx < ids_count; idx += 3) {
        EXPECT_EQ(ids[idx], vtx_n_edges[idx / 3].target_id);
        EXPECT_EQ(ids[idx + 1], vtx_n_edges[idx / 3].source_id);
        EXPECT_EQ(ids[idx + 2], vtx_n_edges[idx / 3].id);
    }

    std::vector<edge_t> expected = vtx_n_edges;
    for (auto _ : vtx_n_edges)
        expected.push_back(edge_t {_.target_id, _.source_id, _.id});

    std::sort(expected.begin(), expected.end(), [](auto const& lhs, auto const& rhs) {
        return lhs.source_id < rhs.source_id;
    });

    auto exp_strided = edges(expected);
    role = ustore_vertex_role_any_k;
    degrees = nullptr;
    ids = nullptr;
    find.tasks_count = exp_strided.source_ids.size();
    find.vertices = exp_strided.source_ids.begin().get();
    find.vertices_stride = exp_strided.source_ids.stride();
    ustore_graph_find_edges(&find);
    EXPECT_TRUE(status);

    ids_count =
        std::transform_reduce(degrees,
                              degrees + exp_strided.source_ids.size(),
                              0ul,
                              std::plus {},
                              [](ustore_vertex_degree_t d) { return d != ustore_vertex_degree_missing_k ? d : 0; });
    ids_count *= 3;

    EXPECT_EQ(ids_count, expected.size() * 3);
    for (size_t idx = 0; idx < ids_count; idx += 3) {
        EXPECT_EQ(ids[idx], expected[idx / 3].source_id);
        EXPECT_EQ(ids[idx + 1], expected[idx / 3].target_id);
        EXPECT_EQ(ids[idx + 2], expected[idx / 3].id);
    }
    db.clear().throw_unhandled();
}

void test_graph_remove_edges() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    auto strided = edges(vtx_n_edges);
    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = status.member_ptr();
    upsert.arena = arena.member_ptr();
    upsert.options = ustore_options_default_k;
    upsert.tasks_count = vtx_n_edges.size();
    upsert.collections = &collection;
    upsert.edges_ids = strided.edge_ids.begin().get();
    upsert.edges_stride = strided.edge_ids.stride();
    upsert.sources_ids = strided.source_ids.begin().get();
    upsert.sources_stride = strided.source_ids.stride();
    upsert.targets_ids = strided.target_ids.begin().get();
    upsert.targets_stride = strided.target_ids.stride();
    ustore_graph_upsert_edges(&upsert);
    EXPECT_TRUE(status);

    ustore_graph_remove_edges_t remove {};
    remove.db = db;
    remove.error = status.member_ptr();
    remove.arena = arena.member_ptr();
    remove.options = ustore_options_default_k;
    remove.tasks_count = vtx_n_edges.size();
    remove.collections = &collection;
    remove.edges_ids = strided.edge_ids.begin().get();
    remove.edges_stride = strided.edge_ids.stride();
    remove.sources_ids = strided.source_ids.begin().get();
    remove.sources_stride = strided.source_ids.stride();
    remove.targets_ids = strided.target_ids.begin().get();
    remove.targets_stride = strided.target_ids.stride();
    ustore_graph_remove_edges(&remove);
    EXPECT_TRUE(status);

    std::vector<ustore_key_t> all_keys;
    all_keys.reserve(vtx_n_edges.size() * 2);
    for (auto key : strided.source_ids)
        all_keys.push_back(key);

    for (auto key : strided.target_ids)
        all_keys.push_back(key);
    std::sort(all_keys.begin(), all_keys.end());

    ustore_vertex_role_t role = ustore_vertex_role_any_k;
    ustore_key_t* ids = nullptr;

    ustore_graph_find_edges_t find {};
    find.db = db;
    find.error = status.member_ptr();
    find.arena = arena.member_ptr();
    find.options = ustore_options_default_k;
    find.tasks_count = all_keys.size();
    find.collections = &collection;
    find.vertices = all_keys.data();
    find.vertices_stride = sizeof(ustore_key_t);
    find.roles = &role;
    find.edges_per_vertex = &ids;
    ustore_graph_find_edges(&find);
    EXPECT_TRUE(status);
    EXPECT_EQ(ids, nullptr);
    db.clear().throw_unhandled();
}

void test_graph_remove_vertices(ustore_vertex_role_t role) {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    auto strided = edges(vtx_n_edges);
    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = status.member_ptr();
    upsert.arena = arena.member_ptr();
    upsert.options = ustore_options_default_k;
    upsert.tasks_count = vtx_n_edges.size();
    upsert.collections = &collection;
    upsert.edges_ids = strided.edge_ids.begin().get();
    upsert.edges_stride = strided.edge_ids.stride();
    upsert.sources_ids = strided.source_ids.begin().get();
    upsert.sources_stride = strided.source_ids.stride();
    upsert.targets_ids = strided.target_ids.begin().get();
    upsert.targets_stride = strided.target_ids.stride();
    ustore_graph_upsert_edges(&upsert);
    EXPECT_TRUE(status);

    std::vector<ustore_key_t> all_keys;
    all_keys.reserve(vtx_n_edges.size() * 2);
    if (role == ustore_vertex_role_any_k || role == ustore_vertex_source_k)
        for (auto key : strided.source_ids)
            all_keys.push_back(key);

    if (role == ustore_vertex_role_any_k || role == ustore_vertex_target_k) {
        for (auto key : strided.target_ids)
            all_keys.push_back(key);
    }

    ustore_graph_remove_vertices_t remove {};
    remove.db = db;
    remove.error = status.member_ptr();
    remove.arena = arena.member_ptr();
    remove.options = ustore_options_default_k;
    remove.tasks_count = all_keys.size();
    remove.collections = &collection;
    remove.vertices = all_keys.data();
    remove.vertices_stride = sizeof(ustore_key_t);
    remove.roles = &role;
    ustore_graph_remove_vertices(&remove);
    EXPECT_TRUE(status);

    ustore_length_t count_limits = vtx_n_edges.size() * 2;
    ustore_length_t* found_counts = nullptr;
    ustore_key_t* found_keys = nullptr;
    ustore_key_t start_keys = 0;

    ustore_scan_t scan {};
    scan.db = db;
    scan.error = status.member_ptr();
    scan.arena = arena.member_ptr();
    scan.tasks_count = 1;
    scan.collections = &collection;
    scan.start_keys = &start_keys;
    scan.count_limits = &count_limits;
    scan.counts = &found_counts;
    scan.keys = &found_keys;
    ustore_scan(&scan);
    EXPECT_TRUE(status);
    if (role == ustore_vertex_role_any_k)
        EXPECT_EQ(*found_counts, 0);
    else if (role == ustore_vertex_source_k) {
        EXPECT_EQ(*found_counts, vtx_n_edges.size());
        size_t idx = 0;
        for (auto key : strided.target_ids) {
            EXPECT_EQ(key, found_keys[idx]);
            ++idx;
        }
    }
    else if (role == ustore_vertex_target_k) {
        EXPECT_EQ(*found_counts, vtx_n_edges.size());
        size_t idx = 0;
        for (auto key : strided.source_ids) {
            EXPECT_EQ(key, found_keys[idx]);
            ++idx;
        }
    }
    db.clear().throw_unhandled();
}

void test_simple_paths_read() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    ustore_octet_t* presences = nullptr;
    ustore_length_t* offsets = nullptr;
    ustore_length_t* lengths = nullptr;
    ustore_byte_t* values = nullptr;

    ustore_paths_read_t read {};
    read.db = db;
    read.error = status.member_ptr();
    read.arena = arena.member_ptr();
    read.options = ustore_options_default_k;
    read.tasks_count = paths.size();
    read.collections = &collection;
    read.paths = (ustore_str_view_t*)paths.front().member_ptr();
    read.paths_stride = sizeof(value_view_t);
    read.paths_lengths = paths.front().member_length();
    read.paths_lengths_stride = sizeof(value_view_t);
    read.presences = &presences;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;
    ustore_paths_read(&read);
    EXPECT_TRUE(status);

    strided_iterator_gt<ustore_length_t const> offs {offsets, sizeof(ustore_length_t)};
    strided_iterator_gt<ustore_length_t const> lens {lengths, sizeof(ustore_length_t)};
    strided_iterator_gt<ustore_bytes_cptr_t const> vals {&values, 0};
    bits_view_t preses {presences};

    contents_arg_t contents {preses, offs, lens, vals, keys.size()};

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }
}

void test_paths_write() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    triplet_t paths_trip;
    paths_trip.fill(paths);

    triplet_t values_trip;
    values_trip.fill(docs);

    ustore_paths_write_t write {};
    write.db = db;
    write.error = status.member_ptr();
    write.arena = arena.member_ptr();
    write.options = ustore_options_default_k;
    write.tasks_count = paths_trip.size();
    write.collections = &collection;
    write.paths = paths_trip.ptr();
    write.paths_offsets = paths_trip.offsets();
    write.paths_offsets_stride = sizeof(ustore_length_t);
    write.paths_lengths = paths_trip.lengths();
    write.paths_lengths_stride = sizeof(ustore_length_t);
    write.values_offsets = values_trip.offsets();
    write.values_offsets_stride = sizeof(ustore_length_t);
    write.values_lengths = values_trip.lengths();
    write.values_lengths_stride = sizeof(ustore_length_t);
    write.values_bytes = (ustore_bytes_cptr_t const*)values_trip.ptr();

    ustore_paths_write(&write);
    EXPECT_TRUE(status);
    test_simple_paths_read();
    db.clear().throw_unhandled();

    write.paths_lengths = nullptr;
    write.paths_lengths_stride = 0;
    ustore_paths_write(&write);
    EXPECT_TRUE(status);
    test_simple_paths_read();
    db.clear().throw_unhandled();

    write.paths = (ustore_str_view_t const*)paths[0].member_ptr();
    write.paths_stride = sizeof(value_view_t);
    write.paths_offsets = nullptr;
    write.paths_offsets_stride = 0;
    write.path_separator = ',';
    ustore_paths_write(&write);
    EXPECT_TRUE(status);
    test_simple_paths_read();
    db.clear().throw_unhandled();

    write.paths_lengths = paths[0].member_length();
    write.paths_lengths_stride = sizeof(value_view_t);
    ustore_paths_write(&write);
    EXPECT_TRUE(status);
    test_simple_paths_read();
    db.clear().throw_unhandled();

    write.values_lengths = nullptr;
    write.values_lengths_stride = 0;
    ustore_paths_write(&write);
    EXPECT_TRUE(status);
    test_simple_paths_read();
    db.clear().throw_unhandled();

    write.values_offsets = nullptr;
    write.values_offsets_stride = 0;
    write.values_lengths = docs[0].member_length();
    write.values_lengths_stride = sizeof(value_view_t);
    write.values_bytes = docs[0].member_ptr();
    write.values_bytes_stride = sizeof(value_view_t);
    ustore_paths_write(&write);
    EXPECT_TRUE(status);
    test_simple_paths_read();
    db.clear().throw_unhandled();
}

void test_paths_read() {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.main();

    triplet_t paths_trip;
    paths_trip.fill(paths);

    triplet_t values_trip;
    values_trip.fill(docs);

    ustore_paths_write_t write {};
    write.db = db;
    write.error = status.member_ptr();
    write.arena = arena.member_ptr();
    write.options = ustore_options_default_k;
    write.tasks_count = paths_trip.size();
    write.collections = &collection;
    write.paths = paths_trip.ptr();
    write.paths_offsets = paths_trip.offsets();
    write.paths_offsets_stride = sizeof(ustore_length_t);
    write.paths_lengths = paths_trip.lengths();
    write.paths_lengths_stride = sizeof(ustore_length_t);
    write.values_offsets = values_trip.offsets();
    write.values_offsets_stride = sizeof(ustore_length_t);
    write.values_lengths = values_trip.lengths();
    write.values_lengths_stride = sizeof(ustore_length_t);
    write.values_bytes = (ustore_bytes_cptr_t const*)values_trip.ptr();

    ustore_paths_write(&write);
    EXPECT_TRUE(status);

    ustore_octet_t* presences = nullptr;
    ustore_length_t* offsets = nullptr;
    ustore_length_t* lengths = nullptr;
    ustore_byte_t* values = nullptr;

    ustore_paths_read_t read {};
    read.db = db;
    read.error = status.member_ptr();
    read.arena = arena.member_ptr();
    read.options = ustore_options_default_k;
    read.tasks_count = paths_trip.size();
    read.collections = &collection;
    read.paths = paths_trip.ptr();
    read.paths_offsets = paths_trip.offsets();
    read.paths_offsets_stride = sizeof(ustore_length_t);
    read.paths_lengths = paths_trip.lengths();
    read.paths_lengths_stride = sizeof(ustore_length_t);
    read.presences = &presences;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;
    ustore_paths_read(&read);
    EXPECT_TRUE(status);

    strided_iterator_gt<ustore_length_t const> offs {offsets, sizeof(ustore_length_t)};
    strided_iterator_gt<ustore_length_t const> lens {lengths, sizeof(ustore_length_t)};
    strided_iterator_gt<ustore_bytes_cptr_t const> vals {&values, 0};
    bits_view_t preses {presences};

    contents_arg_t contents {preses, offs, lens, vals, keys.size()};

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }

    presences = nullptr;
    offsets = nullptr;
    lengths = nullptr;
    values = nullptr;
    read.paths_lengths = nullptr;
    read.paths_lengths_stride = 0;
    ustore_paths_read(&read);
    EXPECT_TRUE(status);

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }

    presences = nullptr;
    offsets = nullptr;
    lengths = nullptr;
    values = nullptr;
    read.paths = (ustore_str_view_t const*)paths[0].member_ptr();
    read.paths_stride = sizeof(value_view_t);
    read.paths_offsets = nullptr;
    read.paths_offsets_stride = 0;
    read.path_separator = ',';
    ustore_paths_read(&read);
    EXPECT_TRUE(status);

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }

    presences = nullptr;
    offsets = nullptr;
    lengths = nullptr;
    values = nullptr;
    write.paths_lengths = paths[0].member_length();
    write.paths_lengths_stride = sizeof(value_view_t);
    ustore_paths_read(&read);
    EXPECT_TRUE(status);

    for (size_t idx = 0; idx < paths.size(); ++idx) {
        EXPECT_EQ(std::strncmp(docs[idx].c_str(), contents[idx].c_str(), docs[idx].size()), 0);
    }
}

TEST(docs, read_n_write) {
    test_single_read_n_write();
    test_batch_read_n_write();
}

TEST(docs, gist) {
    test_gist();
}

// TEST(docs, gather) {
    // TODO: Fix ustore_docs_gather(): output values are wrong
    // test_gather();
// }

TEST(grpah, upsert) {
    test_graph_single_upsert();
    test_graph_batch_upsert_vtx();
    test_graph_batch_upsert_edges();
}

TEST(grpah, find) {
    test_graph_find();
}

TEST(grpah, remove) {
    test_graph_remove_edges();
    test_graph_remove_vertices(ustore_vertex_role_any_k);
    test_graph_remove_vertices(ustore_vertex_source_k);
    test_graph_remove_vertices(ustore_vertex_target_k);
}

TEST(paths, write) {
    test_paths_write();
}

TEST(paths, read) {
    test_paths_read();
}

int main(int argc, char** argv) {
    make_batch();
    db.open().throw_unhandled();
    ::testing::InitGoogleTest(&argc, argv);
    RUN_ALL_TESTS();
    return 0;
}