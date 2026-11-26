// Wire protocol between Tracy.luau and the bridge. See docs/PROTOCOL.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#  error "the bridge decodes Luau buffers (little-endian) with memcpy, so it needs a little-endian host"
#endif

namespace tlb
{

constexpr uint32_t kMagic = 0x41554C54;  // bytes "TLUA"
constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kHeaderSize = 36;

enum : uint16_t
{
    FlagFinal = 1 << 0,  // last batch of a session (VM shutting down / player left)
};

enum : uint8_t
{
    SourceServer = 0,  // 1-254: client VMs, numbered by the server that relays them
    SourceNamed = 255,  // a track named by the TRACK_NAME event that opens its session (imports)
};

enum Op : uint8_t
{
    OpRegisterString = 0x01,  // [id u16][len u16][bytes]
    OpZoneBegin = 0x02,       // [id u16][time f64]
    OpZoneEnd = 0x03,         // [time f64]
    OpFrameMark = 0x04,       // [time f64]
    OpPlotValue = 0x05,       // [id u16][time f64][value f64]
    OpFrameMarkNamed = 0x06,  // [id u16][time f64]
    OpZoneBeginColor = 0x07,  // [id u16][time f64][color u32 0xRRGGBB]
    OpMessage = 0x08,         // [time f64][color u32][len u16][bytes]
    OpPlotConfig = 0x09,      // [id u16][format u8][flags u8][color u32]
    OpTrackName = 0x0A,       // [len u16][bytes]  first event of a SourceNamed session
    OpZoneText = 0x0B,        // [len u16][bytes]  text on the innermost open zone
};

// The TRACK_NAME a SourceNamed batch opens with, if it has one.
inline std::string_view LeadingTrackName( const char* events, size_t length )
{
    if( length < 3 || uint8_t( events[0] ) != OpTrackName ) return {};
    uint16_t len = 0;
    std::memcpy( &len, events + 1, 2 );
    if( size_t( 3 ) + len > length ) return {};
    return std::string_view( events + 3, len );
}

enum : uint8_t
{
    PlotFlagStep = 1 << 0,
    PlotFlagFill = 1 << 1,
};

struct BatchHeader
{
    uint8_t version = 0;
    uint8_t source = 0;
    uint16_t flags = 0;
    uint32_t session = 0;
    uint32_t sequence = 0;
    double flushClock = 0;   // os.clock() when Luau sealed the batch
    double clockOffset = 0;  // add to this batch's times to get the relaying server's clock
    uint32_t length = 0;     // bytes of events after the header
};

class Reader
{
public:
    Reader( const char* data, size_t size )
        : m_begin( reinterpret_cast<const uint8_t*>( data ) ), m_p( m_begin ), m_end( m_begin + size )
    {
    }

    bool Empty() const { return m_p == m_end; }
    size_t Remaining() const { return size_t( m_end - m_p ); }
    size_t Offset() const { return size_t( m_p - m_begin ); }

    template <class T>
    bool Read( T& out )
    {
        if( Remaining() < sizeof( T ) ) return false;
        std::memcpy( &out, m_p, sizeof( T ) );
        m_p += sizeof( T );
        return true;
    }

    bool ReadBytes( size_t n, std::string_view& out )
    {
        if( Remaining() < n ) return false;
        out = std::string_view( reinterpret_cast<const char*>( m_p ), n );
        m_p += n;
        return true;
    }

    bool Skip( size_t n )
    {
        if( Remaining() < n ) return false;
        m_p += n;
        return true;
    }

private:
    const uint8_t* m_begin;
    const uint8_t* m_p;
    const uint8_t* m_end;
};

// Earliest event time in a batch's events (its own clock). False if it has none or
// the events can't be walked. Used to size the lead time when converting recordings.
inline bool EarliestEventTime( const char* events, size_t length, double& earliest )
{
    Reader r( events, length );
    bool found = false;
    auto see = [&]( size_t offset ) {
        double t = 0;
        std::memcpy( &t, events + offset, sizeof( t ) );
        if( t == t && ( !found || t < earliest ) )  // t == t: skip NaN
        {
            earliest = t;
            found = true;
        }
    };
    while( !r.Empty() )
    {
        const size_t at = r.Offset();
        uint8_t op = 0;
        r.Read( op );
        size_t size = 0;      // whole event, opcode included
        size_t timeAt = 0;    // offset of its time from the opcode, 0 = none
        uint16_t len = 0;
        switch( op )
        {
        case OpRegisterString:
            if( !r.Skip( 2 ) || !r.Read( len ) ) return found;
            size = 5 + len;
            break;
        case OpZoneBegin: size = 11, timeAt = 3; break;
        case OpZoneEnd: size = 9, timeAt = 1; break;
        case OpFrameMark: size = 9, timeAt = 1; break;
        case OpPlotValue: size = 19, timeAt = 3; break;
        case OpFrameMarkNamed: size = 11, timeAt = 3; break;
        case OpZoneBeginColor: size = 15, timeAt = 3; break;
        case OpMessage:
            if( !r.Skip( 12 ) || !r.Read( len ) ) return found;
            size = 15 + len;
            timeAt = 1;
            break;
        case OpPlotConfig: size = 9; break;
        case OpTrackName:
        case OpZoneText:
            if( !r.Read( len ) ) return found;
            size = 3 + len;
            break;
        default: return found;
        }
        if( at + size > length ) return found;
        if( timeAt ) see( at + timeAt );
        Reader skipTo( events, length );
        r = skipTo;
        r.Skip( at + size );
    }
    return found;
}

inline bool ParseHeader( Reader& r, BatchHeader& h, const char*& error )
{
    uint32_t magic = 0;
    if( !r.Read( magic ) || magic != kMagic )
    {
        error = "not a Tracy-Luau batch (bad magic)";
        return false;
    }
    if( !r.Read( h.version ) )
    {
        error = "truncated header";
        return false;
    }
    if( h.version != kProtocolVersion )
    {
        error = "unsupported protocol version; update Tracy.luau or the bridge so they match";
        return false;
    }
    if( !r.Read( h.source ) || !r.Read( h.flags ) || !r.Read( h.session ) || !r.Read( h.sequence ) ||
        !r.Read( h.flushClock ) || !r.Read( h.clockOffset ) || !r.Read( h.length ) )
    {
        error = "truncated header";
        return false;
    }
    if( h.length > r.Remaining() )
    {
        error = "batch length runs past the end of the request";
        return false;
    }
    return true;
}

}  // namespace tlb
