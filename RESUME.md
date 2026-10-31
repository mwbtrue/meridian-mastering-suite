# RESUME (updated 2026-10-31)
HEAD pushed: ec80d46 (EBU 3341/3342 test, 32/32 pass). Token refresh: Git_Tool get_github_access_token -> ~/.github-credentials.
Remaining, in order:
1. tests/chain_null_test.cpp: link test exe against target MeridianMasteringSuite (SharedCode lib);
   add include dirs via $<TARGET_PROPERTY:MeridianMasteringSuite,INCLUDE_DIRECTORIES> + COMPILE_DEFINITIONS;
   ScopedJuceInitialiser_GUI; createPluginFilter(); (a) all module_bypass on, (b) neutral params; pink -20 dBFS; compare vs input delayed getLatencySamples(); < -140 dBFS.
   Neutral needs: dither_bits=2, sat_mix 0 / clipper_enabled off, limiter ceiling vs signal, ToneEQ gains 0, img widths 100, lec_mono off, deq/glue/mbd thresholds 0.
2. tests/cpu_bench.cpp default chain 48k/512 60 s -> % core; target <15%. Limiter 16x backstop = 7.4% alone.
3. Loudness-matched bypass: global bool lm_bypass; per-module out/in power ratio tracker (3 s) in processModuleInSlot, frozen + applied (50 ms smooth, +-12 dB) when bypassed.
4. UndoManager member declared BEFORE apvts in PluginProcessor.h; pass &undoManager at line 336 of .cpp; editor Undo/Redo + Ctrl+Z/Ctrl+Shift+Z; beginNewTransaction on timer.
5. pluginval strictness 10 with GUI (DISPLAY=:1); ARCH.md/README update; push; zip.
Build deps reinstall: sudo -n apt-get update -q; sudo -n apt-get install -y -q libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfreetype-dev libglu1-mesa-dev mesa-common-dev libfontconfig1-dev
