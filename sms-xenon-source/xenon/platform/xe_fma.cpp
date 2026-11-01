// fmaf and fma for the Xbox 360 build: VS2010's CRT has neither, and the XDK
// compiler has no fused multiply-add intrinsic. Compiled WITHOUT /fp:precise
// (xenon/README.md), where the compiler contracts a*b+c into one fmadds/fmadd,
// which rounds once: exactly C99's fma. Everything else builds with
// /fp:precise, which contracts nothing, as the port's -ffp-contract=off.
extern "C" __declspec(noinline) float fmaf(float a, float b, float c) { return a * b + c; }
extern "C" __declspec(noinline) double fma(double a, double b, double c) { return a * b + c; }
