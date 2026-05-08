#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
using namespace std;
static constexpr uint32_t CHUNK_MAGIC   = 0x42445354;   
static constexpr uint32_t CHUNK_VERSION = 2;
static constexpr size_t   CHUNK_HEADER_SIZE = 36;        

uint64_t crc64(const uint8_t* data, size_t len);

struct ChunkHeader {
    uint32_t magic          = CHUNK_MAGIC;
    uint32_t version        = CHUNK_VERSION;
    uint32_t point_count    = 0;
    int64_t  first_ts       = 0;
    int64_t  last_ts        = 0;
    uint32_t ts_stream_len  = 0;
    uint32_t val_stream_len = 0;
};

struct FlushStats {
    size_t total_bytes   = 0;
    size_t header_bytes  = 0;
    size_t ts_bytes      = 0;
    size_t val_bytes     = 0;
    size_t point_count   = 0;
};

FlushStats write_chunk(const string& dir,
                       const vector<int64_t>& timestamps,
                       const vector<double>&  values);

struct ChunkData {
    vector<int64_t> timestamps;
    vector<double>  values;
    int64_t first_ts = 0;
    int64_t last_ts  = 0;
    bool    valid    = false;
};

ChunkData read_chunk(const string& path);

struct ChunkMeta {
    string filepath;
    int64_t     first_ts    = 0;
    int64_t     last_ts     = 0;
    uint32_t    point_count = 0;
};

ChunkMeta read_chunk_meta(const string& path);
