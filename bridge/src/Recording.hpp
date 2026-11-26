// Raw recordings (.tlrec): every HTTP request the bridge received, with its arrival
// time, so a session can be replayed into Tracy later (`--convert`).
//
//   file:    "TLUAREC1" | u32 protocol version | u32 reserved
//   record:  u64 arrival ns since the first record | u32 length | length bytes (request body)
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace tlb
{

class Recorder
{
public:
    ~Recorder();
    bool Open( const std::string& path, std::string& error );
    // Thread-safe: HTTP handlers call this concurrently.
    void Write( int64_t nowNs, const std::string& body );
    void Close();
    bool IsOpen() const { return m_file != nullptr; }
    uint64_t Records() const { return m_records; }
    uint64_t Bytes() const { return m_bytes; }

private:
    std::mutex m_lock;
    FILE* m_file = nullptr;
    bool m_started = false;
    bool m_failed = false;
    int64_t m_startNs = 0;
    uint64_t m_records = 0;
    uint64_t m_bytes = 0;
};

class RecordingReader
{
public:
    ~RecordingReader();
    bool Open( const std::string& path, std::string& error );
    // False at the end. A record cut short (the bridge was killed mid-write) also
    // ends the recording; Truncated() then says so.
    bool Next( int64_t& arrivalNs, std::string& body );
    bool Truncated() const { return m_truncated; }

private:
    FILE* m_file = nullptr;
    bool m_truncated = false;
};

// "session.tracy" -> "session.tlrec"
std::string RawPathFor( const std::string& tracyPath );

}  // namespace tlb
