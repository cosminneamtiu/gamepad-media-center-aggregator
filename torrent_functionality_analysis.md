# Torrent Functionality Analysis Plan for Nintendo Switch Branch

## Overview

The goal is to analyze and fix the torrent functionality in the Nintendo Switch branch of the Gamepad Media Center Aggregator (GMCA) application, where torrents don't work despite having enabled the `ENABLE_TORRENT` flag in CI builds. The specific symptom is "no sources available" when trying to watch shows that should have torrent sources.

## Current State Analysis

1. **Codebase Structure**:
   - Dedicated `torrent/` directory with implementation files
   - CMakeLists.txt includes an `ENABLE_TORRENT` option that's OFF by default  
   - In CI, Nintendo Switch builds include `-DENABLE_TORRENT=ON`
   - Code has proper conditional compilation (`#if defined(ENABLE_TORRENT)`)
   
2. **Expected Flow**:
   - When StremioBackend::resolvePlayback is called for a torrent source
   - With `ENABLE_TORRENT` defined, it calls `torrent::EngineSession::instance().open()`
   - This should initialize the torrent engine and return an HTTP URL for mpv to play

3. **Current Problem**: 
   - The symptom "no sources available" indicates that torrents aren't being detected or returned by Stremio addons
   - Even though `ENABLE_TORRENT=ON` is set, the backend isn't properly recognizing or processing torrent sources

## Investigation Plan

### Phase 1: Configuration and Build Verification
- Verify that `ENABLE_TORRENT` is actually compiled into the Switch build 
- Check CMakeLists.txt logic for platform-specific behavior
- Examine if there are additional flags needed for Switch specifically
- Review CI workflow to confirm correct flag usage in all contexts

### Phase 2: Stremio Backend Integration Analysis  
**Key Focus**: The `resolvePlayback` method and source detection flow
- **Core Issue**: When Stremio backend processes media items, it should detect torrent sources but doesn't 
- Examine how the addon system works with torrent extensions on Switch specifically
- Check if `media::SourceKind::Torrent` is being properly identified during item resolution

### Phase 3: Runtime Analysis  
- Add comprehensive logging to track:
  - When `resolvePlayback` is called and what sources are passed in
  - If `version.kind == media::SourceKind::Torrent` condition is met
  - Whether the torrent engine gets invoked or if it fails silently before reaching that point
- Monitor network connectivity and tracker communication attempts on Switch

### Phase 4: Platform-Specific Issues Investigation
**Key Suspicions**:
1. **Addon Loading Failure**: The Stremio torrent extensions aren't being properly loaded on Switch 
2. **Network Stack Limitations**: Nintendo Switch has specific networking constraints that prevent torrents from working
3. **libusbhsfs Conflicts**: When both `ENABLE_TORRENT=ON` and `USE_LIBUSBHSFS=ON`, there could be resource conflicts or initialization order issues  
4. **Missing Runtime Dependencies**: The Switch build might be missing libraries needed for torrent functionality

## Root Cause Hypotheses  

1. **Stremio Addon Integration Failure**: The torrent extensions aren't being properly loaded in the Switch environment, so they never surface as playable sources

2. **Conditional Compilation Issue**: Despite `ENABLE_TORRENT=ON`, there are additional conditional checks that exclude Switch code paths when processing sources for Stremio backends 

3. **Network Stack Problems**: The Switch's network implementation may not support the socket operations required by torrent engine, causing silent failures in source detection

4. **Missing Runtime Dependencies**: While build-time flags might be correct, runtime libraries or services needed for torrents aren't available on Switch

## Testing Approach  

1. **Local Debug Build with Enhanced Logging**:
   - Create a local debug version with `ENABLE_TORRENT=ON` and `PLATFORM_SWITCH=ON`
   - Add detailed logging throughout the entire source detection/resolution flow
   - Compare to working desktop builds for differences in behavior

2. **Stremio Backend Tracing**: 
   - Trace through how Stremio addons are loaded and processed on Switch
   - Verify that torrent sources (e.g., "torrentio" addon) are actually present in the available backends

3. **Network Monitoring**:
   - Monitor network calls to see if any tracker connections are attempted at all
   - Check if there are errors during the source loading phase before reaching torrent engine

## Expected Outcomes 

- Identify whether this is an issue with Stremio addon integration, platform networking limitations, or build configuration 
- Specific code changes needed to resolve Switch torrent functionality for source detection
- Documentation of any Nintendo Switch limitations that prevent proper torrent extension support

This analysis should help determine if the problem lies in:
1. The Stremio backend not detecting torrents properly (source level)
2. The torrent engine not being invoked (execution level)  
3. Network connectivity issues preventing torrent functionality from working once activated

The "no sources available" symptom strongly points to issue at the source detection/resolution layer, indicating that either the addon system isn't loading properly or the backend isn't recognizing the torrent sources as valid.
