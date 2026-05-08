#include "chunk.h"
#include "bitstream.h"
#include "compress.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

using namespace std;

// ── CRC-64 (ECMA-182) ─────────────────────────────────────────────────────
// Simple table-driven CRC-64 using the ECMA polynomial.

static uint64_t crc64_table[256];
static bool     crc64_table_init = false;

static void init_crc64_table() {
    const uint64_t poly = 0x42F0E1EBA9EA3693ULL; // ECMA-182
    for (int i = 0; i < 256; ++i) {
        uint64_t crc = (uint64_t)i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1)
                crc = (crc >> 1) ^ poly;
            else
                crc >>= 1;
        }
        crc64_table[i] = crc;
    }
    crc64_table_init = true;
}

uint64_t crc64(const uint8_t* data, size_t len) {
    if (!crc64_table_init) init_crc64_table();
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (size_t i = 0; i < len; ++i) {
        uint8_t idx = (uint8_t)(crc ^ data[i]);
        crc = (crc >> 8) ^ crc64_table[idx];
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

// ── Little-endian helpers ──────────────────────────────────────────────────

static void write_le32(uint8_t* buf, uint32_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t* buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf[i] = (uint8_t)(v >> (i * 8));
}

static uint32_t read_le32(const uint8_t* buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static uint64_t read_le64(const uint8_t* buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)buf[i] << (i * 8);
    return v;
}

static int64_t read_le64_signed(const uint8_t* buf) {
    uint64_t v = read_le64(buf);
    int64_t s;
    memcpy(&s, &v, 8);
    return s;
}

// ── Ensure directory exists ────────────────────────────────────────────────

static bool ensure_dir(const string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return true;

    // Recursively create parent directories
    size_t pos = path.find_last_of('/');
    if (pos != string::npos && pos > 0) {
        string parent = path.substr(0, pos);
        if (!ensure_dir(parent))
            return false;
    }
    return mkdir(path.c_str(), 0755) == 0;
}

// ── Chunk Writer ───────────────────────────────────────────────────────────

FlushStats write_chunk(const string& dir,
                       const vector<int64_t>& timestamps,
                       const vector<double>&  values)
{
    FlushStats stats;
    if (timestamps.empty()) return stats;

    // Ensure the metric directory exists
    ensure_dir(dir);

    stats.point_count = timestamps.size();

    // --- Compress timestamps ---
    BitWriter ts_writer;
    TimestampEncoder ts_enc;
    for (int64_t ts : timestamps)
        ts_enc.encode(ts_writer, ts);
    ts_writer.flush();
    const vector<uint8_t>& ts_data = ts_writer.data();

    // --- Compress values ---
    BitWriter val_writer;
    ValueEncoder val_enc;
    for (double v : values)
        val_enc.encode(val_writer, v);
    val_writer.flush();
    const vector<uint8_t>& val_data = val_writer.data();

    stats.ts_bytes  = ts_data.size();
    stats.val_bytes = val_data.size();
    stats.header_bytes = CHUNK_HEADER_SIZE;

    // --- Build the file contents ---
    // Header (36 bytes) + ts_data + val_data + crc64 (8 bytes)
    size_t file_size = CHUNK_HEADER_SIZE + ts_data.size() + val_data.size() + 8;
    vector<uint8_t> file_buf(file_size);
    uint8_t* p = file_buf.data();

    // magic "TSDB"
    p[0] = 'T'; p[1] = 'S'; p[2] = 'D'; p[3] = 'B';
    // version = 2
    write_le32(p + 4, CHUNK_VERSION);
    // point_count
    write_le32(p + 8, (uint32_t)timestamps.size());
    // first_timestamp
    write_le64(p + 12, (uint64_t)timestamps.front());
    // last_timestamp
    write_le64(p + 20, (uint64_t)timestamps.back());
    // ts_bitstream_len
    write_le32(p + 28, (uint32_t)ts_data.size());
    // val_bitstream_len
    write_le32(p + 32, (uint32_t)val_data.size());

    // timestamp bitstream
    memcpy(p + CHUNK_HEADER_SIZE, ts_data.data(), ts_data.size());
    // value bitstream
    memcpy(p + CHUNK_HEADER_SIZE + ts_data.size(), val_data.data(), val_data.size());

    // CRC-64 over everything before the CRC field
    uint64_t checksum = crc64(p, file_size - 8);
    write_le64(p + file_size - 8, checksum);

    stats.total_bytes = file_size;

    // --- Write atomically: temp file, fsync, rename ---
    string chunk_name = to_string(timestamps.front()) + ".chunk";
    string final_path = dir + "/" + chunk_name;
    string tmp_path   = final_path + ".tmp";

    ofstream ofs(tmp_path, ios::binary | ios::trunc);
    if (!ofs) {
        cerr << "ERROR: cannot create tmp chunk file: " << tmp_path << "\n";
        stats.total_bytes = 0;
        return stats;
    }
    ofs.write(reinterpret_cast<const char*>(file_buf.data()), (streamsize)file_size);
    ofs.flush();
    ofs.close();

    // fsync the file
    int fd = open(tmp_path.c_str(), O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }

    // rename for atomicity
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        cerr << "ERROR: rename failed: " << tmp_path << " -> " << final_path << "\n";
        stats.total_bytes = 0;
        return stats;
    }

    return stats;
}

// ── Chunk Reader ───────────────────────────────────────────────────────────

ChunkData read_chunk(const string& path) {
    ChunkData result;

    ifstream ifs(path, ios::binary | ios::ate);
    if (!ifs) {
        cerr << "ERROR: cannot open chunk: " << path << "\n";
        return result;
    }

    size_t file_size = (size_t)ifs.tellg();
    if (file_size < CHUNK_HEADER_SIZE + 8) {
        cerr << "ERROR: chunk file too small: " << path << "\n";
        return result;
    }

    ifs.seekg(0);
    vector<uint8_t> buf(file_size);
    ifs.read(reinterpret_cast<char*>(buf.data()), (streamsize)file_size);
    ifs.close();

    const uint8_t* p = buf.data();

    // Verify magic
    if (p[0] != 'T' || p[1] != 'S' || p[2] != 'D' || p[3] != 'B') {
        cerr << "ERROR: bad magic in chunk: " << path << "\n";
        return result;
    }

    // Verify version
    uint32_t version = read_le32(p + 4);
    if (version != CHUNK_VERSION) {
        cerr << "ERROR: unsupported chunk version " << version << " in " << path << "\n";
        return result;
    }

    uint32_t point_count    = read_le32(p + 8);
    int64_t  first_ts       = read_le64_signed(p + 12);
    int64_t  last_ts        = read_le64_signed(p + 20);
    uint32_t ts_stream_len  = read_le32(p + 28);
    uint32_t val_stream_len = read_le32(p + 32);

    // Verify sizes
    size_t expected = CHUNK_HEADER_SIZE + ts_stream_len + val_stream_len + 8;
    if (expected != file_size) {
        cerr << "ERROR: chunk size mismatch in " << path
             << " (expected " << expected << ", got " << file_size << ")\n";
        return result;
    }

    // Verify CRC
    uint64_t stored_crc   = read_le64(p + file_size - 8);
    uint64_t computed_crc = crc64(p, file_size - 8);
    if (stored_crc != computed_crc) {
        cerr << "ERROR: CRC mismatch in chunk: " << path << "\n";
        return result;
    }

    // Decompress timestamps
    const uint8_t* ts_buf = p + CHUNK_HEADER_SIZE;
    BitReader ts_reader(ts_buf, ts_stream_len);
    TimestampDecoder ts_dec;
    result.timestamps.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i)
        result.timestamps.push_back(ts_dec.decode(ts_reader));

    // Decompress values
    const uint8_t* val_buf = p + CHUNK_HEADER_SIZE + ts_stream_len;
    BitReader val_reader(val_buf, val_stream_len);
    ValueDecoder val_dec;
    result.values.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i)
        result.values.push_back(val_dec.decode(val_reader));

    result.first_ts = first_ts;
    result.last_ts  = last_ts;
    result.valid    = true;
    return result;
}

// ── Chunk Metadata Reader (header only, no decompression) ──────────────────

ChunkMeta read_chunk_meta(const string& path) {
    ChunkMeta meta;
    meta.filepath = path;

    ifstream ifs(path, ios::binary);
    if (!ifs) return meta;

    uint8_t hdr[CHUNK_HEADER_SIZE];
    ifs.read(reinterpret_cast<char*>(hdr), CHUNK_HEADER_SIZE);
    if (!ifs || ifs.gcount() < (streamsize)CHUNK_HEADER_SIZE) return meta;

    // Verify magic
    if (hdr[0] != 'T' || hdr[1] != 'S' || hdr[2] != 'D' || hdr[3] != 'B')
        return meta;

    meta.point_count = read_le32(hdr + 8);
    meta.first_ts    = read_le64_signed(hdr + 12);
    meta.last_ts     = read_le64_signed(hdr + 20);
    return meta;
}
