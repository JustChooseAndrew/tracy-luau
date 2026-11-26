// Emits Tracy events with caller-supplied timestamps.
//
// Tracy's public macros (ZoneScoped, FrameMark, TracyPlot) stamp events with the
// current time. Roblox events reach the bridge ~150 ms after they happened, so we
// write the same queue items those macros write, but with the Luau-recorded time
// mapped onto Tracy's clock. This uses Tracy's internal queue API, which is why the
// bridge pins one Tracy version (see third_party/tracy/VERSION).
//
// Zones go to the calling thread's queue, so every replay lane runs on its own OS
// thread and shows up in the viewer as its own track.
#pragma once

#include "client/TracyProfiler.hpp"

#include <cstring>

namespace tlb::emit
{

// Tracy's clock. The bridge builds with TRACY_DISALLOW_HW_TIMER, so this is a
// steady clock in nanoseconds and Luau seconds map onto it with one offset.
inline int64_t Now()
{
    return tracy::Profiler::GetTime();
}

inline void ZoneBegin( const tracy::SourceLocationData* srcloc, int64_t time )
{
    TracyQueuePrepare( tracy::QueueType::ZoneBegin );
    tracy::MemWrite( &item->zoneBegin.time, time );
    tracy::MemWrite( &item->zoneBegin.srcloc, (uint64_t)srcloc );
    TracyQueueCommit( zoneBeginThread );
}

inline void ZoneEnd( int64_t time )
{
    TracyQueuePrepare( tracy::QueueType::ZoneEnd );
    tracy::MemWrite( &item->zoneEnd.time, time );
    TracyQueueCommit( zoneEndThread );
}

// name == nullptr is Tracy's default frame set.
inline void FrameMark( const char* name, int64_t time )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::FrameMarkMsg );
    tracy::MemWrite( &item->frameMark.time, time );
    tracy::MemWrite( &item->frameMark.name, uint64_t( name ) );
    tracy::Profiler::QueueSerialFinish();
}

inline void Plot( const char* name, int64_t time, double value )
{
    TracyLfqPrepare( tracy::QueueType::PlotDataDouble );
    tracy::MemWrite( &item->plotDataDouble.name, (uint64_t)name );
    tracy::MemWrite( &item->plotDataDouble.time, time );
    tracy::MemWrite( &item->plotDataDouble.val, value );
    TracyLfqCommit;
}

// A timeline message on the calling thread's track. color 0 = the viewer's default.
inline void Message( const char* text, size_t length, uint32_t color, int64_t time )
{
    auto copy = (char*)tracy::tracy_malloc( length );  // Tracy frees it once sent
    std::memcpy( copy, text, length );
    const tracy::TaggedUserlandAddress tagged{
        (uint64_t)copy, tracy::MakeMessageMetadata( tracy::MessageSourceType::User, tracy::MessageSeverity::Info ) };
    if( color != 0 )
    {
        TracyQueuePrepare( tracy::QueueType::MessageColor );
        tracy::MemWrite( &item->messageColorFat.time, time );
        tracy::MemWrite( &item->messageColorFat.textAndMetadata, tagged );
        tracy::MemWrite( &item->messageColorFat.b, uint8_t( color & 0xFF ) );
        tracy::MemWrite( &item->messageColorFat.g, uint8_t( ( color >> 8 ) & 0xFF ) );
        tracy::MemWrite( &item->messageColorFat.r, uint8_t( ( color >> 16 ) & 0xFF ) );
        tracy::MemWrite( &item->messageColorFat.size, (uint16_t)length );
        TracyQueueCommit( messageColorFatThread );
    }
    else
    {
        TracyQueuePrepare( tracy::QueueType::Message );
        tracy::MemWrite( &item->messageFat.time, time );
        tracy::MemWrite( &item->messageFat.textAndMetadata, tagged );
        tracy::MemWrite( &item->messageFat.size, (uint16_t)length );
        TracyQueueCommit( messageFatThread );
    }
}

// Text on the calling thread's innermost open zone (several calls add lines).
inline void ZoneText( const char* text, size_t length )
{
    auto copy = (char*)tracy::tracy_malloc( length );  // Tracy frees it once sent
    std::memcpy( copy, text, length );
    TracyQueuePrepare( tracy::QueueType::ZoneText );
    tracy::MemWrite( &item->zoneTextFat.text, (uint64_t)copy );
    tracy::MemWrite( &item->zoneTextFat.size, (uint16_t)length );
    TracyQueueCommit( zoneTextFatThread );
}

// Plot display settings. Tracy keeps these and re-sends them to every viewer that
// connects later, so one call per plot name is enough.
inline void ConfigurePlot( const char* name, uint8_t format, bool step, bool fill, uint32_t color )
{
    tracy::Profiler::ConfigurePlot( name, tracy::PlotFormatType( format ), step, fill, color );
}

inline bool ViewerConnected()
{
    return tracy::GetProfiler().IsConnected();
}

// Changes every time a viewer connects (the bridge builds with TRACY_ON_DEMAND).
inline uint64_t ConnectionId()
{
    return tracy::GetProfiler().ConnectionId();
}

}  // namespace tlb::emit
