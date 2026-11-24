# DVD logo render smoke test

This test is a behavior-level recreation, not a decompilation of a specific
DVD player's screensaver. No verified public source/decompilation of the
original player implementation was found.

## Implemented behavior

- Official DVD-Video logo artwork on a black background.
- Constant-speed diagonal movement, advanced using elapsed time.
- The logo's rectangular bounds bounce independently from the screen edges.
- The tint changes on each horizontal or vertical wall impact.
- A large frame-time pause is clamped to keep the logo from visibly jumping.

The classic diagonal/bounce model is described in
[The Lost Math Lessons: Bouncing DVD Logo](http://lostmathlessons.blogspot.com/2016/03/bouncing-dvd-logo.html).
That describes the recognizable motion, not any one manufacturer's exact
speed, initial position, or color sequence.

## On-console checks

1. Build `360DVD.vcxproj` with the Xbox 360 SDK and the XDK MSBuild tools.
2. Copy `build\Release\360DVD.xex` and `build\Release\Media` to an RGH/JTAG
   console.
3. Confirm a clean black background and an undistorted logo.
4. Confirm smooth diagonal movement and no penetration past any screen edge.
5. Confirm the tint changes at each edge; test corner hits for a change on
   both axes.
6. Leave running for several minutes and watch for stalls, tearing, or GPU
   instability.

Hardware validation is still required. The declared API inventory in
`video-api-surface.json` is not a certification of device capabilities.

On startup, the app writes progress/error messages to `game:\360DVD.log` and
the XDK debug output. If it returns to the dashboard, check that log to see
which initialization stage failed.
