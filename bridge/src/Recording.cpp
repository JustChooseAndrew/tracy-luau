#include "Recording.hpp"

#include "Protocol.hpp"

#include <cstring>

namespace tlb
{

namespace
{
constexpr char kRecMagic[8] = { 'T', 'L', 'U', 'A', 'R', 'E', 'C', '1' };
constexpr uint32_t kMaxRecord = 256u * 1024 * 1024;  // sanity limit when reading
}  // namespace

std::string RawPathFor( const std::string& tracyPath )
{
    const size_t slash = tracyPath.find_last_of( "/\\" );
    const size_t dot = tracyPath.find_last_of( '.' );
    if( dot != std::string::npos && ( slash == std::string::npos || dot > slash ) )
    {
        return tracyPath.substr( 0, dot ) + ".tlrec";
    }
    return tracyPath + ".tlrec";
}

// ---------------------------------------------------------------- Recorder

Recorder::~Recorder()
{
    Close();
}

bool Recorder::Open( const std::string& path, std::string& error )
{
    m_file = std::fopen( path.c_str(), "wb" );
    if( !m_file )
    {
        error = "could not create " + path;
        return false;
    }
    const uint32_t version = kProtocolVersion;
    const uint32_t reserved = 0;
    if( std::fwrite( kRecMagic, 1, 8, m_file ) != 8 || std::fwrite( &version, 4, 1, m_file ) != 1 ||
        std::fwrite( &reserved, 4, 1, m_file ) != 1 )
    {
        error = "could not write to " + path;
        std::fclose( m_file );
        m_file = nullptr;
        return false;
    }
    std::fflush( m_file );
    return true;
}

void Recorder::Write( int64_t nowNs, const std::string& body )
{
    std::lock_guard<std::mutex> lock( m_lock );
    if( !m_file || m_failed ) return;
    if( !m_started )
    {
        m_started = true;
        m_startNs = nowNs;
    }
    const uint64_t arrival = uint64_t( nowNs > m_startNs ? nowNs - m_startNs : 0 );
    const uint32_t length = uint32_t( body.size() );
    if( std::fwrite( &arrival, 8, 1, m_file ) != 1 || std::fwrite( &length, 4, 1, m_file ) != 1 ||
        ( length && std::fwrite( body.data(), 1, length, m_file ) != length ) )
    {
        m_failed = true;
        std::fprintf( stderr, "recording: write failed (disk full?); recording stopped\n" );
        return;
    }
    // Flushed per request (a few per second), so a crash loses at most one.
    std::fflush( m_file );
    m_records++;
    m_bytes += length;
}

void Recorder::Close()
{
    std::lock_guard<std::mutex> lock( m_lock );
    if( m_file )
    {
        std::fclose( m_file );
        m_file = nullptr;
    }
}

// ---------------------------------------------------------------- RecordingReader

RecordingReader::~RecordingReader()
{
    if( m_file ) std::fclose( m_file );
}

bool RecordingReader::Open( const std::string& path, std::string& error )
{
    m_file = std::fopen( path.c_str(), "rb" );
    if( !m_file )
    {
        error = "could not open " + path;
        return false;
    }
    char magic[8];
    uint32_t version = 0, reserved = 0;
    if( std::fread( magic, 1, 8, m_file ) != 8 || std::memcmp( magic, kRecMagic, 8 ) != 0 ||
        std::fread( &version, 4, 1, m_file ) != 1 || std::fread( &reserved, 4, 1, m_file ) != 1 )
    {
        error = path + " is not a Tracy-Luau recording (.tlrec)";
        return false;
    }
    if( version != kProtocolVersion )
    {
        error = path + " was recorded with protocol " + std::to_string( version ) + "; this bridge speaks " +
                std::to_string( kProtocolVersion );
        return false;
    }
    return true;
}

bool RecordingReader::Next( int64_t& arrivalNs, std::string& body )
{
    uint64_t arrival = 0;
    uint32_t length = 0;
    const size_t got = std::fread( &arrival, 1, 8, m_file );
    if( got == 0 ) return false;  // clean end
    if( got != 8 || std::fread( &length, 4, 1, m_file ) != 1 || length > kMaxRecord )
    {
        m_truncated = true;
        return false;
    }
    body.resize( length );
    if( length && std::fread( body.data(), 1, length, m_file ) != length )
    {
        m_truncated = true;
        return false;
    }
    arrivalNs = int64_t( arrival );
    return true;
}

}  // namespace tlb
