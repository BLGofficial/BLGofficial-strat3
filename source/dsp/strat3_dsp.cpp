// Anchor translation unit: gives the static library a stable object even before any
// DSP module has a .cpp (most modules are header-only for inlining in the audio loop).
namespace strat3::dsp { int libraryAnchor() noexcept { return 0; } }
