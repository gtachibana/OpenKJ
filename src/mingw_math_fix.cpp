#if defined(_WIN32) && defined(__GNUC__)
// Fix MinGW 32-bit pseudo-reloc overflow for the libm functions we call.
//
// Calls to these resolve to msvcrt.dll without __declspec(dllimport), so the
// linker emits a 32-bit pseudo-reloc entry for each one. Those are patched at
// process start by _pei386_runtime_relocator, before main() runs -- and if
// msvcrt.dll happens to load more than 2GB from the executable image, the
// 32-bit fixup overflows and the MinGW runtime aborts the process with
// "32 bit pseudo relocation ... out of range". The app dies with no window, no
// log output, and no crash handler: nothing of ours has run yet.
//
// Providing strong local definitions here causes all direct calls to resolve to
// these functions in .text (within 2GB of every caller), and the linker never
// generates pseudo-relocs for them. Each wrapper calls through the IAT pointer
// (__imp_<fn>), which is also in the local image.
//
// CMakeLists must pass -Wl,--allow-multiple-definition so the linker accepts
// these alongside the weak import stubs in libmsvcrt.a.
//
// NOTE: adding a call to a msvcrt math function that is not wrapped here can
// reintroduce the startup abort. It shows up only once the shipped DLL set is
// large enough to push msvcrt out of range, so it can land long after the code
// that caused it -- pow()/ceil() came from {fmt} in spdlog, sin() came from the
// cheer particle animation in dlgcdg.cpp. If you add one, wrap it here too.
extern "C" {
    extern double (*const __imp_pow)(double, double);
    extern double (*const __imp_ceil)(double);
    extern double (*const __imp_floor)(double);
    extern double (*const __imp_fmod)(double, double);
    extern double (*const __imp_sin)(double);
    extern double (*const __imp_cos)(double);
    extern double (*const __imp_tan)(double);
    extern double (*const __imp_atan2)(double, double);
    extern double (*const __imp_exp)(double);
    extern double (*const __imp_log)(double);
    extern double (*const __imp_log10)(double);

    double pow(double x, double y)   { return (*__imp_pow)(x, y); }
    double ceil(double x)            { return (*__imp_ceil)(x); }
    double floor(double x)           { return (*__imp_floor)(x); }
    double fmod(double x, double y)  { return (*__imp_fmod)(x, y); }
    double sin(double x)             { return (*__imp_sin)(x); }
    double cos(double x)             { return (*__imp_cos)(x); }
    double tan(double x)             { return (*__imp_tan)(x); }
    double atan2(double y, double x) { return (*__imp_atan2)(y, x); }
    double exp(double x)             { return (*__imp_exp)(x); }
    double log(double x)             { return (*__imp_log)(x); }
    double log10(double x)           { return (*__imp_log10)(x); }
}
#endif
